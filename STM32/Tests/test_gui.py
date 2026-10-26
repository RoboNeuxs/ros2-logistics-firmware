import os
from pathlib import Path
import sys
import time
import tempfile
import queue
import threading
from types import SimpleNamespace
import unittest
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'Host'))
import motor_host as host
from motor_demo import DemoPort
from motor_gui import SerialWorker

class SwitchablePort(DemoPort):
    def __init__(self,reply=False):
        super().__init__();self.reply=reply;self.writes=[];self.closed=False;self.fail=False
        self.text_only=False;self.legacy=False
        self.block_command=None;self.write_entered=threading.Event();self.write_release=threading.Event()
    def write(self,frame):
        if self.block_command is not None and (frame==b'PING' if self.block_command=='ping' else
                frame!=b'PING' and frame[2]==host.COMMANDS[self.block_command]):
            self.block_command=None;self.write_entered.set()
            if not self.write_release.wait(3): raise TimeoutError('test write gate timed out')
        if self.fail: raise OSError('simulated serial failure')
        self.writes.append(bytes(frame))
        if self.text_only:
            if frame==b'PING': self.pending.extend(b'PONG\r\n')
            elif frame[2]==host.COMMANDS['query']: self.pending.extend(b'IRQ_READY\r\n')
            return len(frame)
        size=super().write(frame)
        if self.legacy and frame!=b'PING':
            self.pending[-57]=0x31;self.pending[-6:-4]=b'\x01\x03'
            self.pending[-1]=host.crc8(self.pending[-62:-1])
        if not self.reply: self.pending.clear()
        return size
    def read(self,size):
        if not self.pending: time.sleep(.005)
        return super().read(size)
    def close(self): self.closed=True

class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.port=SwitchablePort();self.events=queue.Queue();self.observed=[]
        self.serial_patch=patch.dict(sys.modules,serial=SimpleNamespace(Serial=lambda *a,**k:self.port))
        self.serial_patch.start();self.worker=SerialWorker('FAKE',self.events);self.worker.start()
    def drain(self):
        while not self.events.empty(): self.observed.append(self.events.get_nowait())
    def wait_until(self,predicate,timeout=3):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            self.drain()
            if predicate(): return
            time.sleep(.01)
        self.fail('worker event timed out: '+repr(self.observed))
    def tearDown(self):
        self.worker.quit.set();self.worker.join(timeout=2);self.serial_patch.stop()
        self.assertFalse(self.worker.is_alive());self.assertTrue(self.port.closed)
    def test_silent_port_stays_connected_and_sends_motor_and_stop(self):
        self.wait_until(lambda:('connected','FAKE') in self.observed)
        self.assertIsNone(self.worker.last_reply)
        self.worker.submit('motor',1,'pwm',150)
        self.wait_until(lambda:any(kind=='notice' and '未确认' in value for kind,value in self.observed))
        self.wait_until(lambda:any(f[2]==host.COMMANDS['heartbeat'] for f in self.port.writes))
        self.assertEqual(self.port.targets[0],150)
        self.worker.submit('stop',0,False,urgent=True)
        self.wait_until(lambda:self.port.targets[0]==0)
        self.worker.submit('read')
        self.wait_until(lambda:sum(kind=='notice' and '未确认' in value for kind,value in self.observed)>=3)
        self.assertTrue(self.worker.is_alive());self.assertFalse(self.port.closed)
        self.assertFalse(any(kind=='disconnected' for kind,_ in self.observed))
        self.assertFalse(any(kind=='configuration' for kind,_ in self.observed))
        self.assertFalse(any(kind=='notice' and '已确认' in value for kind,value in self.observed))
    def test_reply_loss_and_recovery_do_not_reopen_port(self):
        self.port.reply=True
        self.wait_until(lambda:any(kind=='configuration' for kind,_ in self.observed))
        self.wait_until(lambda:('reply',True) in self.observed)
        self.port.reply=False;self.observed.clear()
        self.wait_until(lambda:('reply',False) in self.observed)
        self.assertTrue(self.worker.is_alive());self.assertFalse(self.port.closed)
        self.observed.clear();self.port.reply=True
        self.wait_until(lambda:('reply',True) in self.observed)
        self.assertFalse(any(kind=='disconnected' for kind,_ in self.observed))
    def test_actual_serial_error_disconnects(self):
        self.wait_until(lambda:('connected','FAKE') in self.observed)
        self.port.fail=True
        self.wait_until(lambda:any(kind=='disconnected' for kind,_ in self.observed))
        self.assertTrue(any(kind=='error' and 'simulated serial failure' in value for kind,value in self.observed))
    def test_diagnostic_text_and_ping_do_not_claim_v4_status(self):
        self.port.text_only=True;self.worker.trace_wire.set()
        self.wait_until(lambda:any(kind=='link_stats' and '诊断程序' in value['detail'] for kind,value in self.observed))
        self.worker.submit('ping')
        self.wait_until(lambda:any(kind=='notice' and '收到 PONG' in value for kind,value in self.observed))
        self.assertEqual(self.worker.frames,0);self.assertGreater(self.worker.rx_bytes,0)
        self.assertTrue(any(kind=='wire' and value[0]=='RX' for kind,value in self.observed))
        self.assertFalse(any(kind in ('status','disconnected') for kind,_ in self.observed))
    def test_protocol_mismatch_keeps_serial_open_and_reports_version(self):
        self.port.legacy=True;self.port.reply=True
        self.wait_until(lambda:any(kind=='error' and '收到 V3.1' in value for kind,value in self.observed))
        self.assertTrue(self.worker.is_alive());self.assertFalse(self.port.closed)
        self.port.legacy=False
        self.wait_until(lambda:self.worker.frames>0 and self.worker.protocol_problem is None)
        self.assertFalse(any(kind=='disconnected' for kind,_ in self.observed))

