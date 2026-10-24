# 个人 Fork 开发与 PR 提交流程

成员在个人 GitHub 仓库保存日常开发成果，完成后向组织模块仓库发起 PR，由项目总负责人最终审核并合并。代码合并到模块主线后，仍需按任务约定完成整机集成验证。

## 固件成员示例

```text
RoboNeuxs/ros2-logistics-firmware（组织模块仓库）
    → Fork 到个人账号
    → 克隆个人 Fork，在 feature/* 分支开发
    → 反复 commit / push 到个人 Fork
    → 向 RoboNeuxs/ros2-logistics-firmware 的 main 发起 PR
    → 技术检查 → 项目总负责人最终审核、合并 → 验收
```

日常 push 不会自动合并进组织模块仓库。私有 Fork 属于原仓库的 Fork 网络，并非仅本人可见的独立私密空间，访问仍受上游权限规则约束。

## 首次准备

1. 接受组织邀请，确认拥有本人模块、公共接口和岗位依赖仓库的读取权限。
2. 在组织对应模块仓库点击 **Fork**，Owner 选择自己的 GitHub 账号。不要 Fork 管理入口来存放本模块代码，也不要另外新建无 Fork 关系的仓库代替此步骤。
3. 克隆个人 Fork，并添加组织模块仓库为 `upstream`。以下命令在终端逐行执行，将 `YOUR_USERNAME` 替换为本人 GitHub 用户名；其他岗位替换模块仓库名。

```powershell
git clone https://github.com/YOUR_USERNAME/ros2-logistics-firmware.git
cd ros2-logistics-firmware
git remote add upstream https://github.com/RoboNeuxs/ros2-logistics-firmware.git
git remote -v
git switch -c feature/firmware-task
```

`origin` 应指向个人 Fork，`upstream` 应指向组织模块仓库。已有本地工作区的成员先运行 `git remote -v`，若 `origin` 仍指向组织仓库，确认个人 Fork 已创建后再调整，不要重复克隆或覆盖未提交代码：

```powershell
git remote set-url origin https://github.com/YOUR_USERNAME/ros2-logistics-firmware.git
```

若尚无 `upstream`，使用前面的 `git remote add upstream`；若已有，则检查并用 `git remote set-url upstream` 修正其地址。

## 日常修改

在个人功能分支修改程序，检查并暂存本次任务需要提交的文件，然后执行：

```powershell
git status
git diff --cached
git commit -m "完成固件任务的一部分"
git push -u origin feature/firmware-task
```

可以多次 commit、push，无需每次申请 PR，也无需先合并到个人 Fork 的 `main`。提交前确认文件已使用 `git add 文件路径` 暂存；后续在同一分支执行 `git push` 即可。

## 同步项目更新

在工作区干净且位于本人功能分支时，获取并合并组织主线的更新：

```powershell
git fetch upstream
git merge upstream/main
git push
```

有冲突时处理冲突，使用 `git add 文件路径` 标记解决后完成 merge commit，再 push；不要强制推送覆盖他人修改。

## 提交与修改 PR

1. 在组织模块仓库的 Pull requests 中选择 **New pull request → compare across forks**。
2. base repository 选 `RoboNeuxs/ros2-logistics-firmware`，base 分支选 `main`；head repository 选个人 Fork，compare 分支选 `feature/firmware-task`。
3. 查看差异，说明改动、验证方法、实际结果和未验证项，关联组织模块仓库的任务 Issue，并请求项目总负责人审核。跨仓库关联任务时使用完整 Issue 链接。
4. 审核要求修改时，在原功能分支继续 commit、push，已有 PR 自动更新；无需重复创建 PR。
5. 技术审核人检查，项目总负责人最终审核并执行合并。任务按验收条件确认后关闭。

## 管理员配置

以下设置需要在 GitHub 实际配置并核验；此文档本身不代表线上配置已完成。

| 范围 | 目标设置 |
|---|---|
| 组织 Member privileges | Base permissions 保持 No permission；Repository forking 允许私有仓库 Fork |
| 9 个开发模块仓库 | Settings → General → Features 中开启 Allow forking，允许成员 Fork 到个人账号 |
| 普通成员的岗位 Team | 对所需模块设置 Read，包含开发入口、公共接口和必要依赖；检查继承和直接授权，移除普通成员的额外写权限 |
| 项目总负责人 | 保留所需仓库的审核、合并和管理权限；普通成员不授予 Write、Maintain 或 Admin |
| 管理入口与其他仓库 | 保持原受限范围，不因采用个人 Fork 开发而增加普通成员访问权限 |
| 主分支保护 | 当前套餐支持时，对 main 要求 PR 和审核；若需限定负责人审核，另配置对应审核规则，不把“开启 Fork”当作已强制审核 |

设置后以普通成员账号验证：能读取本人模块、创建个人私有 Fork、推送个人功能分支并向上游发起 PR；不能直接推送上游或合并 PR。检查项目总负责人可以查看差异并完成合并。不要用真实主线推送作为权限测试。

已有直接 Write 授权、上级 Team 授权或组织 Owner 身份时，仅调整一个岗位 Team 不会自动移除其他来源的权限，必须检查成员的最终有效权限。

参考：[GitHub Fork 规则](https://docs.github.com/en/pull-requests/reference/forks)、[组织 Fork 策略](https://docs.github.com/en/organizations/managing-organization-settings/managing-the-forking-policy-for-your-organization)。
