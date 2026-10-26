# STM32F103C8T6 四路编码电机控制 · V4.3

本工程位于 `ros2-logistics-firmware` 仓库的 `STM32/`。下文命令均在 `STM32/` 目录中执行；从仓库根目录进入时先运行 `cd STM32`。仓库入口见 [README](../README.md)，本次交付结果见 [导入与验证记录](../docs/01-STM32固件导入与验证记录.md)。

这是从现有 STC15 V4.2 移植的独立工程，适用于 STM32F103C8T6、8 MHz 外部晶振、两片 TB6612FNG 和四台编码电机。工程已包含 CMSIS、芯片头文件、启动文件、链接文件、Keil 工程和配套 Python 上位机。引脚按提供的主控图配置，IN2 由已确认的硬件反相电路生成。

## 打开和编译

### 在 VS Code 中编程

使用“文件 → 打开文件夹”打开整个 `STM32/` 目录，资源管理器会显示全部源码，Keil Assistant 会自动找到根目录中的 `STM32F103C8T6.uvprojx`。不要只打开 `MDK-ARM/` 文件夹。

原有 [MDK-ARM/STM32.code-workspace](MDK-ARM/STM32.code-workspace) 仍可浏览整个工程并运行构建任务；使用该工作区时，Keil Assistant 需要通过“Open keil uVision project”手动打开根目录中的 `.uvprojx`，因为扩展只会在工作区文件所在目录自动查找工程。

| 目录 | 用途 / 常改文件 |
|---|---|
| `Application` | 应用源码和头文件：`main.c` 主循环、`control_logic.c` 速度控制、`protocol.c` 通信协议、`parameters.c` 参数 |
| `Board` | 板级源码和头文件：`board_config.h` 引脚配置、`motor_hal.c` 硬件实现、`system_stm32f1xx.c` 系统时钟 |
| `Host` | Python 上位机和串口工具 |
| `docs` | 协议说明 |
| `Tests` / `Scripts` | 测试、构建和固件校验脚本 |
| `Drivers` / `Startup` | CMSIS、芯片支持和启动代码，通常不需要修改 |
| `MDK-ARM` | 链接配置和原有 VS Code 工作区入口；Keil 工程文件位于 `STM32/` 根目录 |
| `Build` | HEX、BIN、AXF、MAP 和构建日志 |

已配置 C/C++ 头文件搜索路径。按 **Ctrl+Shift+B** 或选择“终端 → 运行任务 → STM32: 编译生成 HEX / BIN”可调用 Keil ARM Compiler 6 全量构建并自动校验固件；也可双击根目录的 [Build_HEX.cmd](Build_HEX.cmd)，结束后窗口会显示 HEX/BIN 完整路径，并等待按键关闭。Keil 默认安装在 `D:\Application\Keil5`，其他位置请使用下方带 `-KeilRoot` 的命令。脚本直接构建同一份 `.uvprojx` 工程，再生成 BIN；需要 Python 执行固件校验。

每次构建前，脚本会将已有 HEX/BIN 复制到 `Build/History/<UTC时间戳>/`，同时保存 `SHA256SUMS.txt`，保留旧固件版本。最新烧录文件仍位于 `Build/STM32F103C8T6.hex`；编译或校验失败时不要将已有输出当成本次成功结果。

使用 Keil Assistant 的编译按钮时，在 VS Code **用户设置**中将 `KeilAssistant.MDK.Uv4Path` 设置为 `D:\Application\Keil5\UV4\UV4.exe`（按实际安装位置调整）；`KeilAssistant.C51.Uv4Path` 只用于 C51，不能代替 MDK 设置。若命令以 `null -b` 开头，说明 MDK 路径未配置。本工程的自动化终端使用 `-NoProfile`，避免 PowerShell 启动配置文件被执行策略拦截。修改配置后关闭旧任务终端，再运行构建。

资源管理器隐藏了缓存、扩展日志和编译中间文件，文件仍保存在磁盘上；显示规则见 `.vscode/settings.json`。HEX/BIN 等常用输出仍可直接查看。

