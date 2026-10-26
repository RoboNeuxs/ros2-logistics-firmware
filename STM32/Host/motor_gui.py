"""Tk desktop control panel. Serial I/O belongs to a single background worker."""
from __future__ import annotations
from collections import deque
import csv
from datetime import datetime, timedelta, timezone
import itertools
import math
from pathlib import Path
import queue
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
import motor_host as protocol

MODE_LABELS={'停止':'stop','PWM':'pwm','速度':'speed'}
STATUS_QUERY_INTERVAL=0.5  # Seconds; motion heartbeat has its own interval.

class SerialWorker(threading.Thread):
    def __init__(self,port,events,demo=False):
        super().__init__(daemon=True)
        self.port_name=port;self.events=events;self.demo=demo
        self.jobs=queue.PriorityQueue();self.counter=itertools.count();self.quit=threading.Event()
        self.request_lock=threading.Lock();self.stop_barriers=[-1]*4
        self.timeout_ms=1000
        self.last_reply=None;self.motion_requested=[False]*4
        self.trace_wire=threading.Event();self.tx_bytes=self.rx_bytes=self.frames=0
        self.trace_times={}
        self.recent_rx=bytearray();self.last_chunk=b'';self.protocol_problem=None
    def submit(self,action,*args,urgent=False):
        with self.request_lock:
            sequence=next(self.counter)
            motor=None
            if action=='stop': motor=args[0] if args else 0
            elif action=='motor' and args[1] in ('stop','decel',0,3): motor=args[0]
            elif action in ('read','apply','default'): motor=0
            if motor is not None:
                for i in (range(4) if motor==0 else (motor-1,)):
                    self.stop_barriers[i]=sequence
            self.jobs.put((0 if urgent else 1,sequence,action,args))
    def _begin_motion(self,sequence,action,args):
        # Claimed requests are in flight; keep the stop barrier lock out of serial I/O.
        with self.request_lock:
            if action=='motor':
                motor,mode,target=args
                indices=range(4) if motor==0 else (motor-1,)
                moving=mode in ('pwm','speed',1,2)
                if moving and any(sequence<self.stop_barriers[i] for i in indices): return False
                for i in indices: self.motion_requested[i]=moving and target!=0
            else:
                # Cancel an obsolete batch as a whole to retain atomic four-motor commands.
                if any(sequence<barrier for barrier in self.stop_barriers): return False
                self.motion_requested=[mode in ('pwm','speed',1,2) and target!=0 for mode,target in zip(*args)]
            return True
    def _status(self,status):
        self.last_reply=time.monotonic()
        self.frames+=1;self.protocol_problem=None
        if status.fault: self.motion_requested=[False]*4
        self.events.put(('status',status))
    def _wire(self,direction,data):
        if direction=='TX': self.tx_bytes+=len(data)
        else:
            self.rx_bytes+=len(data);self.last_chunk=data
            self.recent_rx.extend(data);del self.recent_rx[:-256]
        if self.trace_wire.is_set():
            now=time.monotonic()
            # Sample binary traffic; keep text PING/PONG visible immediately.
            if data==b'PING' or b'PONG\r\n' in data or now-self.trace_times.get(direction,float('-inf'))>=1:
                self.trace_times[direction]=now
                self.events.put(('wire',(direction,data)))
    def _protocol_error(self,client,error):
        message=str(error)
        if message!=self.protocol_problem: self.events.put(('error',message+'；串口保持连接。'))
        self.protocol_problem=message;self.motion_requested=[False]*4
        client.parser.buffer.clear()
    def _link_stats(self,client):
        detail=self.protocol_problem or '等待有效 V4 状态'
        if not self.protocol_problem:
            if self.frames and self.last_reply is not None and time.monotonic()-self.last_reply<1: detail='已收到有效 V4 状态'
            elif any(marker in self.recent_rx for marker in (b'IRQ_READY\r\n',b'WAIT_SEND_S\r\n',b'READY\r\n',b'DBG S=')): detail='收到诊断程序文本，未收到 V4 状态'
            elif b'PONG\r\n' in self.recent_rx: detail='PING 收发正常，尚无有效 V4 状态'
            elif self.rx_bytes: detail='已收到原始字节，尚无有效 V4 状态'
            else: detail='接收 0 字节'
        self.events.put(('link_stats',{'tx':self.tx_bytes,'rx':self.rx_bytes,'frames':self.frames,
            'crc':client.parser.crc_errors,'detail':detail,'last_rx':self.last_chunk}))
    def run(self):
        port=None;client=None
        try:
            if self.demo:
                from motor_demo import DemoPort
                port=DemoPort()
            else:
                import serial
                port=serial.Serial(self.port_name,115200,timeout=0.02,write_timeout=0.35)
            port.reset_input_buffer();client=protocol.MotorClient(port,self._status,self._wire)
            self.events.put(('connected','演示模式（不连接硬件）' if self.demo else self.port_name))
            client.send_command('stop');client.send_command('info')
            configuration_pending=True;reply_state=None
            last_query=last_heartbeat=time.monotonic()
            last_stats=0
            while not self.quit.is_set():
                try: _,sequence,action,args=self.jobs.get(timeout=0.01)
                except queue.Empty: action=None;args=()
                if action and (action not in ('motor','batch') or self._begin_motion(sequence,action,args)):
                    try:
                        if action=='motor': client.set_motor(*args)
                        elif action=='batch': client.set_all(*args)
                        elif action=='stop':
                            motor=args[0] if args else 0
                            if motor==0: self.motion_requested=[False]*4
                            else: self.motion_requested[motor-1]=False
                            client.stop(*args)
                        elif action=='clear': client.clear_faults()
                        elif action=='zero': client.zero(*args)
                        elif action=='ping': client.ping(timeout=min(0.35,self.timeout_ms/3000))
                        elif action=='read':
                            self.motion_requested=[False]*4;configuration_pending=False
                            client.stop();configuration=client.read_configuration(self.quit.is_set)
                            self.timeout_ms=configuration['global']['timeout_ms'];self.events.put(('configuration',configuration))
                        elif action=='apply':
                            self.motion_requested=[False]*4;configuration_pending=False
                            client.apply_configuration(args[0],self.quit.is_set);self.timeout_ms=args[0]['global']['timeout_ms']
                            self.events.put(('configuration',args[0]))
                        elif action=='default':
                            self.motion_requested=[False]*4;configuration_pending=False
                            client.stop();client.command('default');self.timeout_ms=1000
                            configuration=client.read_configuration(self.quit.is_set)
                            self.timeout_ms=configuration['global']['timeout_ms']
                            self.events.put(('configuration',configuration))
                        self.events.put(('notice',{'motor':'单台控制已确认','batch':'四台同步指令已确认','stop':'停止指令已确认',
                            'clear':'故障清除已确认','zero':'计数清零已确认','ping':'收到 PONG，PING 收发正常；V4 状态另行确认','read':'已读取设备参数','apply':'参数已应用到 RAM，掉电恢复默认','default':'默认参数已应用到 RAM'}.get(action,action)))
                    except protocol.CommandRejected as error: self.events.put(('error',str(error)))
                    except protocol.ProtocolMismatch as error: self._protocol_error(client,error)
                    except TimeoutError as error:
                        self.events.put(('notice',str(error) if action=='ping' else '本次操作未收到完整应答；串口保持连接，执行结果未确认。'))
                now=time.monotonic()
                status=client.last_status
                running=any(self.motion_requested) or (status and status.running and not status.fault)
                if running and not self.protocol_problem and now-last_heartbeat>=self.timeout_ms/3000:
                    client.send_command('heartbeat')
                    last_heartbeat=time.monotonic()
                try:
                    for status in client.poll():
                        if status.error: self.events.put(('error',str(protocol.CommandRejected(status))))
                except protocol.ProtocolMismatch as error: self._protocol_error(client,error)
                if configuration_pending and self.last_reply is not None and not any(self.motion_requested) and not action:
                    configuration_pending=False
                    try:
                        client.stop();configuration=client.read_configuration(self.quit.is_set)
                        self.timeout_ms=configuration['global']['timeout_ms'];self.events.put(('configuration',configuration))
                    except (TimeoutError,protocol.CommandRejected) as error:
                        self.events.put(('notice','参数未读取完整，串口保持连接：'+str(error)))
                    except protocol.ProtocolMismatch as error: self._protocol_error(client,error)
                now=time.monotonic()
                responding=self.last_reply is not None and now-self.last_reply<1.0
                if responding!=reply_state:
                    self.events.put(('reply',responding));reply_state=responding
                if now-last_stats>=0.2:
                    self._link_stats(client);last_stats=now
                if now-last_query>=STATUS_QUERY_INTERVAL:
                    client.send_command('query');last_query=time.monotonic()
        except Exception as error:
            if not self.quit.is_set(): self.events.put(('error',str(error)))
        finally:
            if client:
                try: client.send_command('stop')
                except Exception as error: self.events.put(('error','断开前停止指令发送失败：'+str(error)))
            if port:
                try: port.close()
                except Exception as error: self.events.put(('error','串口关闭失败：'+str(error)))
            self.events.put(('disconnected',None))

