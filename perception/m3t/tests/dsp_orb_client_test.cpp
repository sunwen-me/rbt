// SPDX-License-Identifier: MIT
#include "rbt_m3t/dsp_orb.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "dsp_orb_client_test: %s\n", message);
    ++g_failures;
}

void set_mode(const char* mode) {
    if (setenv("RBT_M3T_DSP_FAKE_MODE", mode, 1) != 0)
        throw std::runtime_error("could not set fake DSP mode");
}

template <typename Function>
bool throws_runtime(Function&& function) {
    try {
        function();
    } catch (const std::runtime_error&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

template <typename Function>
Function load_control(void* library, const char* name) {
    dlerror();
    void* symbol = dlsym(library, name);
    const char* error = dlerror();
    if (error != nullptr || symbol == nullptr)
        throw std::runtime_error(std::string("missing fake control ") + name);
    return reinterpret_cast<Function>(symbol);
}

struct FakeControls {
    using Reset = void (*)();
    using Count = int (*)();

    explicit FakeControls(void* library)
        : reset(load_control<Reset>(library, "rbt_m3t_orb_fake_reset")),
          opens(load_control<Count>(library, "rbt_m3t_orb_fake_open_count")),
          closes(load_control<Count>(library, "rbt_m3t_orb_fake_close_count")),
          executes(load_control<Count>(library,
                                       "rbt_m3t_orb_fake_execute_count")),
          prepares(load_control<Count>(library,
                                       "rbt_m3t_orb_fake_prepare_count")),
          prepares_before_open(load_control<Count>(
              library, "rbt_m3t_orb_fake_prepare_before_open_count")),
          uri_valid(load_control<Count>(library,
                                        "rbt_m3t_orb_fake_uri_valid")),
          adsp_path_valid(load_control<Count>(
              library, "rbt_m3t_orb_fake_adsp_path_valid")) {}

    Reset reset;
    Count opens;
    Count closes;
    Count executes;
    Count prepares;
    Count prepares_before_open;
    Count uri_valid;
    Count adsp_path_valid;
};

class ScopedEnvironment {
public:
    explicit ScopedEnvironment(const char* name) : name_(name) {
        const char* previous = std::getenv(name_.c_str());
        was_set_ = previous != nullptr;
        if (was_set_) previous_ = previous;
    }

    ~ScopedEnvironment() { restore(); }

    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

    void set(const std::string& value) {
        if (setenv(name_.c_str(), value.c_str(), 1) != 0)
            throw std::runtime_error("could not set test environment " + name_);
    }

    void unset() {
        if (unsetenv(name_.c_str()) != 0)
            throw std::runtime_error("could not unset test environment " + name_);
    }

    bool matches(const std::string& value) const {
        const char* current = std::getenv(name_.c_str());
        return current != nullptr && value == current;
    }

    bool is_unset() const { return std::getenv(name_.c_str()) == nullptr; }

    bool matches_original() const {
        return was_set_ ? matches(previous_) : is_unset();
    }

    bool originally_set() const { return was_set_; }
    const std::string& original_value() const { return previous_; }

private:
    void restore() noexcept {
        if (was_set_)
            setenv(name_.c_str(), previous_.c_str(), 1);
        else
            unsetenv(name_.c_str());
    }

    std::string name_;
    bool was_set_ = false;
    std::string previous_;
};

std::string expected_adsp_library_path(const char* module_path,
                                       bool previous_was_set,
                                       const std::string& previous_value) {
    std::string expected =
        std::filesystem::canonical(module_path).parent_path().string();
    if (previous_was_set) expected += ";" + previous_value;
    return expected;
}

std::string expected_module_uri(const char* module_path) {
    return std::string("file:///") +
           std::filesystem::canonical(module_path).filename().string() +
           "?rbt_m3t_orb_skel_handle_invoke&_modver=1.0&_dom=cdsp";
}

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        directory_ = std::filesystem::temp_directory_path() /
            ("rbt-m3t-dsp-client-" +
             std::to_string(static_cast<long long>(getpid())));
        std::error_code error;
        if (!std::filesystem::create_directory(directory_, error) || error)
            throw std::runtime_error("could not create a unique DSP test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    const std::filesystem::path& path() const { return directory_; }

private:
    std::filesystem::path directory_;
};

cv::Mat packed_image_view() {
    cv::Mat storage(2, 7, CV_8UC1, cv::Scalar(0xee));
    cv::Mat image = storage(cv::Rect(1, 0, 4, 2));
    for (int row = 0; row < image.rows; ++row) {
        for (int col = 0; col < image.cols; ++col)
            image.at<uint8_t>(row, col) =
                static_cast<uint8_t>(row * 10 + col);
    }
    return image;
}

cv::Mat descriptor_view(int rows, int storage_columns, int x_offset,
                        uint8_t base) {
    cv::Mat storage(rows, storage_columns, CV_8UC1, cv::Scalar(0xee));
    cv::Mat descriptors = storage(
        cv::Rect(x_offset, 0, 32, rows));
    for (int row = 0; row < rows; ++row)
        std::memset(descriptors.ptr<uint8_t>(row),
                    static_cast<uint8_t>(base + row * 0x11u), 32u);
    return descriptors;
}

bool has_empty_outputs(const std::vector<cv::KeyPoint>& keypoints,
                       const cv::Mat& descriptors) {
    return keypoints.empty() && descriptors.empty();
}

bool path_entry_exists(const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (error == std::errc::no_such_file_or_directory) return false;
    if (error)
        throw std::runtime_error("could not inspect default DSP alias: " +
                                 path.string());
    return status.type() != std::filesystem::file_type::not_found;
}

bool canonical_matches(const std::filesystem::path& path,
                       const std::filesystem::path& expected) {
    std::error_code error;
    return std::filesystem::canonical(path, error) == expected && !error;
}

void create_occupied_file(const std::filesystem::path& path,
                          const char* contents) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL,
                                  0600);
    if (descriptor < 0)
        throw std::runtime_error("could not create occupied DSP alias: " +
                                 path.string());

    const size_t length = std::strlen(contents);
    size_t written = 0u;
    while (written < length) {
        const ssize_t result = ::write(descriptor, contents + written,
                                       length - written);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) {
            ::close(descriptor);
            std::error_code error;
            std::filesystem::remove(path, error);
            throw std::runtime_error("could not write occupied DSP alias: " +
                                     path.string());
        }
        written += static_cast<size_t>(result);
    }
    if (::close(descriptor) != 0) {
        std::error_code error;
        std::filesystem::remove(path, error);
        throw std::runtime_error("could not close occupied DSP alias: " +
                                 path.string());
    }
}

