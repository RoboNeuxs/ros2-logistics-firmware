#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "board_config.h"
#include "app.h"
#include "motor_driver.h"
#include "encoder.h"
#include "serial_link.h"
#include "protocol.h"
#include "control_logic.h"
#include "parameters.h"
#include "motor_hal.h"

u8 EA,ES,P_SW2,P1,P1PU,P3PU,RI,TI;
u8 MOTOR_STBY,MOTOR_DIR1,MOTOR_DIR2,MOTOR_DIR3,MOTOR_DIR4;
u8 ENCODER_B1,ENCODER_B2,ENCODER_B3,ENCODER_B4;
u16 mock_ccr1,mock_ccr2,mock_ccr3,mock_ccr4,mock_prescaler;
u8 PWMA_EGR,PWMA_SR1;
static u8 captured[STATUS_SIZE];
static unsigned replies;
static u8 captured_length;
static unsigned watchdog_feeds,watchdog_time,watchdog_last_feed;
static u8 watchdog_started,watchdog_expired;
static u16 tx_wait_ms;
static u8 tx_wait_pulses;
static s16 tx_wait_output;

static void watchdog_advance(unsigned milliseconds)
{
    watchdog_time+=milliseconds;
    if(watchdog_started && watchdog_time-watchdog_last_feed>=1000U) watchdog_expired=1;
}
void Hal_Init(void)
{
    EA=ES=0; MOTOR_STBY=0; ENCODER_B1=ENCODER_B2=ENCODER_B3=ENCODER_B4=1;
    watchdog_feeds=watchdog_time=watchdog_last_feed=0;
    watchdog_started=watchdog_expired=0;
    tx_wait_ms=tx_wait_pulses=0; tx_wait_output=0;
}
u8 Hal_IrqSave(void) { u8 previous=EA; EA=0; return previous; }
void Hal_IrqRestore(u8 previous) { EA=previous; }
void Hal_Start(void) { watchdog_started=1; EA=1; }
void Hal_WatchdogFeed(void)
{
    assert(watchdog_started);
    watchdog_last_feed=watchdog_time;
    ++watchdog_feeds;
}
void Hal_Enable(u8 enabled) { MOTOR_STBY=enabled; }
void Hal_Fault(u8 fault) { app_fault |= fault; Hal_Enable(0); }
void Hal_Directions(u8 mask) {
    MOTOR_DIR1=(mask&1)!=0; MOTOR_DIR2=(mask&2)!=0;
    MOTOR_DIR3=(mask&4)!=0; MOTOR_DIR4=(mask&8)!=0;
}
u8 Hal_EncoderB(u8 channel) {
    switch(channel) { case 0: return ENCODER_B1; case 1: return ENCODER_B2;
    case 2: return ENCODER_B3; default: return ENCODER_B4; }
}
void Hal_EncoderFlush(u8 mask) { (void)mask; }
void Hal_WaitTx(void)
{
    u16 milliseconds=tx_wait_ms;
    tx_wait_ms=0; tx_wait_output=motor_applied[0];
    while(milliseconds--) {
        watchdog_advance(1);
        App_TickISR();
        if(tx_wait_pulses) Encoder_M1ISR();
    }
}
void Hal_PWM(const u16 *v) { mock_ccr1=v[0];mock_ccr2=v[1];mock_ccr3=v[2];mock_ccr4=v[3]; }
u8 Hal_Send(const u8 *v,u8 n) { assert(n==STATUS_SIZE || n==6);memcpy(captured,v,n);captured_length=n;++replies;return 1; }

