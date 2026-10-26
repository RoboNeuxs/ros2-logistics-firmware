"""STM32F103C8T6 four-motor V4 protocol, configuration API and command-line client."""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
import struct
import sys
import time

HEADER = b"\xaa\x55"
STATUS_SIZE = 64
COMMANDS = {"pwm":0x17,"speed":0x17,"stop":0x13,"query":0x14,"zero":0x15,
            "single":0x16,"batch":0x17,"heartbeat":0x18,"clear":0x19,"info":0x14,
            "get":0x20,"set":0x21,"default":0x23}
MODES = {"stop":0,"pwm":1,"speed":2,"decel":3}
FAULTS = {1:"通信失联",2:"接收溢出",4:"堵转/无反馈",8:"控制调度超时"}
ERRORS = {1:"命令或保留字段错误",2:"参数范围/选择器错误",3:"当前状态不允许"}
# name, Chinese label, minimum, maximum, default, unit/help
PARAMETERS = (
    ("polarity","方向反转",0,1,0,"0=正常，1=反向"),
    ("max_pwm","最大 PWM",1,1000,1000,"千分比"),
    ("max_speed","最大速度",1,10000,10000,"count/s"),
    ("cpr","输出轴每圈计数",1,60000,1,"A 相下降沿/圈；默认 1 为占位"),
    ("kp","Kp 分子",0,1024,4,"实际增益=值/64"),
    ("ki","Ki 分子",0,1024,1,"实际增益=值/64，每 50 ms 累加"),
    ("i_limit","积分限幅",0,1000,1000,"PWM 千分比"),
    ("pwm_accel","PWM 加速步长",1,1000,100,"千分比/50 ms"),
    ("pwm_decel","PWM 减速步长",1,1000,100,"千分比/50 ms"),
    ("reverse_ms","换向等待",50,5000,100,"ms，向上取整至 50 ms"),
    ("stall_pwm","堵转 PWM 阈值",0,1000,300,"千分比；0=关闭检测"),
    ("stall_speed","堵转低速阈值",0,10000,0,"count/s，反馈幅度不高于此值"),
    ("stall_ms","堵转持续时间",50,10000,500,"ms"),
    ("speed_accel","速度加速步长",1,10000,1000,"count/s /50 ms"),
    ("speed_decel","速度减速步长",1,10000,1000,"count/s /50 ms"),
)
GLOBALS = (("timeout_ms","失联时间",200,10000,1000,"ms"),)
PARAM_IDS = {p[0]:i for i,p in enumerate(PARAMETERS)}
PARAM_IDS.update({p[0]:128+i for i,p in enumerate(GLOBALS)})

def crc8(data: bytes) -> int:
    crc=0
    for value in data:
        crc ^= value
        for _ in range(8):
            crc=((crc<<1) ^ (0x07 if crc&0x80 else 0)) & 255
    return crc

def _integer(value, low, high, name):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(f"{name}: 需要 {low}～{high} 的整数")
    return value

def parameter_id(parameter: str | int) -> int:
    if isinstance(parameter,str):
        if parameter not in PARAM_IDS: raise ValueError(f"未知参数: {parameter}")
        return PARAM_IDS[parameter]
    if type(parameter) is not int or parameter not in (*range(len(PARAMETERS)),128):
        raise ValueError("未知参数编号")
    return parameter

def validate_parameter(parameter, value):
    key=parameter_id(parameter)
    p=GLOBALS[key-128] if key>=128 else PARAMETERS[key]
    _integer(value,p[2],p[3],p[1])
    return value