void prepare_extract_outputs(std::vector<cv::KeyPoint>* keypoints,
                             cv::Mat* descriptors) {
    keypoints->emplace_back(cv::Point2f(7.0f, 8.0f), 4.0f);
    *descriptors = cv::Mat(1, 32, CV_8UC1, cv::Scalar(0xff));
}

void expect_extract_failure(rbt_m3t::dsp::Orb& orb, const cv::Mat& image,
                            const char* fake_mode, const char* message) {
    set_mode(fake_mode);
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
    prepare_extract_outputs(&keypoints, &descriptors);
    check(throws_runtime([&] { orb.extract(image, keypoints, descriptors); }),
          message);
    check(has_empty_outputs(keypoints, descriptors),
          "extract failure clears both outputs");
}

void expect_match_failure(rbt_m3t::dsp::Orb& orb, const cv::Mat& query,
                          const cv::Mat& train, const char* fake_mode,
                          const char* message) {
    set_mode(fake_mode);
    check(throws_runtime([&] { (void)orb.match_2nn(query, train); }), message);
}

void run_session_tests(const char* stub_path, const char* module_path,
                       const char* work_directory, FakeControls& fake) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    const std::string previous_adsp_path = "preserved-first;preserved-second";
    adsp_path.set(previous_adsp_path);
    expected_adsp_path.set(expected_adsp_library_path(
        module_path, true, previous_adsp_path));
    set_mode("ok");
    fake.reset();
    if (setenv("RBT_M3T_DSP_STUB", stub_path, 1) != 0 ||
        setenv("RBT_M3T_DSP_MODULE", module_path, 1) != 0)
        throw std::runtime_error("could not set fake DSP paths");

    const std::filesystem::path alternate_module =
        std::filesystem::path(work_directory) / "alternate-dsp-module.so";
    const std::filesystem::path alternate_stub =
        std::filesystem::path(work_directory) / "alternate-dsp-stub.so";
    {
        std::ofstream output(alternate_module, std::ios::binary);
        output << "fake module path";
    }
    std::filesystem::copy_file(
        stub_path, alternate_stub,
        std::filesystem::copy_options::overwrite_existing);
    check(std::filesystem::is_regular_file(alternate_module),
          "alternate module fixture exists");
    check(std::filesystem::is_regular_file(alternate_stub),
          "alternate stub fixture exists");

    rbt_m3t::dsp::OrbConfig config;
    config.n_features = 16;
    config.n_levels = 3;
    config.scale_factor = 1.2f;
    cv::Mat image = packed_image_view();
    cv::Mat query = descriptor_view(2, 37, 2, 0x11u);
    cv::Mat train = descriptor_view(2, 36, 1, 0x33u);

    {
        rbt_m3t::dsp::Orb first(config);
        check(fake.opens() == 1 && fake.executes() == 1,
              "constructor opens one session and performs capability RPC");
        check(fake.prepares() == 1 && fake.prepares_before_open() == 1,
              "CDSP preparation is called once before FastRPC open");
        check(fake.uri_valid() == 1, "open receives the expected module URI");
        check(fake.adsp_path_valid() == 1 &&
                  adsp_path.matches(expected_adsp_library_path(
                      module_path, true, previous_adsp_path)),
              "open sees the canonical module directory prepended to the existing path");

        std::vector<cv::KeyPoint> keypoints;
        cv::Mat descriptors;
        prepare_extract_outputs(&keypoints, &descriptors);
        first.extract(image, keypoints, descriptors);
        check(keypoints.size() == 2u && descriptors.rows == 2 &&
                  descriptors.cols == 32 && descriptors.type() == CV_8UC1,
              "extract returns typed keypoints and descriptor rows");
        if (keypoints.size() == 2u && descriptors.rows == 2) {
            check(keypoints[0].pt.x == 1.25f && keypoints[0].pt.y == 0.5f &&
                      keypoints[0].size == 7.0f &&
                      keypoints[0].angle == 90.0f &&
                      keypoints[0].octave == 1,
                  "extract maps the DSP keypoint fields");
            check(descriptors.ptr<uint8_t>(0)[0] == 0xa0u &&
                      descriptors.ptr<uint8_t>(1)[31] == 0xa1u,
                  "extract copies descriptors from feature-capacity layout");
        }

        {
            rbt_m3t::dsp::Orb second(config);
            check(fake.opens() == 1,
                  "compatible Orb instances share the FastRPC session");
            const auto matches = second.match_2nn(query, train);
            check(matches.size() == 2u && matches[0].size() == 2u &&
                      matches[1].size() == 2u,
                  "match returns two neighbors for each query");
            if (matches.size() == 2u && matches[0].size() == 2u &&
                matches[1].size() == 2u) {
                check(matches[0][0].queryIdx == 0 &&
                          matches[0][0].trainIdx == 1 &&
                          matches[0][0].distance == 3.0f &&
                          matches[0][1].trainIdx == 0 &&
                          matches[0][1].distance == 8.0f,
                      "match maps sorted indices and Hamming distances");
                check(matches[1][0].queryIdx == 1 &&
                          matches[1][0].trainIdx == 0 &&
                          matches[1][1].trainIdx == 1,
                      "match preserves query-row ordering");
            }

            const cv::Mat empty_query(0, 32, CV_8UC1);
            const int before_empty = fake.executes();
            check(second.match_2nn(empty_query, train).empty(),
                  "zero queries return an empty result");
            check(fake.executes() == before_empty,
                  "zero queries do not send an empty payload RPC");

            const cv::Mat empty_train(0, 32, CV_8UC1);
            const auto no_train = second.match_2nn(query.rowRange(0, 1),
                                                   empty_train);
            check(no_train.size() == 1u && no_train[0].empty(),
                  "zero training rows map to zero neighbors");
            const cv::Mat one_train = descriptor_view(1, 35, 1, 0x33u);
            const auto one_neighbor =
                second.match_2nn(query.rowRange(0, 1), one_train);
            check(one_neighbor.size() == 1u && one_neighbor[0].size() == 1u &&
                      one_neighbor[0][0].trainIdx == 0,
                  "one training row maps to one neighbor");

            set_mode("extract_empty");
            first.extract(image, keypoints, descriptors);
            check(has_empty_outputs(keypoints, descriptors),
                  "empty extract clears previous typed outputs");

            expect_extract_failure(first, image, "extract_rpc_error",
                                   "extract propagates FastRPC errors");
            expect_extract_failure(first, image, "extract_bad_version",
                                   "extract rejects malformed response versions");
            expect_extract_failure(first, image, "extract_bad_mask",
                                   "extract rejects unsupported response features");
            expect_extract_failure(first, image, "extract_bad_reserved",
                                   "extract rejects nonzero response reserved fields");
            expect_extract_failure(first, image, "extract_bad_count",
                                   "extract rejects out-of-capacity feature counts");
            expect_extract_failure(first, image, "extract_bad_point",
                                   "extract rejects malformed keypoint values");
            expect_extract_failure(first, image, "extract_bad_status",
                                   "extract propagates DSP operation status");
            set_mode("ok");
            cv::Mat invalid_color(2, 4, CV_8UC3, cv::Scalar(0));
            expect_extract_failure(first, invalid_color, "ok",
                                   "extract rejects non-grayscale input");

            expect_match_failure(second, query, train, "match_rpc_error",
                                 "match propagates FastRPC errors");
            expect_match_failure(second, query, train, "match_bad_version",
                                 "match rejects malformed response versions");
            expect_match_failure(second, query, train, "match_bad_reserved",
                                 "match rejects nonzero response reserved fields");
            expect_match_failure(second, query, train, "match_bad_mask",
                                 "match rejects unsupported response features");
            expect_match_failure(second, query, train, "match_bad_count",
                                 "match rejects malformed result counts");
            expect_match_failure(second, query, train, "match_bad_index",
                                 "match rejects out-of-range indices");
            expect_match_failure(second, query, train, "match_bad_order",
                                 "match rejects unsorted neighbors");
            expect_match_failure(second, query, train, "match_bad_distance",
                                 "match rejects invalid neighbor distances");

            check(setenv("RBT_M3T_DSP_MODULE", alternate_module.c_str(), 1) ==
                      0,
                  "can change requested module path for conflict test");
            check(throws_runtime([&] { rbt_m3t::dsp::Orb conflict(config); }),
                  "active session rejects a different requested module path");
            check(setenv("RBT_M3T_DSP_MODULE", module_path, 1) == 0,
                  "restores requested module path");
            check(setenv("RBT_M3T_DSP_STUB", alternate_stub.c_str(), 1) ==
                      0,
                  "can change requested stub path for conflict test");
            check(throws_runtime([&] { rbt_m3t::dsp::Orb conflict(config); }),
                  "active session rejects a different requested stub path");
            check(setenv("RBT_M3T_DSP_STUB", stub_path, 1) == 0,
                  "restores requested stub path");
        }
        check(fake.executes() > 10,
              "fake observed extract and batched match RPCs");
        check(fake.closes() == 0,
              "session remains open while an Orb instance is alive");
    }
    check(fake.closes() == 1,
          "last Orb instance closes the shared session exactly once");
    check(adsp_path.matches(previous_adsp_path),
          "last Orb restores the previous ADSP_LIBRARY_PATH");
}

