#ifndef TEST_CORE_CM3_H
#define TEST_CORE_CM3_H
#include <stdint.h>
#define __I volatile const
#define __O volatile
#define __IO volatile
#define __IM volatile const
#define __OM volatile
#define __IOM volatile
typedef struct { volatile uint32_t CTRL,LOAD,VAL,CALIB; } SysTick_Type;
typedef struct { volatile uint32_t VTOR; } SCB_Type;
extern SysTick_Type test_systick;
extern SCB_Type test_scb;
extern uint32_t test_primask;
extern uint8_t test_priority[64],test_irq_enabled[64];
#define SysTick (&test_systick)
#define SCB (&test_scb)
#define SysTick_CTRL_CLKSOURCE_Msk 4U
#define SysTick_CTRL_TICKINT_Msk 2U
#define SysTick_CTRL_ENABLE_Msk 1U
static inline uint32_t __get_PRIMASK(void) { return test_primask; }
static inline void __set_PRIMASK(uint32_t value) { test_primask=value; }
static inline void __disable_irq(void) { test_primask=1; }
static inline void __enable_irq(void) { test_primask=0; }
static inline void __DMB(void) { }
static inline void NVIC_SetPriority(IRQn_Type irq,uint32_t priority) { test_priority[irq+16]=priority; }
static inline void NVIC_EnableIRQ(IRQn_Type irq) { test_irq_enabled[irq+16]=1; }
static inline void NVIC_ClearPendingIRQ(IRQn_Type irq) { (void)irq; }
#endif
