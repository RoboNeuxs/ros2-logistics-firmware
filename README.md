# 实时运动控制固件

ROS 2 多模态 AI 智能搬运机器人 · `ros2-logistics-firmware`。

## 职责

MCU 外设、编码器、四轮闭环和停车保护。

## 任务模块

- **M02 实时驱动与闭环控制**

## 输入与交付

输入：对应 Issue、已确认硬件资料、公共接口基线和上游模块的受控版本。

交付：本模块工程、配置、验证记录，以及每个步骤的完成说明或使用教程。具体指标、负责人和排期在对应 Issue 中确认。

## 个人 Fork 开发与提交

普通开发成员对组织模块仓库拥有 Read 权限。先 Fork 本仓库到个人账号，在个人 Fork 的 `feature/*` 分支编写程序并反复 commit / push；完成后向本仓库的 `main` 发起 PR，由项目总负责人最终审核并合并。

完整步骤、命令和已有工程迁移方法见 **[个人 Fork 开发与 PR 提交流程](COLLABORATION.md)**。

## 开始工作

1. 阅读 [开发入口](https://github.com/RoboNeuxs/ros2-logistics-developer-hub) ，认领对应任务并确认交付与验收要求。
2. 在 Issues 中确认模块工作项与验收条件，再拆分具体任务。
3. 在个人 Fork 开发，成果通过跨仓库 PR 提交到组织本仓库，附教程和验证记录，等待项目总负责人审核合并。
4. 每完成一步，在 `docs/` 添加完成说明或教程，更新 [文档索引](docs/README.md)。

## 文档

- [仓库初始化说明与使用方法](docs/00-仓库初始化说明.md)
- [步骤完成说明与使用教程模板](docs/templates/步骤完成说明与使用教程.md)
- [开发提交规范](https://github.com/RoboNeuxs/ros2-logistics-developer-hub/blob/main/CONTRIBUTING.md)
- [公共接口仓库](https://github.com/RoboNeuxs/ros2-logistics-interfaces)

## 当前状态

仓库与协作模板已初始化；技术任务按 Issue 开展。

[整车工程如何集成](https://github.com/RoboNeuxs/ros2-logistics-developer-hub/blob/main/docs/01-整车工程集成方式.md)