void run_capability_failure(const char* stub_path, const char* module_path,
                            FakeControls& fake, const char* mode,
                            const char* message) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    const std::string previous_adsp_path = "capability-prior;entry";
    adsp_path.set(previous_adsp_path);
    expected_adsp_path.set(expected_adsp_library_path(
        module_path, true, previous_adsp_path));
    fake.reset();
    set_mode(mode);
    setenv("RBT_M3T_DSP_STUB", stub_path, 1);
    setenv("RBT_M3T_DSP_MODULE", module_path, 1);
    rbt_m3t::dsp::OrbConfig config;
    check(throws_runtime([&] { rbt_m3t::dsp::Orb orb(config); }), message);
    check(fake.opens() == 1 && fake.executes() == 1 && fake.closes() == 1,
          "failed capability handshake closes the opened stub session");
    check(fake.prepares() == 1 && fake.prepares_before_open() == 1 &&
              fake.adsp_path_valid() == 1,
          "failed capability handshake still prepares CDSP before open");
    check(adsp_path.matches(previous_adsp_path),
          "constructor exception restores ADSP_LIBRARY_PATH");
}

void run_default_path_test(FakeControls& fake, const char* stub_path,
                           const char* module_path) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    expected_adsp_path.set(expected_adsp_library_path(
        module_path, adsp_path.originally_set(), adsp_path.original_value()));
    fake.reset();
    set_mode("ok");
    const std::filesystem::path executable_directory =
        std::filesystem::canonical("/proc/self/exe").parent_path();
    const std::filesystem::path default_stub =
        executable_directory / "librbt_m3t_orb_stub.so";
    const std::filesystem::path default_module =
        executable_directory / "librbt_m3t_orb_skel.so";
    std::error_code error;
    const std::filesystem::path fake_stub =
        std::filesystem::canonical(stub_path);
    const std::filesystem::path fake_module =
        std::filesystem::canonical(module_path);
    const bool stub_exists = path_entry_exists(default_stub);
    const bool module_exists = path_entry_exists(default_module);
    if (stub_exists && !canonical_matches(default_stub, fake_stub))
        throw std::runtime_error(
            "default DSP stub path does not match the fake stub fixture");
    if (module_exists && !canonical_matches(default_module, fake_module)) {
        throw std::runtime_error(
            "default DSP module path does not match the fake module fixture");
    }
    const bool created_stub = !stub_exists;
    const bool created_module = !module_exists;
    if (created_stub) {
        std::filesystem::create_symlink(std::filesystem::absolute(stub_path),
                                        default_stub, error);
        if (error) throw std::runtime_error("could not create default stub alias");
    }
    if (created_module) {
        std::filesystem::create_symlink(std::filesystem::absolute(module_path),
                                        default_module, error);
        if (error) {
            if (created_stub) std::filesystem::remove(default_stub);
            throw std::runtime_error("could not create default module alias");
        }
    }
    unsetenv("RBT_M3T_DSP_STUB");
    unsetenv("RBT_M3T_DSP_MODULE");
    rbt_m3t::dsp::OrbConfig config;
    try {
        {
            rbt_m3t::dsp::Orb orb(config);
            check(fake.opens() == 1 && fake.executes() == 1 &&
                      fake.prepares() == 1 &&
                      fake.prepares_before_open() == 1 &&
                      fake.uri_valid() == 1 &&
                      fake.adsp_path_valid() == 1,
                  "default resolver finds stub and module beside the executable");
        }
        check(adsp_path.matches_original(),
              "default-resolved session restores the prior library path");
    } catch (...) {
        if (created_module) std::filesystem::remove(default_module);
        if (created_stub) std::filesystem::remove(default_stub);
        throw;
    }
    if (created_module) std::filesystem::remove(default_module);
    if (created_stub) std::filesystem::remove(default_stub);
    check(fake.closes() == 1,
          "default-resolved session closes after its final Orb is destroyed");
}