实际源码目录与 Keil 的 `Startup`、`Board`、`Application` 分组一致，展开目录即可直接编辑 `.c` / `.h` 文件。

### 在 Keil 中编译

Keil µVision5 打开 [STM32F103C8T6.uvprojx](STM32F103C8T6.uvprojx)，选择 `STM32F103C8T6_4Motor_AB`，按 F7 构建。编译器选择 **Arm Compiler 6.22**。工程使用 ARMCLANG，不依赖 Compiler 5；警告仍按错误处理。

已配置 Cortex-M3、64 KB Flash（0x08000000 起）、20 KB RAM（0x20000000 起）、2 KB 栈、MicroLIB、C99 和 HEX 输出。工程内包含头文件、Flash 算法及 SVD；如另一台 Keil 提示缺少设备包，在 Pack Installer 安装 `Keil.STM32F1xx_DFP 2.4.1`，无需添加库源码。不要再加入另一份启动文件或 SystemInit。

也可在 PowerShell 构建并生成 AXF、HEX、BIN：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/build_keil.ps1
# 强制全量重建（与双击入口、Ctrl+Shift+B 相同）：
powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/build_keil.ps1 -Rebuild
# Keil 安装在其他位置时：
powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/build_keil.ps1 -KeilRoot D:\Keil_v5
```

构建后烧录文件是 `Build/STM32F103C8T6.hex`，BIN 为 `Build/STM32F103C8T6.bin`。这些文件由本地构建生成，GitHub 源码下载包中不包含。BIN 烧录地址必须填 `0x08000000`。Keil F7 默认更新 HEX；需要重新生成 BIN 时运行构建脚本。

## 引脚和外设

以下脚号均为 STM32F103C8T6 的 LQFP48 脚号。

| 电机 | PWM | IN1 / 方向 | 编码器 A 相（图中 +） | 编码器 B 相（图中 -） |
|---|---|---|---|---|
| A / M1 | PA8 / 29 / TIM1_CH1 | PB0 / 18 / AN1 | PA0 / 10 / A+ | PA1 / 11 / A- |
| B / M2 | PA9 / 30 / TIM1_CH2 | PB1 / 19 / BN1 | PA6 / 16 / B+ | PA7 / 17 / B- |
| C / M3 | PA10 / 31 / TIM1_CH3 | PB10 / 21 / CN1 | PB6 / 42 / C+ | PB7 / 43 / C- |
| D / M4 | PA11 / 32 / TIM1_CH4 | PB11 / 22 / DN1 | PB12 / 25 / D+ | PB13 / 26 / D- |

UART 使用 **USART2**：PA2（12）为主控 TX，接上位机 RX；PA3（13）为主控 RX，接上位机 TX。串口参数 **115200、8N1**，电平为 3.3V TTL，双方共地。PA9/PA10 已用于 PWM，不能当 USART1 使用。PA11 用于 PWM，不能同时启用 USB。

时钟为 HSE 8 MHz × PLL9 = **72 MHz**；AHB/APB2 为 72 MHz，APB1 为 36 MHz，APB1 定时器为 72 MHz。晶振未启动时主控保持 PWM 低电平，不用错误时钟继续通信。SWD 保留在 PA13/PA14；JTAG 关闭。

四路 TIM1 PWM 都是 **20 kHz**，PSC=0、ARR=3599。千分比指令范围 ±1000，比较值范围 0～3600，支持真 0% 和 100%。原 STC15 的 D 路软件 PWM 已替换为硬件 PWM。

A/B/C 路分别使用 TIM2/TIM3/TIM4 的 CH1 输入捕获，在 A 下降沿中断中采样 B；D 路使用 PB12/EXTI12，同样在下降沿中断中采样 PB13。采用原程序的 **x1 编码器单位**，CPR 应填写输出轴每圈 A 下降沿数。B、C 路分别使用 PA6 和 PB6，避免了二者竞争 EXTI6。默认 A 下降沿且 B 高为正；如果某路编码器方向相反，调整 `Board/board_config.h` 的 `BOARD_ENCODER_INVERT_MASK`。电机安装方向用上位机 `polarity` 参数配置，它同时调整输出与反馈方向。

输入捕获用于记录边沿事件，计数和 B 相方向判定仍由中断完成，并非硬件正交计数。A/B/C 路检测到过捕获时会锁存故障并关断 PWM；D 路 EXTI 没有等价的硬件滤波和过捕获指示，连续边沿可能合并。必须按实际编码器频率验证四路最高可靠计数率和方向判定。

## 驱动接法与停机

每路 IN1 为上述方向引脚，IN2 必须由现有电路反相：IN1=1 时 IN2=0，IN1=0 时 IN2=1。软件不生成 AN2/BN2/CN2/DN2。

补充电路图中两片 TB6612 的 STBY 都常接 5V，因此程序**不把 PB2 当 STBY 输出**。PB2（BOOT1，图中 STRS）保持输入，原外部 10 kΩ 下拉保留。停机、超时、主循环调度超时、接收溢出、UART 接收错误、堵转、输入捕获丢失和处理器异常都直接清 TIM1 的 MOE；OSSI 和输出空闲电平配置保证四路 PWM 低电平。故障后恢复输出必须先清故障，再重新发运动命令。PWM=0 对 TB6612 是短路制动；由于 STBY 固定高，软件不能提供 STBY=0 的高阻滑行模式。

应用安全初始化后启动独立看门狗 IWDG，LSI /64、重装载值 624，在名义 40 kHz LSI 下约 1 秒超时；实际复位时间随 LSI 偏差变化，不作为精确停机计时。仅主循环完成一次控制任务后喂狗，串口和 SysTick 中断不喂狗；健康的故障停机循环仍喂狗，保留故障锁存。若中断被永久屏蔽且 CPU 卡死，IWDG 仍可复位；复位后目标清零、输出禁用、参数恢复默认，不自动恢复旧目标。IWDG 配置更新超时会锁存调度故障并保持中断/PWM 关闭，等待看门狗复位。调试器暂停运行时也可能触发复位，程序未自动冻结 IWDG。

**电平需要与硬件一起确认。** 图中 TB6612 VCC=5V，数据手册 VIH 最小值为 0.7×VCC=3.5V，STM32 的 3.3V 输出不满足保证值。可为 IN1/IN2/PWM 做匹配的电平转换，或将两片 TB6612 的逻辑 VCC 和其 STBY 高电平一起改为 3.3V，并确认反相电路的电平也匹配；VM/VIN 仍按电机电源设计。编码器进入 STM32 的信号应匹配 3.3V，尤其 PA0/PA1/PA6/PA7 不是 5V 容忍脚；需要时使用电平转换。编码器电源按电机规格确定，不要直接从图中的 VIN 推定信号电压。

依据：[ST 芯片数据手册](https://www.st.com/resource/en/datasheet/stm32f103t8.pdf)、[ST RM0008](https://www.st.com/resource/en/reference_manual/rm0008-stm32f101xx-stm32f102xx-stm32f103xx-stm32f105xx-and-stm32f107xx-advanced-armbased-32bit-mcus-stmicroelectronics.pdf)、[东芝 TB6612FNG 数据手册](https://toshiba.semicon-storage.com/info/TB6612FNG_datasheet_en_20141001.pdf?did=10660&prodName=TB6612FNG)。

## 烧录和使用

ST-Link 的 SWDIO 接 PA13、SWCLK 接 PA14、GND 共地，参考电压接板上的 3.3V，NRST 可接复位脚。图中 PA13/PA14 未引出，需要从芯片或测试点连接。工程默认使用 ST-Link；首次使用在 Debug → Settings 确认 SWD 和实际探针，并在 Utilities 确认 STM32F10x Flash 算法。当前只完成构建，未连接探针下载。

正常运行 BOOT0=0、PB2/BOOT1=0。需要 ROM 串口下载时可使用 USART1（PA9/PA10）并临时断开这些 PWM 与驱动的连接；应用运行通信仍使用 USART2。图中的 PA2/PA3 不是 STM32F103C8 的 ROM USART1 下载引脚。DTR 经 Q5 控制 NRST，只负责复位。

```powershell
python -m pip install -r Host/requirements.txt
python Host/motor_gui.py
# 离线界面演示：
python Host/motor_gui.py --demo
```

文本 `PING` 回复 `PONG`，只测试通信，不续期运动看门狗。二进制协议为 V4，固件版本 `0x0403`，参数格式仍为 4；原上位机也可继续使用。保留独立/混合 PWM 与速度模式、PI、加减速、换向等待、默认 1000 ms 失联保护及堵转保护。控制周期 50 ms，调度超过 100 ms 触发保护。参数存在 RAM，掉电恢复默认，可在上位机保存/导入 JSON。完整帧格式见 [docs/protocol.md](docs/protocol.md)。

首次调试将车轮悬空，从单路低 PWM 开始，逐路确认正反转和反馈符号，再填写 CPR 和调整 PI。CPR 默认值 1 是占位值，不代表你的电机参数。不要带负载用默认参数直接调闭环。

上位机提交立即/减速停止时，会取消此前尚未执行且涉及所选电机的运动请求；旧的四台混合请求整条取消，不拆分，也不改写其他电机已经生效的目标。读取/应用/恢复参数会先停止全部，并取消旧运动请求。停止后明确提交的新运动请求仍可运行；已经进入串口发送的请求不能撤回，停止将在当前操作完成或超时后执行。界面停止不是独立硬件急停，不能替代硬件关断。

## 已完成的验证

2026-10-26（仓库导入复核）：在当前仓库使用 Arm Compiler 6.22 全量重建，0 错误、0 警告；HEX/BIN 一致性、向量表及 C8 内存边界校验通过，BIN 为 12,120 字节。Windows Python 3.13.9 上位机测试 16 项、工作线程/队列/Tk 界面测试 23 项通过。本次未连接硬件，Linux/WSL 的 C 模拟和集成测试未重新执行。详细命令与镜像哈希见 [导入与验证记录](../docs/01-STM32固件导入与验证记录.md)。以下为导入前已有的历史验证记录。

2026-10-14（历史构建）：Keil 工程实际构建 0 错误、0 警告；CODE=5362 B、RO=370 B、RW=104 B、ZI=2456 B，ROM=5836 B，RAM=2560 B（包含 2 KB 栈）。

2026-10-16（本轮源码回归）：WSL 的 C 控制逻辑、STM32 HAL 寄存器模拟测试通过，Python/C 协议集成 3 项、上位机 16 项通过；Windows 默认编码下上位机 16 项、工作线程/队列/Tk 界面共 23 项全部通过。实际启动离线演示窗口，操作单台/混合控制、连续运行后停止、停止后重新运行、减速停止与参数应用，已检查窗口截图和关闭流程。未连接真实串口或电机。

捕获测试增加了 SR 写零清标志和不同阶段新边沿/过捕获注入，IWDG 测试覆盖配置更新、喂狗路径及独立于 SysTick 的超时模型。这些测试仍不模拟完整芯片电气行为或精确中断时序，上板后需测量 PWM、关断/复位电平、IWDG 实际复位时间、编码器最大计数率和电平匹配。

2026-10-16（编译器配置修复）：已使用 Arm Compiler 6.22 全量重建，0 错误、0 警告；CODE=11752 B、RO=360 B、RW=4 B、ZI=2572 B。Build 中的 AXF/HEX/BIN 已更新，HEX/BIN 校验通过。本次未执行硬件烧录或电机实测。

Linux/WSL 中运行 C 和协议测试：`sh Scripts/run_tests.sh`。Windows 完整上位机测试：`python -B -m unittest discover -s Tests -p test_host.py -v`；工作线程与界面测试：`python -B -m unittest discover -s Tests -p test_gui.py -v`。重新构建后的烧录文件校验：`python Scripts/verify_image.py`。
