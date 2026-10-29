# DDSM 底盘与 DM IMU 原生移植

用户提供的 DDSM 与 DM IMU ROS 2 源码已用于实现独立的原生 RBT 驱动。底盘进程位于 [drivers/base_controller](../../drivers/base_controller/)，IMU 进程位于 [drivers/dm_imu](../../drivers/dm_imu/)，共享硬件 wire 类型位于 [drivers/hardware_messages](../../drivers/hardware_messages/)。软件实现与 PTY 集成入口已经加入工作区；真实设备、真实单位、安装轴向和 HAT 固件仍未验证。

导航的 A*/ESDF/RPP/DWPP 与运行时保持原样。底盘只消费最终 `VelocityCommand`、急停和 IMU 消息，不链接导航算法，也不发布 TF。同仓库独立包中的底盘、IMU 与硬件消息实现已经加入工作区；真实设备、设备单位、安装轴向和 HAT 固件仍未验证。外部 TF 发布组件使用轮式反馈生成的 `odom→base_footprint` 与固定 `base_footprint→base_link` 外参，组合为动态 `odom→base_link`，并提供动态 `map→odom`；组合变换保留轮式采样时间。runtime 只订阅这两个动态 TF 输入，没有 static-TF subscriber。当前 `Odom2D` 速度按 `base_link` 机体坐标解释，直接使用轮式速度要求 `base_footprint` 与 `base_link` 平面原点和 yaw 相同；安装偏移需由外部里程计/TF 集成处理。

## DDSM 底盘

原生可执行程序名为 `rbt_base_controller`。它订阅默认全局 `/cmd_vel` 的 `rbt.navwire::VelocityCommand`，并订阅 `prefix/estop`；发布 `prefix/wheel_odom`、导航所用的 `prefix/odom` 和诊断 `prefix/base_status`。完整消息契约见 [base-controller-contract.md](base-controller-contract.md)。

驱动复用 ROS 参考包的串口、DDSM 协议、反馈缓存、差速解算、PID 与轮式里程计纯 C++ 模块。独立的 I/O 线程以 100 Hz 为周期目标，在每个周期读取最终速度 gate、计算左右轮指令并与两侧电机交换反馈，不依赖 main IPC 循环持续运行。HAT 单次反馈等待上限为 25 ms，可超过 10 ms 的目标周期，故 100 Hz 不是实际速率保证。`valid_until_ns` 是首要截止时间且最大 TTL 为 250 ms；额外 600 ms 命令流 watchdog 只提供失联兜底，不会延长消息 TTL。反馈年龄超过 50 ms 会在故障计数锁存前停止输出；桥接器错误或电机错误立即触发 I/O fault，其他连续 I/O 失败达到 5 次后锁存。命令到期、急停或 I/O fault 都会让驱动目标归零。急停会锁存，解除后需要新速度命令。fault 需连续 5 个健康反馈周期才解除，恢复时已清除的活动命令不会自动恢复，必须收到新命令。正常退出会尝试向电机发送零指令，进程被强杀、串口断开或底层固件失效时仍须依赖硬件停车保护。

`WheelOdometry` 与 `Odom2D` 只在收到新鲜且有效的一对左右轮反馈后发布，stamp 取 HostMonotonic 的主机接收时间。轮式消息携带 `odom` / `base_footprint` frame、积分位姿、机体速度与 RPM；它不是 TF，过期反馈不会伪装成新的测量。`BaseStatus` 提供拒绝计数、I/O 故障、watchdog、急停、反馈新鲜度和左右电机命令等诊断，不替代命令验证或硬件安全。

底盘通过消息目标而非导航库接入：

```cmake
find_package(rbt_navigation CONFIG REQUIRED)
find_package(rbt_hardware CONFIG REQUIRED)

target_link_libraries(rbt_base_controller PRIVATE
    rbt_navigation::messages
    rbt_hardware::messages)
```

`rbt_navigation::messages` 仅提供已有 `VelocityCommand`、`EmergencyStop`、`Odom2D` wire ABI 与 RBT stream 依赖；`rbt_hardware::messages` 提供 `WheelOdometry`、`BaseStatus` 及时间/stream 辅助函数。底盘不链接 `rbt_navigation::core`、`rbt_navigation::native`，也不创建或发布 TF。

本工作区选用 [ddsm_robot_current.yaml](../../drivers/base_controller/config/ddsm_robot_current.yaml) 作为目标配置：

| 项目 | 配置 |
|---|---|
| 电机与链路 | DDSM115；HAT 二进制协议；`/dev/ddsm`，460800 baud；左右电机 ID 1/2；heartbeat 1000 ms |
| 电机方向 | 左右方向 `+1/-1` |
| 几何 | 轮半径 0.05 m；轮距 0.395 m |
| 调度与期限 | 控制/I/O 100 Hz；反馈超时 50 ms；命令 watchdog 600 ms；最终消息 TTL 上限 250 ms |
| 输出限制 | 电机命令绝对值 330；角速度 PID `1/0/0`，输出限幅 0.5 |
| IMU 使用 | `/imu/data`；新鲜度 0.5 s；不使用 IMU 姿态，只可用新鲜 z 轴角速度作反馈 |
| 故障处理 | 连续 5 次 I/O 失败进入故障；5 个健康周期后恢复，但需新的速度命令 |
| Frame / debug | `odom` / `base_footprint`；底盘不发布 TF；`debug=false` |
| ROS-only 参数 | YAML 留有 `enable_stamped_cmd_vel=true`；原生进程直接订阅 RBT `VelocityCommand`，不启动 ROS Twist/TwistStamped 输入 |

