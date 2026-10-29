#include <rbt_m3t/app_config.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using rbt_m3t::LinkEstimate;
using rbt_m3t::RectifiedPair;
using rbt_m3t::TrackingEstimate;

void check(bool condition, const std::string& description) {
    if (!condition) {
        throw std::runtime_error("check failed: " + description);
    }
}

void expect_invalid(const std::function<void()>& action, const std::string& description) {
    try {
        action();
    } catch (const std::invalid_argument&) {
        return;
    }
    throw std::runtime_error("expected invalid_argument: " + description);
}

class Fixture {
  public:
    Fixture() {
        static std::atomic<std::uint64_t> next_id { 0U };
        const auto id = next_id.fetch_add(1U, std::memory_order_relaxed);
        root_ = std::filesystem::temp_directory_path()
                / ("rbt-m3t-app-config-" + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())
                   + "-" + std::to_string(id));
        std::filesystem::create_directories(root_ / "configs");
        std::filesystem::create_directories(root_ / "models");
        write(root_ / "models/robot.json", robot_json());
        write(root_ / "initialization.json", initialization_json());
        write(config_path(), config_json());
    }

    ~Fixture() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

    [[nodiscard]] std::filesystem::path config_path() const {
        return root_ / "configs/tracking.json";
    }

    static void write(const std::filesystem::path& path, const std::string& content) {
        std::ofstream output(path, std::ios::binary);
        check(output.good(), "open fixture file for writing");
        output << content;
        check(output.good(), "write fixture file");
    }

    [[nodiscard]] static std::string robot_json() {
        return R"json({
  "schema_version": 1,
  "name": "test_robot",
  "root_link": "base",
  "units": "metres_radians",
  "links": [
    {"name": "base", "mesh": "", "visuals": 0},
    {"name": "tip", "mesh": "", "visuals": 0}
  ],
  "joints": [
    {"name": "5", "type": "revolute", "parent": "base", "child": "tip",
     "origin": [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1],
     "axis": [0, 0, 1], "lower": -1.37, "upper": 4.4906552}
  ]
})json";
    }

    [[nodiscard]] static std::string initialization_json(
        const std::string& left_angle = "-1.37",
        const std::string& right_angle = "4.4906552",
        const std::string& left_pose = "[1,0,0,0.25,0,1,0,0,0,0,1,0,0,0,0,1]",
        const std::string& extra_angles = "",
        bool right_fixed_base = false) {
        return "{\n"
               "  \"schema_version\": 1,\n"
               "  \"coordinate_frame\": \"camera/color_optical\",\n"
               "  \"serial\": \"SN-test\",\n"
               "  \"arms\": [\n"
               "    {\"name\":\"so101_left\",\"base_in_camera\":" + left_pose
               + ",\"joint_angles\":{\"5\":" + left_angle + extra_angles
               + "},\"fixed_base\":true},\n"
               "    {\"name\":\"so101_right\",\"base_in_camera\":[1,0,0,-0.25,0,1,0,0,0,0,1,0,0,0,0,1],"
               "\"joint_angles\":{\"5\":" + right_angle
               + "},\"fixed_base\":" + (right_fixed_base ? "true" : "false") + "}\n"
               "  ]\n"
               "}\n";
    }

    [[nodiscard]] static std::string config_json(
        const std::string& model_points = "200",
        const std::string& initialization = "../initialization.json",
        const std::string& extensions = "") {
        return "{\n"
               "  \"schema_version\": 1,\n"
               "  \"robot_model\": \"../models/robot.json\",\n"
               "  \"initialization\": \"" + initialization + "\",\n"
               "  \"cache_directory\": \"../cache\",\n"
               "  \"model_points\": " + model_points + extensions + "\n"
               "}\n";
    }

    [[nodiscard]] static std::string joint_prior_mapping(
        const std::string& arm = "so101_left",
        const std::string& joint = "5",
        const std::string& source_arm = "left",
        const std::string& source_joint = "shoulder_pan",
        const std::string& scale = "0.017453292519943295",
        const std::string& offset = "0",
        const std::string& deviation = "0.1") {
        return "{\"arm\":\"" + arm + "\",\"joint\":\"" + joint
               + "\",\"source_arm\":\"" + source_arm + "\",\"source_joint\":\""
               + source_joint + "\",\"scale_rad_per_unit\":" + scale
               + ",\"offset_rad\":" + offset + ",\"standard_deviation_rad\":"
               + deviation + "}";
    }

    [[nodiscard]] static std::string joint_prior_extension(
        const std::string& mappings,
        const std::string& options = "\"service_name\":\"joint-state\","
                                         "\"controller_id\":\"controller-1\",") {
        return ",\"joint_priors\":{" + options + "\"mappings\":[" + mappings + "]}";
    }

    [[nodiscard]] static std::string camera_motion_fields(const std::string& options = "") {
        return "\"world_frame\":\"world\",\"pitch_frame\":\"camera/pitch\","
               "\"pitch_provider\":\"head-pitch\",\"depth_frame\":\"camera/depth_optical\","
               "\"mount_in_pitch\":[1,0,0,0,0,1,0,0.125,0,0,1,0,0,0,0,1],"
               "\"arm_bases\":["
               "{\"arm\":\"so101_left\",\"base_in_world\":[1,0,0,1,0,1,0,0,0,0,1,0,0,0,0,1]},"
               "{\"arm\":\"so101_right\",\"base_in_world\":[1,0,0,-1,0,1,0,0,0,0,1,0,0,0,0,1]}]"
               + options;
    }

    [[nodiscard]] static std::string camera_motion_extension(const std::string& fields) {
        return ",\"camera_motion\":{" + fields + "}";
    }

  private:
    std::filesystem::path root_;
};

