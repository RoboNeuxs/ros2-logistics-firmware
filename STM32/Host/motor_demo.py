"""Offline GUI demonstration. This model is not a hardware verification."""
import struct
import time
from motor_host import COMMANDS, PARAMETERS, GLOBALS, default_configuration, crc8

class DemoPort:
    def __init__(self):
        self.pending=bytearray();self.configuration=default_configuration()
        self.modes=[0]*4;self.targets=[0]*4;self.pwm=[0.0]*4;self.speeds=[0.0]*4;self.counts=[0.0]*4
        self.last=time.monotonic();self.age_start=self.last;self.fault=0;self.state=0
    @property
    def in_waiting(self): return len(self.pending)
    def reset_input_buffer(self): self.pending.clear()
    def close(self): pass
    def read(self,size):
        data=bytes(self.pending[:size]);del self.pending[:size];return data
    def _advance(self):
        now=time.monotonic();dt=min(now-self.last,0.5);self.last=now
        if any(self.modes) and (now-self.age_start)*1000>=self.configuration['global']['timeout_ms']:
            self.fault=1;self.modes=[0]*4;self.targets=[0]*4
        for i,mode in enumerate(self.modes):
            desired=self.targets[i] if mode==1 else max(-1000,min(1000,self.targets[i]*2)) if mode==2 else 0
            alpha=min(1,dt*8)
            self.pwm[i]+=(desired-self.pwm[i])*alpha
            if mode==0: self.pwm[i]=0
            self.speeds[i]+=(self.pwm[i]/2-self.speeds[i])*alpha
            self.counts[i]+=self.speeds[i]*dt
            if mode==3 and abs(self.pwm[i])<1: self.modes[i]=0;self.pwm[i]=0
    def write(self,frame):
        self._advance()
        if frame==b'PING': self.pending.extend(b'PONG\r\n');return len(frame)
        kind,sequence,*payload=struct.unpack('<BBhhhhB',frame[2:13])
        values=payload[:4];scope=payload[4];error=0;parameter_value=0
        command=next(name for name,value in COMMANDS.items() if value==kind and name not in ('pwm','speed','info'))
        if command in ('single','batch'):
            if self.fault: error=3
            else:
                for i in range(4):
                    if command=='single' and scope and scope!=i+1: continue
                    mode=values[0] if command=='single' else (scope>>(2*i))&3
                    self.modes[i]=mode;self.targets[i]=values[1] if command=='single' else values[i]
                self.age_start=time.monotonic()
        elif command=='stop': self.modes=[0]*4;self.targets=[0]*4;self.pwm=[0.0]*4
        elif command=='heartbeat': self.age_start=time.monotonic()
        elif command=='clear':
            if any(self.modes): error=3
            else: self.fault=0
        elif command=='zero':
            for i in range(4):
                if scope and scope!=i+1: continue
                if self.modes[i]: error=3
            if not error:
                for i in range(4):
                    if not scope or scope==i+1: self.counts[i]=0
        elif command in ('get','set'):
            key=values[0]
            if command=='set' and any(self.modes): error=3
            else:
                name=GLOBALS[key-128][0] if key>=128 else PARAMETERS[key][0]
                if command=='set':
                    value=values[1]&65535
                    if key>=128: self.configuration['global'][name]=value
                    else:
                        for i in range(4):
                            if not scope or scope==i+1: self.configuration['motors'][i][name]=value
                    self.state=2;parameter_value=value
                else:
                    parameter_value=self.configuration['global'][name] if key>=128 else self.configuration['motors'][scope-1][name]
        elif command=='default':
            if any(self.modes): error=3
            else: self.configuration=default_configuration();self.state=0
        result=bytearray(64);result[:4]=b'\xaa\x55\x93'+bytes([sequence])
        struct.pack_into('<BBBBH',result,4,error,self.fault,0,0x40,min(65535,int((time.monotonic()-self.age_start)*1000)))
        for i in range(4):
            struct.pack_into('<Bhhhi',result,10+11*i,self.modes[i],self.targets[i],round(self.pwm[i]),round(self.speeds[i]),round(self.counts[i]))
        struct.pack_into('<HBBHHB',result,54,parameter_value,values[0]&255,scope,0x0403,0x073F,self.state)
        result[63]=crc8(result[2:63]);self.pending.extend(result)
        return len(frame)