def encode_command(mode: str, sequence: int, m1=0,m2=0,m3=0,m4=0, *, flags=0) -> bytes:
    if mode not in COMMANDS: raise ValueError(f"未知命令: {mode}")
    _integer(sequence,0,255,"序号");_integer(flags,0,255,"选择器")
    values=(m1,m2,m3,m4)
    if mode in ("pwm","speed","batch"):
        if mode!="batch" and flags: raise ValueError("保留字段必须为 0")
        for i,value in enumerate(values):
            selected=(flags>>(2*i))&3 if mode=="batch" else MODES[mode]
            limit=1000 if selected==1 else 10000 if selected==2 else 0
            _integer(value,-limit,limit,f"M{i+1}")
    elif mode=="single":
        _integer(flags,0,4,"电机编号");_integer(m1,0,3,"模式")
        _integer(m2,-(1000 if m1==1 else 10000 if m1==2 else 0),
                 1000 if m1==1 else 10000 if m1==2 else 0,"目标")
        if m3 or m4: raise ValueError("保留字段必须为 0")
    elif mode in ("get","set"):
        key=parameter_id(m1);_integer(flags,0 if mode=="set" or key>=128 else 1,4,"电机编号")
        if key>=128 and flags: raise ValueError("全局参数的电机编号必须为 0")
        if m3 or m4 or (mode=="get" and m2): raise ValueError("保留字段必须为 0")
        if mode=="set": validate_parameter(key,m2)
    else:
        if any(values): raise ValueError("此命令的数值必须为 0")
        _integer(flags,0,4 if mode=="zero" else 0,"选择器")
    if mode in ("pwm","speed"): flags=0x55 if mode=="pwm" else 0xAA
    body=struct.pack("<BBHHHHB",COMMANDS[mode],sequence,*(v&65535 for v in values),flags)
    return HEADER+body+bytes([crc8(body)])

@dataclass(frozen=True)
class MotorStatus:
    mode: int
    target: int
    pwm: int
    speed: int
    count: int

@dataclass(frozen=True)
class Status:
    kind: int
    sequence: int
    error: int
    fault: int
    fault_channels: int
    flags: int
    age_ms: int
    motors: tuple[MotorStatus,...]
    parameter_value: int
    parameter: int
    scope: int
    firmware: int
    capabilities: int
    config_state: int

    @property
    def direction_inferred(self): return bool(self.flags&1)
    @property
    def parameter_schema(self): return self.flags>>4
    @property
    def running(self): return any(m.mode for m in self.motors)
    @property
    def fault_text(self):
        return " / ".join(text for flag,text in FAULTS.items() if self.fault&flag) or "正常"
    def __str__(self):
        return (f"V{self.firmware>>8}.{self.firmware&255} "
                f"mode={tuple(m.mode for m in self.motors)} "
                f"target={tuple(m.target for m in self.motors)} "
                f"pwm={tuple(m.pwm for m in self.motors)} "
                f"speed={tuple(m.speed for m in self.motors)} count/s "
                f"counts={tuple(m.count for m in self.motors)} "
                f"fault={self.fault_text} channels=0x{self.fault_channels:X} "
                f"direction={'inferred' if self.direction_inferred else 'A/B'} age={self.age_ms}ms")

class ProtocolMismatch(RuntimeError):
    pass

class StatusParser:
    def __init__(self): self.buffer=bytearray();self.crc_errors=0
    def feed(self,chunk: bytes) -> list[Status]:
        self.buffer.extend(chunk);result=[]
        while True:
            start=self.buffer.find(HEADER)
            if start<0:
                self.buffer[:]=b"\xaa" if self.buffer.endswith(b"\xaa") else b""
                break
            del self.buffer[:start]
            if len(self.buffer)<3: break
            if self.buffer[2]==0x91:
                if len(self.buffer)<42: break
                if crc8(self.buffer[2:41])==self.buffer[41]:
                    raise ProtocolMismatch("检测到旧版固件，需要配套 V4 固件")
            if self.buffer[2] not in (0x93,0x94):
                del self.buffer[0];continue
            if len(self.buffer)<STATUS_SIZE: break
            frame=bytes(self.buffer[:STATUS_SIZE])
            if crc8(frame[2:63])!=frame[63]:
                self.crc_errors+=1
                del self.buffer[0];continue
            error,fault,channels,flags,age=struct.unpack_from("<BBBBH",frame,4)
            motors=tuple(MotorStatus(*struct.unpack_from("<Bhhhi",frame,10+11*i)) for i in range(4))
            value,key,scope,version,capabilities,state=struct.unpack_from("<HBBHHB",frame,54)
            result.append(Status(frame[2],frame[3],error,fault,channels,flags,age,
                                 motors,value,key,scope,version,capabilities,state))
            del self.buffer[:STATUS_SIZE]
        return result