static u16 get16(unsigned offset) { return captured[offset] | ((u16)captured[offset+1]<<8); }
static void setup(void)
{
    u8 value;
    while(Serial_Read(&value)) { }
    Serial_Recover();
    App_Init();replies=0;
    assert(settings.timeout_ms==1000 && config_state==0 && MOTOR_STBY==0);
}
static void request(u8 type,u8 flags,s16 a,s16 b,s16 c,s16 d)
{
    u8 packet[COMMAND_SIZE]={0xAA,0x55,0,7};
    unsigned i,before=replies;
    packet[2]=type;packet[12]=flags;
    Protocol_Put16(packet+4,(u16)a);Protocol_Put16(packet+6,(u16)b);
    Protocol_Put16(packet+8,(u16)c);Protocol_Put16(packet+10,(u16)d);
    packet[13]=Protocol_Crc(packet+2,11);
    for(i=0;i<COMMAND_SIZE;++i) Serial_RxISR(packet[i]);
    App_Poll();
    assert(replies>before && captured_length==STATUS_SIZE && captured[2]==RESPONSE_TYPE && captured[3]==7);
    assert(Protocol_Crc(captured+2,61)==captured[63]);
}
static void param(u8 scope,u8 key,u16 value)
{
    request(CMD_PARAM_SET,scope,key,(s16)value,0,0);assert(captured[4]==0);
}
static void run_ms(unsigned milliseconds)
{
    unsigned i;
    for(i=0;i<milliseconds;++i) { watchdog_advance(1); App_TickISR();App_Poll(); }
}
static void no_stall(void) { param(0,PAR_STALL_PWM,0); }

static void ping_tests(void)
{
    unsigned i,before;
    u8 packet[COMMAND_SIZE]={0xAA,0x55,0x7F,0,'P','I','N','G',0,0,0,0,0,0};
    setup();
    Serial_RxISR('P');Serial_RxISR('I');App_Poll();assert(replies==0);
    Serial_RxISR('N');Serial_RxISR('G');App_Poll();
    assert(replies==1 && captured_length==6 && memcmp(captured,"PONG\r\n",6)==0);
    assert(!app_mode && !MOTOR_STBY);
    packet[13]=Protocol_Crc(packet+2,11);
    before=replies;
    for(i=0;i<COMMAND_SIZE;++i) Serial_RxISR(packet[i]);
    App_Poll();assert(replies==before+1 && captured_length==STATUS_SIZE && captured[4]==2);
    /* A truncated query must expire so it cannot swallow the next text ping. */
    for(i=0;i<5;++i) Serial_RxISR(packet[i]);
    App_Poll();run_ms(101);
    before=replies;
    for(i=0;i<4;++i) Serial_RxISR((u8)"ping"[i]);
    App_Poll();assert(replies==before+1 && memcmp(captured,"PONG\r\n",6)==0);
    no_stall();request(CMD_SINGLE,1,MODE_PWM,100,0,0);run_ms(100);
    before=replies;
    for(i=0;i<4;++i) Serial_RxISR((u8)"PING"[i]);
    App_Poll();assert(replies==before+1 && app_command_age==100 && motor_applied[0]==100);
    run_ms(900);assert((app_fault&FAULT_TIMEOUT) && !MOTOR_STBY);
    for(i=0;i<4;++i) Serial_RxISR((u8)"PING"[i]);
    App_Poll();assert(captured_length==6 && (app_fault&FAULT_TIMEOUT));
}