void run_unset_adsp_path_test(const char* stub_path, const char* module_path,
                              FakeControls& fake) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    adsp_path.unset();
    expected_adsp_path.set(
        expected_adsp_library_path(module_path, false, std::string()));
    fake.reset();
    set_mode("ok");
    setenv("RBT_M3T_DSP_STUB", stub_path, 1);
    setenv("RBT_M3T_DSP_MODULE", module_path, 1);
    {
        rbt_m3t::dsp::OrbConfig config;
        rbt_m3t::dsp::Orb orb(config);
        check(fake.adsp_path_valid() == 1,
              "unset library path becomes the canonical module directory only");
    }
    check(adsp_path.is_unset(),
          "session restores an originally unset ADSP_LIBRARY_PATH");
}

void run_empty_adsp_path_test(const char* stub_path, const char* module_path,
                              FakeControls& fake) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    adsp_path.set(std::string());
    expected_adsp_path.set(
        expected_adsp_library_path(module_path, true, std::string()));
    fake.reset();
    set_mode("ok");
    setenv("RBT_M3T_DSP_STUB", stub_path, 1);
    setenv("RBT_M3T_DSP_MODULE", module_path, 1);
    {
        rbt_m3t::dsp::OrbConfig config;
        rbt_m3t::dsp::Orb orb(config);
        check(fake.adsp_path_valid() == 1,
              "empty existing library path is preserved as an empty list entry");
    }
    check(adsp_path.matches(std::string()),
          "session restores an originally empty ADSP_LIBRARY_PATH exactly");
}

