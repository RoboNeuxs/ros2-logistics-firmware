#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "board_config.h"
#include "motor_hal.h"
#include "app.h"
#include "encoder.h"
#include "protocol.h"
#include "serial_link.h"

SysTick_Type test_systick;
SCB_Type test_scb;
uint32_t test_primask;
uint8_t test_priority[64],test_irq_enabled[64];
void TIM2_IRQHandler(void);
void TIM3_IRQHandler(void);
void TIM4_IRQHandler(void);
void EXTI15_10_IRQHandler(void);
void USART2_IRQHandler(void);
void SysTick_Handler(void);

static unsigned capture_reads;
static u8 capture_injection;
static unsigned watchdog_feeds,watchdog_starts,watchdog_reads;
static unsigned watchdog_time,watchdog_last_feed;
static u8 watchdog_unlocked,watchdog_updates,watchdog_stuck,watchdog_expired;
enum { AFTER_SNAPSHOT=1, BEFORE_CLEAR, AFTER_CLEAR, BEFORE_SECOND_READ, TWICE_AFTER_CLEAR };

static void capture_event(TIM_TypeDef *timer)
{
    if(timer->SR&TIM_SR_CC1IF) timer->SR |= TIM_SR_CC1OF;
    ++timer->CCR1;
    timer->SR |= TIM_SR_CC1IF;
}
u32 Test_CaptureStatus(TIM_TypeDef *timer)
{
    u32 status;
    ++capture_reads;
    if(capture_injection==BEFORE_SECOND_READ && capture_reads==2) {
        capture_event(timer); capture_injection=0;
    }
    status=timer->SR;
    if(capture_injection==AFTER_SNAPSHOT && capture_reads==1) {
        capture_event(timer); capture_injection=0;
    }
    return status;
}
void Test_CaptureClear(TIM_TypeDef *timer,u32 flags)
{
    if(flags==TIM_SR_CC1IF && capture_injection==BEFORE_CLEAR) {
        capture_event(timer); capture_injection=0;
    }
    if(flags&TIM_SR_CC1OF) assert((app_fault&FAULT_ENCODER) && !(TIM1->BDTR&TIM_BDTR_MOE));
    timer->SR &= ~flags;
    if(flags==TIM_SR_CC1IF && (capture_injection==AFTER_CLEAR || capture_injection==TWICE_AFTER_CLEAR)) {
        capture_event(timer);
        if(capture_injection==TWICE_AFTER_CLEAR) capture_event(timer);
        capture_injection=0;
    }
}
void Test_WatchdogWrite(volatile u32 *reg,u32 value)
{
    *reg=value;
    if(reg==&IWDG->KR) {
        if(value==0xCCCCU) { ++watchdog_starts; RCC->CSR |= RCC_CSR_LSION; }
        else if(value==0x5555U) watchdog_unlocked=1;
        else if(value==0xAAAAU) { ++watchdog_feeds; watchdog_last_feed=watchdog_time; }
    } else {
        assert(watchdog_unlocked);
        *(volatile u32 *)&IWDG->SR |= reg==&IWDG->PR ? IWDG_SR_PVU : IWDG_SR_RVU;
        watchdog_updates=2;
    }
}
u32 Test_WatchdogStatus(void)
{
    ++watchdog_reads;
    if(!watchdog_stuck && watchdog_updates && --watchdog_updates==0)
        *(volatile u32 *)&IWDG->SR=0;
    return IWDG->SR;
}
static void watchdog_advance(unsigned milliseconds)
{
    unsigned timeout=(IWDG->RLR+1U)*(4U<<IWDG->PR)/40U;
    watchdog_time+=milliseconds;
    if(watchdog_starts && watchdog_time-watchdog_last_feed>=timeout) watchdog_expired=1;
}
static void edge(TIM_TypeDef *timer,void (*irq)(void)) { capture_event(timer); irq(); }
static void rx(uint8_t value,uint32_t status) { USART2->DR=value; USART2->SR=status; USART2_IRQHandler(); }
static void setup(void)
{
    memset((void *)PERIPH_BASE,0,0x30000);
    memset(&test_systick,0,sizeof(test_systick));
    capture_reads=capture_injection=0;
    watchdog_feeds=watchdog_starts=watchdog_reads=watchdog_time=watchdog_last_feed=0;
    watchdog_unlocked=watchdog_updates=watchdog_stuck=watchdog_expired=0;
    App_Init();
    GPIOA->IDR=(1U<<1)|(1U<<7);
    GPIOB->IDR=(1U<<7)|(1U<<13);
}
static void configuration(void)
{
    u16 compares[4]={0,1800,3600,65535};
    setup();
    assert(TIM1->PSC==0 && TIM1->ARR==3599 && PWM_COUNTS==3600);
    assert(TIM1->CCER==0x1111 && TIM1->CR2==0);
    assert(TIM1->BDTR==(TIM_BDTR_OSSI|TIM_BDTR_OSSR));
    assert((GPIOA->CRH&0xFFFF)==0xAAAA);
    assert(((GPIOB->CRL>>8)&15)==4); /* PB2 is an input, never STBY. */
    assert((GPIOA->CRL&0xFF)==0x88 && ((GPIOB->CRL>>24)&0xFF)==0x88);
    assert((GPIOB->CRH&0xFF0000)==0x880000);
    assert(AFIO->MAPR==AFIO_MAPR_SWJ_CFG_JTAGDISABLE);
    assert((AFIO->EXTICR[3]&15)==1 && EXTI->FTSR&(1U<<12));
    assert(USART2->BRR==313 && USART2->CR2==0 && USART2->CR3==0);
    assert(SysTick->LOAD==71999 && SysTick->CTRL==7 && test_primask==0);
    assert(test_irq_enabled[TIM2_IRQn+16] && test_irq_enabled[TIM3_IRQn+16]);
    assert(test_irq_enabled[TIM4_IRQn+16] && test_irq_enabled[EXTI15_10_IRQn+16]);
    assert(test_priority[SysTick_IRQn+16]==0 && test_priority[USART2_IRQn+16]==2);
    Hal_PWM(compares);
    assert(TIM1->CCR1==0 && TIM1->CCR2==1800 && TIM1->CCR3==3600 && TIM1->CCR4==3600);
    Hal_Directions(5);
    assert(GPIOB->BSRR==((1U<<0)|(1U<<10)|((1U<<1)|(1U<<11))<<16));
    Hal_Enable(1); assert(TIM1->BDTR&TIM_BDTR_MOE);
    Hal_Fault(FAULT_TIMEOUT); assert(!(TIM1->BDTR&TIM_BDTR_MOE));
    Hal_Enable(1); assert(!(TIM1->BDTR&TIM_BDTR_MOE));
    assert(test_primask==0);
    test_primask=1; Hal_Enable(0); assert(test_primask==1); test_primask=0;
}
static void encoders(void)
{
    u32 counts[4];
    setup();
    edge(TIM2,TIM2_IRQHandler); edge(TIM3,TIM3_IRQHandler);
    edge(TIM4,TIM4_IRQHandler); EXTI->PR=1U<<12; EXTI15_10_IRQHandler();
    Encoder_ReadAll(counts);
    assert(counts[0]==1 && counts[1]==1 && counts[2]==1 && counts[3]==1);
    GPIOA->IDR=GPIOB->IDR=0;
    edge(TIM2,TIM2_IRQHandler); edge(TIM3,TIM3_IRQHandler);
    edge(TIM4,TIM4_IRQHandler); EXTI->PR=1U<<12; EXTI15_10_IRQHandler();
    Encoder_ReadAll(counts);
    assert(!counts[0] && !counts[1] && !counts[2] && !counts[3]);
    Hal_Enable(1);
    TIM3->SR=TIM_SR_CC1IF|TIM_SR_CC1OF; TIM3_IRQHandler();
    assert(app_fault&FAULT_ENCODER && fault_channels==2 && !(TIM1->BDTR&TIM_BDTR_MOE));
}
static void capture_races(void)
{
    u32 counts[4];
    u8 phase;
    setup();
    edge(TIM2,TIM2_IRQHandler); TIM2_IRQHandler();
    Encoder_ReadAll(counts);
    assert(counts[0]==1 && !(TIM2->SR&TIM_SR_CC1IF) && !app_fault);
    for(phase=AFTER_SNAPSHOT;phase<=BEFORE_CLEAR;++phase) {
        setup(); Hal_Enable(1);
        capture_injection=phase;
        edge(TIM2,TIM2_IRQHandler);
        Encoder_ReadAll(counts);
        assert(app_fault==FAULT_ENCODER && fault_channels==1 && !counts[0]);
        assert(!(TIM1->BDTR&TIM_BDTR_MOE) && !(TIM2->SR&(TIM_SR_CC1IF|TIM_SR_CC1OF)));
    }
    for(phase=AFTER_CLEAR;phase<=BEFORE_SECOND_READ;++phase) {
        setup(); capture_injection=phase;
        edge(TIM2,TIM2_IRQHandler);
        Encoder_ReadAll(counts);
        assert(counts[0]==1 && !app_fault && (TIM2->SR&TIM_SR_CC1IF));
        TIM2_IRQHandler(); TIM2_IRQHandler();
        Encoder_ReadAll(counts);
        assert(counts[0]==2 && !app_fault && !(TIM2->SR&TIM_SR_CC1IF));
    }
    setup(); Hal_Enable(1); capture_injection=TWICE_AFTER_CLEAR;
    edge(TIM2,TIM2_IRQHandler);
    assert(app_fault==FAULT_ENCODER && fault_channels==1 && !(TIM1->BDTR&TIM_BDTR_MOE));
    assert((TIM2->SR&TIM_SR_CC1IF) && !(TIM2->SR&TIM_SR_CC1OF));
    setup(); Hal_Enable(1); TIM3->SR=TIM_SR_CC1OF;
    TIM3_IRQHandler();
    assert(app_fault==FAULT_ENCODER && fault_channels==2 && !(TIM3->SR&TIM_SR_CC1OF));
}
static void watchdog_safety(void)
{
    unsigned i;
    setup();
    assert(IWDG->PR==BOARD_IWDG_PRESCALER && IWDG->RLR==BOARD_IWDG_RELOAD);
    assert(watchdog_starts==1 && watchdog_feeds==1 && watchdog_reads>=2);
    assert(!IWDG->SR && (RCC->CSR&RCC_CSR_LSION));
    for(i=0;i<50;++i) { watchdog_advance(1); SysTick_Handler(); }
    assert(watchdog_feeds==1);
    App_Poll(); assert(watchdog_feeds==2 && !watchdog_expired);
    Hal_Fault(FAULT_TIMEOUT);
    for(i=0;i<50;++i) { watchdog_advance(1); SysTick_Handler(); }
    App_Poll(); assert(watchdog_feeds==3 && app_fault==FAULT_TIMEOUT);
    test_primask=1; watchdog_advance(1000);
    assert(watchdog_expired && !(TIM1->BDTR&TIM_BDTR_MOE));

    setup(); watchdog_stuck=1; watchdog_feeds=0;
    Hal_Init(); Hal_Start();
    assert((app_fault&FAULT_SCHEDULER) && !(TIM1->BDTR&TIM_BDTR_MOE));
    assert(test_primask==1 && !SysTick->CTRL && !watchdog_feeds);
    watchdog_advance(1000); assert(watchdog_expired);
}
static void uart_and_safety(void)
{
    unsigned i;
    u8 discarded;
    u8 query[14]={0xAA,0x55,CMD_QUERY,42};
    u8 status[64];
    setup();
    query[13]=Protocol_Crc(query+2,11);
    for(i=0;i<14;++i) rx(query[i],USART_SR_RXNE);
    App_Poll();
    for(i=0;i<64;++i) { USART2->SR=USART_SR_TXE; USART2_IRQHandler(); status[i]=(u8)USART2->DR; }
    USART2->SR=USART_SR_TXE; USART2_IRQHandler();
    assert(!(USART2->CR1&USART_CR1_TXEIE));
    assert(status[0]==0xAA && status[1]==0x55 && status[3]==42 && status[4]==0);
    assert(Protocol_Crc(status+2,61)==status[63]);
    Hal_Enable(1);
    rx(0,USART_SR_ORE|USART_SR_RXNE);
    assert(app_fault&FAULT_RX_OVERFLOW && !(TIM1->BDTR&TIM_BDTR_MOE));
    assert(!Serial_Read(&discarded));
    assert(Serial_Recover()==1 && Serial_Recover()==0);
    setup(); Hal_Enable(1); app_mode=1; app_command_age=999;
    SysTick_Handler(); assert(app_fault&FAULT_TIMEOUT && !(TIM1->BDTR&TIM_BDTR_MOE));
    setup(); Hal_Enable(1); app_mode=1; app_control_age=100;
    SysTick_Handler(); assert(app_fault&FAULT_SCHEDULER && !(TIM1->BDTR&TIM_BDTR_MOE));
    setup(); Hal_Enable(1);
    for(i=0;i<128;++i) rx(0,USART_SR_RXNE);
    assert(app_fault&FAULT_RX_OVERFLOW && !(TIM1->BDTR&TIM_BDTR_MOE));
}
int main(void)
{
    void *area=mmap((void *)PERIPH_BASE,0x30000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
    assert(area==(void *)PERIPH_BASE);
    configuration(); encoders(); capture_races(); watchdog_safety(); uart_and_safety();
    puts("STM32 HAL: routing, PWM fail-stop, capture races/overcapture, IWDG, UART errors and SysTick protections passed.");
    munmap(area,0x30000);
    return 0;
}
