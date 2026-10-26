#ifndef MOTOR_APP_H
#define MOTOR_APP_H
#include "project_types.h"
#include "motor_channels.h"
/* 每路工作模式；指令中的模式编码与这里一致。 */
#define MODE_STOP 0  /* 立即停止。 */
#define MODE_PWM 1   /* 开环 PWM，目标单位为千分比。 */
#define MODE_SPEED 2 /* PI 速度闭环，目标单位为 count/s。 */
#define MODE_DECEL 3 /* 目标为零，按 PWM 减速斜坡降至停止。 */
/* 故障使用位图，可同时置多个位；停止不会自动清除故障。 */
#define FAULT_TIMEOUT 1      /* 有效运动指令/心跳超时。 */
#define FAULT_RX_OVERFLOW 2  /* 接收队列满或 UART 接收错误。 */
#define FAULT_ENCODER 4      /* 堵转/低反馈，或输入捕获丢失。 */
#define FAULT_SCHEDULER 8    /* 主循环控制任务未按时运行。 */
/* 这些 volatile 状态由主循环和中断共享。 */
extern volatile u8 app_fault;
extern volatile u8 app_mode;
extern volatile u16 app_ticks;
extern volatile u16 app_command_age;
extern volatile u16 app_control_age;
extern u8 motor_modes[MOTOR_COUNT];
extern u8 fault_channels; /* 编码器/堵转故障通道：bit0~3 对应 M1~M4。 */
extern u32 APP_XDATA encoder_counts[MOTOR_COUNT];
void App_TickISR(void);
#if BOARD_COMM_DIAGNOSTICS
extern volatile u8 app_debug_stage,app_debug_rx_irq,app_debug_rx_main,app_debug_frames;
#endif
#ifdef MOTOR_HOST_TEST
void App_Init(void);
void App_Poll(void);
#endif
#endif