void run_custom_module_basename_test(const char* stub_path,
                                     const char* module_path,
                                     FakeControls& fake) {
    ScopedTempDirectory temporary_directory;
    const std::filesystem::path custom_module =
        temporary_directory.path() / "custom-orb_module.v2.so";
    std::filesystem::copy_file(
        module_path, custom_module,
        std::filesystem::copy_options::overwrite_existing);

    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    ScopedEnvironment expected_uri("RBT_M3T_DSP_FAKE_EXPECTED_MODULE_URI");
    ScopedEnvironment requested_module("RBT_M3T_DSP_MODULE");
    ScopedEnvironment requested_stub("RBT_M3T_DSP_STUB");
    const std::string previous_adsp_path = "custom-prior;entry";
    adsp_path.set(previous_adsp_path);
    expected_adsp_path.set(expected_adsp_library_path(
        custom_module.c_str(), true, previous_adsp_path));
    expected_uri.set(expected_module_uri(custom_module.c_str()));
    requested_module.set(custom_module.string());
    requested_stub.set(std::filesystem::canonical(stub_path).string());

    fake.reset();
    set_mode("ok");
    {
        rbt_m3t::dsp::OrbConfig config;
        rbt_m3t::dsp::Orb orb(config);
        check(fake.opens() == 1 && fake.prepares() == 1 &&
                  fake.prepares_before_open() == 1 && fake.uri_valid() == 1 &&
                  fake.adsp_path_valid() == 1,
              "custom canonical module basename reaches FastRPC open safely");
    }
    check(adsp_path.matches(previous_adsp_path),
          "custom module session restores the previous library path");
}

