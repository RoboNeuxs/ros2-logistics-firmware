#ifndef MOTOR_DRIVER_H
#define MOTOR_DRIVER_H
#include "project_types.h"
#include "motor_channels.h"
/* 实际应用的逻辑 PWM 千分比，已考虑斜坡和换向等待，尚未施加安装极性。 */
extern s16 APP_XDATA motor_applied[MOTOR_COUNT];
/* mask 位图选通道；scope 则用 0=全部、1~4=单台，注意二者不是同一编码。 */
void Motor_Disable(u8 mask);
void Motor_Reconfigure(u8 scope);
/* 每个控制周期调用一次，requested 必须指向四路 PWM 请求数组。 */
void Motor_Apply(const s16 APP_XDATA *requested);
#endif
