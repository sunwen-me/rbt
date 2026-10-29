# 仓库整理记录

2026-10-29（北京时间）。先建立可恢复的源码基线，再分离框架和机器人功能包。

| 仓库 | 维护内容 |
|---|---|
| `/home/sw/rbt` | Runtime、SDK、IDL、TF、时钟、CLI、框架示例与测试 |
| `/home/sw/rbt_robot_ws` | `drivers/`、`navigation/`、`perception/`、机器人部署记录与诊断工具 |

机器人功能包通过安装的 RBT SDK 使用框架，依赖提交记录在机器人仓库的
`framework.lock.json`。本次不更改导航算法或实机服务。

## 源码和数据保留

整理前源码基线为 `7f3ab4be62cebb9ec657f07f3ae2937a25e1c370`，标签
`baseline/pre-split-20261029`；机器人源码导入基线为
`1e88e5f452fc0f3b47eb3e29f580faea7b0e4e26`。
迁移的 383 个原有文件逐个校验一致，原有测试保留。

本机旧的 `drivers`、`navigation`、`perception` 及 addon 打包入口保留为兼容链接。
这些链接被框架 Git 和源码打包器排除，不进入新的框架检出目录。
`target/`、`output/`、历史地图、日志、录包和本地环境文件仍保存在本机，未删除。

完整源码快照、迁移校验和构建记录位于本机
`output/repository-organization/20261029T003025Z/`；索引为
`output/repository-organization/latest.json`。这些执行证据不提交到源码仓库。

## 已执行验证

- 框架 C++ SDK Release 重新构建、安装成功；CTest 13/13 通过。
- 源码发布与归档回归测试 8/8 通过，包括排除机器人兼容链接的新增用例。
- 机器人统一入口的 Release 构建成功，默认地图 IO 开启；本次未重新运行其 CTest 或实机验收。
- 用户要求优先完成 Git 管理，其他验证暂停；历史报告不作为本次重测结果。

## Git 使用

日常分支和提交约定见 [Git 工作流](git-workflow.md)。当前未配置 GitHub 远端，未上传。
本地提交使用命令级 `Codex <codex@localhost>` 作者信息；未修改用户全局 Git 配置。
