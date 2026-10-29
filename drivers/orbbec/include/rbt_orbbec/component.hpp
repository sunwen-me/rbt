#pragma once
#include <rbt/runtime.hpp>
#include <rbt_orbbec/driver.hpp>

namespace rbt_orbbec {
struct ColorCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.Color@1"; };
struct DepthCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.Depth@1"; };
struct LeftIrCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.LeftIr@1"; };
struct RightIrCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.RightIr@1"; };
struct PointsCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.Points@1"; };
struct AccelCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.Accel@1"; };
struct GyroCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.Gyro@1"; };
struct CameraInfoCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.CameraInfo@1"; };
struct ControlCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.Control@1"; };
struct StaticUpdatesCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.StaticUpdates@1"; };
struct StaticSnapshotCapability { static constexpr char TYPE_NAME[] = "rbt.orbbec.StaticSnapshot@1"; };

[[nodiscard]] std::unique_ptr<rbt::Component> make_camera_component(Options options);
// CLI-level hotplug supervision recreates the existing runtime after device
// removal; it does not add a framework executor or reuse old physical history.
[[nodiscard]] int run_camera_supervisor(Options options);
// List supported profiles for the exact selected Gemini335, without streaming.
[[nodiscard]] std::string describe_profiles(const std::string& serial);
} // namespace rbt_orbbec
