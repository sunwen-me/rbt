# 实机雷达安装参考

用户确认 `192.168.1.153:/home/sw/robot-sim` 中的雷达安装与实机一致。本次通过 SSH 只读检查模型与启动文件，未启动 Gazebo、底盘或运动命令。

`launch.sh` 启动 `robot_bringup/full.launch.py`，后者包含 `robot_gazebo_sim/sim.launch.py`。该启动文件通过 `robot_description/urdf/robot.xacro` 生成模型；安装目录里的模型是源码符号链接，文件哈希一致。留存的原始文件位于 [robot-mount/source](../../output/navigation-lio/readiness/robot-mount/source/)。

## 测量坐标系

| 变换（父坐标系 ← 子坐标系） | 平移 m | RPY rad |
|---|---|---|
| `base_footprint ← base_link` | `[-0.196, 0, 0]` | `[0, 0, 0]` |
| `base_link ← mid360_rgl_frame` | `[0.143, 0, 0.735]` | `[π, 0, 0]` |
| 合成 `base_footprint ← mid360_rgl_frame` | `[-0.053, 0, 0.735]` | `[π, 0, 0]` |

这里的 `base_footprint` 是驱动轴中点参考，`base_link` 是后移196 mm的CAD参考。735 mm是相对模型参考坐标系的高度；模型名称本身不能证明该原点落在实际地面。

`robot.gazebo` 中 `base_link` 下的实际 `mid360_rgl` 传感器使用同一位置和姿态，插件发布局部点云、frame为 `mid360_rgl_frame`。因此这里使用测量原点，未使用 `mid360_1` 的网格显示原点。Gazebo的虚拟 `mid360_imu` 则设为正置且与雷达共点；这不是实机雷达内部IMU标定，不能拿它覆盖实机 `lidar_imu`。

## 与现有 LIO 参数的换算

现有LIO源码虽然在历史注释中把 `odom_robo` 称为雷达外参，但初始化、平面约束、ESKF和导航输出实际都将它用于 **IMU原点与车体原点之间的变换**。本次保持源码算法与参数读取方式，换算活动YAML中的这一个外参。

记 `T_AB` 为把B系点变换到A系的变换，B=车体，I=内部IMU，L=雷达。模型提供 `T_BL`，原YAML提供 `T_IL`，因此有效参数为：

```text
R_BI = R_BL · R_ILᵀ
t_BI = t_BL − R_BI · t_IL

原内部标定 t_IL = [-0.011, -0.02329, 0.04412] m，R_IL = I
换算得到 t_BI = [-0.042, -0.02329, 0.77912] m，R_BI = Rx(π)
```

现有解析器将度数RPY对应矩阵转置后存储；当前活动配置 [super_lio-localization.yaml](../../output/navigation-lio/super_lio-localization.yaml) 使用：

```yaml
lio.extrinsic.odom_robo: [-0.042, -0.02329, 0.77912, 180.0, 0.0, 0.0]
```

`lidar_imu`、点云地图、PGM、地图原点和定位质量门限均沿用原值。历史review配置和本次修改前的活动配置留存，用于重现。原配置推导的雷达位置为 `[-0.064, 0.02329, 0.69088] m`，相对模型偏差约51.09 mm；换算后为 `[-0.053, 0, 0.735] m`。

## 验证与范围

```bash
target/navigation-video-tools/bin/python navigation/tests/test_robot_mount_audit.py
target/navigation-video-tools/bin/python navigation/scripts/robot_mount_audit.py \
  --robot-xacro output/navigation-lio/readiness/robot-mount/source/robot.xacro \
  --robot-gazebo output/navigation-lio/readiness/robot-mount/source/robot.gazebo \
  --lio-config output/navigation-lio/super_lio-localization.yaml \
  --output output/navigation-lio/readiness/robot-mount/mount-audit.json
```

审计比较固定关节链、实际Gazebo传感器pose和现有LIO参数的完整旋转/平移；旧参数应失败，新参数应通过。结果与原生LIO `--check-config` 日志保存在同一证据目录。此项确认模型与配置一致，实机安装参考来自用户确认；雷达未上电，本次没有新增真实定位、IMU单位或运动验收结果。独立底盘继续不发布TF。
