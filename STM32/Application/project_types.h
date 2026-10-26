#ifndef PROJECT_TYPES_H
#define PROJECT_TYPES_H
#include <stdint.h>
/* 明确位宽，避免不同编译器的 int/long 大小差异影响串口字段与计数。 */
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int16_t s16;
typedef int32_t s32;
/* 沿用 STC 移植前的存储区标记；在 STM32 上展开为空，不区分这些存储区。 */
#define APP_XDATA
#define APP_DATA
#define APP_IDATA
#endif