void test_load_relative_files_and_exact_joint_seeds() {
    Fixture fixture;
    const auto config = rbt_m3t::load_tracking_config(fixture.config_path());

    check(config.serial == "SN-test", "initialization serial");
    check(config.coordinate_frame == "camera/color_optical", "initialization frame");
    check(config.arms.size() == 2U, "exactly two arms");
    check(config.arms[0].name == "so101_left", "left arm name");
    check(config.arms[1].name == "so101_right", "right arm name");
    check(config.arms[0].base_in_camera.matrix()(0, 3) == 0.25F, "row-major base translation");
    check(config.arms[0].joint_angles.at("5") == -1.37, "lower custom joint 5 bound");
    check(config.arms[1].joint_angles.at("5") == 4.4906552, "upper custom joint 5 bound");
    check(config.engine.cache_directory == fixture.root() / "cache", "relative cache path");
    check(config.engine.model_points == 200, "engine default and override");
    check(config.initialization_path == fixture.root() / "initialization.json",
          "resolved initialization path");
    check(config.engine.use_region && config.engine.use_depth && !config.engine.use_texture,
          "default modality options");
    check(config.engine.measured_occlusions && !config.engine.modeled_occlusions
              && !config.engine.region_checking && config.engine.occlusion_image_size == 512,
          "default occlusion options");
    check(!config.engine.use_static_detector && !config.engine.refine_initialization,
          "default initialization flow");
    check(!config.engine.visualize_correspondences && !config.engine.visualize_optimization
              && !config.engine.visualize_results,
          "default visualization options");
    check(config.engine.constraints.empty(), "default constraints");
    check(!config.joint_prior.enabled && config.joint_prior.time_basis == "capture"
              && config.joint_prior.maximum_age_ns == 50'000'000U
              && config.joint_prior.capacity == 128U,
          "absent joint-prior configuration stays disabled with defaults");
    check(!config.camera_motion.enabled && config.camera_motion.time_basis == "capture"
              && config.camera_motion.maximum_interpolation_gap_ms == 200
              && config.camera_motion.maximum_wait_ms == 200
              && config.camera_motion.maximum_sample_age_ms == 1000
              && !config.camera_motion.allow_unknown_uncertainty,
          "absent camera-motion configuration stays disabled with defaults");

    const auto initialization = rbt_m3t::load_tracking_initialization(
        config.initialization_path, config.model);
    check(initialization.arms.size() == config.arms.size()
              && initialization.coordinate_frame == config.coordinate_frame
              && initialization.serial == config.serial,
          "reload measured initialization using resolved path");
}

