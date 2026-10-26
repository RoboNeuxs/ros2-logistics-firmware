#ifndef MOTOR_ENCODER_H
#define MOTOR_ENCODER_H
#include "project_types.h"
#include "motor_channels.h"
/* bit0~3 选择需要反转反馈符号的通道，由板级反相配置和安装极性组合得到。 */
extern volatile signed char encoder_invert_mask;
/* 原始累计计数保留 u32 回绕位模式；mask 的 bit0~3 选择要清零的通道。 */
void Encoder_ReadAll(u32 APP_XDATA *counts);
void Encoder_Zero(u8 mask);
/* 以下入口由硬件中断调用；每调用一次表示一个 A 相下降沿。 */
void Encoder_M1ISR(void);
void Encoder_M2ISR(void);
void Encoder_M3ISR(void);
void Encoder_M4ISR(void);
#endif
