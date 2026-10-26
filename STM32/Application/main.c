/* 应用入口与任务调度。
 * 上电：启动文件 -> SystemInit() 配时钟 -> main() -> App_Init()。
 * 运行：串口中断收字节 -> App_Poll() 解析命令 -> 每 50 ms 执行控制。
 * SysTick 每 1 ms 独立检查超时，主循环来不及运行时也能切断 PWM。
 * 建议阅读顺序：本文件 -> control_logic.c -> motor_driver.c -> Board/motor_hal.c。
 */
#include "board_config.h"
#include "app.h"
#include "motor_driver.h"
#include "serial_link.h"
#include "protocol.h"
#include "control_logic.h"
#include "encoder.h"
#include "parameters.h"
#include "motor_hal.h"

/* volatile 表示这些状态也会被中断访问，每次读取都要从内存取值。
 * app_fault 是可叠加的故障位；app_mode 表示是否仍有电机处于活动状态。
 * app_ticks 为毫秒节拍；两个 age 分别记录距离有效指令、控制执行的时间。
 */
volatile u8 app_fault,app_mode;
volatile u16 app_ticks,app_command_age,app_control_age;
u8 motor_modes[MOTOR_COUNT],fault_channels;
u32 APP_XDATA encoder_counts[MOTOR_COUNT];
static u32 APP_XDATA previous_counts[MOTOR_COUNT];
/* 数组下标 0~3 对应 M1~M4：
 * targets 为命令目标（PWM 模式用千分比，速度模式用 count/s）；
 * setpoints 为速度斜坡后的目标，speeds 为实测 count/s；
 * outputs 为交给驱动层的有符号 PWM 请求，motor_applied 才是实际施加值。
 */
static s16 APP_XDATA targets[MOTOR_COUNT],setpoints[MOTOR_COUNT],speeds[MOTOR_COUNT],outputs[MOTOR_COUNT];
static u16 APP_XDATA stall_time[MOTOR_COUNT];
static SpeedController  controllers[MOTOR_COUNT];
static PacketParser  parser;
static MotorCommand  command;
static u8  status_packet[STATUS_SIZE];
static u16  parameter_value;
static u16 last_control;
static u8 ping_index;
static u16 last_rx_tick;
static const u8  ping_request[]="PING";
static const u8  ping_reply[]="PONG\r\n";
#if BOARD_COMM_DIAGNOSTICS
volatile u8 app_debug_stage,app_debug_rx_irq,app_debug_rx_main,app_debug_frames;
#define DEBUG_STAGE(value) app_debug_stage=(value)
#define DEBUG_BOOT(value)
#else
#define DEBUG_STAGE(value)
#define DEBUG_BOOT(value)
#endif

/* 文本 PING 仅测试通信；它不续期运动指令，不能用来维持电机运行。 */
static void ping_feed(u8 value)
{
    u8 i;
    /* 收二进制帧时不识别文本，避免载荷中的 PING 被误当成测试指令。 */
    if(parser.used) { ping_index=0; return; }
    value &= 0xDF; /* 将 ASCII 小写字母转成大写，兼容 ping/PING。 */
    if(value==ping_request[ping_index]) ++ping_index;
    else ping_index=value=='P' ? 1 : 0;
    if(ping_index==4) {
        ping_index=0;
        Hal_WaitTx();
        for(i=0;i<6;++i) status_packet[i]=ping_reply[i];
        Serial_Send(status_packet,6);
    }
}

/* 由 SysTick 中断每 1 ms 调用；只做计时与安全关断，不在中断里解析协议。 */
void App_TickISR(void)
{
    ++app_ticks;
    /* age 饱和到 65535，避免溢出回零后被误认为刚收到指令。 */
    if(app_command_age<65535U) ++app_command_age;
    if(app_control_age<65535U) ++app_control_age;
    if(app_mode) {
        if(app_command_age>=settings.timeout_ms) Hal_Fault(FAULT_TIMEOUT);
        if(app_control_age>CONTROL_MAX_DELAY_MS) Hal_Fault(FAULT_SCHEDULER);
        if(app_fault) Hal_Enable(0);
    }
}

/* 任一路模式非停止，或 PWM 尚未降到零，整板就仍处于活动状态。 */
static void update_active(void)
{
    u8 i;
    app_mode=0;
    for(i=0;i<MOTOR_COUNT;++i) if(motor_modes[i] || motor_applied[i]) app_mode=1;
}

/* mask 的 bit0~3 选择 M1~M4；15(0x0F) 为全部。
 * 同时清目标、斜坡、PI 积分和堵转计时，但不清故障锁存。
 */
