/* STM32 硬件层：直接操作 CMSIS 寄存器，不使用 STM32 HAL 库。
 * TIM1：四路 20 kHz PWM；TIM2/3/4 + EXTI12：四路编码器；USART2：上位机。
 * 本文件将硬件中断转换成 Application 层接口调用，控制算法不直接访问寄存器。
 */
#include "board_config.h"
#include "motor_hal.h"
#include "app.h"
#include "encoder.h"
#include "serial_link.h"

/* 发送不复制缓冲区：中断按 tx_pointer 取数，调用方必须等发送取数结束再改写。 */
static const u8 * volatile tx_pointer;
static volatile u8 tx_remaining,tx_busy;

#ifdef MOTOR_HAL_TEST
u32 Test_CaptureStatus(TIM_TypeDef *timer);
void Test_CaptureClear(TIM_TypeDef *timer,u32 flags);
void Test_WatchdogWrite(volatile u32 *reg,u32 value);
u32 Test_WatchdogStatus(void);
#define capture_status(timer) Test_CaptureStatus(timer)
#define capture_clear(timer,flags) Test_CaptureClear(timer,flags)
#define watchdog_write(reg,value) Test_WatchdogWrite(&(reg),value)
#define watchdog_status() Test_WatchdogStatus()
#else
#define capture_status(timer) ((timer)->SR)
#define capture_clear(timer,flags) ((timer)->SR=(u32)~(flags))
#define watchdog_write(reg,value) ((reg)=(value))
#define watchdog_status() (IWDG->SR)
#endif

/* 保存 PRIMASK 后关中断，退出时恢复原状态，允许临界区嵌套。
 * DMB 是内存屏障，保证临界区内外的共享数据访问按预期顺序完成。
 */
u8 Hal_IrqSave(void)
{
    u8 previous=(u8)__get_PRIMASK();
    __disable_irq();
    __DMB();
    return previous;
}
void Hal_IrqRestore(u8 previous) { __DMB(); __set_PRIMASK(previous); }

void Hal_Enable(u8 enabled)
{
    u8 previous=Hal_IrqSave();
    /* MOE 是 TIM1 四路输出的总开关。OSSI 与低电平空闲态使关断后 PWM 为低。
     * 本板 STBY 固定接高电平，因此急停通过关闭 MOE 实现，而不是切换 STBY。
     */
    if(enabled && !app_fault) TIM1->BDTR |= TIM_BDTR_MOE;
    else TIM1->BDTR &= ~TIM_BDTR_MOE;
    Hal_IrqRestore(previous);
}
/* 故障按位累加并立即关 PWM；不等下一个 50 ms 控制周期。 */
void Hal_Fault(u8 fault)
{
    u8 previous=Hal_IrqSave();
    app_fault |= fault;
    TIM1->BDTR &= ~TIM_BDTR_MOE;
    Hal_IrqRestore(previous);
}
/* F1 每个 GPIO 用 4 位配置：低两位 MODE，高两位 CNF。
 * pin0~7 使用 CRL，pin8~15 使用 CRH；只替换目标引脚的配置位。
 */