void test_camera_motion_config_is_opt_in_and_strict() {
    Fixture fixture;
    Fixture::write(fixture.root() / "initialization.json",
                   Fixture::initialization_json("-1.37", "4.4906552",
                                                "[1,0,0,0.25,0,1,0,0,0,0,1,0,0,0,0,1]",
                                                "", true));
    Fixture::write(fixture.config_path(), Fixture::config_json(
        "200", "../initialization.json",
        Fixture::camera_motion_extension(Fixture::camera_motion_fields())));
    const auto config = rbt_m3t::load_tracking_config(fixture.config_path());
    check(config.camera_motion.enabled, "camera-motion object enables motion by default");
    check(config.camera_motion.world_frame == "world"
              && config.camera_motion.pitch_frame == "camera/pitch"
              && config.camera_motion.pitch_provider == "head-pitch"
              && config.camera_motion.depth_frame == "camera/depth_optical",
          "camera-motion frames and provider are parsed");
    check(config.camera_motion.time_basis == "capture"
              && config.camera_motion.maximum_interpolation_gap_ms == 200
              && config.camera_motion.maximum_wait_ms == 200
              && config.camera_motion.maximum_sample_age_ms == 1000
              && !config.camera_motion.allow_unknown_uncertainty,
          "camera-motion options use bounded defaults");
    check(config.camera_motion.mount_in_pitch.matrix()(1, 3) == 0.125F
              && config.camera_motion.arm_bases.size() == 2U
              && config.camera_motion.arm_bases[0].base_in_world.matrix()(0, 3) == 1.0F,
          "camera-motion transforms use row-major metres");

    Fixture::write(fixture.config_path(), Fixture::config_json(
        "200", "../initialization.json",
        Fixture::camera_motion_extension(Fixture::camera_motion_fields(
            ",\"time_basis\":\"received\",\"allow_unknown_uncertainty\":true,"
            "\"maximum_interpolation_gap_ms\":125,\"maximum_wait_ms\":800,"
            "\"maximum_sample_age_ms\":9000"))));
    const auto received = rbt_m3t::load_tracking_config(fixture.config_path());
    check(received.camera_motion.time_basis == "received"
              && received.camera_motion.allow_unknown_uncertainty
              && received.camera_motion.maximum_interpolation_gap_ms == 125
              && received.camera_motion.maximum_wait_ms == 800
              && received.camera_motion.maximum_sample_age_ms == 9000,
          "camera-motion time and uncertainty options are parsed");

    Fixture::write(fixture.config_path(), Fixture::config_json(
        "200", "../initialization.json",
        Fixture::camera_motion_extension("\"enabled\":false")));
    const auto disabled = rbt_m3t::load_tracking_config(fixture.config_path());
    check(!disabled.camera_motion.enabled, "camera-motion can be explicitly disabled");

    const auto expect_invalid_motion = [&fixture](const std::string& fields,
                                                  const std::string& description) {
        Fixture::write(fixture.config_path(), Fixture::config_json(
            "200", "../initialization.json", Fixture::camera_motion_extension(fields)));
        expect_invalid([&fixture] {
            (void)rbt_m3t::load_tracking_config(fixture.config_path());
        }, description);
    };
    const auto base = Fixture::camera_motion_fields();
    expect_invalid_motion(base + ",\"unknown\":true", "unknown camera-motion key");
    expect_invalid_motion(Fixture::camera_motion_fields(
                              ",\"world_frame\":\"other\""),
                          "duplicate camera-motion key");
    auto duplicate_frame = base;
    const auto pitch = duplicate_frame.find("camera/pitch");
    duplicate_frame.replace(pitch, std::string("camera/pitch").size(), "world");
    expect_invalid_motion(duplicate_frame, "camera-motion frames must be distinct");
    auto wrong_depth = base;
    const auto depth = wrong_depth.find("camera/depth_optical");
    wrong_depth.replace(depth, std::string("camera/depth_optical").size(),
                        "camera/color_optical");
    expect_invalid_motion(wrong_depth, "depth frame differs from initialization frame");
    auto bad_mount = base;
    const auto mount = bad_mount.find("[1,0,0,0,0,1,0,0.125");
    bad_mount.replace(mount + 1U, 1U, "2");
    expect_invalid_motion(bad_mount, "camera-motion mount is a proper rigid transform");
    auto missing_arm = base;
    const auto right = missing_arm.find(",{\"arm\":\"so101_right\"");
    missing_arm.erase(right, std::string(",{\"arm\":\"so101_right\",\"base_in_world\":[1,0,0,-1,0,1,0,0,0,0,1,0,0,0,0,1]}").size());
    expect_invalid_motion(missing_arm, "camera-motion arm bases match the arm set");
    expect_invalid_motion(Fixture::camera_motion_fields(",\"maximum_wait_ms\":1001"),
                          "camera-motion wait limit");
    expect_invalid_motion(Fixture::camera_motion_fields(",\"maximum_interpolation_gap_ms\":0"),
                          "camera-motion interpolation gap limit");
    expect_invalid_motion(Fixture::camera_motion_fields(",\"maximum_sample_age_ms\":10001"),
                          "camera-motion sample age limit");
    auto duplicate_arm = base;
    const auto right_arm = duplicate_arm.find("so101_right");
    duplicate_arm.replace(right_arm, std::string("so101_right").size(), "so101_left");
    expect_invalid_motion(duplicate_arm, "camera-motion arm names are unique");

    Fixture::write(fixture.root() / "initialization.json", Fixture::initialization_json());
    expect_invalid_motion(base, "camera-motion requires all arm seeds fixed");
}

