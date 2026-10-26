#ifndef CONTROL_LOGIC_H
#define CONTROL_LOGIC_H
#include "project_types.h"
#include "parameters.h"
#define DUTY_LIMIT 1000
#define SPEED_LIMIT 10000
/* 每路独立保存 PI 积分，内部按 64 倍尺度累计。 */
typedef struct { s32 integral; } SpeedController;
void Speed_Reset(SpeedController APP_XDATA *controller);
/* 输入：目标/实测 count/s；返回：经过限幅的有符号 PWM 千分比。 */
s16 Speed_Step(SpeedController APP_XDATA *controller, s16 target, s16 measured,
               const MotorParameters APP_IDATA *parameters);
/* 向目标靠近一次，变化量不超过 step；数值单位由调用者决定。 */
s16 Control_Ramp(s16 current, s16 target, u16 step);
#endif
