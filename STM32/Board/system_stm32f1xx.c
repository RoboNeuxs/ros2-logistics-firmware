/* 系统时钟初始化：外部 8 MHz 晶振 -> PLL x9 -> 72 MHz 系统时钟。
 * 启动文件在 C 运行库初始化 .data/.bss 之前调用 SystemInit()，
 * 因此这里不能依赖应用全局变量已经有初值。
 */
#include "board_config.h"
uint32_t SystemCoreClock=BOARD_SYSCLK_HZ;
/* 时钟未就绪时不带错误频率继续运行，将 PWM 引脚拉低并停留等待复位。 */
static void clock_failure(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    GPIOA->BRR=0x0F00U;
    GPIOA->CRH=(GPIOA->CRH&~0xFFFFU)|0x2222U;
    __disable_irq();
    while(1) { }
}
/* 有限次数轮询就绪标志；budget 是循环次数，不是准确的毫秒延时。 */
static void wait_ready(volatile uint32_t *reg, uint32_t mask)
{
    uint32_t budget=1000000UL;
    while((*reg&mask)!=mask) if(--budget==0) clock_failure();
}
void SystemInit(void)
{
    __disable_irq();
    /* 先回到内部 HSI，关闭旧 PLL/HSE 配置，再启动外部晶振。 */
    RCC->CR |= RCC_CR_HSION;
    wait_ready(&RCC->CR,RCC_CR_HSIRDY);
    RCC->CFGR=0;
    RCC->CR &= ~(RCC_CR_PLLON|RCC_CR_HSEON|RCC_CR_CSSON|RCC_CR_HSEBYP);
    RCC->CIR=0;
    RCC->CR |= RCC_CR_HSEON;
    wait_ready(&RCC->CR,RCC_CR_HSERDY);
    /* 提升频率前先启用 Flash 预取与两个等待周期。 */
    FLASH->ACR=FLASH_ACR_PRFTBE|FLASH_ACR_LATENCY_2;
    /* AHB=72 MHz，APB2=72 MHz，APB1=36 MHz，ADC=12 MHz。
     * APB1 分频为 /2 时，该总线上的定时器时钟自动倍频，仍是 72 MHz。
     */
    RCC->CFGR=RCC_CFGR_PLLSRC|RCC_CFGR_PLLMULL9|RCC_CFGR_PPRE1_DIV2|RCC_CFGR_ADCPRE_DIV6;
    RCC->CR |= RCC_CR_PLLON;
    wait_ready(&RCC->CR,RCC_CR_PLLRDY);
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    wait_ready(&RCC->CFGR,RCC_CFGR_SWS_PLL);
    RCC->CR |= RCC_CR_CSSON; /* 启用时钟安全检测，HSE 失效时触发 NMI 关断输出。 */
    SCB->VTOR=FLASH_BASE; /* 中断向量表位于 Flash 起始地址 0x08000000。 */
}
void SystemCoreClockUpdate(void) { SystemCoreClock=BOARD_SYSCLK_HZ; }
