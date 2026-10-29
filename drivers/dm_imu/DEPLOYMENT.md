# AidLux 实机 IMU 部署记录

2026-10-29（北京时间）。原生 `rbt_dm_imu` 已在车载 AidLux 编译、安装并连接真实 DM IMU，
通过 RBT IPC 发布 `/imu/data`。本次没有启动底盘控制或发送运动命令。

后续按用户要求，已将 RBT 发布频率改为 **200 Hz**，完成时钟热点优化并更新车载服务。
新版本仍解析和校验设备串口样本，只在发布时隙输出最新有效样本；没有新样本就不发布，
不补发过期样本。现场探针 3 秒收到 591 个有效消息、0 个非法消息，首末样本间接收率
199.995 Hz。当前性能数据、测试范围、修改文件和回滚步骤见
[优化记录](../../output/dm-imu-optimization/20261028T180357Z/REPORT.md)。

## 当前部署位置与服务

| 项目 | 实际值 |
|---|---|
| SSH 目标 | `aidlux`，`192.168.1.232`，用户 `aidlux` |
| 架构与工具链 | ARM64，GCC 11.4，CMake 3.22.1，Release |
| RBT SDK | `/home/aidlux/.local/opt/rbt-sdk-0.8.0-linux-aarch64` |
| 独立部署目录 | `/home/aidlux/rbt-dm-imu-200hz-20261028T182358Z` |
| 当前安装入口 | `/home/aidlux/.local/opt/rbt-dm-imu-current`，指向部署目录内的 `install/` |
| 活动配置 | `/home/aidlux/.config/rbt/dm_imu.yaml` |
| 串口 | `/dev/dm_imu → /dev/ttyACM0`，921600 baud |
| USB 身份 | `ffff:ffff`，序列号 `2025021200`，与已有 udev 规则一致 |
| 发布契约 | `/imu/data`，`rbt.hardware::Imu`，frame `imu_link` |
| 当前发布频率 | YAML `publish_rate_hz: 200`；设备串口输出设置保持原值 |
| 后台服务 | 用户服务 `rbt-dm-imu.service`，`enabled`、`active/running` |

服务配置为失败后 3 秒重启。已启用用户 lingering（`Linger=yes`），允许用户服务在开机时启动、
退出 SSH 后继续运行；没有重启车载机验证完整开机流程。设备配置和参数保存均关闭，
沿用设备现有输出设置。已有 udev 规则、Composer、LIO、底盘和机械臂服务均未修改。

在车载端管理服务：

```bash
systemctl --user status rbt-dm-imu.service
systemctl --user restart rbt-dm-imu.service
systemctl --user stop rbt-dm-imu.service
journalctl --user -u rbt-dm-imu.service -n 30 --no-pager
```

只读检查原生消息：

```bash
source /home/aidlux/.local/opt/rbt-sdk-0.8.0-linux-aarch64/env.sh
/home/aidlux/.local/opt/rbt-dm-imu-current/bin/rbt_dm_imu_probe --duration 5
```

## 首次部署的实际验证

| 场景 | 结果 |
|---|---|
| 真实设备只读串口采集 | 3.0009 秒、171,015 字节、3,000 个完整样本 |
| 独立驱动与订阅进程，5 秒 | 4,381 个有效消息、0 个非法消息；接收约 883.18 Hz；末帧年龄 8.89 ms |
| 后台服务与订阅进程，3 秒 | 2,651 个有效消息、0 个非法消息；接收约 896.44 Hz；末帧年龄 8.82 ms |
| 停止服务后的 0.7 秒负例 | 消息数为 0，探针按预期返回 1 |
| 同一订阅进程跨服务重启，8 秒 | 6,884 个有效消息、0 个非法消息；末帧年龄 7.79 ms；序列递增检查通过 |
| 车载 ARM64 Release parser CTest | 1/1 通过，包含真实报文回归 |
| 开发机 Debug / Release / ASan / UBSan / ASan+UBSan parser CTest | 各 1/1 通过 |
| 开发机原生 RBT IPC + 串口伪终端集成 | pipeline、navigation、navigation_crash，3/3 通过 |

上表接收率是实际订阅测量值，没有将设备串口约 1,000 Hz 的样本率等同为 IPC 无损接收率。
真实消息的四元数模长为 1，向量有限，frame、同主机时钟、新鲜度与序列检查通过。
开发机跨进程测试使用模拟串口，不属于真实电机验收。本次没有重新执行全部导航测试。

## 实机发现并修复的问题

最初车载构建发现系统 yaml-cpp 仅导出 `yaml-cpp` 目标，现增加兼容别名。
探针也修正了 GCC 11 Release 下的 optional 时间戳误报，保留 `-Werror`。

首次真实订阅为 0 帧：原移植错误地使用标准 CCITT-FALSE CRC，而实际设备复用参考驱动的
特殊递推：以高字节 XOR 输入选择多项式 `0x1021` 的贡献项，运行 CRC 每字节仅左移 1 位。
对采集的 9,000 条记录，设备递推匹配 9,000/9,000，标准算法匹配 0/9,000。
现以独立位运算复现设备协议，没有复制参考查表；仍要求三段 CRC 均通过。
新增硬编码真实 57 字节报文、分片/噪声、逐段损坏及标准 CRC 拒绝测试，并修正模拟串口生成器。
最初的失败日志与最终通过记录均保留。

配置仍沿用原输入单位假设，输出按 rad/s、m/s²、归一化四元数编码。
静态采集的加速度模长均值约 10.033，尚未用已知转角/转速验证陀螺仪比例、安装轴向或整机控制效果。
本次完成真实设备读取和消息发布，不表示底盘运动闭环已经验收。

## 证据与修改文件

首次部署的本地完整证据目录为
[`output/dm-imu-deployment/20261028T173655Z`](../../output/dm-imu-deployment/20261028T173655Z/)，
包含串口原始采集、CRC 审计、构建失败/成功日志、远端运行结果、服务与配置快照、
本地测试日志、源文件 SHA-256 校验。远端运行证据位于独立部署目录的 `evidence/`。

修改文件：`drivers/dm_imu/{CMakeLists.txt,README.md,NOTICE.md,src/parser.cpp,src/probe.cpp,tests/parser_test.cpp}`、
`drivers/tests/hardware_ipc_test.py`；新增本记录，更新 `drivers/FILES.md` 与 `drivers/VERIFICATION.md`。
没有修改导航算法、底盘控制实现或消息布局。
