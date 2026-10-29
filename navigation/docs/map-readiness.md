# 用户地图与先验定位联调

原场地保留的导航地图入口是 [navigation-map.yaml](../../output/navigation-lio/maps/navigation-map.yaml)，配对先验为同目录 `prior.pcd`。用户已确认将转换程序输出的未知像素视为白色自由空间；地图包含40,171个自由格、7,980个障碍格、0个未知格。当前已更换场地，新场地PCD重载定位已通过，见文末稠密建图记录；尚未用新场地地图替换这套导航PGM。

## 已完成的离线检查

| 检查 | 实际结果 |
|---|---|
| PCD重新投影与当前PGM逐格对比 | 48,151格全部一致，障碍差异0格 |
| YAML安装外参 | 内部标定保留；按用户确认的robot-sim安装换算活动车体外参 |
| 当前平面车体雷达原点 | 相对 `base_footprint` 为 `[-0.053, 0, 0.735] m`，roll=180°；见安装审计 |
| PCD低位平面候选 | 倾角约0.282°，原点截距约-0.0381 m，残差中位数约0.016 m；未修改地图或外参 |
| 当前PGM原生IPC闭环 | 右侧局部路线到达并停车，全部6个进程退出0 |
| 原复杂场景脚本回归 | rooms、warehouse 2/2通过，52.28 s |
| 定位探针 | Debug/Release/ASan/UBSan各2/2：有效帧通过、过期帧拒绝 |
| AidLux现有MID360/LIO测试 | 新KISS构建下9/9通过，16.83 s |
| 实际PriorMapLocalization初始化 | 非零已知变换恢复通过；定位新鲜度超时检查通过 |

投影与外参报告见 [alignment-audit.json](../../output/navigation-lio/readiness/alignment/alignment-audit.json)；平面仅为点云几何候选，不能据此确认真实地面或安装准确性。

上述alignment报告保留修改前的YAML推导值。随后用户提供实机一致的 `192.168.1.153:/home/sw/robot-sim` 安装模型；完整关节链、参数换算及新旧配置对比见 [雷达安装参考](robot-mount.md)。

## 当前地图上的闭环仿真

测试路线从 `(3.375,-1.325,0)` 到 `(2.675,-0.525,0)`。直达线被障碍挡住，A*路径约1.336 m；实际执行约1.241 m，目标耗时26.77 s，最终位置误差约0.079 m，最终线速度和角速度均为0。地图由原生map_server加载，TF与运动来自二维模拟底盘，全部收到的路径、车体位姿及相邻位姿线段均接受碰撞检查。

该测试使用0.18 m模拟圆形车体、0.10 m额外规划余量、最大线速度0.2 m/s、最大角速度0.5 rad/s。它验证右侧可达区域，不表示整个地图连通。连通性预检显示，当前障碍图在所检查的内部范围内较碎；报告分别保留0.28 m、0.48 m规划半径的结果。实际车体足迹仍须实测，尤其不能把模拟半径当成底盘尺寸标定。

```bash
target/navigation-video-tools/bin/python navigation/scripts/user_map_simulation.py \
  --map output/navigation-lio/maps/navigation-map.yaml \
  --start 3.375 -1.325 0 --goal 2.675 -0.525 0 \
  --planning-clearance 0.10 --max-linear 0.2 --max-angular 0.5 \
  --build-dir target/navigation-lio-debug \
  --output-dir output/navigation-lio/readiness/simulation-interior \
  --timeout 120 --fps 12 --speed 2
```

结果、原始采样与视频在 [simulation-interior](../../output/navigation-lio/readiness/simulation-interior/user-map/result.json)。MP4/GIF按真实IPC采样渲染并标注2×回放。

## 首次先验定位的启动修正

此前独立 `build-navigation` 的 `LIO_WITH_KISS_MATCHER=OFF`，review配置也关闭了 `lio.kiss_matcher.enable`。现有VGICP路径无法在没有初始变换时完成首次定位；通过 `--check-config` 并不证明能建立 `map→odom`。

现在保留原构建，另建：

