// SPDX-License-Identifier: MIT
#include "rbt_m3t/dsp_orb.hpp"

#include "../dsp/include/rbt_orb_protocol.h"

#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

#ifndef RBT_M3T_DSP_INSTALL_LIBDIR
#define RBT_M3T_DSP_INSTALL_LIBDIR "lib"
#endif

namespace rbt_m3t::dsp {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kMaxWidth = 1024u;
constexpr uint32_t kMaxHeight = 1024u;
constexpr uint32_t kMaxFeatures = 2048u;
constexpr uint32_t kMaxLevels = 8u;
constexpr uint32_t kDescriptorBytes = 32u;
constexpr uint32_t kWorkspaceCapacity = 16u * 1024u * 1024u;
constexpr uint32_t kFastThreshold = 20u;
constexpr uint32_t kFeatureMask = 0x7fu;
constexpr uint32_t kImplementationVersion = 1u;
constexpr const char* kStubFilename = "librbt_m3t_orb_stub.so";
constexpr const char* kModuleFilename = "librbt_m3t_orb_skel.so";
constexpr const char* kModuleSuffix =
    "?rbt_m3t_orb_skel_handle_invoke&_modver=1.0&_dom=cdsp";
constexpr const char* kAdspLibraryPath = "ADSP_LIBRARY_PATH";
constexpr const char* kPrepareSymbol = "rbt_m3t_orb_prepare_cdsp";

using OpenFn = int (*)(const char*, uint64_t*);
using CloseFn = int (*)(uint64_t);
using ExecuteFn = int (*)(uint64_t, const uint8_t*, int, uint8_t*, int);
using PrepareFn = int (*)();

std::recursive_mutex g_session_mutex;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

bool checked_add(size_t lhs, size_t rhs, size_t* result) {
    if (lhs > std::numeric_limits<size_t>::max() - rhs) return false;
    *result = lhs + rhs;
    return true;
}

bool checked_multiply(size_t lhs, size_t rhs, size_t* result) {
    if (lhs != 0u && rhs > std::numeric_limits<size_t>::max() / lhs)
        return false;
    *result = lhs * rhs;
    return true;
}

bool valid_address_span(const void* pointer, size_t bytes) {
    if (bytes == 0u) return true;
    if (pointer == nullptr) return false;
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    const uintptr_t maximum = std::numeric_limits<uintptr_t>::max();
    return bytes - 1u <= maximum - address;
}

std::string system_error(const char* operation, const std::string& detail) {
    return std::string(operation) + ": " + detail;
}

fs::path executable_directory() {
    std::array<char, 4096> buffer{};
    const ssize_t length = readlink("/proc/self/exe", buffer.data(),
                                   buffer.size() - 1u);
    if (length <= 0 || static_cast<size_t>(length) >= buffer.size() - 1u)
        fail("cannot resolve executable directory through /proc/self/exe");
    buffer[static_cast<size_t>(length)] = '\0';
    return fs::path(buffer.data()).parent_path();
}

std::string canonical_readable_file(const fs::path& requested,
                                    const char* label) {
    if (!requested.is_absolute()) {
        fail(std::string(label) + " must be an absolute path: " +
             requested.string());
    }
    std::error_code error;
    const fs::path canonical = fs::canonical(requested, error);
    if (error || !fs::is_regular_file(canonical, error) || error ||
        access(canonical.c_str(), R_OK) != 0) {
        fail(std::string(label) + " must be a readable regular file: " +
             requested.string());
    }
    return canonical.string();
}

struct Paths {
    std::string stub;
    std::string module;
};

Paths resolve_paths() {
    const char* stub_env = std::getenv("RBT_M3T_DSP_STUB");
    const char* module_env = std::getenv("RBT_M3T_DSP_MODULE");
    const bool has_stub = stub_env != nullptr;
    const bool has_module = module_env != nullptr;

    Paths paths;
    if (has_stub)
        paths.stub = canonical_readable_file(stub_env, "RBT_M3T_DSP_STUB");
    if (has_module)
        paths.module = canonical_readable_file(module_env,
                                              "RBT_M3T_DSP_MODULE");
    if (!has_stub || !has_module) {
        const fs::path binary_dir = executable_directory();
        const fs::path install_libdir(RBT_M3T_DSP_INSTALL_LIBDIR);
        const fs::path installed_directory = install_libdir.is_absolute()
            ? install_libdir / "rbt-m3t" / "dsp"
            : binary_dir / ".." / install_libdir / "rbt-m3t" / "dsp";
        const std::array<fs::path, 2> directories = {
            binary_dir, installed_directory};
        for (const fs::path& directory : directories) {
            std::error_code error;
            const fs::path stub_candidate = directory / kStubFilename;
            const fs::path module_candidate = directory / kModuleFilename;
            std::string stub;
            std::string module;
            if (!has_stub && fs::is_regular_file(stub_candidate, error) &&
                !error && access(stub_candidate.c_str(), R_OK) == 0) {
                stub = canonical_readable_file(stub_candidate,
                                               "DSP stub library");
            }
            error.clear();
            if (!has_module &&
                fs::is_regular_file(module_candidate, error) && !error &&
                access(module_candidate.c_str(), R_OK) == 0) {
                module = canonical_readable_file(module_candidate,
                                                 "DSP module");
            }
            if ((has_stub || !stub.empty()) &&
                (has_module || !module.empty())) {
                if (!has_stub) paths.stub = std::move(stub);
                if (!has_module) paths.module = std::move(module);
                break;
            }
        }
    }
    if (paths.stub.empty() || paths.module.empty()) {
        fail("DSP stub or module was not found beside the executable or in "
             "../lib/rbt-m3t/dsp; set RBT_M3T_DSP_STUB and "
             "RBT_M3T_DSP_MODULE to absolute paths");
    }
    return paths;
}

std::string module_uri(const std::string& canonical_module) {
    const fs::path module_path(canonical_module);
    const std::string basename = module_path.filename().string();
    const bool safe_basename = !basename.empty() &&
        std::all_of(basename.begin(), basename.end(), [](unsigned char value) {
            return (value >= 'a' && value <= 'z') ||
                   (value >= 'A' && value <= 'Z') ||
                   (value >= '0' && value <= '9') || value == '.' ||
                   value == '_' || value == '-';
        });
    if (!safe_basename) {
        fail("DSP module basename contains characters unsafe for a QuRT file URI");
    }
    const std::string directory = module_path.parent_path().string();
    if (directory.find(';') != std::string::npos) {
        fail("DSP module directory contains the ADSP_LIBRARY_PATH separator ';'");
    }
    // QuRT's loader rejects absolute file URIs. It resolves this basename
    // through ADSP_LIBRARY_PATH, which the session temporarily prepends.
    return std::string("file:///") + basename + kModuleSuffix;
}

// QuRT requires the module URI to use a basename, so FastRPC finds the ELF
// through this session-scoped process environment. No system configuration is
// written, and the prior process value is restored when the session ends.
class AdspLibraryPath {
public:
    explicit AdspLibraryPath(const std::string& canonical_module) {
        const char* previous = std::getenv(kAdspLibraryPath);
        was_set_ = previous != nullptr;
        if (was_set_) previous_ = previous;

        installed_ = fs::path(canonical_module).parent_path().string();
        if (was_set_) installed_ += ";" + previous_;
        if (setenv(kAdspLibraryPath, installed_.c_str(), 1) != 0) {
            fail(std::string("cannot set ADSP_LIBRARY_PATH: ") +
                 std::strerror(errno));
        }
        active_ = true;
    }