class CommandRejected(RuntimeError):
    def __init__(self,status):
        self.status=status
        super().__init__(f"设备拒绝命令：{ERRORS.get(status.error,status.error)}")

class MotorClient:
    """A single owner performs requests; heartbeat renews all active motor commands."""
    def __init__(self,port,on_status=None,on_wire=None):
        self.port=port;self.sequence=0;self.parser=StatusParser();self.last_status=None
        self.on_status=on_status
        self.on_wire=on_wire
    def _receive(self,chunk):
        if chunk and self.on_wire: self.on_wire('RX',bytes(chunk))
        statuses=self.parser.feed(chunk)
        for status in statuses:
            if status.firmware>>8!=4: raise ProtocolMismatch(f"不支持的固件主版本：收到 V{status.firmware>>8}.{status.firmware&255}，需要 V4")
            if status.parameter_schema!=4: raise ProtocolMismatch(f"不支持的参数格式版本：收到 {status.parameter_schema}，需要 4")
            self.last_status=status
            if self.on_status: self.on_status(status)
        return statuses
    def send_command(self,mode,m1=0,m2=0,m3=0,m4=0,*,flags=0):
        """Write one command without waiting for an acknowledgement; return its sequence."""
        sequence=self.sequence;self.sequence=(sequence+1)&255
        frame=encode_command(mode,sequence,m1,m2,m3,m4,flags=flags)
        if self.port.write(frame)!=len(frame): raise OSError("串口未完整写入")
        if self.on_wire: self.on_wire('TX',frame)
        return sequence
    def ping(self,timeout=0.35):
        """Text transport check; neither renews motion nor confirms a V4 status frame."""
        if not math.isfinite(timeout) or timeout<=0: raise ValueError("timeout 必须为正数")
        frame=b'PING'
        if self.port.write(frame)!=len(frame): raise OSError("串口未完整写入")
        if self.on_wire: self.on_wire('TX',frame)
        received=bytearray();deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            chunk=self.port.read(max(1,min(self.port.in_waiting,256)))
            self._receive(chunk)
            received.extend(chunk)
            if b'PONG\r\n' in received: return True
            if len(received)>512: del received[:-128]
        raise TimeoutError("PING 已发送，未收到 PONG；串口保持连接")
    def command(self,mode,m1=0,m2=0,m3=0,m4=0,*,flags=0,timeout=0.35):
        if not math.isfinite(timeout) or timeout<=0: raise ValueError("timeout 必须为正数")
        sequence=self.send_command(mode,m1,m2,m3,m4,flags=flags)
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            chunk=self.port.read(max(1,min(self.port.in_waiting,256)))
            for status in self._receive(chunk):
                if status.kind!=0x93 or status.sequence!=sequence: continue
                if status.error:
                    raise CommandRejected(status)
                return status
        raise TimeoutError("未收到匹配的应答，请检查串口、接线和 V4 固件")
    def poll(self):
        count=min(self.port.in_waiting,1024)
        return self._receive(self.port.read(count)) if count else []
    def set_motor(self,motor,mode,target=0):
        selected=MODES[mode] if isinstance(mode,str) else _integer(mode,0,3,"模式")
        return self.command("single",selected,target,flags=motor)
    def set_all(self,modes,targets):
        if len(modes)!=4 or len(targets)!=4: raise ValueError("必须提供四台模式及目标")
        selected=[MODES[m] if isinstance(m,str) else _integer(m,0,3,"模式") for m in modes]
        return self.command("batch",*targets,flags=sum(m<<(2*i) for i,m in enumerate(selected)))
    def stop(self,motor=0,decelerate=False):
        return self.set_motor(motor,"decel",0) if decelerate else (
            self.command("stop") if motor==0 else self.set_motor(motor,"stop",0))
    def heartbeat(self): return self.command("heartbeat")
    def clear_faults(self): return self.command("clear")
    def zero(self,motor=0): return self.command("zero",flags=motor)
    def get_parameter(self,motor,parameter):
        key=parameter_id(parameter)
        result=self.command("get",key,flags=motor)
        if result.parameter!=key or result.scope!=motor: raise RuntimeError("参数应答不匹配")
        return result.parameter_value
    def set_parameter(self,motor,parameter,value):
        key=parameter_id(parameter)
        return self.command("set",key,validate_parameter(key,value),flags=motor)
    def read_configuration(self,cancelled=None):
        def read(scope,key):
            if cancelled and cancelled(): raise InterruptedError("配置读取已取消")
            return self.get_parameter(scope,key)
        return {"format":"stc8-motor-config-v4",
                "motors":[{p[0]:read(i+1,j) for j,p in enumerate(PARAMETERS)} for i in range(4)],
                "global":{p[0]:read(0,128+i) for i,p in enumerate(GLOBALS)}}
    def apply_configuration(self,configuration,cancelled=None):
        validated=validate_configuration(configuration)
        if cancelled and cancelled(): raise InterruptedError("配置应用已取消")
        self.stop()
        for i,motor in enumerate(validated["motors"],1):
            for key,value in motor.items():
                if cancelled and cancelled(): raise InterruptedError("配置应用已取消")
                self.set_parameter(i,key,value)
        for key,value in validated["global"].items():
            if cancelled and cancelled(): raise InterruptedError("配置应用已取消")
            self.set_parameter(0,key,value)
        return self.command("query")

