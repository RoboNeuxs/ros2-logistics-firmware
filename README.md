# 实时运动控制固件

ROS 2 多模态 AI 智能搬运机器人 · `ros2-logistics-firmware`。

## 职责

MCU 外设、编码器、四轮闭环和停车保护。

## 任务模块

- **M02 实时驱动与闭环控制**

## STM32 工程与操作手册

当前工程为 **STM32F103C8T6 四路编码电机控制 V4.3**，包含固件源码、Keil 工程、Python 串口上位机和测试脚本。

| 入口 | 内容 |
|---|---|
| [STM32 操作手册](STM32/README.md) | 开发环境、引脚接线、编译、烧录、上位机使用与调试 |
| [Keil 工程](STM32/STM32F103C8T6.uvprojx) | 使用 Arm Compiler 6.22 构建 |
| [一键构建](STM32/Build_HEX.cmd) | 生成并校验 HEX/BIN，保存旧固件 |
| [V4 串口协议](STM32/docs/protocol.md) | 帧格式、命令、参数与状态反馈 |
| [ESP32 控制 STM32 串口命令](docs/02-ESP32控制STM32串口命令.md) | 命令码、参数、模式及完整 HEX 指令速查 |
| [导入与验证记录](docs/01-STM32固件导入与验证记录.md) | 本次交付范围、复现命令、实测结果和待验收项 |

首次使用请从操作手册开始。VS Code 打开 `STM32/` 文件夹；生成的烧录文件在本地 `STM32/Build/`，按已有忽略规则不纳入 Git。

## 输入与交付

输入：对应 Issue、已确认硬件资料、公共接口基线和上游模块的受控版本。

交付：本模块工程、配置、验证记录，以及每个步骤的完成说明或使用教程。具体指标、负责人和排期在对应 Issue 中确认。

## 个人 Fork 开发与提交

普通开发成员对组织模块仓库拥有 Read 权限。先 Fork 本仓库到个人账号，在个人 Fork 的 `feature/*` 分支编写程序并反复 commit / push；完成后向本仓库的 `main` 发起 PR，由项目总负责人最终审核并合并。

完整步骤、命令和已有工程迁移方法见 **[个人 Fork 开发与 PR 提交流程](COLLABORATION.md)**。

## 开始工作

1. 阅读 [开发入口](https://github.com/RoboNeuxs/ros2-logistics-hub) ，认领对应任务并确认交付与验收要求。
2. 在 Issues 中确认模块工作项与验收条件，再拆分具体任务。
3. 在个人 Fork 开发，成果通过跨仓库 PR 提交到组织本仓库，附教程和验证记录，等待项目总负责人审核合并。
4. 每完成一步，在 `docs/` 添加完成说明或教程，更新 [文档索引](docs/README.md)。

## 文档

- [仓库初始化说明与使用方法](docs/00-仓库初始化说明.md)
- [步骤完成说明与使用教程模板](docs/templates/步骤完成说明与使用教程.md)
- [开发提交规范](https://github.com/RoboNeuxs/ros2-logistics-hub/blob/main/CONTRIBUTING.md)
- [公共接口仓库](https://github.com/RoboNeuxs/ros2-logistics-interfaces)

## 当前状态

已导入 STM32 V4.3 工程和操作手册。2026-10-26 全量编译与 HEX/BIN 校验通过，上位机及界面自动测试共 39 项通过；尚未完成本次硬件烧录、电机实测或整车联调，技术任务按 Issue 验收。

[整车工程如何集成](https://github.com/RoboNeuxs/ros2-logistics-hub/blob/main/docs/01-整车工程集成方式.md)