void expect_unsafe_module_rejection(const std::filesystem::path& module_path,
                                    const char* expected_error,
                                    ScopedEnvironment& requested_module,
                                    ScopedEnvironment& adsp_path,
                                    FakeControls& fake) {
    requested_module.set(module_path.string());
    fake.reset();
    set_mode("ok");
    std::string actual_error;
    try {
        rbt_m3t::dsp::OrbConfig config;
        rbt_m3t::dsp::Orb orb(config);
    } catch (const std::runtime_error& error) {
        actual_error = error.what();
    }
    check(actual_error == expected_error,
          "unsafe module URI path is rejected with a clear error");
    check(fake.prepares() == 0 && fake.opens() == 0 && fake.executes() == 0 &&
              fake.closes() == 0,
          "unsafe module URI path is rejected before any RPC");
    check(adsp_path.matches("unsafe-path-prior;entry"),
          "unsafe module URI rejection does not change ADSP_LIBRARY_PATH");
}

void run_unsafe_module_path_tests(const char* stub_path,
                                  const char* module_path,
                                  FakeControls& fake) {
    ScopedTempDirectory temporary_directory;
    const std::filesystem::path bad_basename =
        temporary_directory.path() / "bad?module.so";
    std::filesystem::copy_file(
        module_path, bad_basename,
        std::filesystem::copy_options::overwrite_existing);
    const std::filesystem::path bad_directory =
        temporary_directory.path() / "bad;directory";
    std::filesystem::create_directory(bad_directory);
    const std::filesystem::path bad_parent_module =
        bad_directory / "safe-module.so";
    std::filesystem::copy_file(
        module_path, bad_parent_module,
        std::filesystem::copy_options::overwrite_existing);

    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment requested_module("RBT_M3T_DSP_MODULE");
    ScopedEnvironment requested_stub("RBT_M3T_DSP_STUB");
    adsp_path.set("unsafe-path-prior;entry");
    requested_stub.set(std::filesystem::canonical(stub_path).string());
    expect_unsafe_module_rejection(
        bad_basename,
        "DSP module basename contains characters unsafe for a QuRT file URI",
        requested_module, adsp_path, fake);
    expect_unsafe_module_rejection(
        bad_parent_module,
        "DSP module directory contains the ADSP_LIBRARY_PATH separator ';'",
        requested_module, adsp_path, fake);
}

void run_prepare_failure_test(const char* stub_path, const char* module_path,
                              FakeControls& fake) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    const std::string previous_adsp_path = "prepare-prior;entry";
    adsp_path.set(previous_adsp_path);
    expected_adsp_path.set(expected_adsp_library_path(
        module_path, true, previous_adsp_path));
    fake.reset();
    set_mode("prepare_failure");
    setenv("RBT_M3T_DSP_STUB", stub_path, 1);
    setenv("RBT_M3T_DSP_MODULE", module_path, 1);

    std::string error_message;
    try {
        rbt_m3t::dsp::OrbConfig config;
        rbt_m3t::dsp::Orb orb(config);
    } catch (const std::runtime_error& error) {
        error_message = error.what();
    }
    check(error_message ==
              "FastRPC CDSP preparation failed with code -44",
          "CDSP preparation failure is reported clearly");
    check(fake.prepares() == 1 && fake.opens() == 0 && fake.executes() == 0 &&
              fake.closes() == 0,
          "CDSP preparation failure prevents open and execute");
    check(adsp_path.matches(previous_adsp_path),
          "CDSP preparation exception restores the previous library path");
}

