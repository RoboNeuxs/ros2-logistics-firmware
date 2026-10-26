/* 串口收发的应用接口。
 * 接收中断只入队，主循环再解析；发送由 HAL 的 TXE 中断逐字节完成。
 */
#include "board_config.h"
#include "serial_link.h"
#include "app.h"
#include "motor_hal.h"
/* 环形队列大小必须为 2 的幂，才能用 & (size-1) 代替取模。
 * 留一个空槽区分“空”和“满”，128 字节数组实际可存 127 字节。
 */
#define RX_RING_SIZE 128
static volatile u8  rx_ring[RX_RING_SIZE];
static volatile u8 rx_read,rx_write,rx_overflow;
/* 接收溢出或 UART 出错时锁存故障并立即停输出，随后由主循环清理半帧。 */
void Serial_ErrorISR(void)
{
    Hal_Fault(FAULT_RX_OVERFLOW); rx_overflow=1;
}
void Serial_RxISR(u8 value)
{
    u8 next=(rx_write+1)&(RX_RING_SIZE-1);
#if BOARD_COMM_DIAGNOSTICS
    ++app_debug_rx_irq;
#endif
    /* 正常入队时先存字节再发布写指针，主循环只读取已经写完的数据。 */
    if(next==rx_read) {
        Serial_ErrorISR();
    } else { rx_ring[rx_write]=value; rx_write=next; }
}
/* 有字节则写入 *value 并返回 1；队列为空或待恢复时返回 0。 */
u8 Serial_Read(u8 APP_DATA *value)
{
    if(rx_overflow || rx_read==rx_write) return 0;
    *value=rx_ring[rx_read]; rx_read=(rx_read+1)&(RX_RING_SIZE-1);
    return 1;
}
/* 丢弃错误前残留的全部数据，返回 1 通知主循环重置协议解析器。
 * 这里只恢复接收队列，不清除 app_fault，也不会恢复电机输出。
 */
u8 Serial_Recover(void)
{
    u8 old_es;
    if(!rx_overflow) return 0;
    old_es=Hal_IrqSave(); rx_read=rx_write; rx_overflow=0; Hal_IrqRestore(old_es);
    return 1;
}
void Serial_Send(const u8 APP_XDATA *bytes,u8 length) { Hal_Send(bytes,length); }
