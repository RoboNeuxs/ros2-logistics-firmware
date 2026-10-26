"""Run under WSL with MOTOR_FIRMWARE_LIB pointing to the compiled C harness."""
import ctypes
import os
from pathlib import Path
import sys
import unittest
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'Host'))
import motor_host as host

@unittest.skipUnless(os.environ.get('MOTOR_FIRMWARE_LIB'),'requires WSL firmware shared library')
class IntegrationTests(unittest.TestCase):
    def setUp(self):
        self.lib=ctypes.CDLL(os.environ['MOTOR_FIRMWARE_LIB'])
        self.lib.bridge_reset()
        self.lib.bridge_receive.argtypes=(ctypes.POINTER(ctypes.c_uint8),ctypes.POINTER(ctypes.c_uint8))
        self.lib.bridge_receive.restype=ctypes.c_int
        lib=self.lib
        class Port:
            def __init__(self): self.pending=bytearray()
            @property
            def in_waiting(self): return len(self.pending)
            def write(self,data):
                source=(ctypes.c_uint8*14).from_buffer_copy(data);target=(ctypes.c_uint8*64)()
                count=lib.bridge_receive(source,target);self.pending.extend(bytes(target[:count]));return len(data)
            def read(self,n):
                data=bytes(self.pending[:n]);del self.pending[:n];return data
        self.client=host.MotorClient(Port())
    def test_real_firmware_parameters_and_single_controls(self):
        c=self.client
        self.assertEqual(c.get_parameter(0,'timeout_ms'),1000)
        c.set_parameter(2,'cpr',60000);self.assertEqual(c.get_parameter(2,'cpr'),60000)
        c.set_parameter(0,'stall_pwm',0)
        c.set_all(['pwm','speed','pwm','stop'],[300,400,-200,0])
        self.lib.bridge_advance(100)
        status=c.set_motor(4,'pwm',500)
        self.assertEqual([m.target for m in status.motors],[300,400,-200,500])
        self.assertEqual([m.mode for m in status.motors],[1,2,1,1])
        with self.assertRaises(host.CommandRejected): c.set_parameter(1,'kp',8)
        c.stop();self.assertEqual(c.last_status.config_state,2)
        self.lib.bridge_reset();self.assertEqual(c.get_parameter(2,'cpr'),1)
    def test_real_firmware_watchdog_and_fault_recovery(self):
        c=self.client;c.set_parameter(0,'stall_pwm',0);c.set_parameter(0,'timeout_ms',200)
        c.set_motor(1,'pwm',100);self.lib.bridge_advance(200)
        status=c.command('query');self.assertTrue(status.fault&1);self.assertFalse(status.running)
        with self.assertRaises(host.CommandRejected): c.set_motor(1,'pwm',100)
        c.stop();c.clear_faults();c.set_motor(1,'pwm',100)
        self.lib.bridge_advance(100);c.heartbeat();self.lib.bridge_advance(100)
        self.assertEqual(c.command('query').fault,0)
    def test_full_configuration_roundtrip(self):
        cfg=host.default_configuration();cfg['motors'][0]['polarity']=1;cfg['motors'][3]['kp']=16
        cfg['global']['timeout_ms']=800
        self.client.apply_configuration(cfg)
        self.assertEqual(self.client.read_configuration(),cfg)

if __name__=='__main__': unittest.main()