class QueuedMotionTests(unittest.TestCase):
    def setUp(self):
        self.port=SwitchablePort(reply=True);self.events=queue.Queue();self.observed=[]
        self.serial_patch=patch.dict(sys.modules,serial=SimpleNamespace(Serial=lambda *a,**k:self.port))
        self.serial_patch.start();self.worker=SerialWorker('FAKE',self.events)
    def tearDown(self):
        self.port.write_release.set();self.worker.quit.set()
        if self.worker.ident is not None: self.worker.join(timeout=2)
        self.serial_patch.stop();self.assertFalse(self.worker.is_alive())
    def wait_until(self,predicate,timeout=3):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            while not self.events.empty(): self.observed.append(self.events.get_nowait())
            if predicate(): return
            time.sleep(.005)
        self.fail('queued motion timed out: '+repr(self.observed))
    def flush(self,blocked_ping=False):
        # A FIFO nonurgent PING acknowledges that all preceding jobs were visited.
        self.wait_until(lambda:True)
        count=sum(kind=='notice' and '收到 PONG' in value for kind,value in self.observed)+int(blocked_ping)+1
        self.worker.submit('ping')
        if blocked_ping: self.port.write_release.set()
        if self.worker.ident is None: self.worker.start()
        self.wait_until(lambda:sum(kind=='notice' and '收到 PONG' in value for kind,value in self.observed)>=count)
        self.assertFalse(any(kind=='error' for kind,_ in self.observed))
    def singles(self):
        return [(f[12],int.from_bytes(f[4:6],'little'),int.from_bytes(f[6:8],'little',signed=True))
                for f in self.port.writes if f!=b'PING' and f[2]==host.COMMANDS['single']]
    def batches(self):
        return [f for f in self.port.writes if f!=b'PING' and f[2]==host.COMMANDS['batch']]
    def queue_old_motion(self):
        self.worker.submit('motor',1,'pwm',100)
        self.worker.submit('motor',0,'speed',200)
        self.worker.submit('batch',['pwm']*4,[300]*4)
    def test_all_stop_discards_old_single_all_and_batch_without_confirmation(self):
        self.queue_old_motion();self.worker.submit('stop',0,False,urgent=True);self.flush()
        self.assertEqual(self.singles(),[]);self.assertEqual(self.batches(),[])
        self.assertEqual(self.port.targets,[0]*4);self.assertEqual(self.worker.motion_requested,[False]*4)
        self.assertIn(('notice','停止指令已确认'),self.observed)
        self.assertFalse(any(kind=='notice' and value in ('单台控制已确认','四台同步指令已确认')
                             for kind,value in self.observed))
    def test_new_single_and_batch_after_all_stop_execute_normally(self):
        self.queue_old_motion();self.worker.submit('stop',urgent=True)
        self.worker.submit('motor',2,'pwm',250)
        self.worker.submit('batch',['speed']*4,[110,220,330,440]);self.flush()
        self.assertEqual(self.singles(),[(2,1,250)]);self.assertEqual(len(self.batches()),1)
        self.assertEqual(self.port.targets,[110,220,330,440])
        self.assertEqual(self.worker.motion_requested,[True]*4)
    def test_single_stop_keeps_unrelated_pending_motion_and_allows_new_motion(self):
        self.queue_old_motion()
        self.worker.submit('motor',2,'pwm',220);self.worker.submit('motor',3,'speed',330)
        self.worker.submit('stop',1,False,urgent=True)
        self.worker.submit('motor',1,'pwm',150);self.flush()
        self.assertEqual(self.singles(),[(1,0,0),(2,1,220),(3,2,330),(1,1,150)])
        self.assertEqual(self.batches(),[]);self.assertEqual(self.port.targets,[150,220,330,0])
        self.assertEqual(self.worker.motion_requested,[True,True,True,False])
    def test_inflight_write_is_not_recalled_and_other_running_motor_is_untouched(self):
        self.worker.submit('motor',2,'pwm',240);self.flush()
        self.port.block_command='single';self.worker.submit('motor',1,'pwm',120)
        self.assertTrue(self.port.write_entered.wait(1))
        self.queue_old_motion();self.worker.submit('motor',3,'pwm',330)
        # submit must remain responsive even while a serial write is blocked.
        submitted=threading.Event()
        def stop():
            self.worker.submit('stop',1,False,urgent=True);submitted.set()
        submitter=threading.Thread(target=stop,daemon=True);submitter.start()
        try:
            self.assertTrue(submitted.wait(1),'stop submission held up by serial I/O')
            self.worker.submit('motor',4,'pwm',440)
        finally:
            self.port.write_release.set();submitter.join(timeout=1)
        self.flush()
        self.assertEqual(self.singles(),[(2,1,240),(1,1,120),(1,0,0),(3,1,330),(4,1,440)])
        self.assertEqual(self.batches(),[]);self.assertEqual(self.port.targets,[0,240,330,440])
        self.assertEqual(self.worker.motion_requested,[False,True,True,True])
    def test_stop_invalidates_dequeued_motion_before_it_is_claimed(self):
        dequeued=threading.Event();release=threading.Event();begin=self.worker._begin_motion
        def paused_begin(sequence,action,args):
            if action=='motor' and args==(1,'pwm',100):
                dequeued.set()
                if not release.wait(3): raise TimeoutError('test claim gate timed out')
            return begin(sequence,action,args)
        with patch.object(self.worker,'_begin_motion',side_effect=paused_begin):
            self.worker.submit('motor',1,'pwm',100);self.worker.start()
            try:
                self.assertTrue(dequeued.wait(1))
                self.worker.submit('stop',1,False,urgent=True)
                self.worker.submit('motor',2,'pwm',220)
            finally: release.set()
            self.flush()
        self.assertEqual(self.singles(),[(1,0,0),(2,1,220)])
        self.assertEqual(self.worker.motion_requested,[False,True,False,False])
    def test_nonurgent_decelerating_stop_is_not_overwritten_by_old_targets(self):
        self.worker.submit('motor',1,'pwm',600);self.flush()
        self.port.block_command='ping';self.worker.submit('ping')
        self.assertTrue(self.port.write_entered.wait(1))
        self.queue_old_motion();self.worker.submit('stop',1,True)
        self.worker.submit('motor',2,'pwm',220)
        self.flush(blocked_ping=True)
        self.assertEqual(self.singles(),[(1,1,600),(1,3,0),(2,1,220)])
        self.assertEqual(self.batches(),[]);self.assertEqual(self.port.targets,[0,220,0,0])
        self.assertIn(self.port.modes[0],(0,3));self.assertFalse(self.worker.motion_requested[0])
    def test_all_decelerating_stop_discards_all_old_motion(self):
        self.queue_old_motion();self.worker.submit('stop',0,True);self.flush()
        self.assertEqual(self.singles(),[(0,3,0)]);self.assertEqual(self.batches(),[])
        self.assertEqual(self.port.targets,[0]*4);self.assertEqual(self.worker.motion_requested,[False]*4)
    def test_motor_stop_and_decel_modes_set_channel_barriers(self):
        for motor,mode in enumerate(('stop','decel',0,3),1):
            self.worker.submit('motor',motor,'pwm',motor*100)
            self.worker.submit('motor',motor,mode,0)
        self.flush()
        self.assertEqual(self.singles(),[(1,0,0),(2,3,0),(3,0,0),(4,3,0)])
        self.assertEqual(self.port.targets,[0]*4);self.assertEqual(self.worker.motion_requested,[False]*4)
    def test_motor_all_stop_mode_discards_old_motion(self):
        self.queue_old_motion();self.worker.submit('motor',0,'stop',0);self.flush()
        self.assertEqual(self.singles(),[(0,0,0)]);self.assertEqual(self.batches(),[])
        self.assertEqual(self.worker.motion_requested,[False]*4)
    def check_configuration_barrier(self,action):
        self.queue_old_motion()
        self.worker.submit(action,*([host.default_configuration()] if action=='apply' else []))
        self.worker.submit('motor',1,'pwm',400);self.flush()
        self.assertEqual(self.singles(),[(1,1,400)]);self.assertEqual(self.batches(),[])
        self.assertEqual(self.port.targets,[400,0,0,0])
        self.assertEqual(sum(kind=='configuration' for kind,_ in self.observed),1)
    def test_read_discards_older_motion(self): self.check_configuration_barrier('read')
    def test_apply_discards_older_motion(self): self.check_configuration_barrier('apply')
    def test_default_discards_older_motion(self): self.check_configuration_barrier('default')

