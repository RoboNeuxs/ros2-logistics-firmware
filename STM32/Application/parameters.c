/* 参数保存在 RAM，掉电恢复默认；本文件不写 Flash。
 * 三张表使用 parameters.h 的 PAR_* 编号作下标，顺序必须保持一致。
 */
#include "parameters.h"
#include "board_config.h"

Configuration APP_IDATA settings;
u8 config_state;
/* 顺序：极性、最大 PWM、最大速度、CPR、Kp、Ki、积分限幅、
 * PWM 加速/减速、换向等待、堵转 PWM/速度/时间、速度加速/减速。
 * 默认 CPR=1 只是占位值，需按电机输出轴每圈 A 下降沿数量配置。
 */
static const u16  defaults[MOTOR_PARAM_COUNT] = {
    0,1000,10000,1,4,1,1000,100,100,100,300,0,500,1000,1000
};
static const u16  minimum[MOTOR_PARAM_COUNT] = {
    0,1,1,1,0,0,0,1,1,50,0,0,50,1,1
};
static const u16  maximum[MOTOR_PARAM_COUNT] = {
    1,1000,10000,60000,1024,1024,1000,1000,1000,5000,1000,10000,10000,10000,10000
};
/* 四路先复制相同默认值，再按板级位图分别设置安装极性。 */
void Config_Default(void)
{
    u8 i,j;
    for(i=0;i<MOTOR_COUNT;++i)
        for(j=0;j<MOTOR_PARAM_COUNT;++j) settings.motor[i].value[j]=defaults[j];
    for(i=0;i<MOTOR_COUNT;++i) settings.motor[i].value[PAR_POLARITY]=(BOARD_INVERT_DEFAULT_MASK>>i)&1;
    settings.timeout_ms=1000;
    config_state=0;
}

/* write=0 读、非零写；value 同时用作写入值或读取结果。
 * 电机参数：scope=1~4 选单台，写入时 scope=0 可广播给全部。
 * 全局超时 PAR_TIMEOUT 必须 scope=0；返回 0=成功、2=范围/作用域错误。
 * 此处只校验参数范围，运行中禁止修改的规则由 main.c 检查。
 */
u8 Config_Access(u8 write,u8 scope,u8 parameter,u16 APP_XDATA *value)
{
    u8 i,first,last;
    if(scope>4) return 2;
    if(parameter==PAR_TIMEOUT) {
        if(scope || (write && (*value<200 || *value>10000))) return 2;
        if(write) { settings.timeout_ms=*value; config_state=2; }
        else *value=settings.timeout_ms;
        return 0;
    }
    if(parameter>=MOTOR_PARAM_COUNT || (!write && !scope)) return 2;
    if(write && (*value<minimum[parameter] || *value>maximum[parameter])) return 2;
    if(!write) { *value=settings.motor[scope-1].value[parameter]; return 0; }
    first=scope ? scope-1 : 0;
    last=scope ? scope : MOTOR_COUNT;
    for(i=first;i<last;++i) settings.motor[i].value[parameter]=*value;
    config_state=2;
    return 0;
}
