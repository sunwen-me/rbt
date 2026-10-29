#include <Eigen/Geometry>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#include <pcl/common/transforms.h>

#include "basic/Manifold.h"
#include "lio/params.h"
#include "lio/prior_map_localization.h"
#include "rbt_adapter/parameters.h"

namespace {

using Clock = std::chrono::steady_clock;
using Point = pcl::PointXYZI;
using Cloud = pcl::PointCloud<Point>;

struct CallbackState {
  std::mutex mutex;
  std::condition_variable changed;
  std::size_t calls = 0;
  BASIC::SE3 map_odom;
};

void require(bool condition, const std::string& message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void loadReviewConfiguration(const std::string& path) {
  LI2Sup::Parameters parameters(path);
  LI2Sup::load_parameters(parameters);

  // Match the existing component's parameter consumption so the actual
  // review YAML is checked by the same parser, including its rbt.* keys.
  (void)parameters.get<std::string>("rbt.input_namespace", "");
  (void)parameters.get<std::string>("rbt.component_id", "super-lio");
  (void)parameters.get<int>("rbt.imu_buffer_capacity", 4000);
  (void)parameters.get<int>("rbt.lidar_buffer_capacity", 32);
  (void)parameters.get<int>("rbt.max_output_points", 200000);
  (void)parameters.get<int>("rbt.max_path_poses", 5000);
  (void)parameters.get<double>("lio.sensor.time_offset", 0.0);
  (void)parameters.get<bool>("rbt.navigation_output", false);
  parameters.check_unknown();
}

BASIC::SE3 expectedMapOdom() {
  const Eigen::Matrix3d rotation =
      Eigen::AngleAxisd(20.0 * M_PI / 180.0, Eigen::Vector3d::UnitZ())
          .toRotationMatrix();
  const BASIC::M3 basic_rotation = rotation.cast<BASIC::scalar>();
  return BASIC::SE3(basic_rotation, BASIC::V3(1.1, -0.7, 0.25));
}

Cloud::Ptr makeCroppedScan(const std::string& map_path,
                           const BASIC::SE3& map_odom) {
  Cloud::Ptr map(new Cloud());
  require(pcl::io::loadPCDFile<Point>(map_path, *map) == 0,
          "failed to read configured prior PCD");
  require(map->size() == 92678,
          "prior PCD point count differs from the reviewed map");

  Cloud::Ptr crop(new Cloud());
  crop->reserve(map->size());
  for (const auto& point : map->points) {
    if (point.x >= -6.0F && point.x <= 2.0F &&
        point.y >= -3.5F && point.y <= 2.0F &&
        point.z >= -0.3F && point.z <= 2.5F) {
      crop->push_back(point);
    }
  }
  crop->width = static_cast<std::uint32_t>(crop->size());
  crop->height = 1;
  crop->is_dense = true;
  require(crop->size() >= 30000,
          "selected prior-map crop is too small for feature registration");

  Cloud::Ptr scan(new Cloud());
  const Eigen::Matrix4f odom_map = map_odom.inverse().matrix().cast<float>();
  pcl::transformPointCloud(*crop, *scan, odom_map);
  scan->is_dense = true;
  return scan;
}

std::pair<double, double> poseErrors(const BASIC::SE3& actual,
                                     const BASIC::SE3& expected) {
  const auto delta = expected.inverse() * actual;
  const double translation_error = delta.t_.norm();
  const double rotation_error =
      Eigen::AngleAxisd(delta.R_.cast<double>()).angle() * 180.0 / M_PI;
  return {translation_error, rotation_error};
}

void submitStartupFrames(LI2Sup::PriorMapLocalization& localization,
                         const Cloud::Ptr& scan) {
  const BASIC::SE3 odom_lidar;
  for (int index = 0; index < LI2Sup::g_prior_map_loc_local_frames; ++index) {
    localization.submit(static_cast<double>(index), scan, odom_lidar);
    // The worker intentionally drops submissions while it is busy. Pace the
    // synthetic scans so each of the configured startup frames is accepted.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
  }
}

void writeResult(const std::string& path,
                 std::size_t map_points,
                 std::size_t crop_points,
                 double translation_error,
                 double rotation_error,
                 bool stale) {
  std::ofstream out(path);
  require(out.good(), "cannot write result JSON");
  out << std::fixed << std::setprecision(6)
      << "{\n"
      << "  \"result\": \"PASS\",\n"
      << "  \"configuration_loaded_by_existing_parser\": true,\n"
      << "  \"kiss_enabled_for_positive_case\": true,\n"
      << "  \"prior_map_points\": " << map_points << ",\n"
      << "  \"synthetic_scan_crop_points\": " << crop_points << ",\n"
      << "  \"initial_max_fitness\": "
      << LI2Sup::g_prior_map_loc_initial_max_fitness << ",\n"
      << "  \"max_fitness\": " << LI2Sup::g_prior_map_loc_max_fitness << ",\n"
      << "  \"translation_error_m\": " << translation_error << ",\n"
      << "  \"rotation_error_deg\": " << rotation_error << ",\n"
      << "  \"navigation_ready_stale_after_2s\": "
      << (stale ? "true" : "false") << "\n"
      << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "usage: prior_map_acceptance REVIEW_CONFIG RESULT_JSON\n";
    return 2;
  }

  try {
    loadReviewConfiguration(argv[1]);
    require(LI2Sup::g_prior_map_loc_enable,
            "review YAML did not enable prior-map localization");
    require(!LI2Sup::g_kiss_matcher_enable,
            "review YAML must keep KISS disabled; test enables it only in memory");
    require(std::abs(LI2Sup::g_prior_map_loc_initial_max_fitness - 0.01) < 1e-12,
            "review YAML initial fitness threshold changed");
    require(std::abs(LI2Sup::g_prior_map_loc_max_fitness - 0.1) < 1e-12,
            "review YAML maximum fitness threshold changed");

    const std::string map_path = LI2Sup::g_prior_map_loc_map_file;
    require(std::filesystem::is_regular_file(map_path),
            "configured reviewed prior map does not exist");
    const BASIC::SE3 expected = expectedMapOdom();
    Cloud::Ptr scan = makeCroppedScan(map_path, expected);
    std::cout << "review_config=" << argv[1] << "\n"
              << "prior_map=" << map_path << "\n"
              << "synthetic_scan_points=" << scan->size() << "\n"
              << "truth_translation=1.1,-0.7,0.25 truth_yaw_deg=20\n";

    // Negative control: with the YAML's original false KISS setting, the
    // existing PriorMapLocalization path must not initialize from startup.
    {
      CallbackState callback;
      LI2Sup::PriorMapLocalization disabled;
      require(disabled.init([&callback](const BASIC::SE3& transform) {
                std::lock_guard lock(callback.mutex);
                ++callback.calls;
                callback.map_odom = transform;
                callback.changed.notify_all();
              }),
              "negative-control prior-map init failed unexpectedly");
      submitStartupFrames(disabled, scan);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      require(!disabled.navigationReady(),
              "KISS-disabled negative control unexpectedly became ready");
      {
        std::lock_guard lock(callback.mutex);
        require(callback.calls == 0,
                "KISS-disabled negative control published map->odom");
      }
      disabled.shutdown();
      std::cout << "kiss_disabled_negative_control=PASS no_initialization\n";
    }

    // Positive control changes only the parsed in-memory KISS enable flag.
    LI2Sup::g_kiss_matcher_enable = true;
    CallbackState callback;
    LI2Sup::PriorMapLocalization localization;
    require(localization.init([&callback](const BASIC::SE3& transform) {
              std::lock_guard lock(callback.mutex);
              ++callback.calls;
              callback.map_odom = transform;
              callback.changed.notify_all();
            }),
            "positive-control prior-map init failed");

    const auto scan_start = Clock::now();
    submitStartupFrames(localization, scan);
    {
      std::unique_lock lock(callback.mutex);
      require(callback.changed.wait_for(lock, std::chrono::seconds(120),
                                        [&callback] { return callback.calls > 0; }),
              "KISS/VGICP did not accept a startup map registration within 120 s");
    }
    const auto callback_time = Clock::now();

    BASIC::SE3 actual;
    {
      std::lock_guard lock(callback.mutex);
      actual = callback.map_odom;
    }
    const auto [translation_error, rotation_error] = poseErrors(actual, expected);
    require(translation_error <= 0.05,
            "accepted map->odom translation error exceeds 0.05 m");
    require(rotation_error <= 1.0,
            "accepted map->odom rotation error exceeds 1 degree");
    require(localization.navigationReady(),
            "accepted startup registration did not set navigationReady");

    const auto stale_deadline = callback_time + std::chrono::milliseconds(2200);
    std::this_thread::sleep_until(stale_deadline);
    const bool stale = !localization.navigationReady();
    require(stale,
            "navigationReady remained true after more than two seconds without registration");

    writeResult(argv[2], 92678, scan->size(), translation_error,
                rotation_error, stale);
    std::cout << std::fixed << std::setprecision(6)
              << "kiss_enabled_positive_control=PASS map_to_odom="
              << actual.t_.transpose() << " translation_error_m="
              << translation_error << " rotation_error_deg=" << rotation_error
              << " navigation_ready=1 stale_after_2s=1 elapsed_s="
              << std::chrono::duration<double>(callback_time - scan_start).count()
              << "\n";
    localization.shutdown();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "prior_map_acceptance=FAIL reason=" << error.what() << "\n";
    return 1;
  }
}
