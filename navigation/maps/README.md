# 地图样例

本目录包含 `simulation_obstacle.yaml` 和对应的 `simulation_obstacle.pgm`。它们是现有视频仿真地图的精确输入：80 × 60 格，分辨率 0.1 米/格。PGM 中的合成墙仅用于软件仿真和演示，不代表实地测绘结果。

可用独立地图服务器发布样例：

```bash
target/navigation-build/rbt_navigation_map_server \
  --map navigation/maps/simulation_obstacle.yaml \
  --prefix rbt/navigation/demo \
  --publish-hz 2
```

地图 YAML 字段、图像格式、模式映射、构建依赖和安装位置见 [地图文件与发布说明](../docs/map-files.md)。

## 复杂导航场景

两张地图均为 120 × 80 格、0.1 m/格，带边界墙和未观测区域。未知格仍按障碍处理。

| 地图 | 场景 | 起点 x/y/yaw | 目标 x/y/yaw |
|---|---|---|---|
| [complex_rooms.yaml](complex_rooms.yaml) + [PGM](complex_rooms.pgm) | 错开门口、多房间穿行，主要门口宽 1.4 m | 1.25 / 1.25 / 0 | 10.45 / 6.85 / 0 |
| [complex_warehouse.yaml](complex_warehouse.yaml) + [PGM](complex_warehouse.pgm) | 货架端部转弯与立柱绕行，预设路线主要通道至少 1.6 m | 1.25 / 1.05 / 0 | 9.45 / 6.85 / 0 |

目录中的 [complex_scenarios.json](complex_scenarios.json) 是运行入口配置。两组起终点直线均被障碍挡住；可用仓库的地图生成脚本重新生成完全相同的文件。完整六进程仿真与视频命令见 [导航 README](../README.md#复杂-pgm-场景)。这些是合成环境。
