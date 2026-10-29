# 地图文件与发布

`rbt_navigation` 的地图读入器位于可选的 `rbt_navigation::map_io` 组件中。它读取常见的栅格地图 YAML 与图像文件，输出核心使用的 `OccupancyGrid`；核心库和消息接口本身不依赖图像或 YAML 库。格式与阈值约定参考 [Nav2 map_io](https://github.com/ros-navigation/navigation2/blob/main/nav2_map_server/src/map_io.cpp)，此处为独立实现，不依赖 ROS，也没有复制 ROS 代码。

## 构建和链接

默认 `RBT_NAVIGATION_MAP_IO=ON`。关闭该选项可不构建地图读入器和地图服务器；关闭时也不需要 yaml-cpp 或 libpng。启用后构建依赖 yaml-cpp 和 libpng。独立地图服务器还需要启用原生 RBT 支持。

安装后的 CMake 使用方式：

```cmake
find_package(rbt_navigation CONFIG REQUIRED COMPONENTS map_io)
target_link_libraries(my_map_tool PRIVATE rbt_navigation::map_io)
```

`rbt_navigation::core` 和消息目标不会因此引入 yaml-cpp 或 libpng。地图服务器安装到 `${prefix}/bin/rbt_navigation_map_server`；地图资源安装到 `${prefix}/share/rbt_navigation/maps/`。

## YAML 和图像格式

YAML 中的 `image`、`resolution`、`origin`、`negate`、`occupied_thresh` 和 `free_thresh` 为必填项。`mode` 可省略，省略时使用 `trinary`。示例：

```yaml
image: floor.pgm
mode: trinary
resolution: 0.05
origin: [-2.0, -1.0, 0.0]
negate: 0
occupied_thresh: 0.65
free_thresh: 0.196
```

`image` 可写相对路径（相对于 YAML 文件所在目录）或绝对路径。`resolution` 单位为米/格；`origin` 依次为地图左下角的 x、y（米）和 yaw（弧度）。`negate` 接受 `0`、`1` 或布尔值。阈值必须满足 `0 <= free_thresh < occupied_thresh <= 1`。

支持 PGM P2（文本）和 P5（二进制，最多 16 位灰度）以及 PNG。默认 `loadOccupancyMap(path)` 最多读取 1,000,000 格；也可通过 `max_cells` 参数调整上限。图像第一行在顶部，读入后会翻转行，使栅格数据以地图左下角为起点、按行存储。输出格值为 `-1`（未知）或 `0..100`（占据值）。

地图优先使用灰度图。彩色 PNG 用四舍五入的 Rec.601 亮度 `0.299 R + 0.587 G + 0.114 B` 转成灰度，不承诺与其他软件的彩色转灰度结果逐像素相同。PGM 的非 255 最大灰度先归一化到 8 位；二进制 16 位像素按大端读取。YAML 最大 1 MiB，图像文件也有独立字节上限；重复的已知 YAML 字段、非有限几何或超限尺寸被拒绝。

模式决定灰度如何映射为占据值：

| `mode` | 行为 |
|---|---|
| `trinary` | 应用 `negate` 后，达到 `occupied_thresh` 的像素变为 100，不高于 `free_thresh` 的像素变为 0，中间区域为 -1。 |
| `scale` | 两端阈值与 `trinary` 相同；阈值之间按比例映射到 0..100。 |
| `raw` | 灰度 0..100 直接作为占据值，大于 100 的值变为 -1；忽略 `negate` 和 alpha。 |

在 `trinary` 和 `scale` 模式中，alpha 小于 255 的透明像素为未知值。核心地图将占据值达到 50 的格子和未知格视为障碍；低于 50 的格子视为可通行。

## 独立地图服务器

`rbt_navigation_map_server` 启动时加载一次地图，并周期性发布同一地图内容作为心跳。下面的命令发布本仓库的合成障碍图：

```bash
target/navigation-build/rbt_navigation_map_server \
  --map navigation/maps/simulation_obstacle.yaml \
  --prefix rbt/navigation/demo \
  --publish-hz 2
```

地图发布到 `rbt/navigation/demo/map`。`--publish-hz` 支持 1 到 20 Hz，默认 2 Hz；可选 `--duration SECONDS` 设置运行时长，省略时持续运行到收到终止信号。一次只为每个 prefix 启动一个地图发布进程。文件无效或读取失败时，进程在发布前退出并返回非零状态。

每次发布的时间戳来自本机 HostClock。启动时以 HostClock 纳秒值生成固定地图版本，同一进程的每次心跳保持该版本；重启服务器会生成新版本。服务器不提供热重载、TF、SLAM 或定位。使用地图进行导航仍需单独的 SLAM/定位组件提供动态 `map→odom` 变换。

地图包文件可随安装前缀分发；将 YAML 与其图像保持在相对路径关系中即可直接加载。仓库中的样例见 [maps/README.md](../maps/README.md)。