```text
/home/aidlux/rbt-navigation-20261027/build-prior-localization
```

这个构建启用现有KISS Matcher、FastVGICP和导航输出。PMC/Xenium使用原vendored CMake规定的固定提交，完整记录在 [build-manifest.json](../../output/navigation-lio/readiness/localization/build-manifest.json)。活动配置为 `output/navigation-lio/super_lio-localization.yaml`：启用 `lio.kiss_matcher.enable`，并随后按robot-sim安装换算 `odom_robo`；内部雷达标定和质量门限保持原值。

真实原生定位类测试使用先验PCD中的62,707个点生成带已知变换的合成局部扫描，真值为平移 `(1.1,-0.7,0.25) m`、yaw 20°。KISS得到104个final inliers；VGICP fitness为0.00182565，保持初次门限0.01、跟踪门限0.1。最终变换误差约4.984 mm、0.037672°。关闭KISS的负例不能初始化；停止注册2.2 s后 `navigationReady()` 失效。此测试证明软件初始化链路，未模拟真实传感器遮挡、噪声或实际安装误差。

## 雷达上电后的只读验收

在AidLux上执行：

```bash
bash /home/aidlux/rbt-navigation-20261027/output/navigation-lio/readiness/run-localization-check.sh
```

命令启动独立Composer、原生MID360驱动、KISS已启用的SuperLIO和只读探针，观察20 s后关闭自己的进程。它不启动导航、Guard或底盘，不发送运动命令。当前AidLux原生 `driver.conf` 使用接收IP `192.168.5.5`、设备IP `192.168.5.3`，点云/IMU端口56301/56401；雷达需要向该主机发送数据，驱动本身不会给雷达上电或配置雷达。实际设备和接口依据见下方2026-10-28记录。

通过条件为至少5个合法定位帧，且结束前250 ms内仍有合法帧。探针复用桥接的时间、坐标与变换校验，输出实际 `map→odom`、map中车体位姿、雷达与车体高度差和全部进程日志。干净的未定位帧只计为等待，不续期成功状态。

2026-10-27的未连接运行中，5 s被动监听两个端口均为0包；实际新构建的无数据验收返回失败、定位帧0，所有源进程正常关闭，未强杀。该结果不是硬件定位通过，保留为断连负例。

当前仅支持MID360 mode 0主机收包时基；mode 1/2缺少校准时拒绝导航输出。新KISS构建的模拟UDP回归也已通过：mode 0收到153帧，结束帧年龄约21 ms；mode 1输出0帧。

## 2026-10-28：连接真实MID360后的只读观察

AidLux的 `br-lan` 地址为 `192.168.5.5/24`，现场MID360为 `192.168.5.3`。活动原生驱动配置已改为接收IP `192.168.5.5`、雷达IP `192.168.5.3`，点云/IMU端口仍为56301/56401，雷达源端口仍为56300/56400；配置镜像见 [mid360-driver.conf](../../output/navigation-lio/mid360-driver.conf)，修改前备份见 [driver.before.conf](../../output/navigation-lio/readiness/hardware-20261028/driver.before.conf)。原ROS 2 `MID360_config.json`快照记录host IP `192.168.5.5`和历史设备IP `192.168.5.170`；后者不代表本次现场设备地址，这份快照也不能确认当前ROS 2启动参数。快照见 [ros2-MID360_config.json](../../output/navigation-lio/readiness/hardware-20261028/ros2-MID360_config.json)。

5 s被动监听收到10,418个点云包和1,000个IMU包，观测到的时基为mode 0，详见 [passive-network.json](../../output/navigation-lio/readiness/hardware-20261028/passive-network.json)。随后进行25 s真实传感器只读定位观察：探针收到242帧，但242帧均未定位，有效定位帧为0，最大时基映射不确定度211,623 ns。KISS候选未通过VGICP初次fitness门限0.01（记录值1.66769），未产生 `map→odom`；探针返回1。驱动与SuperLIO退出码均为0，Composer按流程以-15结束，无强制杀进程。完整结果与探针输出见 [result.json](../../output/navigation-lio/readiness/hardware-20261028/localization-20261027T174853Z-d8eb1bf7/result.json) 和 [probe-result.json](../../output/navigation-lio/readiness/hardware-20261028/localization-20261027T174853Z-d8eb1bf7/probe-result.json)。