static void protocol_tests(void)
{
    PacketParser p={{0},0};MotorCommand c;
    u8 packet[COMMAND_SIZE]={0xAA,0x55,CMD_BATCH,255};
    u8 stream[COMMAND_SIZE*3];unsigned i,accepted=0;
    assert(Protocol_Crc((const u8 *)"123456789",9)==0xF4);
    Protocol_Put16(packet+4,(u16)(s16)-1000);
    Protocol_Put16(packet+6,1000);Protocol_Put16(packet+8,(u16)(s16)-500);Protocol_Put16(packet+10,500);
    packet[13]=Protocol_Crc(packet+2,11);
    memcpy(stream,packet,14);stream[5]^=1;
    memcpy(stream+14,packet,14);memcpy(stream+28,packet,14);
    for(i=0;i<sizeof(stream);++i) accepted+=Protocol_Feed(&p,stream[i],&c);
    assert(accepted==2 && c.values[0]==-1000 && c.values[3]==500 && c.sequence==255);
    Protocol_Reset(&p);accepted=0;
    for(i=0;i<5;++i) accepted+=Protocol_Feed(&p,packet[i],&c);
    for(i=0;i<14;++i) accepted+=Protocol_Feed(&p,packet[i],&c);
    assert(accepted==1);
}
static void independent_and_atomic_tests(void)
{
    setup();no_stall();
    request(CMD_BATCH,0x55,1000,-500,250,-1000);run_ms(500);
    assert(motor_applied[0]==1000 && motor_applied[1]==-500 && motor_applied[2]==250 && motor_applied[3]==-1000);
    assert(mock_ccr1==PWM_COUNTS && mock_ccr2==PWM_COUNTS/2 && mock_ccr3==PWM_COUNTS/4 && mock_ccr4==PWM_COUNTS);
    assert(MOTOR_DIR1 && !MOTOR_DIR2 && MOTOR_DIR3 && !MOTOR_DIR4);
    request(CMD_SINGLE,2,MODE_STOP,0,0,0);
    assert(!motor_modes[1] && motor_applied[1]==0 && motor_applied[0]==1000 && MOTOR_STBY);
    request(CMD_SINGLE,3,MODE_SPEED,200,0,0);
    assert(motor_modes[2]==MODE_SPEED && motor_modes[0]==MODE_PWM);
    request(CMD_BATCH,0x99,100,200,300,10001); /* PWM,SPEED,PWM,SPEED */
    assert(captured[4]==2 && motor_applied[0]==1000 && motor_modes[1]==MODE_STOP);
    request(CMD_BATCH,0x99,100,200,-300,-400);
    assert(captured[4]==0 && motor_modes[1]==MODE_SPEED && motor_modes[3]==MODE_SPEED);
    request(CMD_QUERY,0,0,0,0,0);
    assert(get16(11)==100 && get16(22)==200 && captured[10]==MODE_PWM && captured[21]==MODE_SPEED);
    request(CMD_STOP,0,0,0,0,0);assert(!app_mode && !MOTOR_STBY && !mock_ccr1 && !mock_ccr4);
    setup();no_stall();request(CMD_BATCH,0x55,600,600,0,0);run_ms(300);
    request(CMD_BATCH,0x24,0,200,100,0);
    assert(motor_applied[0]==600 && motor_applied[1]==600);
    run_ms(49);assert(motor_applied[0]==600 && motor_applied[1]==600);
    run_ms(1);assert(motor_applied[0]==0 && motor_applied[1]==500);
}
static void reversal_decel_and_limits(void)
{
    setup();no_stall();
    param(1,PAR_PWM_ACCEL,200);param(1,PAR_PWM_DECEL,50);param(1,PAR_REVERSE_MS,150);
    request(CMD_SINGLE,1,MODE_PWM,600,0,0);run_ms(150);assert(motor_applied[0]==600);
    request(CMD_SINGLE,1,MODE_PWM,-600,0,0);
    run_ms(150);assert(motor_applied[0]==0 && !MOTOR_STBY);
    run_ms(50);assert(motor_applied[0]==-200 && !MOTOR_DIR1);
    request(CMD_SINGLE,1,MODE_DECEL,0,0,0);run_ms(50);assert(motor_applied[0]==-150);
    run_ms(150);assert(!motor_modes[0] && motor_applied[0]==0);
    param(1,PAR_POLARITY,1);param(1,PAR_MAX_PWM,300);param(1,PAR_MAX_SPEED,200);
    request(CMD_SINGLE,1,MODE_PWM,301,0,0);assert(captured[4]==2 && !app_mode);
    request(CMD_SINGLE,1,MODE_SPEED,201,0,0);assert(captured[4]==2);
    request(CMD_SINGLE,1,MODE_PWM,300,0,0);run_ms(250);assert(MOTOR_DIR1==0);
    request(CMD_PARAM_SET,1,PAR_KP,8,0,0);assert(captured[4]==3);
    setup();no_stall();param(1,PAR_PWM_ACCEL,1000);param(1,PAR_REVERSE_MS,150);
    request(CMD_SINGLE,1,MODE_PWM,600,0,0);run_ms(50);
    request(CMD_SINGLE,1,MODE_PWM,-600,0,0);run_ms(50);assert(motor_applied[0]==0);
    request(CMD_SINGLE,1,MODE_STOP,0,0,0);
    request(CMD_SINGLE,1,MODE_PWM,-600,0,0);run_ms(100);assert(motor_applied[0]==0);
    run_ms(50);assert(motor_applied[0]==-600);
    request(CMD_SINGLE,1,MODE_STOP,0,0,0);param(1,PAR_REVERSE_MS,500);
    request(CMD_SINGLE,1,MODE_PWM,-600,0,0);run_ms(450);assert(motor_applied[0]==0);
    run_ms(100);assert(motor_applied[0]==-600);
}
static void protection_tests(void)
{
    unsigned i;
    setup();no_stall();param(0,PAR_TIMEOUT,200);
    request(CMD_BATCH,0x55,100,100,100,100);run_ms(100);
    request(CMD_QUERY,0,0,0,0,0);assert(app_command_age==100);
    request(CMD_SINGLE,1,MODE_PWM,1001,0,0);assert(captured[4]==2 && app_command_age==100);
    run_ms(100);assert((app_fault&FAULT_TIMEOUT) && !MOTOR_STBY && !app_mode);
    request(CMD_STOP,0,0,0,0,0);assert(app_fault&FAULT_TIMEOUT);
    request(CMD_BATCH,0x55,100,100,100,100);assert(captured[4]==3);
    request(CMD_CLEAR,0,0,0,0,0);assert(!app_fault);
    request(CMD_BATCH,0x55,100,100,100,100);run_ms(150);
    request(CMD_HEARTBEAT,0,0,0,0,0);run_ms(150);assert(!app_fault);
    for(i=0;i<101;++i) App_TickISR();
    assert((app_fault&FAULT_SCHEDULER) && !MOTOR_STBY);
    setup();no_stall();request(CMD_BATCH,0x55,100,100,100,100);run_ms(50);
    for(i=0;i<128;++i) Serial_RxISR((u8)i);
    assert((app_fault&FAULT_RX_OVERFLOW) && !MOTOR_STBY);
    App_Poll();assert(!app_mode);
    setup();param(0,PAR_PWM_ACCEL,1000);param(0,PAR_STALL_MS,100);
    request(CMD_SINGLE,4,MODE_PWM,600,0,0);run_ms(150);
    assert((app_fault&FAULT_ENCODER) && fault_channels==8 && !app_mode && !MOTOR_STBY);
}
static void feedback_encoder_and_zero(void)
{
    setup();no_stall();
    Encoder_M1ISR();Encoder_M2ISR();Encoder_M3ISR();Encoder_M4ISR();run_ms(50);
    request(CMD_QUERY,0,0,0,0,0);assert(get16(15)==20 && get16(26)==20);
    request(CMD_ZERO,2,0,0,0,0);Encoder_ReadAll(encoder_counts);
    assert(encoder_counts[0]==1 && encoder_counts[1]==0 && encoder_counts[2]==1);
    ENCODER_B1=0;Encoder_M1ISR();Encoder_M1ISR();Encoder_ReadAll(encoder_counts);
    assert(encoder_counts[0]==UINT32_MAX);
    request(CMD_SINGLE,1,MODE_PWM,100,0,0);
    request(CMD_ZERO,1,0,0,0,0);assert(captured[4]==3);
    request(CMD_ZERO,4,0,0,0,0);assert(captured[4]==0);
    request(CMD_STOP,0,0,0,0,0);
    replies=0;run_ms(1000);assert(replies==0); /* Query-only telemetry. */
}
static void quadrature_tests(void)
{
    setup();
    ENCODER_B1=ENCODER_B2=ENCODER_B3=ENCODER_B4=0;
    Encoder_M1ISR();Encoder_M2ISR();Encoder_M3ISR();Encoder_M4ISR();
    Encoder_ReadAll(encoder_counts);
    assert(!app_mode && encoder_counts[0]==UINT32_MAX && encoder_counts[3]==UINT32_MAX);
    ENCODER_B1=ENCODER_B2=ENCODER_B3=ENCODER_B4=1;
    Encoder_M1ISR();Encoder_M2ISR();Encoder_M3ISR();Encoder_M4ISR();
    Encoder_ReadAll(encoder_counts);
    assert(!encoder_counts[0] && !encoder_counts[1] && !encoder_counts[2] && !encoder_counts[3]);
    param(2,PAR_POLARITY,1);Encoder_M2ISR();Encoder_ReadAll(encoder_counts);
    assert(encoder_counts[1]==UINT32_MAX);
    request(CMD_QUERY,0,0,0,0,0);
    assert(captured[7]==0x40 && !(get16(60)&0x0800));
    /* Logical positive output cannot override a negative encoder pulse. */
    request(CMD_SINGLE,1,MODE_PWM,100,0,0);run_ms(50);
    ENCODER_B1=0;Encoder_M1ISR();Encoder_ReadAll(encoder_counts);
    assert(encoder_counts[0]==UINT32_MAX);
}