static void stop_mask(u8 mask)
{
    u8 i;
    for(i=0;i<MOTOR_COUNT;++i) if(mask&(1U<<i)) {
        motor_modes[i]=MODE_STOP; targets[i]=setpoints[i]=outputs[i]=0;
        stall_time[i]=0; Speed_Reset(&controllers[i]);
    }
    Motor_Disable(mask);
    update_active();
}

/* 组装 64 字节应答；sequence 原样返回，供上位机匹配请求。
 * status_packet 是异步发送的缓冲区，改写前必须等上一次发送取完数据。
 */
static void send_status(u8 type,u8 sequence,u8 error)
{
    u8 i,offset,old_ea;
    Hal_WaitTx();
    status_packet[0]=0xAA; status_packet[1]=0x55;
    status_packet[2]=type; status_packet[3]=sequence;
    status_packet[4]=error; status_packet[5]=app_fault; status_packet[6]=fault_channels;
    status_packet[7]=0 | (CONFIG_SCHEMA_VERSION<<4); /* 高四位为参数版本；bit0=0 表示由 A/B 相实测方向。 */
    old_ea=Hal_IrqSave();
    Protocol_Put16(status_packet+8,app_command_age); Hal_IrqRestore(old_ea);
    Encoder_ReadAll(encoder_counts);
    /* 每台电机占 11 字节：模式 1 + 目标 2 + PWM 2 + 速度 2 + 累计计数 4。 */
    for(i=0;i<MOTOR_COUNT;++i) {
        offset=10+11*i;
        status_packet[offset]=motor_modes[i];
        Protocol_Put16(status_packet+offset+1,(u16)targets[i]);
        Protocol_Put16(status_packet+offset+3,(u16)motor_applied[i]);
        Protocol_Put16(status_packet+offset+5,(u16)speeds[i]);
        Protocol_Put32(status_packet+offset+7,encoder_counts[i]);
    }
    Protocol_Put16(status_packet+54,parameter_value);
    status_packet[56]=(u8)command.values[0]; status_packet[57]=command.flags;
    Protocol_Put16(status_packet+58,FW_VERSION);
    Protocol_Put16(status_packet+60,FW_CAPABILITIES);
    status_packet[62]=config_state;
    status_packet[63]=Protocol_Crc(status_packet+2,61); /* CRC 覆盖字节 2~62，不含 AA 55。 */
    Serial_Send(status_packet,STATUS_SIZE);
}

/* 处理运动指令：返回 0=成功，2=参数越界，3=故障锁存、不允许运行。
 * SINGLE：flags=0 控制全部，1~4 控制单台，values[0/1] 为模式/目标。
 * BATCH：values[0~3] 为各路目标，flags 每两位表示一路模式。
 */
static u8 motion_command(void)
{
    u8 i,mode,old_ea;
    s16 wanted,limit;
    if(command.type==CMD_SINGLE && (command.flags>4 || command.values[0]<0 ||
       command.values[0]>3 || command.values[2] || command.values[3])) return 2;
    /* 先校验所有选中电机，再统一更新，避免前几路已执行、后一路才发现越界。 */
    for(i=0;i<MOTOR_COUNT;++i) {
        if(command.type==CMD_SINGLE) {
            if(command.flags && command.flags!=i+1) continue;
            mode=(u8)command.values[0]; wanted=command.values[1];
        } else { mode=(command.flags>>(2*i))&3; wanted=command.values[i]; }
        limit=mode==MODE_PWM ? (s16)settings.motor[i].value[PAR_MAX_PWM] :
              (mode==MODE_SPEED ? (s16)settings.motor[i].value[PAR_MAX_SPEED] : 0);
        if(wanted<-limit || wanted>limit) return 2;
    }
    /* 检查故障与重置计时放在同一临界区，避免中断插入造成状态不一致。 */
    old_ea=Hal_IrqSave();
    if(app_fault) { Hal_IrqRestore(old_ea); return 3; }
    app_command_age=app_control_age=0;
    Hal_IrqRestore(old_ea);
    for(i=0;i<MOTOR_COUNT;++i) {
        if(command.type==CMD_SINGLE) {
            if(command.flags && command.flags!=i+1) continue;
            mode=(u8)command.values[0]; wanted=command.values[1];
        } else { mode=(command.flags>>(2*i))&3; wanted=command.values[i]; }
        /* 换模式或反向时丢弃旧积分与速度斜坡，避免旧方向的控制量突然施加。 */
        if(motor_modes[i]!=mode || (targets[i]>0 && wanted<0) || (targets[i]<0 && wanted>0)) {
            Speed_Reset(&controllers[i]); setpoints[i]=0; stall_time[i]=0;
        }
        motor_modes[i]=mode; targets[i]=wanted;
    }
    if(command.type==CMD_SINGLE)
        for(i=0;i<MOTOR_COUNT;++i)
            if(motor_modes[i]==MODE_STOP && (!command.flags || command.flags==i+1)) stop_mask(1U<<i);
    update_active();
    return 0;
}