void run_external_adsp_change_test(const char* stub_path,
                                   const char* module_path,
                                   FakeControls& fake) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    const std::string previous_adsp_path = "external-prior;entry";
    adsp_path.set(previous_adsp_path);
    expected_adsp_path.set(expected_adsp_library_path(
        module_path, true, previous_adsp_path));
    fake.reset();
    set_mode("ok");
    setenv("RBT_M3T_DSP_STUB", stub_path, 1);
    setenv("RBT_M3T_DSP_MODULE", module_path, 1);

    const std::string external_value = "external-update;preserve-this";
    {
        rbt_m3t::dsp::OrbConfig config;
        rbt_m3t::dsp::Orb orb(config);
        check(adsp_path.matches(expected_adsp_library_path(
                  module_path, true, previous_adsp_path)),
              "active session has installed the module search path");
        adsp_path.set(external_value);
    }
    check(adsp_path.matches(external_value),
          "session cleanup preserves an external library-path change");
    check(fake.closes() == 1,
          "external environment update does not prevent session close");
}

void run_session_lifetime_race_test(const char* stub_path,
                                    const char* module_path,
                                    FakeControls& fake) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    const std::string previous_adsp_path = "race-prior;entry";
    adsp_path.set(previous_adsp_path);
    expected_adsp_path.set(expected_adsp_library_path(
        module_path, true, previous_adsp_path));
    fake.reset();
    set_mode("ok");
    setenv("RBT_M3T_DSP_STUB", stub_path, 1);
    setenv("RBT_M3T_DSP_MODULE", module_path, 1);

    constexpr int kThreadCount = 4;
    constexpr int kSessionsPerThread = 20;
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(kThreadCount);
    for (int worker = 0; worker < kThreadCount; ++worker) {
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < kSessionsPerThread;
                 ++iteration) {
                try {
                    rbt_m3t::dsp::OrbConfig config;
                    rbt_m3t::dsp::Orb orb(config);
                } catch (...) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    check(failures.load() == 0,
          "concurrent session construction and teardown succeeds");
    check(fake.opens() == fake.closes() &&
              fake.prepares() == fake.opens() &&
              fake.prepares_before_open() == fake.opens() &&
              fake.executes() == fake.opens() &&
              fake.adsp_path_valid() == fake.opens(),
          "concurrent session lifetimes prepare, open, and close without path drift");
    check(adsp_path.matches(previous_adsp_path),
          "concurrent session lifetimes restore the original library path");
}

bool run_occupied_default_alias_test(FakeControls& fake,
                                     const char* stub_path,
                                     const char* module_path) {
    fake.reset();
    set_mode("ok");
    unsetenv("RBT_M3T_DSP_STUB");
    unsetenv("RBT_M3T_DSP_MODULE");

    const std::filesystem::path executable_directory =
        std::filesystem::canonical("/proc/self/exe").parent_path();
    const std::filesystem::path default_stub =
        executable_directory / "librbt_m3t_orb_stub.so";
    const std::filesystem::path default_module =
        executable_directory / "librbt_m3t_orb_skel.so";
    const std::filesystem::path fake_stub =
        std::filesystem::canonical(stub_path);
    const std::filesystem::path fake_module =
        std::filesystem::canonical(module_path);

    std::error_code error;
    const bool stub_exists = path_entry_exists(default_stub);
    const bool module_exists = path_entry_exists(default_module);
    bool created_stub = false;
    bool created_module = false;
    const char* expected_error = nullptr;
    bool preexisting_mismatch = false;

    if (stub_exists && !canonical_matches(default_stub, fake_stub)) {
        expected_error =
            "default DSP stub path does not match the fake stub fixture";
        preexisting_mismatch = true;
    } else if (module_exists &&
               !canonical_matches(default_module, fake_module)) {
        expected_error =
            "default DSP module path does not match the fake module fixture";
        preexisting_mismatch = true;
    } else if (!stub_exists) {
        create_occupied_file(default_stub,
                             "occupied non-fake DSP stub path");
        created_stub = true;
        expected_error =
            "default DSP stub path does not match the fake stub fixture";
    } else if (!module_exists) {
        create_occupied_file(default_module,
                             "occupied non-fake DSP module path");
        created_module = true;
        expected_error =
            "default DSP module path does not match the fake module fixture";
    } else {
        check(false,
              "cannot exercise occupied-alias guard because both default aliases already match fixtures");
        return true;
    }

    std::string actual_error;
    try {
        run_default_path_test(fake, stub_path, module_path);
    } catch (const std::runtime_error& exception) {
        actual_error = exception.what();
    }
    check(actual_error == expected_error,
          "occupied non-fake default alias is rejected before Orb startup");
    check(fake.opens() == 0 && fake.executes() == 0 && fake.closes() == 0,
          "occupied non-fake default alias causes no fake RPC or hardware call");

    if (created_module) std::filesystem::remove(default_module, error);
    if (created_stub) std::filesystem::remove(default_stub, error);
    return !preexisting_mismatch;
}