static void parameters_and_reboot(void)
{
    unsigned i;
    setup();param(2,PAR_CPR,60000);
    request(CMD_PARAM_GET,2,PAR_CPR,0,0,0);assert(get16(54)==60000);
    request(CMD_PARAM_SET,1,PAR_MAX_PWM,1001,0,0);assert(captured[4]==2);
    request(CMD_PARAM_SET,1,PAR_KP,8,1,0);assert(captured[4]==2);
    request(CMD_PARAM_SET,5,PAR_KP,8,0,0);assert(captured[4]==2);
    param(0,PAR_KP,16);for(i=0;i<4;++i) assert(settings.motor[i].value[PAR_KP]==16);
    request(CMD_QUERY,0,0,0,0,0);
    assert(get16(58)==FW_VERSION && get16(60)==FW_CAPABILITIES && captured[7]==0x40 && config_state==2);
    request(CMD_PARAM_GET,0,129,0,0,0);assert(captured[4]==2);
    request(CMD_PARAM_SET,0,129,100,0,0);assert(captured[4]==2);
    request(0x22,0,0,0,0,0);assert(captured[4]==1); /* No EEPROM save. */
    request(0x11,0,0,0,0,0);assert(captured[4]==1);
    request(0x12,0,0,0,0,0);assert(captured[4]==1);
    request(0x1A,0,0,0,0,0);assert(captured[4]==1);
    App_Init();assert(settings.motor[1].value[PAR_CPR]==1 && config_state==0 && !MOTOR_STBY);
    param(2,PAR_CPR,500);
    request(CMD_DEFAULT,0,0,0,0,0);assert(settings.motor[1].value[PAR_CPR]==1 && config_state==0);
}
static void scheduling_and_watchdog_tests(void)
{
    unsigned i;
    setup(); run_ms(49);
    tx_wait_ms=11; tx_wait_pulses=1;
    request(CMD_QUERY,0,0,0,0,0);
    assert(app_ticks==60 && app_control_age==0 && watchdog_feeds==1);
    request(CMD_QUERY,0,0,0,0,0);
    assert(get16(15)==183);
    run_ms(50); request(CMD_QUERY,0,0,0,0,0);
    assert(get16(15)==0);

    setup(); no_stall(); request(CMD_SINGLE,1,MODE_PWM,500,0,0);
    for(i=0;i<50;++i) { watchdog_advance(1); App_TickISR(); }
    tx_wait_ms=1;
    request(CMD_QUERY,0,0,0,0,0);
    assert(tx_wait_output==100 && get16(13)==100 && watchdog_feeds==1);

    setup(); run_ms(65520);
    tx_wait_ms=30; tx_wait_pulses=1;
    request(CMD_QUERY,0,0,0,0,0);
    assert(app_ticks==14 && app_control_age==0);
    request(CMD_QUERY,0,0,0,0,0);
    assert(get16(15)==600 && !watchdog_expired);

    setup();
    for(i=0;i<20;++i) request(CMD_QUERY,0,0,0,0,0);
    assert(watchdog_started && !watchdog_feeds);
    watchdog_advance(1000);
    assert(watchdog_expired && app_ticks==0);

    setup(); run_ms(50); assert(watchdog_feeds==1);
    for(i=0;i<100;++i) App_TickISR();
    assert(watchdog_feeds==1);
    watchdog_advance(1000); assert(watchdog_expired);

    setup(); Hal_Fault(FAULT_RX_OVERFLOW); run_ms(1500);
    assert(app_fault==FAULT_RX_OVERFLOW && !MOTOR_STBY && watchdog_feeds==30 && !watchdog_expired);
}

