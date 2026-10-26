/* 与硬件无关的控制算法：目标斜坡 + 定点 PI（比例/积分）速度控制。
 * 输入速度单位为 count/s；输出为有符号 PWM 千分比，正负代表方向。
 */
#include "control_logic.h"
/* 停止、换向或切换模式时清积分，防止历史误差影响下一次启动。 */
void Speed_Reset(SpeedController APP_XDATA *controller) { controller->integral=0; }

/* 每次调用最多变化 step；不足一步时直接到目标，避免越过目标来回振荡。
 * step 是“每个控制周期”的增量，不是每秒增量。
 */
s16 Control_Ramp(s16 current,s16 target,u16 step)
{
    /* 目标幅值不超过 10000，两个目标之差仍能放进 s16。 */
    s16 difference=target-current;
    if(difference>(s16)step) return current+(s16)step;
    if(difference<-(s16)step) return current-(s16)step;
    return target;
}

/* PI：PWM = (Kp * 误差 + 累积的 Ki * 误差) / 64。
 * Kp、Ki 用整数保存，除以 64 相当于带小数的增益，避免浮点计算。
 * Ki 已按每次控制调用定义，此处不再额外乘以毫秒间隔。
 */
s16 Speed_Step(SpeedController APP_XDATA *controller,s16 target,s16 measured,
               const MotorParameters APP_IDATA *parameters)
{
    s32 error=(s32)target-measured;
    s32 limit=(s32)parameters->value[PAR_I_LIMIT]*64L;
    s32 candidate=controller->integral+error*parameters->value[PAR_KI];
    s32 proportional=error*parameters->value[PAR_KP];
    s32 effort;
    s32 maximum=(s32)parameters->value[PAR_MAX_PWM]*64L;
    s32 lower=target<0 ? -maximum : 0;
    s32 upper=target>0 ? maximum : 0;
    if(!target) { Speed_Reset(controller); return 0; }
    if(candidate>limit) candidate=limit;
    if(candidate<-limit) candidate=-limit;
    effort=proportional+candidate;
    /* 禁止反向驱动时，零输出也是饱和边界；必须在定点截断前判断。 */
    if(!((effort>upper && error>0) || (effort<lower && error<0)))
        controller->integral=candidate;
    effort=proportional+controller->integral;
    if(effort>upper) effort=upper;
    if(effort<lower) effort=lower;
    return (s16)(effort/64L);
}
