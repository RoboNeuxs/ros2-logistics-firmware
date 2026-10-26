#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H
#include "project_types.h"
/* 板级时钟与通信配置：外部 8 MHz，系统 72 MHz，APB1 36 MHz。 */
#define BOARD_HSE_HZ 8000000UL
#define BOARD_SYSCLK_HZ 72000000UL
#define BOARD_APB1_HZ (BOARD_SYSCLK_HZ/2UL)
#define UART_BAUD 115200UL
#define PWM_FREQUENCY 20000UL
#define PWM_COUNTS (BOARD_SYSCLK_HZ/PWM_FREQUENCY) /* 3600；TIM1 ARR=3599。 */
#define CONTROL_PERIOD_MS 50U     /* 主循环的电机控制周期。 */
#define CONTROL_MAX_DELAY_MS 100U /* 超过该时间未执行控制，触发调度故障。 */
#define BOARD_IWDG_PRESCALER 4U   /* LSI / 64。 */
#define BOARD_IWDG_RELOAD 624U    /* LSI 名义 40 kHz 时为 1 秒；实际随 LSI 偏差变化。 */
#define BOARD_COMM_DIAGNOSTICS 0  /* 1=仅通信诊断，拒绝运动输出；正常使用保持 0。 */
#define BOARD_INVERT_DEFAULT_MASK 0U /* 默认安装极性，bit0~3 对应 M1~M4。 */
#ifndef BOARD_ENCODER_INVERT_MASK
#define BOARD_ENCODER_INVERT_MASK 0U /* 接线导致的编码器反相，bit0~3 独立配置。 */
#endif
#if BOARD_HSE_HZ != 8000000UL || BOARD_SYSCLK_HZ != 72000000UL
#error Clock setup requires an 8 MHz external crystal and 72 MHz system clock
#endif
#if BOARD_ENCODER_INVERT_MASK > 15U
#error Encoder inversion mask uses bits 0..3 only
#endif
/* 四路按 A/B/C/D（M1~M4）顺序：
 * PWM：PA8/9/10/11，TIM1 CH1/2/3/4，不重映射。
 * IN1：PB0/1/10/11；IN2 由现有硬件电路反相生成。
 * 编码器 A 相：PA0、PA6、PB6、PB12；B 相：PA1、PA7、PB7、PB13。
 * USART2 TX/RX：PA2/PA3；PB2（BOOT1/STRS）保持输入。
 * 两片 TB6612 的 STBY 固定接 5 V；软件急停通过清 TIM1 的 MOE 实现。
 */
#if defined(MOTOR_HOST_TEST) && !defined(MOTOR_HAL_TEST)
/* 主机测试使用模拟寄存器；实际固件使用芯片的 CMSIS 寄存器定义。 */
#include "mock_registers.h"
#else
#include "stm32f103xb.h"
#endif
#endif