void test_reject_missing_malformed_and_stale_config() {
    Fixture fixture;
    Fixture::write(fixture.config_path(), R"json({
      "schema_version":1,
      "robot_model":"../models/robot.json",
      "cache_directory":"../cache"
    })json");
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "missing initialization path");

    Fixture::write(fixture.config_path(), "{ not valid JSON");
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "malformed config JSON");

    Fixture::write(fixture.config_path(), Fixture::config_json("null"));
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "null optional field must not select a default");

    Fixture::write(fixture.config_path(), Fixture::config_json("\"200\""));
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "string value in integer field");
}

void test_reject_missing_init_and_invalid_physical_seeds() {
    Fixture fixture;
    Fixture::write(fixture.config_path(),
                   Fixture::config_json("200", "../missing-initialization.json"));
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "missing initialization file");

    Fixture::write(fixture.root() / "initialization.json",
                   Fixture::initialization_json("-1.370001"));
    Fixture::write(fixture.config_path(), Fixture::config_json());
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "joint angle below URDF limit");

    Fixture::write(fixture.root() / "initialization.json", Fixture::initialization_json(
        "-1.37", "4.4906552", "[-1,0,0,0.25,0,1,0,0,0,0,1,0,0,0,0,1]"));
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "left-handed base transform");

    Fixture::write(fixture.root() / "initialization.json", Fixture::initialization_json(
        "-1.37", "4.4906552", "[1,0,0,0.25,0,1,0,0,0,0,1,0,0,0,0,1]",
        ",\"extra_joint\":0"));
    expect_invalid([&] { (void)rbt_m3t::load_tracking_config(fixture.config_path()); },
                   "extra joint angle");
}