static void gpio_mode(GPIO_TypeDef *port,u8 pin,u8 mode)
{
    volatile uint32_t *reg=pin<8 ? &port->CRL : &port->CRH;
    u8 shift=(pin&7U)*4U;
    *reg=(*reg&~(0xFUL<<shift))|((uint32_t)mode<<shift);
}
static void capture_init(TIM_TypeDef *timer)
{
    timer->CR1=0; timer->PSC=0; timer->ARR=65535U;
    /* CH1 直接捕获 TI1 下降沿，输入滤波采用定时器时钟连续 8 次采样。
     * 用捕获中断计 A 相边沿，避免 PA6/PB6 共用 EXTI6 的冲突，保留 x1 单位。
     */
    timer->CCMR1=TIM_CCMR1_CC1S_0|TIM_CCMR1_IC1F_0|TIM_CCMR1_IC1F_1;
    timer->CCER=TIM_CCER_CC1E|TIM_CCER_CC1P;
    timer->EGR=TIM_EGR_UG; timer->SR=0;
    timer->DIER=TIM_DIER_CC1IE; timer->CR1=TIM_CR1_CEN;
}
void Hal_Init(void)
{
    u8 pin;
    __disable_irq();
    /* 访问外设寄存器前先开启对应总线时钟。初始化完成前保持中断关闭。 */
    RCC->APB2ENR |= RCC_APB2ENR_AFIOEN|RCC_APB2ENR_IOPAEN|RCC_APB2ENR_IOPBEN|RCC_APB2ENR_TIM1EN;
    RCC->APB1ENR |= RCC_APB1ENR_TIM2EN|RCC_APB1ENR_TIM3EN|RCC_APB1ENR_TIM4EN|RCC_APB1ENR_USART2EN;
    /* 不重映射定时器/串口；关闭 JTAG 释放引脚，同时保留 PA13/PA14 的 SWD。 */
    AFIO->MAPR=AFIO_MAPR_SWJ_CFG_JTAGDISABLE;
    /* 先把 PA8~11 设为低电平普通输出，等 TIM1 安全配置完再切换到复用输出。
     * GPIO 模式：2=2 MHz 推挽输出，4=浮空输入，8=上下拉输入，0xA=复用推挽。
     */
    GPIOA->BRR=0x0F00U;
    for(pin=8;pin<=11;++pin) gpio_mode(GPIOA,pin,2);
    GPIOB->BRR=(1U<<0)|(1U<<1)|(1U<<10)|(1U<<11);
    gpio_mode(GPIOB,0,2); gpio_mode(GPIOB,1,2);
    gpio_mode(GPIOB,10,2); gpio_mode(GPIOB,11,2);
    gpio_mode(GPIOB,2,4); /* PB2 为 BOOT1/STRS，保持输入，不驱动该网络。 */
    /* 输入模式为 8 时，ODR=1 选择内部上拉；USART2 RX 和编码器输入均上拉。 */
    GPIOA->BSRR=(1U<<0)|(1U<<1)|(1U<<3)|(1U<<6)|(1U<<7);
    GPIOB->BSRR=(1U<<6)|(1U<<7)|(1U<<12)|(1U<<13);
    gpio_mode(GPIOA,0,8); gpio_mode(GPIOA,1,8);
    gpio_mode(GPIOA,6,8); gpio_mode(GPIOA,7,8);
    gpio_mode(GPIOB,6,8); gpio_mode(GPIOB,7,8);
    gpio_mode(GPIOB,12,8); gpio_mode(GPIOB,13,8);
    gpio_mode(GPIOA,2,0xA); gpio_mode(GPIOA,3,8);

    RCC->APB2RSTR |= RCC_APB2RSTR_TIM1RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_TIM1RST;
    /* PWM 频率 = 72 MHz / (PSC+1) / (ARR+1) = 20 kHz。
     * 四路用 PWM 模式 1 和比较预装载，初始占空比全部为零。
     */
    TIM1->PSC=0; TIM1->ARR=PWM_COUNTS-1U;
    TIM1->CCR1=TIM1->CCR2=TIM1->CCR3=TIM1->CCR4=0;
    TIM1->CCMR1=TIM_CCMR1_OC1M_1|TIM_CCMR1_OC1M_2|TIM_CCMR1_OC1PE|
                  TIM_CCMR1_OC2M_1|TIM_CCMR1_OC2M_2|TIM_CCMR1_OC2PE;
    TIM1->CCMR2=TIM_CCMR2_OC3M_1|TIM_CCMR2_OC3M_2|TIM_CCMR2_OC3PE|
                  TIM_CCMR2_OC4M_1|TIM_CCMR2_OC4M_2|TIM_CCMR2_OC4PE;
    TIM1->CR2=0; /* 空闲态全部为低，互补输出不启用。 */
    TIM1->BDTR=TIM_BDTR_OSSI|TIM_BDTR_OSSR;
    TIM1->CCER=TIM_CCER_CC1E|TIM_CCER_CC2E|TIM_CCER_CC3E|TIM_CCER_CC4E;
    TIM1->EGR=TIM_EGR_UG; TIM1->SR=0;
    TIM1->CR1=TIM_CR1_ARPE|TIM_CR1_CEN;
    for(pin=8;pin<=11;++pin) gpio_mode(GPIOA,pin,0xA);

    /* M1~M3 使用 TIM2/3/4 的捕获中断；M4 的 PB12 使用 EXTI12 下降沿中断。 */
    capture_init(TIM2); capture_init(TIM3); capture_init(TIM4);
    AFIO->EXTICR[3]=(AFIO->EXTICR[3]&~AFIO_EXTICR4_EXTI12)|AFIO_EXTICR4_EXTI12_PB;
    EXTI->IMR |= 1U<<12;
    EXTI->RTSR &= ~(1U<<12); EXTI->FTSR |= 1U<<12;
    EXTI->PR=1U<<12;
    tx_pointer=0; tx_remaining=tx_busy=0;
    USART2->CR1=0; USART2->CR2=0; USART2->CR3=0;
    /* USART2 挂在 APB1 上，按 36 MHz 算波特率；默认 8 数据位、无校验、1 停止位。 */
    USART2->BRR=(BOARD_APB1_HZ+UART_BAUD/2U)/UART_BAUD;
    USART2->CR1=USART_CR1_UE|USART_CR1_TE|USART_CR1_RE|USART_CR1_RXNEIE;
    /* 数值越小优先级越高：安全计时 > 编码器 > 串口。 */
    NVIC_SetPriority(SysTick_IRQn,0);
    NVIC_SetPriority(TIM2_IRQn,1); NVIC_SetPriority(TIM3_IRQn,1);
    NVIC_SetPriority(TIM4_IRQn,1); NVIC_SetPriority(EXTI15_10_IRQn,1);
    NVIC_SetPriority(USART2_IRQn,2);
    NVIC_ClearPendingIRQ(TIM2_IRQn); NVIC_ClearPendingIRQ(TIM3_IRQn);
    NVIC_ClearPendingIRQ(TIM4_IRQn); NVIC_ClearPendingIRQ(EXTI15_10_IRQn);
    NVIC_ClearPendingIRQ(USART2_IRQn);
    NVIC_EnableIRQ(TIM2_IRQn); NVIC_EnableIRQ(TIM3_IRQn); NVIC_EnableIRQ(TIM4_IRQn);
    NVIC_EnableIRQ(EXTI15_10_IRQn); NVIC_EnableIRQ(USART2_IRQn);
    SysTick->LOAD=BOARD_SYSCLK_HZ/1000UL-1U;
    SysTick->VAL=0; SysTick->CTRL=0; /* App_Init 设置好全部状态后才启动 1 ms 节拍。 */
}
void Hal_WatchdogFeed(void) { watchdog_write(IWDG->KR,0xAAAAU); }
static u8 watchdog_start(void)
{
    u32 budget=1000000UL;
    watchdog_write(IWDG->KR,0xCCCCU);
    watchdog_write(IWDG->KR,0x5555U);
    watchdog_write(IWDG->PR,BOARD_IWDG_PRESCALER);
    watchdog_write(IWDG->RLR,BOARD_IWDG_RELOAD);
    while(watchdog_status()&(IWDG_SR_PVU|IWDG_SR_RVU)) {
        if(--budget==0) { Hal_Fault(FAULT_SCHEDULER); return 0; }
    }
    Hal_WatchdogFeed();
    return 1;
}
void Hal_Start(void)
{
    if(!watchdog_start()) return;
    SysTick->VAL=0;
    SysTick->CTRL=SysTick_CTRL_CLKSOURCE_Msk|SysTick_CTRL_TICKINT_Msk|SysTick_CTRL_ENABLE_Msk;
    __enable_irq();
}
/* 逻辑 bit0~3 映射到 PB0/PB1/PB10/PB11；BSRR 低 16 位置位、高 16 位复位。
 * 一次写入同时更新四路 IN1；各路 IN2 由硬件反相生成。
 */
