import importlib.util
from pathlib import Path
import struct
import sys
import io
import json
import tempfile
import copy
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/"Host"))
import motor_host as host
from motor_demo import DemoPort

def status_frame(sequence=7,fault=0,error=0,kind=0x93,parameter=0,scope=0,value=0):
    frame=bytearray(64);frame[:4]=b"\xaa\x55"+bytes([kind,sequence])
    struct.pack_into("<BBBBH",frame,4,error,fault,8 if fault&4 else 0,0x41,25)
    for i in range(4):
        struct.pack_into("<Bhhhi",frame,10+11*i,1 if i%2==0 else 2,100,-300+i*100,-120+i*40,-123+i*200)
    struct.pack_into("<HBBHHB",frame,54,value,parameter,scope,0x0400,0x0F3F,2)
    frame[63]=host.crc8(frame[2:63]);return bytes(frame)

class FakePort:
    def __init__(self,*,error=0,fault=0,reply=True):
        self.pending=bytearray();self.error=error;self.fault=fault;self.reply=reply;self.writes=[]
    @property
    def in_waiting(self): return len(self.pending)
    def write(self,frame):
        self.writes.append(frame)
        if self.reply:
            self.pending.extend(status_frame(frame[3],kind=0x94))
            self.pending.extend(status_frame((frame[3]-1)&255))
            self.pending.extend(status_frame(frame[3],self.fault,self.error,parameter=frame[4],scope=frame[12],value=60000))
        return len(frame)
    def read(self,size):
        result=bytes(self.pending[:min(size,3)]);del self.pending[:len(result)];return result
    def reset_input_buffer(self): self.pending.clear()
    def __enter__(self): return self
    def __exit__(self,*_): pass

