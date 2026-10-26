#ifndef MOTOR_HAL_H
#define MOTOR_HAL_H
#include "project_types.h"
/* 应用层调用这些接口，不直接访问 GPIO/定时器/UART 寄存器。 */
/* Init 配置外设并保持中断关闭，Start 在应用状态初始化后启动中断。 */
void Hal_Init(void);
void Hal_Start(void);
void Hal_WatchdogFeed(void);
/* Save 返回进入前的中断屏蔽状态，Restore 必须传回该值，不能无条件开中断。 */
u8 Hal_IrqSave(void);
void Hal_IrqRestore(u8 previous);
/* TIM1 四路公共使能；有故障时，即使 enabled=1 也不会打开输出。 */
void Hal_Enable(u8 enabled);
void Hal_Directions(u8 mask);
/* 四个比较计数值，范围 0~PWM_COUNTS，不是 PWM 千分比。 */
void Hal_PWM(const u16 *compare);
void Hal_WaitTx(void);
/* 异步发送，不复制 bytes；WaitTx 等发送中断取完缓冲区后才允许重用。 */
u8 Hal_Send(const u8 *bytes, u8 length);
u8 Hal_EncoderB(u8 channel);
void Hal_EncoderFlush(u8 mask);
/* 锁存故障位并立即关断四路 PWM。 */
void Hal_Fault(u8 fault);
#endif
