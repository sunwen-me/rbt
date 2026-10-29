// SPDX-License-Identifier: MIT
#include "../dsp/include/rbt_orb_protocol.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace {

std::atomic<int> g_open_count{0};
std::atomic<int> g_close_count{0};
std::atomic<int> g_execute_count{0};
std::atomic<int> g_prepare_count{0};
std::atomic<int> g_prepare_before_open_count{0};
std::atomic<int> g_uri_valid{0};
std::atomic<int> g_adsp_path_valid{0};

const char* mode() {
    const char* value = std::getenv("RBT_M3T_DSP_FAKE_MODE");
    return value == nullptr ? "ok" : value;
}

bool is_mode(const char* value) {
    return std::strcmp(mode(), value) == 0;
}

void write_header(uint8_t* response, uint32_t operation,
                  uint32_t response_bytes, int status, uint32_t count,
                  uint32_t features) {
    RbtOrbResponse header{};
    header.magic = RBT_ORB_RESPONSE_MAGIC;
    header.version = RBT_ORB_PROTOCOL_VERSION;
    header.operation = operation;
    header.response_bytes = response_bytes;
    header.status = status;
    header.count = count;
    header.implemented_features = features;
    std::memcpy(response, &header, sizeof(header));
}

void set_header_field(uint8_t* response, size_t offset, uint32_t value) {
    std::memcpy(response + offset, &value, sizeof(value));
}

bool valid_request(const uint8_t* request, int request_size,
                   uint32_t operation) {
    if (request == nullptr || request_size <
                                  static_cast<int>(sizeof(RbtOrbRequest)))
        return false;
    RbtOrbRequest header{};
    std::memcpy(&header, request, sizeof(header));
    return header.magic == RBT_ORB_REQUEST_MAGIC &&
           header.version == RBT_ORB_PROTOCOL_VERSION &&
           header.operation == operation &&
           header.request_bytes == static_cast<uint32_t>(request_size) &&
           header.reserved[0] == 0u && header.reserved[1] == 0u &&
           header.reserved[2] == 0u;
}

}  // namespace

extern "C" int rbt_m3t_orb_prepare_cdsp() {
    ++g_prepare_count;
    return is_mode("prepare_failure") ? -44 : 0;
}

extern "C" int rbt_m3t_orb_open(const char* uri, uint64_t* handle) {
    const int open_count = ++g_open_count;
    if (g_prepare_count.load() == open_count) ++g_prepare_before_open_count;
    if (uri == nullptr || handle == nullptr) return -10;
    const char* expected_uri =
        std::getenv("RBT_M3T_DSP_FAKE_EXPECTED_MODULE_URI");
    if (expected_uri == nullptr || std::strcmp(uri, expected_uri) != 0) {
        return -11;
    }

    const char* expected_path =
        std::getenv("RBT_M3T_DSP_FAKE_EXPECTED_ADSP_PATH");
    const char* actual_path = std::getenv("ADSP_LIBRARY_PATH");
    if (expected_path == nullptr ||
        (actual_path != nullptr &&
         std::strcmp(actual_path, expected_path) == 0)) {
        ++g_adsp_path_valid;
    }
    ++g_uri_valid;
    *handle = 0x4d33545242544f52ull;
    return 0;
}

extern "C" int rbt_m3t_orb_close(uint64_t handle) {
    if (handle != 0x4d33545242544f52ull) return -12;
    ++g_close_count;
    return is_mode("close_failure") ? -55 : 0;
}