void test_load_nested_options_metadata_and_constraints() {
    Fixture fixture;
    const auto metadata_dir = fixture.root() / "metadata";
    std::filesystem::create_directories(metadata_dir);
    Fixture::write(metadata_dir / "region.yaml", "region: 1\n");
    Fixture::write(metadata_dir / "depth.yaml", "depth: 1\n");
    Fixture::write(metadata_dir / "texture.yaml", "texture: 1\n");
    Fixture::write(metadata_dir / "optimizer.yaml", "optimizer: 1\n");
    Fixture::write(metadata_dir / "refiner.yaml", "refiner: 1\n");
    Fixture::write(metadata_dir / "constraint.yaml", "constraint: 1\n");
    Fixture::write(fixture.config_path(), Fixture::config_json("200", "../initialization.json",
        R"json(,
  "modalities":{"region":false,"depth":true,"texture":true},
  "occlusion":{"measured":false,"modeled":true,"region_checking":true,"image_size":1024},
  "metadata":{"region":"../metadata/region.yaml","depth":"../metadata/depth.yaml",
    "texture":"../metadata/texture.yaml","optimizer":"../metadata/optimizer.yaml",
    "refiner":"../metadata/refiner.yaml"},
  "initialization_flow":{"static_detector":true,"refine":true},
  "visualization":{"correspondences":true,"optimization":true,"results":true},
  "constraints":[
    {"name":"left_hard","arm":"so101_left","link1":"base","link2":"tip",
     "type":"hard","metadata":"../metadata/constraint.yaml"},
    {"name":"right_soft","arm":"so101_right","link1":"tip","link2":"base",
     "type":"soft","metadata":"../metadata/constraint.yaml"}
  ])json"));

    const auto config = rbt_m3t::load_tracking_config(fixture.config_path());
    check(!config.engine.use_region && config.engine.use_depth && config.engine.use_texture,
          "configured modality options");
    check(!config.engine.measured_occlusions && config.engine.modeled_occlusions
              && config.engine.region_checking && config.engine.occlusion_image_size == 1024,
          "configured occlusion options");
    check(config.engine.region_metadata == metadata_dir / "region.yaml"
              && config.engine.depth_metadata == metadata_dir / "depth.yaml"
              && config.engine.texture_metadata == metadata_dir / "texture.yaml"
              && config.engine.optimizer_metadata == metadata_dir / "optimizer.yaml"
              && config.engine.refiner_metadata == metadata_dir / "refiner.yaml",
          "resolved metadata paths");
    check(config.engine.use_static_detector && config.engine.refine_initialization,
          "configured initialization flow");
    check(config.engine.visualize_correspondences && config.engine.visualize_optimization
              && config.engine.visualize_results,
          "configured visualization options");
    check(config.engine.constraints.size() == 2U, "loaded constraints");
    check(config.engine.constraints[0].name == "left_hard"
              && config.engine.constraints[0].arm == "so101_left"
              && config.engine.constraints[0].link1 == "base"
              && config.engine.constraints[0].link2 == "tip"
              && !config.engine.constraints[0].soft,
          "hard constraint mapping");
    check(config.engine.constraints[1].name == "right_soft"
              && config.engine.constraints[1].arm == "so101_right"
              && config.engine.constraints[1].soft,
          "soft constraint mapping");
    check(config.engine.constraints[0].metadata == metadata_dir / "constraint.yaml",
          "resolved constraint metadata path");
}

void test_reject_invalid_nested_options_constraints_and_metadata_paths() {
    Fixture fixture;
    Fixture::write(fixture.root() / "constraint.yaml", "constraint: 1\n");
    const auto expect_config_invalid = [&fixture](const std::string& extensions,
                                                  const std::string& description) {
        Fixture::write(fixture.config_path(), Fixture::config_json("200", "../initialization.json",
                                                                    extensions));
        expect_invalid([&fixture] {
            (void)rbt_m3t::load_tracking_config(fixture.config_path());
        }, description);
    };

    expect_config_invalid(R"json(,"modalities":{"region":"true"})json",
                          "modality must be a boolean");
    expect_config_invalid(R"json(,"modalities":{"unknown":true})json",
                          "unknown modality option");
    expect_config_invalid(R"json(,"modalities":{"region":false,"depth":false,"texture":false})json",
                          "all modalities disabled");
    expect_config_invalid(R"json(,"occlusion":{"image_size":63})json",
                          "occlusion image size lower bound");
    expect_config_invalid(R"json(,"initialization_flow":{"refine":1.0})json",
                          "initialization flow boolean type");
    expect_config_invalid(R"json(,"visualization":{"unknown":true})json",
                          "unknown visualization option");
    expect_config_invalid(R"json(,"constraints":{})json", "constraints must be an array");
    expect_config_invalid(R"json(,"constraints":[{
      "name":"c", "arm":"missing", "link1":"base",
      "link2":"tip", "type":"hard", "metadata":"../constraint.yaml"
    }])json", "unknown constraint arm");
    expect_config_invalid(R"json(,"constraints":[{
      "name":"c", "arm":"so101_left", "link1":"base",
      "link2":"missing", "type":"hard", "metadata":"../constraint.yaml"
    }])json", "unknown constraint link");
    expect_config_invalid(R"json(,"constraints":[{
      "name":"c", "arm":"so101_left", "link1":"base",
      "link2":"base", "type":"hard", "metadata":"../constraint.yaml"
    }])json", "same constraint link");
    expect_config_invalid(R"json(,"constraints":[{
      "name":"c", "arm":"so101_left", "link1":"base",
      "link2":"tip", "type":"other", "metadata":"../constraint.yaml"
    }])json", "unknown constraint type");
    expect_config_invalid(R"json(,"constraints":[
      {"name":"same", "arm":"so101_left", "link1":"base", "link2":"tip",
       "type":"hard", "metadata":"../constraint.yaml"},
      {"name":"same", "arm":"so101_right", "link1":"base", "link2":"tip",
       "type":"soft", "metadata":"../constraint.yaml"}
    ])json", "duplicate constraint name");
    expect_config_invalid(R"json(,"constraints":[{
      "name":"c", "arm":"so101_left", "link1":"base",
      "link2":"tip", "type":"hard"
    }])json", "missing required constraint metadata");

    expect_config_invalid(R"json(,"metadata":{"region":"../missing.yaml"})json",
                          "missing metadata file");
    std::filesystem::create_directories(fixture.root() / "metadata-directory");
    expect_config_invalid(R"json(,"metadata":{"region":"../metadata-directory"})json",
                          "metadata must be a regular file");
    Fixture::write(fixture.root() / "unreadable.yaml", "ignored: true\n");
    std::filesystem::permissions(fixture.root() / "unreadable.yaml",
                                 std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
    expect_config_invalid(R"json(,"metadata":{"region":"../unreadable.yaml"})json",
                          "metadata path must be readable");

    const auto absolute_metadata = (fixture.root() / "constraint.yaml").string();
    expect_config_invalid(",\"metadata\":{\"region\":\"" + absolute_metadata + "\"}",
                          "metadata path must be relative");

    std::string too_many_constraints = R"json(,"constraints":[)json";
    for (int index = 0; index < 65; ++index) {
        if (index != 0) {
            too_many_constraints += ',';
        }
        too_many_constraints += "{}";
    }
    too_many_constraints += "]";
    expect_config_invalid(too_many_constraints, "constraint count limit");
}