/* 分发完整命令；参数写入、清故障、清零等操作受停机条件约束。
 * 每条完成解析的命令都返回状态帧；CRC 错误的帧不会到达这里。
 */
static void process_command(void)
{
    u8 error=0,i,mask,old_ea;
    parameter_value=0;
    if(command.type==CMD_SINGLE || command.type==CMD_BATCH) error=motion_command();
    else if(command.type==CMD_PARAM_GET || command.type==CMD_PARAM_SET) {
        if(command.values[0]<0 || command.values[0]>128 || command.values[2] || command.values[3] ||
           (command.type==CMD_PARAM_GET && command.values[1])) error=2;
        else if(command.type==CMD_PARAM_SET && app_mode) error=3;
        else {
            parameter_value=(u16)command.values[1];
            error=Config_Access(command.type==CMD_PARAM_SET,command.flags,
                                (u8)command.values[0],&parameter_value);
            if(!error && command.type==CMD_PARAM_SET &&
               (command.values[0]==PAR_POLARITY || command.values[0]==PAR_REVERSE_MS))
                Motor_Reconfigure(command.flags);
        }
    } else {
        for(i=0;i<MOTOR_COUNT;++i) if(command.values[i]) error=2;
        if(command.flags && command.type!=CMD_ZERO) error=1;
        if(!error) switch(command.type) {
        case CMD_STOP: stop_mask(15); break;
        case CMD_QUERY: break;
        /* 心跳只续期已接受的目标；查询与参数读取不会续期。 */
        case CMD_HEARTBEAT:
            if(app_fault) error=3;
            else { old_ea=Hal_IrqSave(); app_command_age=0; Hal_IrqRestore(old_ea); }
            break;
        /* 清故障不会恢复旧运动目标，必须在停止后重新发送运动命令。 */
        case CMD_CLEAR:
            if(app_mode) error=3;
            else { old_ea=Hal_IrqSave(); app_fault=0; fault_channels=0; Hal_IrqRestore(old_ea); }
            break;
        case CMD_ZERO:
            if(command.flags>4) { error=2; break; }
            mask=command.flags ? (1U<<(command.flags-1)) : 15;
            for(i=0;i<MOTOR_COUNT;++i) if((mask&(1U<<i)) && (motor_modes[i] || motor_applied[i])) error=3;
            if(!error) {
                /* 编码器与测速基准一起清零，避免下个周期把清零当成高速反转。 */
                Encoder_Zero(mask);
                for(i=0;i<MOTOR_COUNT;++i) if(mask&(1U<<i)) {
                    encoder_counts[i]=previous_counts[i]=0; speeds[i]=0;
                }
            }
            break;
        case CMD_DEFAULT:
            if(app_mode) error=3;
            else { Config_Default(); Motor_Reconfigure(0); }
            break;
        default: error=1;
        }
    }
#if BOARD_COMM_DIAGNOSTICS
    /* 通信诊断模式保留主循环和协议，但拒绝运动，便于离线排查通信。 */
    if(command.type==CMD_SINGLE || command.type==CMD_BATCH) { stop_mask(15); error=3; }
#endif
    send_status(RESPONSE_TYPE,command.sequence,error);
}

/* 速度 = 本周期有符号计数差 * 1000 / 实际间隔(ms)，单位 count/s。
 * 先做 u32 回绕减法，再按 s32 解释方向；返回值限制在 int16 可用范围内。
 * 这里不除以 CPR；上位机如需转速，再根据每圈计数换算。
 */
static s16 speed_from_delta(u32 current,u32 previous,u16 elapsed)
{
    s32 delta=(s32)(current-previous),result;
    if(delta>32767L) return 32767;
    if(delta<-32767L) return -32767;
    result=delta*1000L/elapsed;
    if(result>32767L) return 32767;
    if(result<-32767L) return -32767;
    return (s16)result;
}

/* 一次控制周期：读编码器 -> 测速 -> 速度斜坡/PI -> 堵转判断 -> 应用 PWM。
 * elapsed 为实际经过的毫秒数；斜坡和 PI 则每次调用推进一步。
 */