extern "C" int rbt_m3t_orb_execute(uint64_t handle, const uint8_t* request,
                                    int request_size, uint8_t* response,
                                    int response_capacity) {
    ++g_execute_count;
    if (handle != 0x4d33545242544f52ull || response == nullptr ||
        response_capacity < static_cast<int>(sizeof(RbtOrbResponse)))
        return -20;
    if (request == nullptr || request_size <
                                  static_cast<int>(sizeof(RbtOrbRequest)))
        return -21;
    RbtOrbRequest parsed{};
    std::memcpy(&parsed, request, sizeof(parsed));
    if (parsed.response_capacity != static_cast<uint32_t>(response_capacity))
        return -22;

    if (parsed.operation == RBT_ORB_CAPABILITIES) {
        if (!valid_request(request, request_size, RBT_ORB_CAPABILITIES) ||
            request_size != static_cast<int>(sizeof(RbtOrbRequest)) ||
            response_capacity < static_cast<int>(sizeof(RbtOrbResponse) +
                                                 sizeof(RbtOrbCapabilities)))
            return -23;
        const uint32_t response_bytes =
            sizeof(RbtOrbResponse) + sizeof(RbtOrbCapabilities);
        write_header(response, RBT_ORB_CAPABILITIES, response_bytes, 0, 1u,
                     is_mode("bad_caps_mask") ? 0x3fu : 0x7fu);
        RbtOrbCapabilities capabilities{};
        capabilities.max_width = RBT_ORB_MAX_WIDTH;
        capabilities.max_height = RBT_ORB_MAX_HEIGHT;
        capabilities.max_features = RBT_ORB_MAX_FEATURES;
        capabilities.max_levels = RBT_ORB_MAX_LEVELS;
        capabilities.workspace_capacity = RBT_ORB_WORKSPACE_CAPACITY;
        capabilities.implementation_version =
            is_mode("bad_caps_version") ? 2u : RBT_ORB_IMPLEMENTATION_VERSION;
        std::memcpy(response + sizeof(RbtOrbResponse), &capabilities,
                    sizeof(capabilities));
        return 0;
    }

    if (parsed.operation == RBT_ORB_EXTRACT) {
        if (!valid_request(request, request_size, RBT_ORB_EXTRACT)) return -24;
        const uint32_t width = parsed.args.extract.width;
        const uint32_t height = parsed.args.extract.height;
        const uint32_t features = parsed.args.extract.config.n_features;
        const uint32_t image_bytes = parsed.args.extract.image_bytes;
        const size_t expected_request = sizeof(RbtOrbRequest) + image_bytes;
        const size_t expected_response = sizeof(RbtOrbResponse) +
            static_cast<size_t>(features) * sizeof(RbtOrbKeypoint) +
            static_cast<size_t>(features) * RBT_ORB_DESCRIPTOR_BYTES;
        if (width == 0u || height == 0u || image_bytes != width * height ||
            parsed.args.extract.stride != width ||
            expected_request != static_cast<size_t>(request_size) ||
            expected_response > static_cast<size_t>(response_capacity) ||
            parsed.args.extract.config.fast_threshold != 20u ||
            parsed.args.extract.config.n_levels != 3u ||
            parsed.args.extract.config.scale_factor != 1.2f) {
            return -25;
        }
        const uint8_t* image = request + sizeof(RbtOrbRequest);
        for (uint32_t y = 0u; y < height; ++y) {
            for (uint32_t x = 0u; x < width; ++x) {
                if (image[static_cast<size_t>(y) * width + x] !=
                    static_cast<uint8_t>(y * 10u + x))
                    return -26;
            }
        }
        const uint32_t count = is_mode("extract_empty") ? 0u :
            is_mode("extract_bad_count") ? features + 1u :
            std::min(features, 2u);
        const bool status_error = is_mode("extract_bad_status");
        write_header(response, RBT_ORB_EXTRACT,
                     static_cast<uint32_t>(expected_response),
                     status_error ? RBT_ORB_INTERNAL_ERROR : 0,
                     status_error ? 0u : count,
                     status_error ? 0u :
                         (is_mode("extract_bad_mask") ? 0x3fu : 0x7fu));
        if (is_mode("extract_bad_version"))
            set_header_field(response, sizeof(uint32_t), 2u);
        if (is_mode("extract_bad_reserved"))
            set_header_field(response, offsetof(RbtOrbResponse, reserved), 1u);
        const size_t point_offset = sizeof(RbtOrbResponse);
        for (uint32_t index = 0u; index < std::min(count, features); ++index) {
            RbtOrbKeypoint point{};
            point.x = index == 0u ? 1.25f : 3.0f;
            point.y = index == 0u ? 0.5f : 1.0f;
            point.size = 7.0f + static_cast<float>(index);
            point.angle = 90.0f;
            point.response = 0.75f;
            point.octave = 1;
            if (is_mode("extract_bad_point")) point.angle = 360.0f;
            std::memcpy(response + point_offset +
                            static_cast<size_t>(index) * sizeof(point),
                        &point, sizeof(point));
        }
        const size_t descriptor_offset = sizeof(RbtOrbResponse) +
            static_cast<size_t>(features) * sizeof(RbtOrbKeypoint);
        for (uint32_t index = 0u; index < count && index < features; ++index) {
            std::memset(response + descriptor_offset +
                            static_cast<size_t>(index) *
                                RBT_ORB_DESCRIPTOR_BYTES,
                        static_cast<int>(0xa0u + index),
                        RBT_ORB_DESCRIPTOR_BYTES);
        }
        return is_mode("extract_rpc_error") ? -37 : 0;
    }

    if (parsed.operation == RBT_ORB_MATCH) {
        if (!valid_request(request, request_size, RBT_ORB_MATCH)) return -30;
        const uint32_t queries = parsed.args.match.query_count;
        const uint32_t trains = parsed.args.match.train_count;
        const size_t query_bytes =
            static_cast<size_t>(queries) * RBT_ORB_DESCRIPTOR_BYTES;
        const size_t train_bytes =
            static_cast<size_t>(trains) * RBT_ORB_DESCRIPTOR_BYTES;
        const size_t expected_request =
            sizeof(RbtOrbRequest) + query_bytes + train_bytes;
        const size_t expected_response = sizeof(RbtOrbResponse) +
            static_cast<size_t>(queries) * sizeof(RbtOrbMatch);
        if (queries == 0u || queries > RBT_ORB_MAX_FEATURES ||
            trains > RBT_ORB_MAX_FEATURES ||
            expected_request != static_cast<size_t>(request_size) ||
            expected_response > static_cast<size_t>(response_capacity))
            return -31;
        const uint8_t* query = request + sizeof(RbtOrbRequest);
        const uint8_t* train = query + query_bytes;
        if (queries == 2u && trains == 2u) {
            for (uint32_t byte = 0u; byte < RBT_ORB_DESCRIPTOR_BYTES; ++byte) {
                if (query[byte] != 0x11u ||
                    query[RBT_ORB_DESCRIPTOR_BYTES + byte] != 0x22u ||
                    train[byte] != 0x33u ||
                    train[RBT_ORB_DESCRIPTOR_BYTES + byte] != 0x44u)
                    return -32;
            }
        }
        const uint32_t count =
            is_mode("match_bad_count") ? queries - 1u : queries;
        write_header(response, RBT_ORB_MATCH,
                     static_cast<uint32_t>(expected_response), 0, count,
                     is_mode("match_bad_mask") ? 0x3fu : 0x7fu);
        for (uint32_t index = 0u; index < queries; ++index) {
            RbtOrbMatch match{};
            if (trains == 0u) {
                match.first_index = -1;
                match.second_index = -1;
                match.first_distance = 257u;
                match.second_distance = 257u;
            } else if (trains == 1u) {
                match.first_index = 0;
                match.second_index = -1;
                match.first_distance = 6u;
                match.second_distance = 257u;
            } else if (index == 0u) {
                match.first_index = 1;
                match.second_index = 0;
                match.first_distance = 3u;
                match.second_distance = 8u;
            } else {
                match.first_index = 0;
                match.second_index = 1;
                match.first_distance = 1u;
                match.second_distance = 2u;
            }
            if (is_mode("match_bad_index")) match.first_index = 99;
            if (is_mode("match_bad_order")) {
                match.first_distance = 9u;
                match.second_distance = 3u;
            }
            if (is_mode("match_bad_distance")) match.first_distance = 257u;
            std::memcpy(response + sizeof(RbtOrbResponse) +
                            static_cast<size_t>(index) * sizeof(match),
                        &match, sizeof(match));
        }
        if (is_mode("match_bad_version"))
            set_header_field(response, sizeof(uint32_t), 2u);
        if (is_mode("match_bad_reserved"))
            set_header_field(response, offsetof(RbtOrbResponse, reserved), 1u);
        return is_mode("match_rpc_error") ? -38 : 0;
    }
    return -39;
}

extern "C" void rbt_m3t_orb_fake_reset() {
    g_open_count = 0;
    g_close_count = 0;
    g_execute_count = 0;
    g_prepare_count = 0;
    g_prepare_before_open_count = 0;
    g_uri_valid = 0;
    g_adsp_path_valid = 0;
}

extern "C" int rbt_m3t_orb_fake_open_count() { return g_open_count.load(); }
extern "C" int rbt_m3t_orb_fake_close_count() { return g_close_count.load(); }
extern "C" int rbt_m3t_orb_fake_prepare_count() {
    return g_prepare_count.load();
}
extern "C" int rbt_m3t_orb_fake_prepare_before_open_count() {
    return g_prepare_before_open_count.load();
}
extern "C" int rbt_m3t_orb_fake_execute_count() {
    return g_execute_count.load();
}
extern "C" int rbt_m3t_orb_fake_uri_valid() { return g_uri_valid.load(); }
extern "C" int rbt_m3t_orb_fake_adsp_path_valid() {
    return g_adsp_path_valid.load();
}