void Hal_Directions(u8 mask)
{
    uint32_t pins=(1U<<0)|(1U<<1)|(1U<<10)|(1U<<11);
    uint32_t set=(mask&3U)|((uint32_t)(mask&12U)<<8);
    GPIOB->BSRR=set|((pins&~set)<<16);
}
/* compare 是定时器计数值，不是千分比；允许等于 PWM_COUNTS 以输出 100%。 */
void Hal_PWM(const u16 *compare)
{
    u8 previous=Hal_IrqSave();
    TIM1->CCR1=compare[0]>PWM_COUNTS ? PWM_COUNTS : compare[0];
    TIM1->CCR2=compare[1]>PWM_COUNTS ? PWM_COUNTS : compare[1];
    TIM1->CCR3=compare[2]>PWM_COUNTS ? PWM_COUNTS : compare[2];
    TIM1->CCR4=compare[3]>PWM_COUNTS ? PWM_COUNTS : compare[3];
    TIM1->EGR=TIM_EGR_UG; /* 立即装载四路预装值，再由 Motor_Apply 打开 MOE。 */
    Hal_IrqRestore(previous);
}
u8 Hal_EncoderB(u8 channel)
{
    switch(channel) {
    case 0: return (GPIOA->IDR&(1U<<1))!=0;
    case 1: return (GPIOA->IDR&(1U<<7))!=0;
    case 2: return (GPIOB->IDR&(1U<<7))!=0;
    default: return (GPIOB->IDR&(1U<<13))!=0;
    }
}
void Hal_EncoderFlush(u8 mask)
{
    if(mask&1) TIM2->SR=(uint32_t)~(TIM_SR_CC1IF|TIM_SR_CC1OF);
    if(mask&2) TIM3->SR=(uint32_t)~(TIM_SR_CC1IF|TIM_SR_CC1OF);
    if(mask&4) TIM4->SR=(uint32_t)~(TIM_SR_CC1IF|TIM_SR_CC1OF);
    if(mask&8) EXTI->PR=1U<<12;
}
/* 仅供中断已开启的主循环调用；等待 TXE 中断取完缓冲区，才能安全改写缓冲区。
 * tx_busy 清零不等于最后一个停止位已发送完，不用于判断物理总线空闲。
 */