自动整定参数完整保留在 YAML：测试角速度 0.35 rad/s，预检 1.0 s，脉冲 2.5 s，间歇 1.0 s，验证 2.0 s，最大时长 30 s，静止阈值 0.08，归一化 RMSE 上限 0.35，超调比上限 1.5。原生进程启动时不会执行整定，这些值未由真实硬件验证。轮式里程计只积分连续健康反馈且间隔在反馈期限内的区间；坏反馈或长间隔会保留位姿，并从下一对健康反馈恢复积分。半径、轮距、方向、PID 和限值是指定配置，不等同于实测标定或机器人 footprint。`hat_binary` 需要工作区未提供的配套 ESP32 二进制桥接固件，不能用 HAT 原厂 JSON 固件替代。裸 RS485 模式无法在进程或 USB 设备失效后继续发送零指令。

## DM IMU

`rbt_dm_imu` 是独立原生串口进程，默认读取 `/dev/dm_imu`、921600 baud，发布 `rbt::hardware::Imu` 到 `/imu/data`，frame 默认 `imu_link`。重连退避由 100 ms 增长到最多 5 s。它没有 ROS 依赖，也不提供 TF 或地图定位。

解析器把三条连续 19 字节记录作为一个 57 字节样本；三条 CRC 和全部九个浮点字段必须有效才发布，坏帧不混用旧字段。串口读取使用 20 ms 有界 POSIX `poll`，断连后以 100 ms 起步、最多 5 s 的退避间隔重连；消息 stamp 是完整 HostMonotonic 主机接收时间，而非传感器采样时间。姿态由设备 Euler 度数转成归一化 x/y/z/w 四元数一次。协方差为零，表示协议没有提供不确定度；加速度按配置缩放后原样输出，不做重力补偿，静止时轴向和重力响应仍须实机核对。

必须保留的设备协议不确定性：

- 当前证据只确认第一条记录 tag `55 aa 01 01`。第二、三条 tag 和各记录尾字节默认不校验，只有获取真实设备帧后才能在配置中开启校验。
- 默认输入单位 `rad/s` 与 `m/s²` 是显式假设，因为参考驱动的角速度/加速度 wire 单位没有说明。配置支持输入为 `deg/s` 或 `g` 时转换为 SI；在确认真实设备输出前不得把默认假设写成设备事实。
- 轴值按设备原样发布。设备轴向、安装方向和标定均未实测；机体到 `imu_link` 的旋转由外部静态外参配置。

配置默认不向设备发送参数命令，也不保存参数。`--configure-device` 可选择发送参考驱动的输出配置命令；只有显式指定 `--save-parameters` 才会发送保存命令。未知 tag/terminator、单位、轴向或固件行为未实测前，不应把启动配置或保存操作用于真实设备。

## 硬件消息与测试边界

[rbt_hardware::messages](../../drivers/hardware_messages/) 定义 ABI 1 的 `rbt.hardware` 消息：`Imu`、`WheelOdometry`、`BaseStatus`。所有消息 stamp 带 48 字节 HostMonotonic source identity、generation、kind 和纳秒。硬件消息包只导出生成类型和 RBT stream 辅助接口，不包含导航算法、底盘实现或 TF publisher。IMU 的单位/协方差约定依据 [ROS 2 Imu 消息定义](https://github.com/ros2/common_interfaces/blob/rolling/sensor_msgs/msg/Imu.msg)，但符合输出约定不代表输入硬件已验证。

导航既有四进程测试仍使用纯软件模拟底盘和合成地图/TF，不启动真实硬件驱动。新增的底盘、IMU 进程集成测试位于 [drivers/tests](../../drivers/tests/) 并通过伪终端模拟串口；它们是软件/协议测试，不是实车或真实传感器验收。测试结果由根工作区的最终验收记录维护，本文件不声称测试通过或硬件工作正常。

## 源码来源与许可

底盘参考 package manifest 保存在 [provenance/upstream-package.xml](../../drivers/base_controller/provenance/upstream-package.xml)，其中声明 MIT；用户提供的归档未附 MIT 许可正文。[底盘 NOTICE](../../drivers/base_controller/NOTICE.md) 记录了逐文件复制的纯 C++ 核心与新增 native-port 文件。IMU 参考包的许可字段为 TODO，归档中也没有许可文件。当前移植保留了这些来源与不确定性：分发从参考代码衍生的内容前，应先核实并补齐原始许可。

DM IMU 的解析器与 POSIX 串口实现根据参考协议布局独立实现；没有复制 ROS wrapper 或参考源实现。[drivers/dm_imu/NOTICE.md](../../drivers/dm_imu/NOTICE.md) 保留了来源记录。DDSM 的原始纯 C++ 模块来源及 MIT 声明也应随 [drivers/base_controller](../../drivers/base_controller/) 一并保留，不能因原生进程存在而将硬件适配视为重新验证或重新授权。
