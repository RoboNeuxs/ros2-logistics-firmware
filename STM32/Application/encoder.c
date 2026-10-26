/* 编码器计数层：中断更新原始计数，主循环读取四路快照。
 * 只在 A 相下降沿读取 B 相判方向，即 x1 计数，不是四倍频解码。
 */
#include "board_config.h"
#include "encoder.h"
#include "motor_hal.h"
/* 用 u32 保存回绕位模式；反转时从零递减会回绕，测速层按有符号差值解释。 */
static volatile u32 raw_counts[MOTOR_COUNT];
volatile signed char encoder_invert_mask;

/* 短暂屏蔽中断，一次复制四路，避免读到由不同中断时刻拼出的快照。 */
void Encoder_ReadAll(u32 *counts)
{
    u8 i, previous=Hal_IrqSave();
    for(i=0;i<MOTOR_COUNT;++i) counts[i]=raw_counts[i];
    Hal_IrqRestore(previous);
}
void Encoder_Zero(u8 mask)
{
    u8 i, previous=Hal_IrqSave();
    /* 先丢弃待处理的捕获标志，避免清零前的边沿在清零后又被计入。 */
    Hal_EncoderFlush(mask);
    for(i=0;i<MOTOR_COUNT;++i) if(mask&(1U<<i)) raw_counts[i]=0;
    Hal_IrqRestore(previous);
}
static void edge(u8 channel)
{
    /* 默认 A 下降沿时 B 高为正；对应反相位为 1 时颠倒计数方向。 */
    if((Hal_EncoderB(channel)!=0) != ((encoder_invert_mask&(1U<<channel))!=0))
        ++raw_counts[channel];
    else --raw_counts[channel];
}
void Encoder_M1ISR(void) { edge(0); }
void Encoder_M2ISR(void) { edge(1); }
void Encoder_M3ISR(void) { edge(2); }
void Encoder_M4ISR(void) { edge(3); }
