/* 固定长度二进制协议解析；本层只负责帧头、CRC 和字段拆解。
 * 命令是否合法、当前是否允许执行，由 main.c 的 process_command() 判断。
 */
#include "protocol.h"

/* CRC-8：多项式 0x07，初值 0，高位先算，无输入/输出反射，结果不异或。 */
u8 Protocol_Crc(const u8 APP_XDATA *bytes, u8 length)
{
    u8 crc = 0;
    u8 bit_index;
    while (length--) {
        crc ^= *bytes++;
        for (bit_index = 0; bit_index < 8; ++bit_index)
            crc = (u8)((crc & 0x80) ? (crc << 1) ^ 0x07 : crc << 1);
    }
    return crc;
}

/* 多字节数据统一按小端发送：低字节在前，不依赖结构体布局或对齐。 */
void Protocol_Put16(u8 APP_XDATA *bytes, u16 value)
{
    bytes[0] = (u8)value;
    bytes[1] = (u8)(value >> 8);
}

void Protocol_Put32(u8 APP_XDATA *bytes, u32 value)
{
    Protocol_Put16(bytes, (u16)value);
    Protocol_Put16(bytes + 2, (u16)(value >> 16));
}

void Protocol_Reset(PacketParser APP_XDATA *parser)
{
    parser->used = 0;
}

/* 每次喂入一个字节；仅在收齐 14 字节且 CRC 正确时返回 1。
 * 帧布局：AA 55 | type | sequence | 四个 int16 小端值 | flags | CRC。
 * used=0 找 AA，used=1 等 55，其余状态继续收包。
 */
u8 Protocol_Feed(PacketParser APP_XDATA *parser, u8 value, MotorCommand APP_XDATA *command)
{
    u8 i;
    u8 j;
    const u8 APP_XDATA *input;
    s16 APP_XDATA *output;
    if (parser->used == 0) {
        if (value == 0xAA) parser->bytes[parser->used++] = value;
        return 0;
    }
    /* 连续收到 AA AA 时保留第二个 AA，仍可识别后续的 55。 */
    if (parser->used == 1 && value != 0x55) {
        parser->used = (value == 0xAA) ? 1 : 0;
        return 0;
    }
    parser->bytes[parser->used++] = value;
    if (parser->used < COMMAND_SIZE) return 0;
    /* 校验只包含功能码到 flags（字节 2~12），不包含帧头和 CRC 自身。 */
    if (Protocol_Crc(parser->bytes + 2, COMMAND_SIZE - 3) == parser->bytes[COMMAND_SIZE - 1]) {
        command->type = parser->bytes[2];
        command->sequence = parser->bytes[3];
        input=parser->bytes+4;
        output=command->values;
        for (i = 0; i < MOTOR_COUNT; ++i) {
            *output++ = (s16)((u16)input[0] | ((u16)input[1]<<8));
            input+=2;
        }
        command->flags = parser->bytes[COMMAND_SIZE - 2];
        parser->used = 0;
        return 1;
    }
    /* 错包不应答；在当前数据内继续找下一组帧头，保留已收到的帧尾部分。
     * 这样插入/丢失一个字节后仍有机会重新对齐，不必等串口超时。
     */
    for (i = 1; i < COMMAND_SIZE - 1; ++i) {
        if (parser->bytes[i] == 0xAA && parser->bytes[i + 1] == 0x55) {
            parser->used = COMMAND_SIZE - i;
            for (j = 0; j < parser->used; ++j) parser->bytes[j] = parser->bytes[i + j];
            return 0;
        }
    }
    parser->used = (value == 0xAA) ? 1 : 0;
    if (parser->used) parser->bytes[0] = 0xAA;
    return 0;
}