void run_close_failure_test(const char* stub_path, const char* module_path,
                            FakeControls& fake) {
    ScopedEnvironment adsp_path("ADSP_LIBRARY_PATH");
    ScopedEnvironment expected_adsp_path("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    expected_adsp_path.set(expected_adsp_library_path(
        module_path, adsp_path.originally_set(), adsp_path.original_value()));
    fake.reset();
    set_mode("ok");
    setenv("RBT_M3T_DSP_STUB", stub_path, 1);
    setenv("RBT_M3T_DSP_MODULE", module_path, 1);
    FILE* captured_stderr = std::tmpfile();
    if (captured_stderr == nullptr)
        throw std::runtime_error("could not create stderr capture");
    const int saved_stderr = dup(STDERR_FILENO);
    if (saved_stderr < 0 || dup2(fileno(captured_stderr), STDERR_FILENO) < 0) {
        if (saved_stderr >= 0) ::close(saved_stderr);
        std::fclose(captured_stderr);
        throw std::runtime_error("could not redirect stderr for close test");
    }

    try {
        rbt_m3t::dsp::OrbConfig config;
        {
            rbt_m3t::dsp::Orb orb(config);
            set_mode("close_failure");
        }
    } catch (...) {
        std::fflush(stderr);
        dup2(saved_stderr, STDERR_FILENO);
        ::close(saved_stderr);
        std::fclose(captured_stderr);
        throw;
    }
    std::fflush(stderr);
    dup2(saved_stderr, STDERR_FILENO);
    ::close(saved_stderr);

    std::rewind(captured_stderr);
    std::string diagnostic;
    char buffer[256];
    while (std::fgets(buffer, sizeof(buffer), captured_stderr) != nullptr)
        diagnostic += buffer;
    std::fclose(captured_stderr);
    check(fake.closes() == 1,
          "close failure is logged without retrying the stub close");
    check(fake.prepares() == 1 && fake.prepares_before_open() == 1 &&
              fake.adsp_path_valid() == 1 && adsp_path.matches_original(),
          "close cleanup restores the prior library path after preparation");
    check(diagnostic.find("DSP ORB FastRPC close failed with code -55") !=
              std::string::npos,
          "close failure writes a clear diagnostic even when audit is disabled");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr,
                     "usage: dsp_orb_client_test STUB MODULE WORK_DIRECTORY\n");
        return 2;
    }
    void* control_library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (control_library == nullptr) {
        std::fprintf(stderr, "cannot load fake stub for controls: %s\n",
                     dlerror());
        return 2;
    }
    try {
        ScopedEnvironment expected_module_uri_env(
            "RBT_M3T_DSP_FAKE_EXPECTED_MODULE_URI");
        expected_module_uri_env.set(expected_module_uri(argv[2]));
        FakeControls fake(control_library);
        run_session_tests(argv[1], argv[2], argv[3], fake);
        run_prepare_failure_test(argv[1], argv[2], fake);
        run_capability_failure(argv[1], argv[2], fake, "bad_caps_mask",
                               "constructor rejects capability feature mask");
        run_capability_failure(argv[1], argv[2], fake, "bad_caps_version",
                               "constructor rejects incompatible implementation version");
        if (run_occupied_default_alias_test(fake, argv[1], argv[2]))
            run_default_path_test(fake, argv[1], argv[2]);
        run_custom_module_basename_test(argv[1], argv[2], fake);
        run_unsafe_module_path_tests(argv[1], argv[2], fake);
        run_unset_adsp_path_test(argv[1], argv[2], fake);
        run_empty_adsp_path_test(argv[1], argv[2], fake);
        run_external_adsp_change_test(argv[1], argv[2], fake);
        run_session_lifetime_race_test(argv[1], argv[2], fake);
        run_close_failure_test(argv[1], argv[2], fake);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "dsp_orb_client_test: unexpected exception: %s\n",
                     error.what());
        ++g_failures;
    }
    dlclose(control_library);
    if (g_failures != 0) return 1;
    std::puts("dsp_orb_client_test: all checks passed");
    return 0;
}
