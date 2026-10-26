#ifndef MOTOR_PARAMETERS_H
#define MOTOR_PARAMETERS_H
#include "project_types.h"
#include "motor_channels.h"
/* 参数编号也是协议 ID 和 value[] 下标，不能随意调整顺序。 */
#define PAR_POLARITY 0    /* 安装极性：1 同时反转输出与反馈。 */
#define PAR_MAX_PWM 1     /* PWM 幅值上限，单位千分比。 */
#define PAR_MAX_SPEED 2   /* 速度目标幅值上限，单位 count/s。 */
#define PAR_CPR 3         /* 输出轴每圈 x1 计数，用于上位机转速换算。 */
#define PAR_KP 4          /* 比例增益，实际按参数值 / 64 使用。 */
#define PAR_KI 5          /* 每控制周期的积分增益，实际按参数值 / 64 使用。 */
#define PAR_I_LIMIT 6     /* 积分对 PWM 输出的贡献上限，单位千分比。 */
#define PAR_PWM_ACCEL 7   /* 每周期 PWM 幅值增加的最大千分比。 */
#define PAR_PWM_DECEL 8   /* 每周期 PWM 幅值减少的最大千分比。 */
#define PAR_REVERSE_MS 9  /* 换向零输出等待，单位 ms，按控制周期向上取整。 */
#define PAR_STALL_PWM 10  /* 堵转判断的 PWM 下限；0 禁用堵转判断。 */
#define PAR_STALL_SPEED 11 /* 堵转判断的速度上限，单位 count/s。 */
#define PAR_STALL_MS 12   /* 堵转条件持续多久才报错，单位 ms。 */
#define PAR_SPEED_ACCEL 13 /* 每周期速度目标幅值增加的最大 count/s。 */
#define PAR_SPEED_DECEL 14 /* 每周期速度目标幅值减少的最大 count/s。 */
#define MOTOR_PARAM_COUNT 15
#define CONFIG_SCHEMA_VERSION 4U
#define PAR_TIMEOUT 128 /* 整板的运动指令/心跳超时，单位 ms，不是电机数组下标。 */
typedef struct { u16 value[MOTOR_PARAM_COUNT]; } MotorParameters;
typedef struct {
    MotorParameters motor[MOTOR_COUNT];
    u16 timeout_ms;
} Configuration;
extern Configuration APP_IDATA settings;
/* 0=默认参数，2=RAM 参数已修改；不写入 Flash，掉电后不会保留。 */
extern u8 config_state;
void Config_Default(void);
u8 Config_Access(u8 write, u8 scope, u8 parameter, u16 APP_XDATA *value);
#endif