该观察确认真实传感器数据进入驱动和SuperLIO，但不构成先验定位通过。是否处于与 `prior.pcd` 对应的场景仍待确认；本次未启动导航、Guard、底盘或电机，也未发送 `/cmd_vel`，且未重跑完整40项测试矩阵。

离线审计脚本依赖NumPy、Pillow、PyYAML、Matplotlib，仅用于验证/制图；导航运行时沿用原生C++与RBT接口。软件没有增加ROS 2运行库依赖。

## 2026-10-28：新场地60秒建图并重新加载

用户随后确认已更换场地，并要求采集一分钟后保存、重新加载测试。原生LIO完成地图初始化后计时60.000476 s，正常停止时合并12个PCD片段，保存5,871个有限XYZ点，PCD为94,122字节。关闭先验定位与KISS进行建图，保留当前安装外参；输出使用独立绝对目录，旧地图和活动导航地图未覆盖。

随后重新启动原生驱动与LIO，加载本次PCD（5,871原始点，经现有先验地图体素处理后为2,040点），观察25 s。探针收到242帧，有效定位帧0；KISS得到对应点，但VGICP最好分数0.0101882仍高于原初次门限0.01，本次定位验收失败。未放宽门限，也未将未通过的新地图提升为活动导航地图。建图与重新加载的驱动、LIO均退出0，未启动导航、Guard或底盘。

地图、配置、原始日志、预览和实际结果见 [新场地验收](../../output/navigation-lio/new-scene/mapping-20261027T190444Z-69ee692b/acceptance.json)、[保存的PCD](../../output/navigation-lio/new-scene/mapping-20261027T190444Z-69ee692b/map/map.pcd)、[地图预览](../../output/navigation-lio/new-scene/mapping-20261027T190444Z-69ee692b/preview/map-preview.png)。该次结果确认采集、保存和读取链路；定位质量仍须排查，不能作为实车导航通过。

## 2026-10-28：稠密保存后重采并通过定位

第一轮沿用 `filter_rate=3` 和 `map.if_filter=true`，存图取自已经过0.5 m体素处理的 `ds_undistort_`。末端的5 cm过滤不能恢复先前被删除的细节。本次沿用原生LIO和现有配置接口，将采集配置设为 `lio.sensor.filter_rate=1`、`lio.map.if_filter=false`，保存完整有效的去畸变点云；在线匹配体素仍为0.5 m，外参和定位门限保留。

地图初始化后重新采集60.000428 s，保存6,303,245个有限XYZI点，原始PCD为100,852,112字节。独立使用现成 `pcl_voxel_grid -leaf 0.05,0.05,0.05` 导出16,921点的5 cm地图；原始稠密点云保留。加载原生PCL压缩格式的5 cm地图后，先验模块现有0.1 m内部体素得到4,141点。本次25 s探针收到242帧，其中9帧等待定位、233帧有效定位，结束前最近有效帧年龄21.668270 ms，最大时基映射不确定度224,049 ns。

KISS初次候选通过VGICP，初次分数0.00109322，原门限0.01；共47次接受注册，分数范围0.000741976至0.00109322，最后一次为0.000746568。该分数是配准质量指标，不是绝对位姿精度。此结果确认本轮采集位置附近的新地图重载定位，未包含移动中的定位或底盘闭环验收。

当前只读检查入口 `readiness/run-localization-check.sh` 已指向本次验证的定位配置，默认观察25 s；旧入口和旧地图均留存。原始地图、5 cm导出、参数、日志与验收结果见 [稠密建图验收](../../output/navigation-lio/new-scene/dense-mapping-20261027T191536Z-d9d6148e/acceptance.json)。没有启动导航、Guard、底盘或ROS 2运行时，也没有放宽定位门限或修改C++算法。