    ~AdspLibraryPath() { restore(); }

    AdspLibraryPath(const AdspLibraryPath&) = delete;
    AdspLibraryPath& operator=(const AdspLibraryPath&) = delete;

    void restore() noexcept {
        if (!active_) return;
        const char* current = std::getenv(kAdspLibraryPath);
        if (current != nullptr && installed_ == current) {
            const int result = was_set_
                ? setenv(kAdspLibraryPath, previous_.c_str(), 1)
                : unsetenv(kAdspLibraryPath);
            if (result != 0) {
                std::fprintf(stderr,
                             "DSP ORB could not restore ADSP_LIBRARY_PATH: "
                             "%s\n",
                             std::strerror(errno));
            }
        }
        active_ = false;
    }

private:
    bool was_set_ = false;
    bool active_ = false;
    std::string previous_;
    std::string installed_;
};

bool audit_enabled() {
    const char* value = std::getenv("RBT_M3T_DSP_AUDIT");
    return value != nullptr && std::strcmp(value, "1") == 0;
}

void write_request_header(uint8_t* bytes, size_t request_size,
                          uint32_t response_capacity, uint32_t operation) {
    RbtOrbRequest request{};
    request.magic = RBT_ORB_REQUEST_MAGIC;
    request.version = RBT_ORB_PROTOCOL_VERSION;
    request.operation = operation;
    request.request_bytes = static_cast<uint32_t>(request_size);
    request.response_capacity = response_capacity;
    std::memcpy(bytes, &request, sizeof(request));
}

RbtOrbResponse read_response_header(const std::vector<uint8_t>& response,
                                    uint32_t operation,
                                    size_t expected_success_bytes,
                                    size_t capacity) {
    if (response.size() < sizeof(RbtOrbResponse))
        fail("DSP response is shorter than its header");
    RbtOrbResponse header{};
    std::memcpy(&header, response.data(), sizeof(header));
    if (header.magic != RBT_ORB_RESPONSE_MAGIC ||
        header.version != RBT_ORB_PROTOCOL_VERSION ||
        header.operation != operation || header.reserved != 0u) {
        fail("DSP response has an invalid header or reserved field");
    }
    if (header.response_bytes < sizeof(RbtOrbResponse) ||
        header.response_bytes > capacity ||
        header.response_bytes > response.size()) {
        fail("DSP response length is outside the supplied capacity");
    }
    if (header.status < RBT_ORB_OK ||
        header.status > RBT_ORB_INTERNAL_ERROR) {
        fail("DSP response has an unknown status");
    }
    if (header.status == RBT_ORB_OK) {
        if (header.response_bytes != expected_success_bytes ||
            header.implemented_features != kFeatureMask) {
            fail("DSP success response has an invalid length or feature mask");
        }
    } else if (header.count != 0u || header.implemented_features != 0u ||
               (header.response_bytes != sizeof(RbtOrbResponse) &&
                header.response_bytes != expected_success_bytes)) {
        fail("DSP error response has invalid count, mask, or length");
    }
    return header;
}

void check_rpc_result(int result, const char* operation) {
    if (result != 0) {
        fail(std::string("FastRPC ") + operation + " failed with code " +
             std::to_string(result));
    }
}

void validate_image(const cv::Mat& image) {
    if (image.dims != 2 || image.type() != CV_8UC1 || image.cols <= 0 ||
        image.rows <= 0 || image.cols > static_cast<int>(kMaxWidth) ||
        image.rows > static_cast<int>(kMaxHeight)) {
        fail("DSP ORB requires a nonempty CV_8UC1 image no larger than "
             "1024x1024");
    }
    const size_t row_bytes = static_cast<size_t>(image.cols);
    if (image.data == nullptr || image.step[0] < row_bytes ||
        image.step[1] != 1u) {
        fail("DSP ORB image has an invalid row stride");
    }
    size_t last_offset = 0u;
    size_t span = 0u;
    if (!checked_multiply(image.step[0],
                          static_cast<size_t>(image.rows - 1), &last_offset) ||
        !checked_add(last_offset, row_bytes, &span) ||
        span > kWorkspaceCapacity || !valid_address_span(image.data, span)) {
        fail("DSP ORB image stride overflows addressable memory");
    }
}

uint32_t validate_descriptors(const cv::Mat& descriptors,
                              const char* name) {
    if (descriptors.dims != 2 || descriptors.type() != CV_8UC1 ||
        descriptors.cols != static_cast<int>(kDescriptorBytes) ||
        descriptors.rows < 0 ||
        descriptors.rows > static_cast<int>(kMaxFeatures)) {
        fail(std::string("DSP ORB ") + name +
             " descriptors must be CV_8UC1 with 32 columns and at most "
             "2048 rows");
    }
    if (descriptors.rows == 0) return 0u;
    if (descriptors.data == nullptr ||
        descriptors.step[0] < kDescriptorBytes || descriptors.step[1] != 1u) {
        fail(std::string("DSP ORB ") + name +
             " descriptors have an invalid row stride");
    }
    size_t last_offset = 0u;
    size_t span = 0u;
    if (!checked_multiply(descriptors.step[0],
                          static_cast<size_t>(descriptors.rows - 1),
                          &last_offset) ||
        !checked_add(last_offset, kDescriptorBytes, &span) ||
        span > kWorkspaceCapacity ||
        !valid_address_span(descriptors.data, span)) {
        fail(std::string("DSP ORB ") + name +
             " descriptor stride overflows addressable memory");
    }
    return static_cast<uint32_t>(descriptors.rows);
}

std::vector<uint8_t> pack_descriptors(const cv::Mat& descriptors,
                                      uint32_t count) {
    size_t bytes = 0u;
    if (!checked_multiply(count, kDescriptorBytes, &bytes))
        fail("DSP ORB descriptor payload size overflow");
    std::vector<uint8_t> packed(bytes);
    for (uint32_t row = 0u; row < count; ++row) {
        std::memcpy(packed.data() +
                        static_cast<size_t>(row) * kDescriptorBytes,
                    descriptors.ptr<uint8_t>(static_cast<int>(row)),
                    kDescriptorBytes);
    }
    return packed;
}

struct Session {
    explicit Session(Paths requested_paths)
        : paths(std::move(requested_paths)),
          uri(module_uri(paths.module)),
          adsp_library_path(paths.module), audit(audit_enabled()) {
        try {
            library = dlopen(paths.stub.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (library == nullptr) {
                const char* error = dlerror();
                fail(system_error("cannot load DSP stub",
                                  error ? error : "unknown error"));
            }
            open = load_symbol<OpenFn>("rbt_m3t_orb_open");
            close = load_symbol<CloseFn>("rbt_m3t_orb_close");
            execute = load_symbol<ExecuteFn>("rbt_m3t_orb_execute");
            prepare_cdsp = load_symbol<PrepareFn>(kPrepareSymbol);
            check_rpc_result(prepare_cdsp(), "CDSP preparation");
            check_rpc_result(open(uri.c_str(), &handle), "open");
            is_open = true;
            verify_capabilities();
            if (audit) {
                std::fprintf(stderr,
                             "{\"event\":\"m3t_dsp_ready\","
                             "\"implversion\":%u,\"featuresmask\":%u}\n",
                             capabilities.implementation_version,
                             kFeatureMask);
            }
        } catch (...) {
            cleanup();
            throw;
        }
    }

    ~Session() { cleanup(); }

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    template <typename Function>
    Function load_symbol(const char* name) {
        dlerror();
        void* symbol = dlsym(library, name);
        const char* error = dlerror();
        if (error != nullptr || symbol == nullptr) {
            fail(std::string("DSP stub is missing ") + name +
                 (error ? std::string(": ") + error : std::string()));
        }
        return reinterpret_cast<Function>(symbol);
    }

    int call(const uint8_t* request, size_t request_size,
             uint8_t* response, size_t response_capacity) {
        if (request_size > static_cast<size_t>(std::numeric_limits<int>::max()) ||
            response_capacity >
                static_cast<size_t>(std::numeric_limits<int>::max())) {
            fail("FastRPC payload exceeds the generated API integer limit");
        }
        std::lock_guard<std::mutex> lock(rpc_mutex);
        return execute(handle, request, static_cast<int>(request_size), response,
                       static_cast<int>(response_capacity));
    }

    void verify_capabilities() {
        constexpr size_t response_capacity =
            sizeof(RbtOrbResponse) + sizeof(RbtOrbCapabilities);
        std::vector<uint8_t> request(sizeof(RbtOrbRequest), 0u);
        std::vector<uint8_t> response(response_capacity, 0u);
        write_request_header(request.data(), request.size(),
                             static_cast<uint32_t>(response_capacity),
                             RBT_ORB_CAPABILITIES);
        check_rpc_result(call(request.data(), request.size(), response.data(),
                              response.size()),
                         "capability handshake");
        const RbtOrbResponse header = read_response_header(
            response, RBT_ORB_CAPABILITIES, response_capacity,
            response_capacity);
        if (header.count != 1u) {
            fail("DSP capability handshake returned an invalid count");
        }
        std::memcpy(&capabilities, response.data() + sizeof(RbtOrbResponse),
                    sizeof(capabilities));
        if (capabilities.max_width != kMaxWidth ||
            capabilities.max_height != kMaxHeight ||
            capabilities.max_features != kMaxFeatures ||
            capabilities.max_levels != kMaxLevels ||
            capabilities.workspace_capacity != kWorkspaceCapacity ||
            capabilities.implementation_version != kImplementationVersion ||
            capabilities.reserved[0] != 0u || capabilities.reserved[1] != 0u) {
            fail("DSP capability handshake reported incompatible limits or version");
        }
    }

    void cleanup() noexcept {
        std::lock_guard<std::recursive_mutex> lifecycle_lock(g_session_mutex);
        if (is_open && close != nullptr) {
            const int close_result = close(handle);
            is_open = false;
            if (close_result != 0) {
                std::fprintf(stderr,
                             "DSP ORB FastRPC close failed with code %d\n",
                             close_result);
            }
        }
        handle = 0u;
        if (library != nullptr) {
            dlclose(library);
            library = nullptr;
        }
        adsp_library_path.restore();
    }

    Paths paths;
    std::string uri;
    AdspLibraryPath adsp_library_path;
    bool audit = false;
    void* library = nullptr;
    OpenFn open = nullptr;
    CloseFn close = nullptr;
    ExecuteFn execute = nullptr;
    PrepareFn prepare_cdsp = nullptr;
    uint64_t handle = 0u;
    bool is_open = false;
    std::mutex rpc_mutex;
    RbtOrbCapabilities capabilities{};
};

std::weak_ptr<Session> g_session;

std::shared_ptr<Session> acquire_session() {
    Paths paths = resolve_paths();
    std::lock_guard<std::recursive_mutex> lock(g_session_mutex);
    if (std::shared_ptr<Session> current = g_session.lock()) {
        if (current->paths.stub != paths.stub ||
            current->paths.module != paths.module) {
            fail("a DSP ORB session is already active for different stub or "
                 "module paths");
        }
        return current;
    }
    std::shared_ptr<Session> created =
        std::make_shared<Session>(std::move(paths));
    g_session = created;
    return created;
}

RbtOrbConfig protocol_config(const OrbConfig& config) {
    if (config.n_features < 1 ||
        config.n_features > static_cast<int>(kMaxFeatures) ||
        config.n_levels < 1 || config.n_levels > static_cast<int>(kMaxLevels) ||
        !std::isfinite(config.scale_factor) || config.scale_factor < 1.05f ||
        config.scale_factor > 2.0f) {
        fail("DSP ORB configuration is outside the supported bounds");
    }
    RbtOrbConfig result{};
    result.n_features = static_cast<uint32_t>(config.n_features);
    result.n_levels = static_cast<uint32_t>(config.n_levels);
    result.scale_factor = config.scale_factor;
    result.fast_threshold = kFastThreshold;
    return result;
}

void require_success_count(const RbtOrbResponse& header, uint32_t expected,
                           const char* operation) {
    if (header.count != expected) {
        fail(std::string("DSP ") + operation +
             " response has an invalid result count");
    }
}

}  // namespace

struct Orb::Impl {
    explicit Impl(OrbConfig requested) : config(protocol_config(requested)) {
        session = acquire_session();
    }
    ~Impl() {
        std::lock_guard<std::recursive_mutex> lock(g_session_mutex);
        session.reset();
    }
    RbtOrbConfig config{};
    std::shared_ptr<Session> session;
};

Orb::Orb(OrbConfig config) : impl_(std::make_unique<Impl>(config)) {}
Orb::~Orb() = default;

void Orb::extract(const cv::Mat& gray,
                  std::vector<cv::KeyPoint>& keypoints,
                  cv::Mat& descriptors) {
    keypoints.clear();
    descriptors.release();
    try {
        validate_image(gray);
        const RbtOrbConfig config = impl_->config;
        const size_t image_bytes =
            static_cast<size_t>(gray.cols) * static_cast<size_t>(gray.rows);
        const size_t request_size = sizeof(RbtOrbRequest) + image_bytes;
        size_t keypoint_slots = 0u;
        size_t descriptor_slots = 0u;
        size_t response_capacity = sizeof(RbtOrbResponse);
        if (!checked_multiply(config.n_features, sizeof(RbtOrbKeypoint),
                              &keypoint_slots) ||
            !checked_multiply(config.n_features, kDescriptorBytes,
                              &descriptor_slots) ||
            !checked_add(response_capacity, keypoint_slots,
                         &response_capacity) ||
            !checked_add(response_capacity, descriptor_slots,
                         &response_capacity) ||
            request_size > std::numeric_limits<uint32_t>::max() ||
            response_capacity > std::numeric_limits<uint32_t>::max()) {
            fail("DSP ORB extract payload size overflow");
        }
        std::vector<uint8_t> request(request_size, 0u);
        std::vector<uint8_t> response(response_capacity, 0u);
        write_request_header(request.data(), request.size(),
                             static_cast<uint32_t>(response_capacity),
                             RBT_ORB_EXTRACT);
        RbtOrbRequest parsed_request{};
        std::memcpy(&parsed_request, request.data(), sizeof(parsed_request));
        parsed_request.args.extract.width = static_cast<uint32_t>(gray.cols);
        parsed_request.args.extract.height = static_cast<uint32_t>(gray.rows);
        parsed_request.args.extract.stride = static_cast<uint32_t>(gray.cols);
        parsed_request.args.extract.config = config;
        parsed_request.args.extract.image_bytes =
            static_cast<uint32_t>(image_bytes);
        std::memcpy(request.data(), &parsed_request, sizeof(parsed_request));
        for (int row = 0; row < gray.rows; ++row) {
            std::memcpy(request.data() + sizeof(RbtOrbRequest) +
                            static_cast<size_t>(row) *
                                static_cast<size_t>(gray.cols),
                        gray.ptr<uint8_t>(row),
                        static_cast<size_t>(gray.cols));
        }

        check_rpc_result(impl_->session->call(
                             request.data(), request.size(), response.data(),
                             response.size()),
                         "extract");
        const RbtOrbResponse header = read_response_header(
            response, RBT_ORB_EXTRACT, response_capacity, response_capacity);
        if (header.status != RBT_ORB_OK) {
            fail("DSP ORB extract returned status " +
                 std::to_string(header.status));
        }
        if (header.count > config.n_features) {
            fail("DSP ORB extract returned more keypoints than requested");
        }

        std::vector<cv::KeyPoint> new_keypoints;
        new_keypoints.reserve(header.count);
        for (uint32_t index = 0u; index < header.count; ++index) {
            RbtOrbKeypoint point{};
            const size_t point_offset = sizeof(RbtOrbResponse) +
                static_cast<size_t>(index) * sizeof(RbtOrbKeypoint);
            std::memcpy(&point, response.data() + point_offset, sizeof(point));
            if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
                !std::isfinite(point.size) || !std::isfinite(point.angle) ||
                !std::isfinite(point.response) || point.x < 0.0f ||
                point.y < 0.0f || point.x >= static_cast<float>(gray.cols) ||
                point.y >= static_cast<float>(gray.rows) || point.size <= 0.0f ||
                point.angle < 0.0f || point.angle >= 360.0f ||
                point.octave < 0 ||
                point.octave >= static_cast<int32_t>(config.n_levels)) {
                fail("DSP ORB extract returned an invalid keypoint");
            }
            new_keypoints.emplace_back(
                cv::Point2f(point.x, point.y), point.size, point.angle,
                point.response, point.octave, -1);
        }

        cv::Mat new_descriptors;
        if (header.count != 0u) {
            new_descriptors.create(static_cast<int>(header.count),
                                   static_cast<int>(kDescriptorBytes), CV_8UC1);
            const size_t descriptor_offset =
                sizeof(RbtOrbResponse) + keypoint_slots;
            for (uint32_t row = 0u; row < header.count; ++row) {
                std::memcpy(new_descriptors.ptr<uint8_t>(static_cast<int>(row)),
                            response.data() + descriptor_offset +
                                static_cast<size_t>(row) * kDescriptorBytes,
                            kDescriptorBytes);
            }
        }
        descriptors = std::move(new_descriptors);
        keypoints.swap(new_keypoints);
        if (impl_->session->audit) {
            std::fprintf(stderr,
                         "{\"event\":\"m3t_dsp_extract\","
                         "\"width\":%d,\"height\":%d,\"features\":%u,"
                         "\"rpc_calls\":1}\n",
                         gray.cols, gray.rows, header.count);
        }
    } catch (const std::exception& error) {
        keypoints.clear();
        descriptors.release();
        throw std::runtime_error(std::string("DSP ORB extract failed: ") +
                                 error.what());
    } catch (...) {
        keypoints.clear();
        descriptors.release();
        throw std::runtime_error("DSP ORB extract failed with an unknown error");
    }
}

