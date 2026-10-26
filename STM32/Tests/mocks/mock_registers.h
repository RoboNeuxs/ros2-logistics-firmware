#ifndef MOCK_REGISTERS_H
#define MOCK_REGISTERS_H
#include "project_types.h"
#define MAIN_Fosc 24000000L
#define xdata
#define code
#define ENABLE 1
#define DISABLE 0
#define Polity_1 1
#define Polity_2 2
#define Polity_3 3
extern u8 EA, ES, P_SW2, P1, P1PU, P3PU, RI, TI;
extern u8 MOTOR_STBY, MOTOR_DIR1, MOTOR_DIR2, MOTOR_DIR3, MOTOR_DIR4;
extern u8 ENCODER_B1, ENCODER_B2, ENCODER_B3, ENCODER_B4;
extern u16 mock_ccr1, mock_ccr2, mock_ccr3, mock_ccr4, mock_prescaler;
extern u8 PWMA_EGR, PWMA_SR1;
#define EAXSFR() (P_SW2 |= 0x80)
#define EAXRAM() (P_SW2 &= ~0x80)
extern u8 IP,IE,IE2,INT_CLKO,AUXR,PCON,P2,P3,P1M1,P1M0,P2M1,P2M0,P3M1,P3M0,P5M1,P5M0;
extern u8 P_SW1,CCON,CMOD,CL,CH,CCAP0L,CCAP0H,CCAP1L,CCAP1H,CCAP2L,CCAP2H;
extern u8 PCA_PWM0,PCA_PWM1,PCA_PWM2,CCAPM0,CCAPM1,CCAPM2,T2H,T2L,SCON,SBUF;
extern u8 PS,IE0,IE1,IT0,IT1,EX0,EX1,PX0,PX1,IP2,TR0,TMOD,TH0,TL0,TF0,ET0,MOTOR_PWM4;
#endif
