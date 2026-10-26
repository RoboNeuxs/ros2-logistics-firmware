/* 电机输出层：把控制算法给出的 PWM 请求转换为方向引脚和 TIM1 比较值。
 * 负责 PWM 加减速、安装极性与换向等待，具体寄存器操作交给 motor_hal.c。
 */
#include "board_config.h"
#include "motor_driver.h"
#include "app.h"
#include "encoder.h"
#include "parameters.h"
#include "control_logic.h"
#include "motor_hal.h"

/* motor_applied 是斜坡/换向等待之后的逻辑 PWM（-1000~1000）。
 * last_sign 保存经过安装极性修正的方向；reverse_wait 单位为控制周期数。
 */
s16 APP_XDATA motor_applied[MOTOR_COUNT];
static signed char APP_XDATA last_sign[MOTOR_COUNT];
static u8 APP_XDATA reverse_wait[MOTOR_COUNT];
static u16 APP_XDATA compare_values[MOTOR_COUNT];

/* scope：0=全部，1~4=单台。
 * 安装极性同时修正电机输出和编码器反馈，保持闭环的正负方向一致。
 */
void Motor_Reconfigure(u8 scope)
{
    u8 i,mask=BOARD_ENCODER_INVERT_MASK,old_ea;
    for(i=0;i<MOTOR_COUNT;++i) if(settings.motor[i].value[PAR_POLARITY]) mask ^= (1U<<i);
    old_ea=Hal_IrqSave(); encoder_invert_mask=mask; Hal_IrqRestore(old_ea);
    for(i=0;i<MOTOR_COUNT;++i) if((!scope || scope==i+1) && last_sign[i])
        reverse_wait[i]=(u8)((settings.motor[i].value[PAR_REVERSE_MS]+CONTROL_PERIOD_MS-1)/CONTROL_PERIOD_MS);
}

/* bit0~3 选择立即停机的通道；bit7 额外清除所有通道的方向历史。
 * 只要其他通道仍有输出，就不关闭 TIM1 的公共输出使能。
 */
void Motor_Disable(u8 mask)
{
    u8 i,active=0;
    for(i=0;i<MOTOR_COUNT;++i) {
        if(mask&(1U<<i)) { motor_applied[i]=0; compare_values[i]=0; }
        if(mask&128) { last_sign[i]=0; reverse_wait[i]=0; }
        if(motor_applied[i]) active=1;
    }
    if(!active) Hal_Enable(0);
    Hal_PWM(compare_values);
}

void Motor_Apply(const s16 APP_XDATA *requested)
{
    u8 i,directions=0,active=0,old_ea;
    s16 value,wanted;
    signed char sign;
    u16 step;
    const MotorParameters APP_IDATA *p;
    /* 更新四路方向和比较值之前先关闭公共输出，避免出现半更新状态。 */
    Hal_Enable(0);
    for(i=0;i<MOTOR_COUNT;++i) {
        p=&settings.motor[i]; wanted=requested[i];
        if(motor_modes[i]==MODE_STOP) {
            value=0;
            if(reverse_wait[i]) --reverse_wait[i];
        } else if(reverse_wait[i]) { --reverse_wait[i]; value=0; }
        else {
            sign=wanted>0 ? 1 : (wanted<0 ? -1 : 0);
            if(p->value[PAR_POLARITY]) sign=-sign;
            if(sign && last_sign[i] && sign!=last_sign[i]) {
                /* 检测到反向请求时先置零 PWM，再等待配置的时间。
                 * 毫秒数向上取整为控制周期数；当前这次零输出已占一个周期。
                 */
                value=0; last_sign[i]=sign;
                reverse_wait[i]=(u8)((p->value[PAR_REVERSE_MS]+CONTROL_PERIOD_MS-1)/CONTROL_PERIOD_MS-1);
            } else {
                if(sign) last_sign[i]=sign;
                /* 按幅值增减选择加速/减速步长，正反转使用相同规则。 */
                step=((wanted>=0 ? wanted : -wanted) >
                      (motor_applied[i]>=0 ? motor_applied[i] : -motor_applied[i]))
                     ? p->value[PAR_PWM_ACCEL] : p->value[PAR_PWM_DECEL];
                value=Control_Ramp(motor_applied[i],wanted,step);
            }
        }
        motor_applied[i]=value;
        if(value) {
            active=1;
        }
        if(p->value[PAR_POLARITY]) value=-value;
        if(value>0) directions |= (1U<<i);
        /* 千分比换算为计数：1000 -> PWM_COUNTS，实现真正的 100% 占空比。 */
        compare_values[i]=(u16)(((u32)(value<0 ? -value : value)*PWM_COUNTS)/1000UL);
    }
    Hal_Directions(directions);
    Hal_PWM(compare_values);
    /* 故障检查和使能放在同一临界区，防止中断刚关断又被主循环重新打开。 */
    old_ea=Hal_IrqSave();
    if(!app_fault && active) Hal_Enable(1);
    Hal_IrqRestore(old_ea);
}