std::vector<std::vector<cv::DMatch>> Orb::match_2nn(
    const cv::Mat& query, const cv::Mat& train) {
    try {
        const uint32_t query_count = validate_descriptors(query, "query");
        const uint32_t train_count = validate_descriptors(train, "train");
        std::vector<std::vector<cv::DMatch>> result(query_count);
        if (query_count == 0u) return result;

        const std::vector<uint8_t> packed_query =
            pack_descriptors(query, query_count);
        const std::vector<uint8_t> packed_train =
            pack_descriptors(train, train_count);
        size_t request_size = sizeof(RbtOrbRequest);
        if (!checked_add(request_size, packed_query.size(), &request_size) ||
            !checked_add(request_size, packed_train.size(), &request_size)) {
            fail("DSP ORB match request size overflow");
        }
        size_t match_bytes = 0u;
        size_t response_capacity = sizeof(RbtOrbResponse);
        if (!checked_multiply(query_count, sizeof(RbtOrbMatch), &match_bytes) ||
            !checked_add(response_capacity, match_bytes, &response_capacity) ||
            request_size > std::numeric_limits<uint32_t>::max() ||
            response_capacity > std::numeric_limits<uint32_t>::max()) {
            fail("DSP ORB match response size overflow");
        }

        std::vector<uint8_t> request(request_size, 0u);
        std::vector<uint8_t> response(response_capacity, 0u);
        write_request_header(request.data(), request.size(),
                             static_cast<uint32_t>(response_capacity),
                             RBT_ORB_MATCH);
        RbtOrbRequest parsed_request{};
        std::memcpy(&parsed_request, request.data(), sizeof(parsed_request));
        parsed_request.args.match.query_count = query_count;
        parsed_request.args.match.train_count = train_count;
        std::memcpy(request.data(), &parsed_request, sizeof(parsed_request));
        size_t offset = sizeof(RbtOrbRequest);
        if (!packed_query.empty()) {
            std::memcpy(request.data() + offset, packed_query.data(),
                        packed_query.size());
            offset += packed_query.size();
        }
        if (!packed_train.empty()) {
            std::memcpy(request.data() + offset, packed_train.data(),
                        packed_train.size());
        }

        check_rpc_result(impl_->session->call(
                             request.data(), request.size(), response.data(),
                             response.size()),
                         "match");
        const RbtOrbResponse header = read_response_header(
            response, RBT_ORB_MATCH, response_capacity, response_capacity);
        if (header.status != RBT_ORB_OK) {
            fail("DSP ORB match returned status " +
                 std::to_string(header.status));
        }
        require_success_count(header, query_count, "match");

        uint32_t neighbor_count = 0u;
        for (uint32_t index = 0u; index < query_count; ++index) {
            RbtOrbMatch match{};
            std::memcpy(&match,
                        response.data() + sizeof(RbtOrbResponse) +
                            static_cast<size_t>(index) * sizeof(RbtOrbMatch),
                        sizeof(match));
            const bool first_missing = match.first_index == -1;
            const bool second_missing = match.second_index == -1;
            if (match.first_distance > 257u || match.second_distance > 257u ||
                first_missing != (match.first_distance == 257u) ||
                second_missing != (match.second_distance == 257u) ||
                (first_missing && !second_missing) ||
                (!first_missing &&
                 (match.first_index < 0 ||
                  static_cast<uint32_t>(match.first_index) >= train_count ||
                  match.first_distance > 256u)) ||
                (!second_missing &&
                 (match.second_index < 0 ||
                  static_cast<uint32_t>(match.second_index) >= train_count ||
                  match.second_distance > 256u)) ||
                (train_count == 0u && (!first_missing || !second_missing)) ||
                (train_count == 1u && (first_missing || !second_missing)) ||
                (train_count >= 2u && (first_missing || second_missing)) ||
                (!first_missing && !second_missing &&
                 (match.first_index == match.second_index ||
                  match.first_distance > match.second_distance ||
                  (match.first_distance == match.second_distance &&
                   match.first_index > match.second_index)))) {
                fail("DSP ORB match returned invalid, unsorted, or duplicate neighbors");
            }
            if (!first_missing) {
                ++neighbor_count;
                result[index].emplace_back(static_cast<int>(index),
                                           match.first_index, 0,
                                           static_cast<float>(match.first_distance));
            }
            if (!second_missing) {
                ++neighbor_count;
                result[index].emplace_back(static_cast<int>(index),
                                           match.second_index, 0,
                                           static_cast<float>(match.second_distance));
            }
        }
        if (impl_->session->audit) {
            std::fprintf(stderr,
                         "{\"event\":\"m3t_dsp_match\","
                         "\"queries\":%u,\"train\":%u,"
                         "\"rpc_calls\":1,\"neighbors\":%u}\n",
                         query_count, train_count, neighbor_count);
        }
        return result;
    } catch (const std::exception& error) {
        throw std::runtime_error(std::string("DSP ORB match failed: ") +
                                 error.what());
    } catch (...) {
        throw std::runtime_error("DSP ORB match failed with an unknown error");
    }
}

}  // namespace rbt_m3t::dsp
