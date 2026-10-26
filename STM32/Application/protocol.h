#ifndef MOTOR_PROTOCOL_H
#define MOTOR_PROTOCOL_H
#include "project_types.h"
#include "motor_channels.h"
/* 命令固定 14 字节，应答固定 64 字节；均以 AA 55 开头并以 CRC-8 结尾。 */
#define COMMAND_SIZE 14
#define STATUS_SIZE 64
#define FW_VERSION 0x0403U
#define FW_CAPABILITIES 0x073FU
#ifdef BOARD_COMM_DIAGNOSTICS
#if BOARD_COMM_DIAGNOSTICS
#undef FW_CAPABILITIES
#define FW_CAPABILITIES 0x0F20U
#endif
#endif
#define RESPONSE_TYPE 0x93
#define CMD_STOP 0x13      /* 立即停止全部。 */
#define CMD_QUERY 0x14     /* 查询，不续期运动超时。 */
#define CMD_ZERO 0x15      /* 停止后清编码器计数。 */
#define CMD_SINGLE 0x16    /* 单台或全部使用同一模式/目标。 */
#define CMD_BATCH 0x17     /* 四台分别指定模式/目标。 */
#define CMD_HEARTBEAT 0x18 /* 续期已接受的运动目标。 */
#define CMD_CLEAR 0x19     /* 停止后清除故障锁存。 */
#define CMD_PARAM_GET 0x20 /* 读取参数。 */
#define CMD_PARAM_SET 0x21 /* 停止后写入 RAM 参数。 */
#define CMD_DEFAULT 0x23   /* 停止后恢复默认参数。 */
typedef struct {
    u8 bytes[COMMAND_SIZE];
    u8 used; /* 已收字节数，也是解析状态；0 表示正在寻找帧头。 */
} PacketParser;
typedef struct {
    u8 type;
    u8 sequence; /* 上位机的请求序号，应答原样返回。 */
    s16 values[MOTOR_COUNT]; /* 四个值的含义随命令变化，不一定都是电机目标。 */
    u8 flags; /* 可能是电机选择器，也可能是四路各占两位的模式，取决于 type。 */
} MotorCommand;
u8 Protocol_Crc(const u8 APP_XDATA *bytes, u8 length);
void Protocol_Reset(PacketParser APP_XDATA *parser);
u8 Protocol_Feed(PacketParser APP_XDATA *parser, u8 value, MotorCommand APP_XDATA *command);
void Protocol_Put16(u8 APP_XDATA *bytes, u16 value);
void Protocol_Put32(u8 APP_XDATA *bytes, u32 value);
#endif