@unittest.skipIf(os.name!='nt' and not os.environ.get('DISPLAY'),'Tk display required')
class GuiTests(unittest.TestCase):
    def setUp(self):
        import tkinter as tk
        from motor_gui import MotorApp
        self.root=tk.Tk();self.root.withdraw();self.app=MotorApp(self.root,demo=True)
        self.port=SwitchablePort(reply=True)
        self.demo_patch=patch('motor_demo.DemoPort',return_value=self.port);self.demo_patch.start()
        self.app.connect()
        self.wait_until(lambda:self.app.last_status is not None)
    def wait_until(self,predicate,timeout=3):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            self.root.update()
            if predicate(): return
            time.sleep(.01)
        self.fail('GUI event timed out')
    def tearDown(self):
        self.port.write_release.set();self.app.worker.quit.set();self.app.worker.join(timeout=2)
        self.demo_patch.stop();self.assertFalse(self.app.worker.is_alive())
        for timer in self.root.tk.call('after','info'): self.root.after_cancel(timer)
        self.app.stop_record();self.root.destroy()
    def test_single_motor_and_stop(self):
        self.app.motor_inputs[0][1].set('300');self.app.run_motor(0)
        self.wait_until(lambda:self.app.last_status.motors[0].target==300)
        self.assertEqual(self.app.last_status.motors[1].mode,0)
        self.app.send('stop',1,False,urgent=True)
        self.wait_until(lambda:self.app.last_status.motors[0].mode==0)
    def test_rapid_run_controls_then_emergency_stop_do_not_restart(self):
        self.port.block_command='ping';self.app.send('ping')
        self.assertTrue(self.port.write_entered.wait(1))
        buttons=self.app.connected_buttons
        run=next(button for button in buttons if button['text']=='应用到此电机')
        batch=next(button for button in buttons if button['text']=='将以上四台设置同步应用')
        stop=next(button for button in buttons if button['text']=='立即停止全部')
        for target in ('100','200','300'):
            self.app.motor_inputs[0][1].set(target);run.invoke()
        for i in range(4): self.app.motor_inputs[i][1].set(str((i+1)*100))
        batch.invoke();stop.invoke()
        self.app.send('ping');self.port.write_release.set()
        self.wait_until(lambda:self.app.log.get('1.0','end').count('收到 PONG')>=2)
        self.assertIn('停止指令已确认',self.app.log.get('1.0','end'))
        self.assertNotIn('单台控制已确认',self.app.log.get('1.0','end'))
        self.assertNotIn('四台同步指令已确认',self.app.log.get('1.0','end'))
        self.assertFalse(any(f!=b'PING' and f[2] in (host.COMMANDS['single'],host.COMMANDS['batch'])
                             for f in self.port.writes))
        self.assertEqual(self.port.targets,[0]*4)
        self.assertEqual(self.app.worker.motion_requested,[False]*4)
        self.assertTrue(all(motor.mode==0 and motor.target==0 for motor in self.app.last_status.motors))
    def test_import_does_not_apply_then_apply_to_ram(self):
        configuration=host.default_configuration();configuration['motors'][0]['cpr']=600
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'params.json';host.save_configuration(path,configuration)
            with patch('motor_gui.filedialog.askopenfilename',return_value=str(path)): self.app.import_config()
        self.assertEqual(self.app.parameters[3][0].get(),'600')
        self.assertEqual(self.app.configuration['motors'][0]['cpr'],1)
        self.app.apply_fields()
        self.wait_until(lambda:self.app.configuration['motors'][0]['cpr']==600)
        self.assertEqual(self.app.last_status.config_state,2)
        self.assertNotIn('report_ms',self.app.global_fields)
        self.assertFalse(any('保存已应用' in str(button['text']) for button in self.app.connected_buttons))
    def test_invalid_fields_and_scrollable_layout(self):
        self.app.parameters[1][0].set('1001')
        with patch('motor_gui.messagebox.showerror') as show:
            self.app.apply_fields();self.assertTrue(show.called)
        self.assertEqual(self.app.configuration['motors'][0]['max_pwm'],1000)
        self.assertEqual(len(self.app.scroll_pages),2)
        for index in (0,1,2):
            self.app.notebook.select(index);self.root.update_idletasks()
        self.app.draw_chart()
    def test_no_reply_keeps_controls_enabled(self):
        self.app.events.put(('reply',False));self.app.poll_events()
        self.assertIn('已连接',self.app.connection.get());self.assertIn('尚无 V4 状态帧',self.app.connection.get())
        self.assertEqual(str(self.app.connect_button['text']),'断开')
        self.assertTrue(all(str(button['state'])=='normal' for button in self.app.connected_buttons))
        self.app.status_changed(self.app.last_status)
        self.assertIn('固件 V4.',self.app.connection.get())
    def test_ping_and_raw_diagnostics_are_visible(self):
        self.app.trace.set(True);self.app.toggle_trace();self.assertTrue(self.app.worker.trace_wire.is_set())
        self.app.send('ping')
        self.wait_until(lambda:'收到 PONG' in self.app.notice.get())
        self.assertIn('有效帧',self.app.link_stats.get())
        self.assertIn('TX [4 B] 50 49 4E 47',self.app.log.get('1.0','end'))

if __name__=='__main__': unittest.main()