def default_configuration():
    return {"format":"stc8-motor-config-v4",
            "motors":[{p[0]:p[4] for p in PARAMETERS} for _ in range(4)],
            "global":{p[0]:p[4] for p in GLOBALS}}

def validate_configuration(configuration):
    if not isinstance(configuration,dict) or set(configuration)!={"format","motors","global"}:
        raise ValueError("配置必须包含 format、motors 和 global")
    if configuration["format"]!="stc8-motor-config-v4": raise ValueError("配置版本不匹配")
    if not isinstance(configuration["motors"],list) or len(configuration["motors"])!=4:
        raise ValueError("配置必须包含四台电机")
    expected={p[0] for p in PARAMETERS};global_keys={p[0] for p in GLOBALS}
    for motor in configuration["motors"]:
        if not isinstance(motor,dict) or set(motor)!=expected: raise ValueError("电机参数字段不完整或含未知字段")
        for key,value in motor.items(): validate_parameter(key,value)
    if not isinstance(configuration["global"],dict) or set(configuration["global"])!=global_keys:
        raise ValueError("全局参数字段不完整")
    for key,value in configuration["global"].items(): validate_parameter(key,value)
    return json.loads(json.dumps(configuration))

def load_configuration(path): return validate_configuration(json.loads(Path(path).read_text(encoding="utf-8")))
def save_configuration(path,configuration):
    Path(path).write_text(json.dumps(validate_configuration(configuration),ensure_ascii=False,indent=2)+"\n",encoding="utf-8")

def rpm_to_counts(rpm,cpr):
    if not math.isfinite(rpm): raise ValueError("RPM 必须为有限数")
    _integer(cpr,1,60000,"每圈计数")
    return round(rpm*cpr/60)
def counts_to_rpm(counts,cpr):
    _integer(cpr,1,60000,"每圈计数")
    return counts*60/cpr

