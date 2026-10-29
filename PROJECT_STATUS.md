# RBT 项目状态

更新日期：2026-10-29（北京时间）。本仓库维护通用 RBT 框架。

| 范围 | 当前入口 / 状态 |
|---|---|
| Runtime、Composer、IPC、IDL、CLI | `crates/`，当前 SDK 产品版本 0.8.0 |
| Rust / C++ / Python SDK、TF、时钟 | 本仓库；公开接口和兼容版本见 `docs/compatibility.md` |
| 框架构建与安装 | [开发指南](docs/development.md)、`scripts/install_v01.sh` |
| 框架集成验证 | `scripts/verify_tf.sh --quick` / `--full`；历史结果见 [文档索引](docs/README.md) |
| 导航、Guard、硬件驱动、机器人感知 | 独立本地仓库 `/home/sw/rbt_robot_ws`；其 `PROJECT_STATUS.md` 为机器人项目状态入口 |
| GitHub | 框架私有仓库 [sunwen-me/rbt](https://github.com/sunwen-me/rbt)，默认分支 `main` |

## 当前版本与整理记录

整理前的完整源码基线：`7f3ab4be62cebb9ec657f07f3ae2937a25e1c370`，标签 `baseline/pre-split-20261029`。
这是保留当前工作区的源码快照，包含其他会话当时的进行中内容，不作为新的整机验收声明。
当前分支、版本与工作区修改通过 `git status --short --branch` 和 `git log -1` 查询。

拆分后框架只通过安装的 SDK 向机器人项目提供能力；机器人项目在 `framework.lock.json`
记录依赖的框架提交、SDK 版本和 native 源码指纹。

本机保留指向机器人仓库的旧路径兼容链接；这些链接被 Git 忽略，框架源码包也排除仓库外链接。
从框架仓库单独检出时不依赖这些本地链接。机器人项目从其独立仓库开发和提交。

本次整理的构建/测试结果、迁移校验和本地数据索引见
[整理记录](docs/repository-organization.md)。
