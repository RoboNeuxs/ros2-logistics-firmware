# 实时运动控制固件

ROS 2 多模态智能搬运机器人 · `ros2-logistics-firmware`。

## 职责

MCU 外设、编码器、四轮闭环和停车保护。

## 任务模块

- **M02 实时驱动与闭环控制**

## 输入与交付

输入：项目任务书、已确认硬件资料、公共接口基线和上游模块的受控版本。

交付：本模块工程、配置、验证记录，以及每个步骤的完成说明或使用教程。具体指标、负责人和排期在对应 Issue 中确认。

## 开始工作

1. 阅读 [项目中心](https://github.com/RoboNeuxs/ros2-logistics-hub) 的 V1.2 方案和任务分工。
2. 在 Issues 中确认模块工作项与验收条件，再拆分具体任务。
3. 开发成果通过 PR 提交，附教程和验证记录。
4. 每完成一步，在 `docs/` 添加完成说明或教程，更新 [文档索引](docs/README.md)。

## 文档

- [仓库初始化说明与使用方法](docs/00-仓库初始化说明.md)
- [步骤完成说明与使用教程模板](docs/templates/步骤完成说明与使用教程.md)
- [项目协作规范](https://github.com/RoboNeuxs/ros2-logistics-hub/blob/main/CONTRIBUTING.md)
- [公共接口仓库](https://github.com/RoboNeuxs/ros2-logistics-interfaces)

## 当前状态

仓库与协作模板已初始化；技术任务按 Issue 开展。