void test_joint_prior_config_is_explicit_and_bounded() {
    Fixture fixture;
    const auto mapping = Fixture::joint_prior_mapping();
    Fixture::write(fixture.config_path(), Fixture::config_json(
        "200", "../initialization.json",
        Fixture::joint_prior_extension(mapping,
            "\"service_name\":\"joint-state\",\"controller_id\":\"controller-1\","
            "\"required\":false,\"time_basis\":\"received\","
            "\"maximum_age_ms\":75,\"capacity\":16,")));
    const auto config = rbt_m3t::load_tracking_config(fixture.config_path());
    check(config.joint_prior.enabled, "joint prior object enables priors by default");
    check(!config.joint_prior.required, "joint prior required defaults false");
    check(config.joint_prior.initialize_from_measurement,
          "joint prior measurement initialization defaults true");
    check(config.joint_prior.service_name == "joint-state"
              && config.joint_prior.controller_id == "controller-1",
          "joint prior service and controller identifiers");
    check(config.joint_prior.time_basis == "received", "explicit receipt time basis");
    check(config.joint_prior.maximum_age_ns == 75'000'000U
              && config.joint_prior.capacity == 16U,
          "joint prior age conversion and capacity");
    check(config.joint_prior.mappings.size() == 1U
              && config.joint_prior.mappings[0].joint == "5",
          "partial joint mapping is retained without inferred wrist mappings");

    Fixture::write(fixture.config_path(), Fixture::config_json(
        "200", "../initialization.json", Fixture::joint_prior_extension(mapping)));
    const auto defaults = rbt_m3t::load_tracking_config(fixture.config_path());
    check(defaults.joint_prior.time_basis == "capture"
              && defaults.joint_prior.maximum_age_ns == 50'000'000U
              && defaults.joint_prior.capacity == 128U,
          "enabled joint-prior defaults are strict capture, 50 ms, and 128 samples");

    Fixture::write(fixture.config_path(), Fixture::config_json(
        "200", "../initialization.json", R"json(,"joint_priors":{"enabled":false})json"));
    const auto disabled = rbt_m3t::load_tracking_config(fixture.config_path());
    check(!disabled.joint_prior.enabled, "explicit disabled joint-prior object");
    check(disabled.joint_prior.mappings.empty(), "disabled priors do not invent mappings");

    const auto expect_invalid_joint_prior = [&fixture](const std::string& joint_prior,
                                                        const std::string& description) {
        Fixture::write(fixture.config_path(), Fixture::config_json(
            "200", "../initialization.json", ",\"joint_priors\":" + joint_prior));
        expect_invalid([&fixture] {
            (void)rbt_m3t::load_tracking_config(fixture.config_path());
        }, description);
    };
    expect_invalid_joint_prior(R"json({"service_name":"s","controller_id":"c",)json"
                               R"json("mappings":[{"arm":"so101_left","joint":"5",)json"
                               R"json("source_arm":"left","source_joint":"not_a_joint",)json"
                               R"json("scale_rad_per_unit":1,"offset_rad":0,)json"
                               R"json("standard_deviation_rad":0.1}]})json",
                              "unknown canonical source joint");
    expect_invalid_joint_prior(R"json({"service_name":"s","controller_id":"c",)json"
                               R"json("mappings":[)json" + Fixture::joint_prior_mapping()
                               + "," + Fixture::joint_prior_mapping() + "]}",
                              "duplicate target arm and joint mapping");
    expect_invalid_joint_prior(R"json({"service_name":"s","controller_id":"c",)json"
                               R"json("mappings":[)json"
                               + Fixture::joint_prior_mapping("so101_left", "5", "left",
                                                              "shoulder_pan", "1", "0", "-0.1")
                               + "]}",
                              "negative mapping standard deviation");
    expect_invalid_joint_prior(R"json({"unknown":true})json", "unknown joint-prior key");
    expect_invalid_joint_prior(R"json({"service_name":"s","controller_id":"c",)json"
                               R"json("maximum_age_ms":1001,"mappings":[)json"
                               + Fixture::joint_prior_mapping() + "]}",
                              "joint-prior age outside range");
    expect_invalid_joint_prior(R"json({"service_name":"s","controller_id":"c",)json"
                               R"json("capacity":513,"mappings":[)json"
                               + Fixture::joint_prior_mapping() + "]}",
                              "joint-prior capacity outside range");
    expect_invalid_joint_prior(R"json({"service_name":"s","controller_id":"c",)json"
                               R"json("mappings":[{"arm":"so101_left","joint":"5",)json"
                               R"json("source_arm":"left","source_joint":"shoulder_pan",)json"
                               R"json("scale_rad_per_unit":0,"offset_rad":0,)json"
                               R"json("standard_deviation_rad":0.1}]})json",
                              "zero mapping scale");
    expect_invalid_joint_prior(R"json({"service_name":"s","controller_id":"c",)json"
                               R"json("mappings":[)json"
                               + Fixture::joint_prior_mapping("so101_left", "missing") + "]}",
                              "unknown model joint");
}