class HostTests(unittest.TestCase):
    def test_crc_and_signed_commands(self):
        self.assertEqual(host.crc8(b"123456789"),0xF4)
        frame=host.encode_command("pwm",255,-1000,1000,-500,500)
        self.assertEqual(frame[:4],b"\xaa\x55\x17\xff")
        self.assertEqual(struct.unpack("<hhhh",frame[4:12]),(-1000,1000,-500,500))
        frame=host.encode_command("set",0,3,60000,flags=2)
        self.assertEqual(struct.unpack("<HH",frame[4:8]),(3,60000))
    def test_command_validation(self):
        invalid=[("pwm",0,1001),("speed",0,-10001),("stop",0,1)]
        for args in invalid:
            with self.assertRaises(ValueError): host.encode_command(*args)
        for args,flags in [(("single",0,0,10),1),(("get",0,4),0),(("set",0,1,1001),1),
                           (("batch",0,100,0,0,0),0),(("query",0),1)]:
            with self.assertRaises(ValueError): host.encode_command(*args,flags=flags)
    def test_v4_aliases_and_removed_features(self):
        values=(100,-200,300,-400)
        self.assertEqual(host.encode_command('pwm',9,*values),host.encode_command('batch',9,*values,flags=0x55))
        self.assertEqual(host.encode_command('speed',9,*values),host.encode_command('batch',9,*values,flags=0xAA))
        self.assertEqual(host.encode_command('info',9),host.encode_command('query',9))
        with self.assertRaises(ValueError): host.encode_command('save',0)
        with self.assertRaises(ValueError): host.parameter_id(129)
        configuration=host.default_configuration()
        self.assertEqual(configuration['format'],'stc8-motor-config-v4')
        self.assertEqual(set(configuration['global']),{'timeout_ms'})
        configuration['format']='stc8-motor-config-v3'
        with self.assertRaises(ValueError): host.validate_configuration(configuration)
        frame=bytearray(status_frame());struct.pack_into('<H',frame,58,0x0301);frame[63]=host.crc8(frame[2:63])
        with self.assertRaisesRegex(RuntimeError,'主版本'): host.MotorClient(FakePort())._receive(frame)
    def test_split_corrupt_and_concatenated_feedback(self):
        parser=host.StatusParser();damaged=bytearray(status_frame());damaged[12]^=1
        stream=b"noise\xaa"+damaged+status_frame()+status_frame(8,kind=0x94)
        result=[]
        for byte in stream: result.extend(parser.feed(bytes([byte])))
        self.assertEqual([s.sequence for s in result],[7,8])
        self.assertEqual(result[0].motors[0].count,-123)
        self.assertEqual(result[0].motors[3].pwm,0)
        self.assertTrue(result[0].direction_inferred)
        self.assertEqual(result[1].kind,0x94)
        self.assertEqual(parser.crc_errors,1)
    def test_text_ping_is_observable_without_status_or_watchdog_renewal(self):
        class SplitPort(DemoPort):
            def read(self,size): return super().read(min(size,1))
        port=SplitPort();observed=[];client=host.MotorClient(port,on_wire=lambda direction,data:observed.append((direction,data)))
        age=port.age_start
        self.assertTrue(client.ping());self.assertEqual(client.sequence,0);self.assertIsNone(client.last_status)
        self.assertEqual(port.age_start,age)
        self.assertEqual(observed[0],('TX',b'PING'))
        self.assertEqual(b''.join(data for direction,data in observed if direction=='RX'),b'PONG\r\n')
        with self.assertRaises(TimeoutError): host.MotorClient(FakePort(reply=False)).ping(timeout=.001)
        with self.assertRaises(ValueError): client.ping(timeout=0)
    def test_legacy_version_is_rejected(self):
        frame=bytearray(42);frame[:3]=b"\xaa\x55\x91";frame[41]=host.crc8(frame[2:41])
        with self.assertRaisesRegex(RuntimeError,"旧版"): host.StatusParser().feed(frame)
    def test_sequence_matching_reports_and_wrap(self):
        port=FakePort();observed=[];client=host.MotorClient(port,observed.append);client.sequence=255
        self.assertEqual(client.command("query").sequence,255)
        self.assertEqual(client.stop().sequence,0)
        self.assertTrue(any(s.kind==0x94 for s in observed))
        self.assertEqual(port.writes[-1][2],0x13)
        self.assertEqual(client.get_parameter(2,"cpr"),60000)
    def test_rejection_timeout_and_reported_fault(self):
        with self.assertRaises(host.CommandRejected): host.MotorClient(FakePort(error=3)).command("pwm",100,100)
        with self.assertRaises(TimeoutError): host.MotorClient(FakePort(reply=False)).command("query",timeout=.001)
        self.assertEqual(host.MotorClient(FakePort(fault=4)).command("query").fault_channels,8)
        with self.assertRaises(ValueError): host.MotorClient(FakePort()).command("query",timeout=float("nan"))
    def test_send_without_reply_then_receive_later(self):
        port=FakePort(reply=False);client=host.MotorClient(port);client.sequence=255
        self.assertEqual(client.send_command('query'),255)
        self.assertEqual(client.send_command('stop'),0)
        self.assertIsNone(client.last_status)
        port.pending.extend(status_frame(255))
        while port.in_waiting: client.poll()
        self.assertEqual(client.last_status.sequence,255)
        self.assertEqual([f[2] for f in port.writes],[0x14,0x13])
    def test_single_and_mixed_modes(self):
        port=DemoPort();client=host.MotorClient(port)
        client.set_all(["pwm","speed","stop","pwm"],[100,200,0,-300])
        result=client.set_motor(3,"speed",-400)
        self.assertEqual([m.target for m in result.motors],[100,200,-400,-300])
        result=client.stop(2)
        self.assertEqual([m.mode for m in result.motors],[1,0,2,1])
        client.stop();client.clear_faults();self.assertFalse(client.command("query").running)
    def test_configuration_roundtrip_and_no_implicit_save(self):
        port=DemoPort();client=host.MotorClient(port);cfg=client.read_configuration()
        cfg["motors"][1]["cpr"]=60000;cfg["global"]["timeout_ms"]=800
        client.apply_configuration(cfg)
        self.assertEqual(client.read_configuration(),cfg)
        self.assertEqual(client.last_status.config_state,2)
        self.assertFalse(hasattr(port,"saved"))
        with self.assertRaises(ValueError): client.command("save")
        self.assertEqual(host.MotorClient(DemoPort()).get_parameter(2,"cpr"),1)
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/"settings.json";host.save_configuration(path,cfg)
            self.assertEqual(host.load_configuration(path),cfg)
            path.write_text(json.dumps({"format":"invalid"}))
            with self.assertRaises(ValueError): host.load_configuration(path)
    def test_configuration_validation_before_io(self):
        port=FakePort();client=host.MotorClient(port)
        cfg=host.default_configuration();cfg["motors"][3]["max_pwm"]=1001
        with self.assertRaises(ValueError): client.apply_configuration(cfg)
        self.assertEqual(port.writes,[])
        for value in (1,49,2001):
            with self.assertRaises(ValueError): host.validate_parameter("report_ms",value)
        with self.assertRaises(ValueError): host.validate_parameter("kp",True)
    def test_rpm_conversion(self):
        self.assertEqual(host.rpm_to_counts(60,600),600)
        self.assertEqual(host.rpm_to_counts(-30,600),-300)
        self.assertEqual(host.counts_to_rpm(-300,600),-30)
        with self.assertRaises(ValueError): host.rpm_to_counts(float("inf"),600)
        with self.assertRaises(ValueError): host.counts_to_rpm(100,0)
    def test_cli_attempts_stop_after_write_failure(self):
        class FailingMotionPort(FakePort):
            def write(self,frame):
                if frame[2]==0x17: self.writes.append(frame);raise OSError("simulated write failure")
                return super().write(frame)
        port=FailingMotionPort();fake=SimpleNamespace(Serial=lambda *a,**k:port,SerialException=OSError)
        with patch.dict(sys.modules,serial=fake),patch("sys.stdout",io.StringIO()),patch("sys.stderr",io.StringIO()):
            result=host.main(["--port","FAKE","--mode","pwm","--left","300"])
        self.assertEqual(result,1);self.assertEqual([f[2] for f in port.writes],[0x17,0x13])
    def test_parameter_schema_matches_firmware(self):
        import re
        text=(ROOT/"Application/parameters.c").read_text(encoding="utf-8")
        for label,index in (("minimum",2),("maximum",3),("defaults",4)):
            array=re.search(r"\b"+label+r"\[MOTOR_PARAM_COUNT\]\s*=\s*\{([^}]+)\}",text).group(1)
            values=[int(v.strip()) for v in array.split(",")]
            self.assertEqual(values,[p[index] for p in host.PARAMETERS])
    def test_configuration_cancellation_and_schema_guard(self):
        port=FakePort();client=host.MotorClient(port)
        with self.assertRaises(InterruptedError): client.read_configuration(lambda:True)
        with self.assertRaises(InterruptedError): client.apply_configuration(host.default_configuration(),lambda:True)
        self.assertEqual(port.writes,[])
        frame=bytearray(status_frame());frame[7]=0x31;frame[63]=host.crc8(frame[2:63])
        with self.assertRaisesRegex(RuntimeError,"参数格式"): client._receive(frame)

if __name__=="__main__": unittest.main()