static void pi_tests(void)
{
    SpeedController c;MotorParameters p;
    int t,m; s16 effort;
    setup();p=settings.motor[0];p.value[PAR_MAX_PWM]=250;p.value[PAR_KP]=1024;p.value[PAR_KI]=1024;
    p.value[PAR_I_LIMIT]=100;
    for(t=-10000;t<=10000;t+=1000) {
        Speed_Reset(&c);
        for(m=-32767;m<=32767;m+=999) {
            effort=Speed_Step(&c,(s16)t,(s16)m,&p);
            assert(effort>=-250 && effort<=250 && c.integral>=-6400 && c.integral<=6400);
            assert(t==0 ? effort==0 : t>0 ? effort>=0 : effort<=0);
        }
    }
    assert(Control_Ramp(-10000,10000,10000)==0 && Control_Ramp(10000,-10000,10000)==0);
    p=settings.motor[0];
    for(t=-1;t<=1;t+=2) {
        Speed_Reset(&c);
        for(m=0;m<20;++m) assert(Speed_Step(&c,(s16)(t*100),(s16)(t*200),&p)==0);
        assert(c.integral==0);
        assert(Speed_Step(&c,(s16)(t*100),0,&p)*t>0);
        c.integral=t*1000;
        assert(Speed_Step(&c,0,(s16)(t*100),&p)==0 && c.integral==0);

        Speed_Reset(&c); p.value[PAR_MAX_PWM]=1; p.value[PAR_KP]=64; p.value[PAR_KI]=1;
        assert(Speed_Step(&c,(s16)t,0,&p)==t && c.integral==0);
        p=settings.motor[0];
    }
}
#ifdef MOTOR_INTEGRATION_TEST
void bridge_reset(void) { setup(); }
int bridge_receive(const u8 *input,u8 *output)
{
    unsigned i,before=replies;
    for(i=0;i<COMMAND_SIZE;++i) Serial_RxISR(input[i]);
    App_Poll();
    if(replies==before) return 0;
    memcpy(output,captured,STATUS_SIZE);return STATUS_SIZE;
}
void bridge_advance(unsigned milliseconds) { run_ms(milliseconds); }
#endif
int main(void)
{
    protocol_tests();ping_tests();independent_and_atomic_tests();reversal_decel_and_limits();
    protection_tests();feedback_encoder_and_zero();parameters_and_reboot();quadrature_tests();pi_tests();
    scheduling_and_watchdog_tests();
    puts("STM32 V4 firmware: independent/mixed modes, atomic validation, reversal/deceleration, protections, telemetry, encoder wrap, parameter ranges, volatile configuration/reboot defaults and PI passed.");
    return 0;
}