[[nodiscard]] rbt::time::ClockIdentity device_clock() {
    std::array<std::uint8_t, 48> source {};
    source[0] = 1U;
    auto clock = rbt::time::ClockIdentity::create(
        rbt::time::ClockKind::Device, rbt::time::ClockSourceId(std::move(source)),
        std::numeric_limits<std::uint64_t>::max());
    check(clock.has_value(), "valid device clock identity");
    return std::move(clock).value();
}

[[nodiscard]] RectifiedPair output_pair(bool known_uncertainty = true) {
    const auto clock = device_clock();
    constexpr std::int64_t color_ns = 9007199254740976LL;
    constexpr std::int64_t depth_ns = 9007199254740993LL;
    RectifiedPair pair {};
    pair.timing.capture = rbt::time::Timestamp { clock, depth_ns };
    pair.timing.received = rbt::time::Timestamp { clock, depth_ns + 100 };
    pair.timing.capture_uncertainty_ns = known_uncertainty
        ? std::optional<std::uint64_t> { 18446744073709551614ULL }
        : std::nullopt;
    pair.color_capture = rbt::time::Timestamp { clock, color_ns };
    pair.skew_ns = 17U;
    pair.frame_id = "camera/color_optical";
    return pair;
}

void test_serializer_preserves_integer_timing_and_row_major_pose() {
    TrackingEstimate estimate;
    Eigen::Affine3f pose = Eigen::Affine3f::Identity();
    pose.matrix()(0, 0) = 0.0F;
    pose.matrix()(0, 1) = -1.0F;
    pose.matrix()(1, 0) = 1.0F;
    pose.matrix()(1, 1) = 0.0F;
    pose.matrix()(0, 3) = 2.0F;
    pose.matrix()(1, 3) = 3.0F;
    pose.matrix()(2, 3) = 4.0F;
    estimate.links.push_back(LinkEstimate { "so101_left", "tip", pose });
    estimate.joint_angles["so101_left"]["5"] = -1.37;

    const auto json = rbt_m3t::tracking_json(estimate, output_pair(), 9007199254740995ULL, 7U);
    check(json.find("\"capture_ns\":9007199254740993") != std::string::npos,
          "exact depth capture timestamp");
    check(json.find("\"color_capture_ns\":9007199254740976") != std::string::npos,
          "exact color capture timestamp");
    check(json.find("\"frame_sequence\":9007199254740995") != std::string::npos,
          "exact frame sequence");
    check(json.find("\"capture_clock_generation\":18446744073709551615")
              != std::string::npos,
          "exact clock generation");
    check(json.find("\"uncertainty_ns\":18446744073709551614") != std::string::npos,
          "exact capture uncertainty");
    check(json.find("\"frame_id\":\"so101_left/tip\",\"pose\":[0,-1,0,2,1,0,0,3,0,0,1,4,0,0,0,1]")
              != std::string::npos,
          "row-major link pose");
    check(json.find("\"algorithm\":\"m3t_region_depth_articulated\"") != std::string::npos,
          "legacy algorithm for region and depth");
    check(json.find("\"modalities\":[\"region\",\"depth\"]") != std::string::npos,
          "default modality names in output");
    check(json.find("\"joint_priors\"") == std::string::npos,
          "disabled joint priors are omitted from output");

    const auto pair = output_pair();
    rbt_m3t::CameraMotionSelection motion;
    motion.camera_in_world = Eigen::Affine3f::Identity();
    motion.world_frame = "world";
    motion.time_basis = "capture";
    motion.lookup.requested_timestamp = *pair.timing.capture;
    motion.lookup.timing_uncertainty_ns = 17U;
    motion.pitch_sample_timestamp = rbt::time::Timestamp {
        pair.timing.capture->clock, pair.timing.capture->ns - 10 };
    motion.pitch_sample_offset_ns = -10;
    motion.pitch_sample_offset_abs_ns = 10U;
    const auto with_motion = rbt_m3t::tracking_json(estimate, pair, 1U, 0U, &motion);
    check(with_motion.find("\"camera_motion\":{\"enabled\":true,\"status\":\"used\",\"used\":true")
              != std::string::npos,
          "camera-motion usage status is serialized");
    check(with_motion.find("\"selection_policy\":\"nearest\"") != std::string::npos
              && with_motion.find("\"pitch_sample_offset_ns\":-10,\"pitch_sample_offset_abs_ns\":10")
                     != std::string::npos,
          "nearest pitch-sample timing is serialized");
    check(with_motion.find("\"camera_in_world\":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]")
              != std::string::npos,
          "camera pose is serialized in row-major form");

    const auto unknown = rbt_m3t::tracking_json(estimate, output_pair(false), 1U, 0U);
    check(unknown.find("\"uncertainty_ns\":null") != std::string::npos,
          "unknown uncertainty remains null");

    estimate.modalities = { "texture" };
    const auto texture = rbt_m3t::tracking_json(estimate, output_pair(), 1U, 0U);
    check(texture.find("\"algorithm\":\"m3t_articulated\"") != std::string::npos,
          "general algorithm for other modality sets");
    check(texture.find("\"modalities\":[\"texture\"]") != std::string::npos,
          "configured modality names in output");

    estimate.joint_prior.enabled = true;
    estimate.joint_prior.status = "applied";
    estimate.joint_prior.time_basis = "received";
    estimate.joint_prior.controller_id = "controller\"one";
    estimate.joint_prior.driver_session = "session-1";
    estimate.joint_prior.sequence = 42U;
    estimate.joint_prior.age_ns = 7U;
    estimate.joint_prior.capture_time_associated = false;
    estimate.joint_prior.priors.push_back(
        rbt_m3t::JointAnglePrior { "so101_left", "5", 0.25, 0.1 });
    estimate.joint_prior.rejected_mappings.push_back("wrist_roll: unavailable");
    const auto with_prior = rbt_m3t::tracking_json(estimate, output_pair(), 1U, 0U);
    check(with_prior.find("\"joint_priors\":{\"enabled\":true,\"status\":\"applied\"")
              != std::string::npos,
          "joint prior status is serialized");
    check(with_prior.find("\"time_basis\":\"received\",\"controller_id\":\"controller\\\"one\"")
              != std::string::npos,
          "receipt time basis and escaped controller id are serialized");
    check(with_prior.find("\"driver_session\":\"session-1\",\"sequence\":42,\"age_ns\":7")
              != std::string::npos,
          "joint prior source session, sequence, and age are serialized");
    check(with_prior.find("\"capture_time_associated\":false,\"prior_count\":1")
              != std::string::npos,
          "receipt association flag and count are serialized");
    check(with_prior.find("\"arm\":\"so101_left\",\"joint\":\"5\",\"angle_rad\":0.25,\"standard_deviation_rad\":0.10000000000000001")
              != std::string::npos,
          "joint prior angle and deviation are serialized");
    check(with_prior.find("\"rejected_mappings\":[\"wrist_roll: unavailable\"]")
              != std::string::npos,
          "mapping rejection reasons are serialized");
    estimate.joint_prior.age_ns.reset();
    const auto unknown_age = rbt_m3t::tracking_json(estimate, output_pair(), 1U, 0U);
    check(unknown_age.find("\"age_ns\":null") != std::string::npos,
          "unknown joint prior age remains null");

    estimate.modalities.clear();
    expect_invalid([&] { (void)rbt_m3t::tracking_json(estimate, output_pair(), 0U, 0U); },
                   "empty output modalities");
    estimate.modalities = { "unknown" };
    expect_invalid([&] { (void)rbt_m3t::tracking_json(estimate, output_pair(), 0U, 0U); },
                   "unknown output modality");
    estimate.modalities = { "depth", "depth" };
    expect_invalid([&] { (void)rbt_m3t::tracking_json(estimate, output_pair(), 0U, 0U); },
                   "duplicate output modality");
}

