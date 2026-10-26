#ifndef SERIAL_LINK_H
#define SERIAL_LINK_H
#include "project_types.h"
/* ISR 接口供 USART2 中断调用；主循环用 Read/Recover 消费数据并恢复收包。 */
void Serial_RxISR(u8 value);
void Serial_ErrorISR(void);
u8 Serial_Read(u8 APP_DATA *value);
u8 Serial_Recover(void);
/* 异步发送且不复制 bytes；调用前等待 Hal_WaitTx()，发送取数完成前不要改写。 */
void Serial_Send(const u8 APP_XDATA *bytes, u8 length);
#endif