void Hal_WaitTx(void) { while(tx_busy) { } }
/* 异步发送：返回 1 表示接受请求，0 表示忙或长度为零；不复制 bytes 内容。 */
u8 Hal_Send(const u8 *bytes,u8 length)
{
    u8 previous=Hal_IrqSave();
    if(tx_busy || !length) { Hal_IrqRestore(previous); return 0; }
    tx_pointer=bytes; tx_remaining=length; tx_busy=1;
    USART2->CR1 |= USART_CR1_TXEIE;
    Hal_IrqRestore(previous);
    return 1;
}
/* RXNE：读出接收字节入队；TXE：发送寄存器空时填下一个字节。
 * 接收出错立即触发保护，不能将错误字节继续当正常命令解析。
 */
void USART2_IRQHandler(void)
{
    uint32_t status=USART2->SR;
    if(status&(USART_SR_RXNE|USART_SR_ORE|USART_SR_FE|USART_SR_NE|USART_SR_PE)) {
        u8 value=(u8)USART2->DR; /* 按 SR -> DR 的顺序读取，清除接收及错误标志。 */
        if(status&(USART_SR_ORE|USART_SR_FE|USART_SR_NE|USART_SR_PE))
            Serial_ErrorISR();
        else if(status&USART_SR_RXNE) Serial_RxISR(value);
    }
    if((status&USART_SR_TXE) && (USART2->CR1&USART_CR1_TXEIE)) {
        if(tx_remaining) { USART2->DR=*tx_pointer++; --tx_remaining; }
        else { USART2->CR1 &= ~USART_CR1_TXEIE; tx_busy=0; }
    }
}
static void capture_irq(TIM_TypeDef *timer,void (*callback)(void),u8 channel)
{
    u32 status=capture_status(timer);
    if(!(status&(TIM_SR_CC1IF|TIM_SR_CC1OF))) return;
    /* 不读 CCR；先只清 IF，保留清除前到达的新边沿所置的 OF。 */
    if(status&TIM_SR_CC1IF) capture_clear(timer,TIM_SR_CC1IF);
    status |= capture_status(timer);
    if(status&TIM_SR_CC1OF) {
        u8 previous=Hal_IrqSave();
        fault_channels |= (1U<<channel);
        Hal_Fault(FAULT_ENCODER);
        Hal_IrqRestore(previous);
        capture_clear(timer,TIM_SR_CC1OF);
    } else callback();
}
void TIM2_IRQHandler(void) { capture_irq(TIM2,Encoder_M1ISR,0); }
void TIM3_IRQHandler(void) { capture_irq(TIM3,Encoder_M2ISR,1); }
void TIM4_IRQHandler(void) { capture_irq(TIM4,Encoder_M3ISR,2); }
void EXTI15_10_IRQHandler(void)
{
    if(EXTI->PR&(1U<<12)) { EXTI->PR=1U<<12; Encoder_M4ISR(); }
}
void SysTick_Handler(void) { App_TickISR(); }
/* 处理器异常时停止调度，关闭 PWM 并把 PA8~11 强制设为低电平普通输出。 */
static void exception_stop(void)
{
    __disable_irq();
    TIM1->BDTR &= ~TIM_BDTR_MOE;
    GPIOA->BRR=0x0F00U;
    GPIOA->CRH=(GPIOA->CRH&~0xFFFFU)|0x2222U;
    while(1) { }
}
void NMI_Handler(void) { exception_stop(); } /* 包括 HSE 时钟安全系统检测到的失效。 */
void HardFault_Handler(void) { exception_stop(); }
void MemManage_Handler(void) { exception_stop(); }
void BusFault_Handler(void) { exception_stop(); }
void UsageFault_Handler(void) { exception_stop(); }