void test_serializer_rejects_nonrigid_and_nonfinite_poses() {
    TrackingEstimate estimate;
    auto pose = Eigen::Affine3f::Identity();
    pose.matrix()(0, 0) = 2.0F;
    estimate.links.push_back(LinkEstimate { "so101_left", "tip", pose });
    expect_invalid([&] { (void)rbt_m3t::tracking_json(estimate, output_pair(), 0U, 0U); },
                   "non-rigid estimate pose");

    pose = Eigen::Affine3f::Identity();
    pose.matrix()(0, 0) = std::numeric_limits<float>::quiet_NaN();
    estimate.links[0].in_camera = pose;
    expect_invalid([&] { (void)rbt_m3t::tracking_json(estimate, output_pair(), 0U, 0U); },
                   "non-finite estimate pose");
}

} // namespace

int main() {
    try {
        test_load_relative_files_and_exact_joint_seeds();
        test_camera_motion_config_is_opt_in_and_strict();
        test_reject_missing_malformed_and_stale_config();
        test_reject_missing_init_and_invalid_physical_seeds();
        test_load_nested_options_metadata_and_constraints();
        test_reject_invalid_nested_options_constraints_and_metadata_paths();
        test_joint_prior_config_is_explicit_and_bounded();
        test_serializer_preserves_integer_timing_and_row_major_pose();
        test_serializer_rejects_nonrigid_and_nonfinite_poses();
    } catch (const std::exception& error) {
        std::cerr << "M3T app config test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "M3T app config tests passed\n";
    return 0;
}