def main(argv=None):
    parser=argparse.ArgumentParser(description="STM32F103C8T6 四电机 V4 串口控制")
    parser.add_argument("--port");parser.add_argument("--mode",choices=(*COMMANDS,"read-config","write-config"),default="query")
    parser.add_argument("--motor",type=int,choices=range(5),default=0,help="1～4 单台，0 为全部/全局")
    parser.add_argument("--value",type=float,help="单台目标或参数值")
    parser.add_argument("--param",choices=PARAM_IDS)
    parser.add_argument("--config",help="JSON 配置文件")
    for i in range(1,5): parser.add_argument(f"--m{i}",type=float)
    parser.add_argument("--left",type=float);parser.add_argument("--right",type=float)
    parser.add_argument("--seconds",type=float,default=3)
    parser.add_argument("--unit",choices=("counts","rpm"),default="counts")
    parser.add_argument("--cpr",type=int,help="RPM 换算使用的每圈计数；省略时从设备读取")
    parser.add_argument("--dry-run",action="store_true")
    args=parser.parse_args(argv)
    if not math.isfinite(args.seconds) or args.seconds<=0: parser.error("--seconds 必须为正数")
    def prepare(client=None):
        mode=args.mode;flags=0;values=[0]*4
        if mode in ("pwm","speed"):
            if args.motor:
                raw=[args.value if args.value is not None else getattr(args,f"m{args.motor}")]
                if raw[0] is None: raise ValueError("单台控制需要 --value")
            else:
                raw=[getattr(args,f"m{i}") if getattr(args,f"m{i}") is not None else
                     (args.left if i in (1,3) else args.right) for i in range(1,5)]
                raw=[0 if v is None else v for v in raw]
            converted=[]
            for i,v in enumerate(raw):
                if not math.isfinite(v): raise ValueError("目标必须为有限数")
                if mode=="speed" and args.unit=="rpm":
                    cpr=args.cpr if args.cpr is not None else (
                        client.get_parameter(args.motor or i+1,"cpr") if client else None)
                    if cpr is None: raise ValueError("RPM dry-run 需要 --cpr")
                    converted.append(rpm_to_counts(v,cpr))
                else:
                    if int(v)!=v: raise ValueError("PWM/counts 目标必须为整数")
                    converted.append(int(v))
            if args.motor: values=[MODES[mode],converted[0],0,0];flags=args.motor;mode="single"
            else: values=converted
        elif mode in ("get","set"):
            if args.param is None: raise ValueError("需要 --param")
            values[0]=parameter_id(args.param);flags=args.motor
            if mode=="set":
                if args.value is None or not math.isfinite(args.value) or int(args.value)!=args.value:
                    raise ValueError("设置参数需要整数 --value")
                values[1]=int(args.value)
        elif mode=="stop" and args.motor: mode="single";flags=args.motor
        elif mode=="zero": flags=args.motor
        elif mode in ("single","batch"): raise ValueError("使用 Python API 的 set_motor/set_all 操作此命令")
        return mode,values,flags
    if args.dry_run:
        if args.mode in ("read-config","write-config"): parser.error("配置文件操作需要实际连接")
        try:
            mode,values,flags=prepare();print(encode_command(mode,0,*values,flags=flags).hex(" "))
            return 0
        except ValueError as error: parser.error(str(error))
    if not args.port: parser.error("需要 --port")
    try: import serial
    except ImportError:
        print("请安装：python -m pip install -r Host/requirements.txt",file=sys.stderr);return 1
    try:
        with serial.Serial(args.port,115200,timeout=0.02,write_timeout=0.35) as port:
            port.reset_input_buffer();client=MotorClient(port)
            if args.mode=="read-config":
                if not args.config: raise ValueError("需要 --config")
                client.stop();save_configuration(args.config,client.read_configuration());return 0
            if args.mode=="write-config":
                if not args.config: raise ValueError("需要 --config")
                print(client.apply_configuration(load_configuration(args.config)));return 0
            mode,values,flags=prepare(client)
            if args.mode in ("pwm","speed"):
                try:
                    deadline=time.monotonic()+args.seconds
                    while time.monotonic()<deadline:
                        status=client.command(mode,*values,flags=flags);print(status)
                        if status.fault: raise RuntimeError(status.fault_text)
                        time.sleep(0.05)
                finally:
                    try: client.stop(args.motor)
                    except (OSError,TimeoutError,RuntimeError) as error:
                        print("停止应答未确认，固件失联保护仍有效："+str(error),file=sys.stderr)
                        raise
            else:
                status=client.command(mode,*values,flags=flags)
                print(status.parameter_value if mode=="get" else status)
        return 0
    except KeyboardInterrupt: return 130
    except (OSError,TimeoutError,RuntimeError,ValueError,serial.SerialException) as error:
        print(str(error),file=sys.stderr);return 1

if __name__=="__main__": raise SystemExit(main())