static void control_step(u16 elapsed)
{
    u8 i,old_ea;
    s16 absolute_pwm,absolute_speed;
    u16 step;
    const MotorParameters APP_IDATA *p;
    for(i=0;i<MOTOR_COUNT;++i) {
        speeds[i]=speed_from_delta(encoder_counts[i],previous_counts[i],elapsed);
        previous_counts[i]=encoder_counts[i];
        p=&settings.motor[i];
        outputs[i]=targets[i]; /* PWM 模式直接使用目标；速度模式在下面换算为 PWM。 */
        if(motor_modes[i]==MODE_SPEED) {
            step=((targets[i]>=0 ? targets[i] : -targets[i]) >
                  (setpoints[i]>=0 ? setpoints[i] : -setpoints[i])) ?
                   p->value[PAR_SPEED_ACCEL] : p->value[PAR_SPEED_DECEL];
            setpoints[i]=Control_Ramp(setpoints[i],targets[i],step);
            outputs[i]=Speed_Step(&controllers[i],setpoints[i],speeds[i],p);
        }
        absolute_pwm=motor_applied[i]>=0 ? motor_applied[i] : -motor_applied[i];
        absolute_speed=speeds[i]>=0 ? speeds[i] : -speeds[i];
        /* 持续“大 PWM、低反馈速度”才判堵转；阈值 PWM=0 表示禁用此检测。 */
        if(motor_modes[i]!=MODE_STOP && motor_modes[i]!=MODE_DECEL && targets[i] &&
           p->value[PAR_STALL_PWM] && absolute_pwm>=p->value[PAR_STALL_PWM] &&
           absolute_speed<=p->value[PAR_STALL_SPEED]) {
            if(stall_time[i]<p->value[PAR_STALL_MS]) stall_time[i]+=elapsed;
            if(stall_time[i]>=p->value[PAR_STALL_MS]) {
                old_ea=Hal_IrqSave();
                fault_channels |= (1U<<i); Hal_Fault(FAULT_ENCODER);
                Hal_IrqRestore(old_ea);
            }
        } else stall_time[i]=0;
    }
    /* 任一路触发故障都停止四路；无故障时才允许驱动层重新使能输出。 */
    if(app_fault) stop_mask(15);
    else {
        Motor_Apply(outputs);
        for(i=0;i<MOTOR_COUNT;++i)
            if(motor_modes[i]==MODE_DECEL && !motor_applied[i]) stop_mask(1U<<i);
        update_active();
    }
}

static void poll_control(void)
{
    u8 previous=Hal_IrqSave();
    u16 now=app_ticks;
    u16 elapsed=(u16)(now-last_control);
    if(elapsed<CONTROL_PERIOD_MS) { Hal_IrqRestore(previous); return; }
    /* 节拍和计数在同一临界区采样，避免串口等待改变测速窗口。 */
    Encoder_ReadAll(encoder_counts);
    last_control=now;
    app_control_age=0;
    Hal_IrqRestore(previous);
    DEBUG_STAGE(5);
    control_step(elapsed);
    Hal_WatchdogFeed();
}

/* 先在关中断状态下配置外设与软件状态，最后 Hal_Start() 才启动节拍和中断。 */
void App_Init(void)
{
    u8 i;
    Hal_Init();
    DEBUG_STAGE(1); DEBUG_BOOT('H');
    Config_Default();
    DEBUG_STAGE(2); DEBUG_BOOT('D');
    app_fault=app_mode=fault_channels=0;
    app_ticks=app_command_age=app_control_age=last_control=0;
    ping_index=0; last_rx_tick=0;
    Protocol_Reset(&parser);
    for(i=0;i<MOTOR_COUNT;++i) {
        motor_modes[i]=0; speeds[i]=0; previous_counts[i]=0;
    }
    Motor_Reconfigure(0);
    Encoder_Zero(15); stop_mask(255); /* 0xFF 还要求驱动层清除历史方向和换向等待。 */
    DEBUG_STAGE(3); DEBUG_BOOT('M');
    Hal_Start();
}

/* 主循环反复调用；不使用固定延时，让串口和控制任务都有机会及时运行。 */
void App_Poll(void)
{
    u8 value,budget,old_ea;
    u16 now;
    DEBUG_STAGE(4);
    if(app_fault && app_mode) stop_mask(15);
    poll_control();
    old_ea=Hal_IrqSave(); now=app_ticks; Hal_IrqRestore(old_ea);
    /* 接收错误或字节间隔超过 100 ms 时丢弃半帧，从下一帧重新同步。 */
    if(Serial_Recover() || (u16)(now-last_rx_tick)>100U) {
        Protocol_Reset(&parser); ping_index=0;
    }
    /* 每轮最多处理 32 字节，防止串口连续输入一直占用主循环、拖延控制任务。 */
    for(budget=0;budget<32 && Serial_Read(&value);++budget) {
#if BOARD_COMM_DIAGNOSTICS
        ++app_debug_rx_main;
#endif
        ping_feed(value);
        if(Protocol_Feed(&parser,value,&command)) {
#if BOARD_COMM_DIAGNOSTICS
            ++app_debug_frames;
#endif
            process_command();
        }
        old_ea=Hal_IrqSave(); last_rx_tick=app_ticks; Hal_IrqRestore(old_ea);
    }
    poll_control();
    DEBUG_STAGE(6);
}
#ifndef MOTOR_HOST_TEST
int main(void) { App_Init(); while(1) App_Poll(); }
#endif