class MotorApp:
    def __init__(self,root,demo=False):
        self.root=root;self.demo=demo;self.events=queue.Queue();self.worker=None
        self.configuration=protocol.default_configuration();self.last_status=None;self.closing=False
        self.connection_name=None
        self.history=[deque(maxlen=1200) for _ in range(4)];self.started=time.monotonic()
        self.csv_file=None;self.csv_writer=None;self.last_fault=None
        self.connected_buttons=[];self.parameters=[]
        root.title('STM32F103C8T6 四电机控制 · V4'+(' · 演示' if demo else ''))
        width=min(1180,root.winfo_screenwidth()-60);height=min(790,root.winfo_screenheight()-100)
        root.geometry(f'{width}x{height}');root.minsize(min(1050,width),min(650,height))
        style=ttk.Style(root);style.theme_use('clam')
        style.configure('TButton',padding=(10,5));style.configure('Stop.TButton',foreground='#b91c1c')
        style.configure('Title.TLabel',font=('Microsoft YaHei UI',13,'bold'))
        style.configure('State.TLabel',font=('Microsoft YaHei UI',10))
        outer=ttk.Frame(root,padding=14);outer.pack(fill='both',expand=True)
        top=ttk.Frame(outer);top.pack(fill='x')
        ttk.Label(top,text='四电机控制',style='Title.TLabel').pack(side='left',padx=(0,22))
        self.port=tk.StringVar();self.port_box=ttk.Combobox(top,textvariable=self.port,width=17)
        self.port_box.pack(side='left');ttk.Button(top,text='刷新串口',command=self.refresh_ports).pack(side='left',padx=5)
        self.connect_button=ttk.Button(top,text='连接',command=self.connect);self.connect_button.pack(side='left',padx=5)
        self.connection=tk.StringVar(value='未连接' if not demo else '演示模式：不连接硬件')
        ttk.Label(top,textvariable=self.connection,style='State.TLabel').pack(side='right')
        toolbar=ttk.Frame(outer);toolbar.pack(fill='x',pady=12)
        self._button(toolbar,'立即停止全部',lambda:self.send('stop',0,False,urgent=True),'Stop.TButton').pack(side='left')
        self._button(toolbar,'全部减速停止',lambda:self.send('stop',0,True)).pack(side='left',padx=6)
        self._button(toolbar,'清除故障',lambda:self.send('clear')).pack(side='left',padx=6)
        self._button(toolbar,'全部计数清零',lambda:self.send('zero',0)).pack(side='left',padx=6)
        self.fault=tk.StringVar(value='状态：等待连接')
        ttk.Label(toolbar,textvariable=self.fault).pack(side='right')
        linkbar=ttk.Frame(outer);linkbar.pack(fill='x',pady=(0,8))
        self._button(linkbar,'PING 检查',lambda:self.send('ping')).pack(side='left')
        self.trace=tk.BooleanVar(value=False)
        ttk.Checkbutton(linkbar,text='串口调试（每秒采样）',variable=self.trace,command=self.toggle_trace).pack(side='left',padx=8)
        self.link_stats=tk.StringVar(value='发送 0 B · 接收 0 B · 有效帧 0 · CRC 错误 0')
        ttk.Label(linkbar,textvariable=self.link_stats,wraplength=850).pack(side='left',padx=8)
        self.notebook=ttk.Notebook(outer);self.notebook.pack(fill='both',expand=True)
        self.scroll_pages={}
        control_shell,control=self._scroll_page()
        params_shell,params=self._scroll_page()
        plots=ttk.Frame(self.notebook,padding=12)
        self.notebook.add(control_shell,text='电机控制');self.notebook.add(params_shell,text='参数配置');self.notebook.add(plots,text='曲线与日志')
        root.bind_all('<MouseWheel>',self.scroll_wheel,add='+')
        self.motor_inputs=[];self.motor_labels=[]
        for i in range(4):
            panel=ttk.LabelFrame(control,text=f'M{i+1} · '+['左前','右前','左后','右后'][i],padding=14)
            panel.grid(row=0,column=i,sticky='nsew',padx=5,pady=5);control.columnconfigure(i,weight=1)
            mode=tk.StringVar(value='PWM');unit=tk.StringVar(value='count/s');target=tk.StringVar(value='0')
            ttk.Label(panel,text='控制模式').pack(anchor='w')
            ttk.Combobox(panel,textvariable=mode,values=list(MODE_LABELS),state='readonly',width=16).pack(fill='x',pady=(3,10))
            ttk.Label(panel,text='目标值（负值反转）').pack(anchor='w')
            ttk.Entry(panel,textvariable=target,width=16).pack(fill='x',pady=4)
            ttk.Combobox(panel,textvariable=unit,values=['count/s','RPM'],state='readonly',width=16).pack(fill='x',pady=(0,10))
            self._button(panel,'应用到此电机',lambda n=i:self.run_motor(n)).pack(fill='x',pady=3)
            row=ttk.Frame(panel);row.pack(fill='x',pady=3)
            self._button(row,'立即停止',lambda n=i:self.send('stop',n+1,False,urgent=True),'Stop.TButton').pack(side='left',expand=True,fill='x')
            self._button(row,'减速停止',lambda n=i:self.send('stop',n+1,True)).pack(side='left',expand=True,fill='x',padx=(4,0))
            self._button(panel,'计数清零',lambda n=i:self.send('zero',n+1)).pack(fill='x',pady=3)
            labels=[]
            for text in ['模式：停止','目标：0','PWM：0 / 1000','速度：0 count/s','RPM：请设置每圈计数','累计计数：0']:
                value=tk.StringVar(value=text);ttk.Label(panel,textvariable=value,wraplength=230).pack(anchor='w',pady=5);labels.append(value)
            self.motor_inputs.append((mode,target,unit));self.motor_labels.append(labels)
        self._button(control,'将以上四台设置同步应用',self.run_all).grid(row=1,column=0,columnspan=4,pady=14)
        ttk.Label(control,text='PWM 目标使用千分比：300 = 30%。速度模式才使用 count/s 或 RPM。\nSTM32F103C8T6 接 A/B 两相：在 A 下降沿读取 B，相位决定反馈方向。',justify='left').grid(row=2,column=0,columnspan=4,sticky='w',padx=5)
        self._parameters_ui(params)
        plotbar=ttk.Frame(plots);plotbar.pack(fill='x')
        self.plot_motor=tk.StringVar(value='M1')
        ttk.Combobox(plotbar,textvariable=self.plot_motor,values=['M1','M2','M3','M4'],state='readonly',width=8).pack(side='left')
        ttk.Label(plotbar,text='最近 30 秒 · 目标转速 / 反馈转速 / PWM').pack(side='left',padx=12)
        self.record_button=ttk.Button(plotbar,text='记录 CSV',command=self.toggle_record);self.record_button.pack(side='right')
        ttk.Button(plotbar,text='导出事件日志',command=self.export_events).pack(side='right',padx=6)
        ttk.Button(plotbar,text='清空日志',command=self.clear_log).pack(side='right')
        self.canvas=tk.Canvas(plots,background='#f8fafc',highlightthickness=0,height=300);self.canvas.pack(fill='both',expand=True,pady=10)
        self.log=tk.Text(plots,height=7,wrap='word',state='disabled',font=('Microsoft YaHei UI',9));self.log.pack(fill='x')
        self.notice=tk.StringVar(value='连接后会先停止全部电机；断开前尝试停止，失联保护持续有效。')
        ttk.Label(outer,textvariable=self.notice,wraplength=1130).pack(fill='x',pady=(10,0))
        self.set_connected(False);self.refresh_ports();self.load_fields(self.configuration)
        root.protocol('WM_DELETE_WINDOW',self.close);root.after(30,self.poll_events);root.after(200,self.draw_chart)
    def _scroll_page(self):
        shell=ttk.Frame(self.notebook)
        canvas=tk.Canvas(shell,highlightthickness=0,background='#dcdad5')
        vertical=ttk.Scrollbar(shell,orient='vertical',command=canvas.yview)
        horizontal=ttk.Scrollbar(shell,orient='horizontal',command=canvas.xview)
        vertical.pack(side='right',fill='y');horizontal.pack(side='bottom',fill='x');canvas.pack(fill='both',expand=True)
        canvas.configure(yscrollcommand=vertical.set,xscrollcommand=horizontal.set)
        content=ttk.Frame(canvas,padding=12);window=canvas.create_window(0,0,window=content,anchor='nw')
        content.bind('<Configure>',lambda event:canvas.configure(scrollregion=canvas.bbox('all')))
        canvas.bind('<Configure>',lambda event:canvas.itemconfigure(window,width=max(event.width,content.winfo_reqwidth())))
        self.scroll_pages[str(shell)]=canvas
        return shell,content
    def scroll_wheel(self,event):
        canvas=self.scroll_pages.get(self.notebook.select())
        if canvas and not isinstance(event.widget,ttk.Combobox): canvas.yview_scroll(-int(event.delta/120),'units')
    def _button(self,parent,text,command,style='TButton'):
        button=ttk.Button(parent,text=text,command=command,style=style);self.connected_buttons.append(button);return button
    def _parameters_ui(self,parent):
        ttk.Label(parent,text='读取、应用或恢复参数时先停止全部电机。参数仅保留在 RAM；用“导出 JSON”保存到电脑，重启后导入并应用。').grid(row=0,column=0,columnspan=6,sticky='w',pady=(0,9))
        ttk.Label(parent,text='参数').grid(row=1,column=0,sticky='w')
        for i in range(4): ttk.Label(parent,text=f'M{i+1}').grid(row=1,column=i+1)
        ttk.Label(parent,text='单位 / 说明').grid(row=1,column=5,sticky='w')
        for row,p in enumerate(protocol.PARAMETERS,2):
            ttk.Label(parent,text=p[1]).grid(row=row,column=0,sticky='w',padx=(0,12),pady=3)
            fields=[]
            for i in range(4):
                value=tk.StringVar();ttk.Entry(parent,textvariable=value,width=10).grid(row=row,column=i+1,padx=10);fields.append(value)
            self.parameters.append(fields)
            ttk.Label(parent,text=p[5]).grid(row=row,column=5,sticky='w',padx=10)
        self.global_fields={}
        for row,p in enumerate(protocol.GLOBALS,len(protocol.PARAMETERS)+2):
            ttk.Label(parent,text=p[1]).grid(row=row,column=0,sticky='w',pady=5)
            value=tk.StringVar();ttk.Entry(parent,textvariable=value,width=10).grid(row=row,column=1,padx=10)
            self.global_fields[p[0]]=value;ttk.Label(parent,text=p[5]).grid(row=row,column=2,columnspan=4,sticky='w',padx=10)
        buttons=ttk.Frame(parent);buttons.grid(row=20,column=0,columnspan=6,sticky='w',pady=12)
        self._button(buttons,'停机并读取',lambda:self.send('read')).pack(side='left',padx=3)
        self._button(buttons,'停机并应用',self.apply_fields).pack(side='left',padx=3)
        self._button(buttons,'恢复默认',lambda:self.send('default')).pack(side='left',padx=3)
        ttk.Button(buttons,text='导入 JSON',command=self.import_config).pack(side='left',padx=3)
        ttk.Button(buttons,text='导出 JSON',command=self.export_config).pack(side='left',padx=3)
        self.saved_state=tk.StringVar(value='参数状态：尚未读取')
        ttk.Label(parent,textvariable=self.saved_state).grid(row=21,column=0,columnspan=6,sticky='w')
        for i in range(1,5): parent.columnconfigure(i,weight=1)
    def set_connected(self,connected):
        for button in self.connected_buttons: button.configure(state='normal' if connected else 'disabled')
    def refresh_ports(self):
        if self.demo: ports=['DEMO']
        else:
            try:
                from serial.tools import list_ports
                ports=[p.device for p in list_ports.comports()]
            except ImportError: ports=[]
        self.port_box.configure(values=ports)
        if ports and self.port.get() not in ports: self.port.set(ports[0])
    def connect(self):
        if self.worker and self.worker.is_alive():
            self.worker.quit.set();self.connect_button.configure(state='disabled');return
        if not self.demo and not self.port.get(): messagebox.showerror('连接','请选择或填写串口');return
        self.worker=SerialWorker(self.port.get(),self.events,self.demo)
        self.toggle_trace()
        self.last_status=None;self.last_fault=None
        self.link_stats.set('发送 0 B · 接收 0 B · 有效帧 0 · CRC 错误 0')
        self.saved_state.set('参数状态：尚未读取');self.fault.set('状态：等待固件回复')
        self.connect_button.configure(state='disabled');self.connection.set('正在打开串口…');self.worker.start()
    def send(self,action,*args,urgent=False):
        if self.worker and self.worker.is_alive(): self.worker.submit(action,*args,urgent=urgent)
    def toggle_trace(self):
        if self.worker:
            if self.trace.get():
                self.worker.trace_times.clear();self.worker.trace_wire.set()
            else: self.worker.trace_wire.clear()
    def collect_motor(self,index):
        mode,target,unit=self.motor_inputs[index];selected=MODE_LABELS[mode.get()]
        value=float(target.get())
        if not math.isfinite(value): raise ValueError('目标必须为有限数')
        if selected=='stop': return selected,0
        parameters=self.configuration['motors'][index]
        if selected=='speed' and unit.get()=='RPM': value=protocol.rpm_to_counts(value,parameters['cpr'])
        elif int(value)!=value: raise ValueError('PWM 和 count/s 目标需要整数')
        value=int(value);limit=parameters['max_pwm'] if selected=='pwm' else parameters['max_speed']
        if abs(value)>limit: raise ValueError(f'M{index+1} 目标超出已应用限制 ±{limit}')
        return selected,value
    def run_motor(self,index):
        try: mode,value=self.collect_motor(index);self.send('motor',index+1,mode,value)
        except ValueError as error: messagebox.showerror('目标设置',str(error))
    def run_all(self):
        try:
            commands=[self.collect_motor(i) for i in range(4)]
            self.send('batch',[v[0] for v in commands],[v[1] for v in commands])
        except ValueError as error: messagebox.showerror('目标设置',str(error))
    def load_fields(self,configuration):
        self.configuration=protocol.validate_configuration(configuration)
        for j,p in enumerate(protocol.PARAMETERS):
            for i in range(4): self.parameters[j][i].set(str(configuration['motors'][i][p[0]]))
        for key,value in configuration['global'].items(): self.global_fields[key].set(str(value))
    def collect_fields(self):
        configuration=protocol.default_configuration()
        for j,p in enumerate(protocol.PARAMETERS):
            for i in range(4): configuration['motors'][i][p[0]]=int(self.parameters[j][i].get())
        for key,value in self.global_fields.items(): configuration['global'][key]=int(value.get())
        return protocol.validate_configuration(configuration)
    def apply_fields(self):
        try: self.send('apply',self.collect_fields())
        except ValueError as error: messagebox.showerror('参数设置',str(error))
    def import_config(self):
        path=filedialog.askopenfilename(filetypes=[('JSON 配置','*.json')])
        if not path: return
        try:
            configuration=protocol.load_configuration(path)
            # Import changes editable fields only; applied configuration stays unchanged.
            applied=self.configuration;self.load_fields(configuration);self.configuration=applied
            self.notice.set('配置已导入编辑区；点击“停机并应用”后才会发送至设备。')
        except (OSError,ValueError) as error: messagebox.showerror('导入配置',str(error))
    def export_config(self):
        try: configuration=self.collect_fields()
        except ValueError as error: messagebox.showerror('导出配置',str(error));return
        path=filedialog.asksaveasfilename(defaultextension='.json',filetypes=[('JSON 配置','*.json')])
        if path:
            try: protocol.save_configuration(path,configuration);self.notice.set('配置文件已导出。')
            except OSError as error: messagebox.showerror('导出配置',str(error))
    def append_log(self,text):
        self.log.configure(state='normal');self.log.insert('end',datetime.now(timezone(timedelta(hours=8))).strftime('%H:%M:%S')+' '+text+'\n')
        if int(self.log.index('end-1c').split('.')[0])>300: self.log.delete('1.0','50.0')
        self.log.see('end');self.log.configure(state='disabled')
    def clear_log(self):
        self.log.configure(state='normal');self.log.delete('1.0','end');self.log.configure(state='disabled')
    def status_changed(self,status):
        self.update_connection(True,status)
        self.last_status=status;self.fault.set(('故障：' if status.fault else '状态：')+status.fault_text+(' · M'+','.join(str(i+1) for i in range(4) if status.fault_channels&(1<<i)) if status.fault_channels else ''))
        self.saved_state.set('设备参数：'+{0:'默认值',2:'RAM 已修改，掉电恢复默认'}.get(status.config_state,'未知'))
        now=time.monotonic()-self.started
        for i,motor in enumerate(status.motors):
            labels=self.motor_labels[i];cpr=self.configuration['motors'][i]['cpr']
            labels[0].set('模式：'+{0:'停止',1:'PWM',2:'速度',3:'减速停止'}.get(motor.mode,'未知'))
            labels[1].set(f'目标：{motor.target}'+(' count/s' if motor.mode==2 else ' ‰'));labels[2].set(f'PWM：{motor.pwm} / 1000')
            labels[3].set(f'速度：{motor.speed} count/s')
            labels[4].set(f'RPM：{protocol.counts_to_rpm(motor.speed,cpr):.1f}' if cpr!=1 else 'RPM：请设置每圈计数')
            labels[5].set(f'累计计数：{motor.count}（'+('方向推定' if status.direction_inferred else 'A/B 方向')+'）')
            self.history[i].append((now,motor.mode,motor.target,motor.pwm,motor.speed))
        if status.fault!=self.last_fault:
            self.append_log(self.fault.get());self.last_fault=status.fault
        if self.csv_writer:
            try:
                timestamp=datetime.now(timezone(timedelta(hours=8))).isoformat(timespec='milliseconds')
                for i,m in enumerate(status.motors,1):
                    cpr=self.configuration['motors'][i-1]['cpr']
                    self.csv_writer.writerow([timestamp,i,m.mode,m.target,m.pwm,m.speed,protocol.counts_to_rpm(m.speed,cpr),m.count,status.fault,status.fault_channels,status.age_ms,status.direction_inferred])
                self.csv_file.flush()
            except OSError as error: self.stop_record();self.append_log('记录失败：'+str(error))
    def update_connection(self,responding,status=None):
        if self.connection_name is None: return
        if responding:
            status=status or self.last_status
            detail=f'固件 V{status.firmware>>8}.{status.firmware&255} · 参数 V{status.parameter_schema}' if status else '固件有回复'
        else: detail='尚无 V4 状态帧'
        self.connection.set(f'{self.connection_name} · 已连接 · {detail}')
    def poll_events(self):
        try:
            while True:
                kind,value=self.events.get_nowait()
                if kind=='connected':
                    self.connection_name=value;self.update_connection(False)
                    self.connect_button.configure(text='断开',state='normal');self.set_connected(True)
                    self.notice.set('串口已打开，可发送指令；等待固件回复。')
                    self.append_log('串口已连接：'+value)
                elif kind=='reply':
                    self.update_connection(value)
                    if not value: self.fault.set('状态：未收到有效 V4 状态帧，反馈数据未确认')
                elif kind=='configuration': self.load_fields(value)
                elif kind=='link_stats':
                    self.link_stats.set(f"发送 {value['tx']} B · 接收 {value['rx']} B · 有效帧 {value['frames']} · CRC 错误 {value['crc']} · {value['detail']}")
                elif kind=='wire':
                    if not self.trace.get(): continue
                    direction,data=value
                    self.append_log(f'{direction} [{len(data)} B] {data[:80].hex(" ").upper()}'+(' …' if len(data)>80 else ''))
                elif kind=='status': self.status_changed(value)
                elif kind in ('notice','error'):
                    self.notice.set(value);self.append_log(('错误：' if kind=='error' else '')+value)
                elif kind=='disconnected':
                    self.connection_name=None
                    self.connection.set('已断开');self.connect_button.configure(text='连接',state='normal');self.set_connected(False)
                    self.append_log('串口已断开')
        except queue.Empty: pass
        if self.closing and (not self.worker or not self.worker.is_alive()): self.stop_record();self.root.destroy();return
        self.root.after(30,self.poll_events)
    def draw_chart(self):
        canvas=self.canvas;canvas.delete('all');w=max(canvas.winfo_width(),200);h=max(canvas.winfo_height(),150)
        i=int(self.plot_motor.get()[1])-1;now=time.monotonic()-self.started
        data=[point for point in self.history[i] if point[0]>=now-30]
        left=62;right=w-20;top=24;bottom=h-26;split=top+(bottom-top)*0.58
        speedmax=max([100]+[abs(p[4]) for p in data]+[abs(p[2]) for p in data if p[1]==2])
        def plot(field,low,high,y0,y1,color,only_speed=False):
            points=[]
            for p in data:
                if only_speed and p[1]!=2:
                    if len(points)>=4: canvas.create_line(*points,fill=color,width=2)
                    points=[];continue
                x=left+(p[0]-(now-30))/30*(right-left)
                y=y1-(p[field]-low)/(high-low)*(y1-y0);points.extend((x,y))
            if len(points)>=4: canvas.create_line(*points,fill=color,width=2)
        for y0,y1,limit,label in [(top,split-22,speedmax,'count/s'),(split+12,bottom,1000,'PWM')]:
            canvas.create_rectangle(left,y0,right,y1,outline='#cbd5e1')
            canvas.create_line(left,(y0+y1)/2,right,(y0+y1)/2,fill='#cbd5e1')
            canvas.create_text(left-7,y0,text=str(limit),anchor='e',fill='#64748b')
            canvas.create_text(left-7,y1,text=str(-limit),anchor='e',fill='#64748b')
            canvas.create_text(left+5,y0+10,text=label,anchor='w',fill='#64748b')
        plot(2,-speedmax,speedmax,top,split-22,'#ea580c',True)
        plot(4,-speedmax,speedmax,top,split-22,'#2563eb')
        plot(3,-1000,1000,split+12,bottom,'#16a34a')
        canvas.create_text(right,12,text='橙：速度目标   蓝：速度反馈   绿：PWM',anchor='e',fill='#475569')
        canvas.create_text(left,bottom+14,text='-30 s',anchor='w',fill='#64748b');canvas.create_text(right,bottom+14,text='现在',anchor='e',fill='#64748b')
        self.root.after(200,self.draw_chart)
    def toggle_record(self):
        if self.csv_file: self.stop_record();return
        path=filedialog.asksaveasfilename(defaultextension='.csv',filetypes=[('运行日志','*.csv')])
        if not path: return
        try:
            self.csv_file=Path(path).open('w',encoding='utf-8-sig',newline='');self.csv_writer=csv.writer(self.csv_file)
            self.csv_writer.writerow(['time_UTC+8','motor','mode','target','pwm','counts_per_s','rpm','count','fault','fault_channels','command_age_ms','direction_inferred'])
            self.record_button.configure(text='停止记录');self.append_log('开始记录：'+path)
        except OSError as error: messagebox.showerror('记录日志',str(error))
    def export_events(self):
        path=filedialog.asksaveasfilename(defaultextension='.txt',filetypes=[('事件日志','*.txt')])
        if path:
            try:
                Path(path).write_text(self.log.get('1.0','end-1c')+'\n',encoding='utf-8')
                self.notice.set('当前事件日志已导出。')
            except OSError as error: messagebox.showerror('导出日志',str(error))
    def stop_record(self):
        if self.csv_file:
            try: self.csv_file.close()
            except OSError as error: self.append_log('日志关闭失败：'+str(error))
        self.csv_file=self.csv_writer=None;self.record_button.configure(text='记录 CSV')
    def close(self):
        self.closing=True;self.notice.set('正在停止电机并断开…');self.set_connected(False)
        if self.worker: self.worker.quit.set()

def main():
    import argparse
    parser=argparse.ArgumentParser(description='STM32F103C8T6 四电机图形上位机')
    parser.add_argument('--demo',action='store_true',help='离线演示，不连接硬件')
    args=parser.parse_args();root=tk.Tk();MotorApp(root,args.demo);root.mainloop()

if __name__=='__main__': main()
