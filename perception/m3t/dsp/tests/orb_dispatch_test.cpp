// SPDX-License-Identifier: MIT
#include "rbt_orb_protocol.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace {

const uint8_t kCanary = 0xA5u;

RbtOrbRequest make_header(uint32_t operation, uint32_t response_capacity) {
    RbtOrbRequest request = {};
    request.magic = RBT_ORB_REQUEST_MAGIC;
    request.version = RBT_ORB_PROTOCOL_VERSION;
    request.operation = operation;
    request.request_bytes = sizeof(RbtOrbRequest);
    request.response_capacity = response_capacity;
    return request;
}

void store_header(std::vector<uint8_t>* bytes,
                  const RbtOrbRequest& request_header) {
    RbtOrbRequest header = request_header;
    header.request_bytes = static_cast<uint32_t>(bytes->size());
    memcpy(bytes->data(), &header, sizeof(header));
}

RbtOrbRequest load_header(const std::vector<uint8_t>& bytes) {
    RbtOrbRequest header;
    assert(bytes.size() >= sizeof(header));
    memcpy(&header, bytes.data(), sizeof(header));
    return header;
}

RbtOrbResponse load_response(const uint8_t* bytes) {
    RbtOrbResponse header;
    memcpy(&header, bytes, sizeof(header));
    return header;
}

std::vector<uint8_t> make_capabilities_request(uint32_t response_capacity) {
    std::vector<uint8_t> bytes(sizeof(RbtOrbRequest), 0u);
    store_header(&bytes,
                 make_header(RBT_ORB_CAPABILITIES, response_capacity));
    return bytes;
}

std::vector<uint8_t> make_match_request(uint32_t query_count,
                                       uint32_t train_count,
                                       uint32_t response_capacity) {
    const size_t payload_bytes =
        static_cast<size_t>(query_count + train_count) *
        RBT_ORB_DESCRIPTOR_BYTES;
    std::vector<uint8_t> bytes(sizeof(RbtOrbRequest) + payload_bytes, 0u);
    RbtOrbRequest header = make_header(RBT_ORB_MATCH, response_capacity);
    header.args.match.query_count = query_count;
    header.args.match.train_count = train_count;
    store_header(&bytes, header);
    for (size_t index = sizeof(RbtOrbRequest); index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>((index * 37u + 11u) & 0xffu);
    }
    return bytes;
}

std::vector<uint8_t> make_extract_request(uint32_t width, uint32_t height,
                                          uint32_t stride,
                                          const RbtOrbConfig& config,
                                          uint32_t response_capacity) {
    const size_t image_bytes = static_cast<size_t>(stride) * height;
    std::vector<uint8_t> bytes(sizeof(RbtOrbRequest) + image_bytes, 0u);
    RbtOrbRequest header = make_header(RBT_ORB_EXTRACT, response_capacity);
    header.args.extract.width = width;
    header.args.extract.height = height;
    header.args.extract.stride = stride;
    header.args.extract.config = config;
    header.args.extract.image_bytes = static_cast<uint32_t>(image_bytes);
    store_header(&bytes, header);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < stride; ++x) {
            const uint32_t value = x * 37u + y * 53u +
                                   ((x / 7u + y / 9u) & 1u) * 101u +
                                   (x * y * 13u);
            bytes[sizeof(RbtOrbRequest) +
                  static_cast<size_t>(y) * stride + x] =
                static_cast<uint8_t>(value & 0xffu);
        }
    }
    return bytes;
}

bool expect_error(const std::vector<uint8_t>& request,
                  uint32_t request_bytes, uint32_t response_capacity,
                  int expected_status) {
    std::vector<uint8_t> response(response_capacity + 8u, kCanary);
    const int status = rbt_orb_dispatch(
        request.data(), request_bytes, response.data(), response_capacity,
        nullptr, 0u);
    if (status != expected_status) {
        std::fprintf(stderr, "expected status %d, got %d\n", expected_status,
                     status);
        return false;
    }
    if (response_capacity >= sizeof(RbtOrbResponse)) {
        const RbtOrbResponse header = load_response(response.data());
        if (header.magic != RBT_ORB_RESPONSE_MAGIC ||
            header.version != RBT_ORB_PROTOCOL_VERSION ||
            header.status != expected_status || header.count != 0u ||
            header.response_bytes != sizeof(RbtOrbResponse)) {
            std::fprintf(stderr, "error response header is inconsistent\n");
            return false;
        }
    }
    for (size_t index = response_capacity; index < response.size(); ++index) {
        if (response[index] != kCanary) {
            std::fprintf(stderr, "dispatcher wrote beyond response capacity\n");
            return false;
        }
    }
    return true;
}

uint32_t independent_distance(const uint8_t* lhs, const uint8_t* rhs) {
    uint32_t distance = 0u;
    for (uint32_t byte = 0; byte < RBT_ORB_DESCRIPTOR_BYTES; ++byte) {
        uint8_t value = static_cast<uint8_t>(lhs[byte] ^ rhs[byte]);
        for (uint32_t bit = 0; bit < 8u; ++bit) {
            distance += value & 1u;
            value = static_cast<uint8_t>(value >> 1u);
        }
    }
    return distance;
}

RbtOrbMatch independent_match(const uint8_t* query, const uint8_t* train,
                              uint32_t train_count) {
    RbtOrbMatch result = {-1, -1, 257u, 257u};
    for (uint32_t train_index = 0; train_index < train_count; ++train_index) {
        const uint32_t distance = independent_distance(
            query, train + static_cast<size_t>(train_index) *
                       RBT_ORB_DESCRIPTOR_BYTES);
        if (distance < result.first_distance) {
            result.second_index = result.first_index;
            result.second_distance = result.first_distance;
            result.first_index = static_cast<int32_t>(train_index);
            result.first_distance = distance;
        } else if (distance < result.second_distance) {
            result.second_index = static_cast<int32_t>(train_index);
            result.second_distance = distance;
        }
    }
    return result;
}

bool same_match(const RbtOrbMatch& lhs, const RbtOrbMatch& rhs) {
    return lhs.first_index == rhs.first_index &&
           lhs.second_index == rhs.second_index &&
           lhs.first_distance == rhs.first_distance &&
           lhs.second_distance == rhs.second_distance;
}

bool test_capabilities_and_unaligned_headers() {
    std::vector<uint8_t> request = make_capabilities_request(72u);
    std::vector<uint8_t> unaligned_request(request.size() + 1u, kCanary);
    memcpy(unaligned_request.data() + 1u, request.data(), request.size());
    std::vector<uint8_t> response(73u, kCanary);
    const int status = rbt_orb_dispatch(
        unaligned_request.data() + 1u,
        static_cast<uint32_t>(request.size()), response.data() + 1u, 72u,
        nullptr, 0u);
    assert(status == RBT_ORB_OK);
    const RbtOrbResponse header = load_response(response.data() + 1u);
    assert(header.magic == RBT_ORB_RESPONSE_MAGIC);
    assert(header.version == RBT_ORB_PROTOCOL_VERSION);
    assert(header.operation == RBT_ORB_CAPABILITIES);
    assert(header.response_bytes == 64u);
    assert(header.status == RBT_ORB_OK);
    assert(header.count == 1u);
    assert(header.implemented_features == 0x7fu);
    assert(header.reserved == 0u);

    RbtOrbCapabilities capabilities;
    memcpy(&capabilities, response.data() + 1u + sizeof(RbtOrbResponse),
           sizeof(capabilities));
    assert(capabilities.max_width == RBT_ORB_MAX_WIDTH);
    assert(capabilities.max_height == RBT_ORB_MAX_HEIGHT);
    assert(capabilities.max_features == RBT_ORB_MAX_FEATURES);
    assert(capabilities.max_levels == RBT_ORB_MAX_LEVELS);
    assert(capabilities.workspace_capacity == RBT_ORB_WORKSPACE_CAPACITY);
    assert(capabilities.implementation_version ==
           RBT_ORB_IMPLEMENTATION_VERSION);
    assert(capabilities.reserved[0] == 0u && capabilities.reserved[1] == 0u);
    assert(response[0] == kCanary && response[65] == kCanary);
    for (size_t index = 65u; index < response.size(); ++index) {
        assert(response[index] == kCanary);
    }
    return true;
}

bool test_matching() {
    const uint32_t query_count = 3u;
    const uint32_t train_count = 5u;
    const uint32_t response_capacity =
        sizeof(RbtOrbResponse) + query_count * sizeof(RbtOrbMatch);
    std::vector<uint8_t> request =
        make_match_request(query_count, train_count, response_capacity);
    uint8_t* const payload = request.data() + sizeof(RbtOrbRequest);
    memset(payload, 0, static_cast<size_t>(query_count + train_count) *
                           RBT_ORB_DESCRIPTOR_BYTES);
    uint8_t* const train = payload +
        static_cast<size_t>(query_count) * RBT_ORB_DESCRIPTOR_BYTES;
    train[0] = 1u;
    train[RBT_ORB_DESCRIPTOR_BYTES] = 1u;
    for (uint32_t byte = 0; byte < RBT_ORB_DESCRIPTOR_BYTES; ++byte) {
        train[3u * RBT_ORB_DESCRIPTOR_BYTES + byte] =
            static_cast<uint8_t>(byte * 19u + 3u);
        train[4u * RBT_ORB_DESCRIPTOR_BYTES + byte] =
            static_cast<uint8_t>(byte * 7u + 5u);
    }

    std::vector<uint8_t> response(response_capacity + 8u, kCanary);
    const int status = rbt_orb_dispatch(
        request.data(), static_cast<uint32_t>(request.size()), response.data(),
        response_capacity, nullptr, 0u);
    assert(status == RBT_ORB_OK);
    const RbtOrbResponse header = load_response(response.data());
    assert(header.operation == RBT_ORB_MATCH);
    assert(header.response_bytes == response_capacity);
    assert(header.status == RBT_ORB_OK);
    assert(header.count == query_count);
    assert(header.implemented_features == 0x7fu);

    for (uint32_t query_index = 0; query_index < query_count; ++query_index) {
        const RbtOrbMatch expected = independent_match(
            payload + static_cast<size_t>(query_index) *
                          RBT_ORB_DESCRIPTOR_BYTES,
            train, train_count);
        RbtOrbMatch actual;
        memcpy(&actual,
               response.data() + sizeof(RbtOrbResponse) +
                   static_cast<size_t>(query_index) * sizeof(RbtOrbMatch),
               sizeof(actual));
        assert(same_match(actual, expected));
    }
    for (size_t index = response_capacity; index < response.size(); ++index) {
        assert(response[index] == kCanary);
    }

    std::vector<uint8_t> empty_request = make_match_request(0u, 0u, 32u);
    std::vector<uint8_t> empty_response(40u, kCanary);
    assert(rbt_orb_dispatch(empty_request.data(),
                            static_cast<uint32_t>(empty_request.size()),
                            empty_response.data(), 32u, nullptr, 0u) ==
           RBT_ORB_OK);
    const RbtOrbResponse empty_header = load_response(empty_response.data());
    assert(empty_header.response_bytes == sizeof(RbtOrbResponse));
    assert(empty_header.count == 0u);
    for (size_t index = 32u; index < empty_response.size(); ++index) {
        assert(empty_response[index] == kCanary);
    }
    return true;
}

bool test_extract_serialization() {
    const uint32_t width = 96u;
    const uint32_t height = 88u;
    const uint32_t stride = width + 5u;
    RbtOrbConfig config = {64u, 2u, 1.2f, 10u};
    const uint32_t response_capacity = static_cast<uint32_t>(
        sizeof(RbtOrbResponse) + config.n_features *
            (sizeof(RbtOrbKeypoint) + RBT_ORB_DESCRIPTOR_BYTES));
    std::vector<uint8_t> request = make_extract_request(
        width, height, stride, config, response_capacity);
    std::vector<uint8_t> response(response_capacity + 8u, kCanary);
    alignas(128) static uint8_t workspace[512u * 1024u];
    const size_t workspace_bytes =
        rbt_orb_workspace_size(width, height, &config);
    assert(workspace_bytes > 0u && workspace_bytes <= sizeof(workspace));

    const int status = rbt_orb_dispatch(
        request.data(), static_cast<uint32_t>(request.size()), response.data(),
        response_capacity, workspace, workspace_bytes);
    assert(status == RBT_ORB_OK);
    const RbtOrbResponse header = load_response(response.data());
    assert(header.operation == RBT_ORB_EXTRACT);
    assert(header.response_bytes == response_capacity);
    assert(header.status == RBT_ORB_OK);
    assert(header.implemented_features == 0x7fu);

    const uint8_t* const image = request.data() + sizeof(RbtOrbRequest);
    RbtOrbKeypoint direct_keypoints[RBT_ORB_MAX_FEATURES] = {};
    std::vector<uint8_t> direct_descriptors(
        static_cast<size_t>(config.n_features) * RBT_ORB_DESCRIPTOR_BYTES,
        0u);
    uint32_t direct_count = 0u;
    assert(rbt_orb_extract(image, width, height, stride, &config,
                           direct_keypoints, direct_descriptors.data(),
                           config.n_features, &direct_count, workspace,
                           workspace_bytes) == RBT_ORB_OK);
    assert(header.count == direct_count);

    const uint8_t* const serialized_keypoints =
        response.data() + sizeof(RbtOrbResponse);
    const uint8_t* const serialized_descriptors = serialized_keypoints +
        static_cast<size_t>(config.n_features) * sizeof(RbtOrbKeypoint);
    assert(memcmp(serialized_keypoints, direct_keypoints,
                  static_cast<size_t>(direct_count) *
                      sizeof(RbtOrbKeypoint)) == 0);
    assert(memcmp(serialized_descriptors, direct_descriptors.data(),
                  static_cast<size_t>(direct_count) *
                      RBT_ORB_DESCRIPTOR_BYTES) == 0);
    for (size_t index = static_cast<size_t>(direct_count) *
                        sizeof(RbtOrbKeypoint);
         index < static_cast<size_t>(config.n_features) *
                     sizeof(RbtOrbKeypoint);
         ++index) {
        assert(serialized_keypoints[index] == 0u);
    }
    for (size_t index = static_cast<size_t>(direct_count) *
                        RBT_ORB_DESCRIPTOR_BYTES;
         index < static_cast<size_t>(config.n_features) *
                     RBT_ORB_DESCRIPTOR_BYTES;
         ++index) {
        assert(serialized_descriptors[index] == 0u);
    }
    for (size_t index = response_capacity; index < response.size(); ++index) {
        assert(response[index] == kCanary);
    }

    std::vector<uint8_t> short_workspace_response(response_capacity + 8u,
                                                   kCanary);
    assert(workspace_bytes > 1u);
    assert(rbt_orb_dispatch(request.data(),
                            static_cast<uint32_t>(request.size()),
                            short_workspace_response.data(), response_capacity,
                            workspace, workspace_bytes - 1u) ==
           RBT_ORB_WORKSPACE_TOO_SMALL);
    assert(load_response(short_workspace_response.data()).status ==
           RBT_ORB_WORKSPACE_TOO_SMALL);
    for (size_t index = sizeof(RbtOrbResponse);
         index < short_workspace_response.size(); ++index) {
        assert(short_workspace_response[index] == kCanary);
    }

    const RbtOrbConfig empty_config = {16u, 1u, 1.2f, 10u};
    const uint32_t empty_response_capacity = static_cast<uint32_t>(
        sizeof(RbtOrbResponse) + empty_config.n_features *
            (sizeof(RbtOrbKeypoint) + RBT_ORB_DESCRIPTOR_BYTES));
    std::vector<uint8_t> empty_request =
        make_extract_request(30u, 30u, 30u, empty_config,
                             empty_response_capacity);
    std::vector<uint8_t> empty_output(empty_response_capacity + 8u, kCanary);
    const size_t empty_workspace_bytes =
        rbt_orb_workspace_size(30u, 30u, &empty_config);
    assert(empty_workspace_bytes > 0u &&
           empty_workspace_bytes <= sizeof(workspace));
    assert(rbt_orb_dispatch(empty_request.data(),
                            static_cast<uint32_t>(empty_request.size()),
                            empty_output.data(), empty_response_capacity,
                            workspace, empty_workspace_bytes) == RBT_ORB_OK);
    const RbtOrbResponse empty_extract_header =
        load_response(empty_output.data());
    assert(empty_extract_header.count == 0u);
    for (size_t index = sizeof(RbtOrbResponse);
         index < empty_response_capacity; ++index) {
        assert(empty_output[index] == 0u);
    }
    for (size_t index = empty_response_capacity; index < empty_output.size();
         ++index) {
        assert(empty_output[index] == kCanary);
    }
    return true;
}

bool test_malformed_and_capacity() {
    std::vector<uint8_t> request = make_capabilities_request(64u);
    request.resize(63u);
    assert(expect_error(request, 63u, 64u, RBT_ORB_INVALID_ARGUMENT));
    request = make_capabilities_request(64u);

    RbtOrbRequest header = load_header(request);
    header.magic ^= 1u;
    store_header(&request, header);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 64u,
                        RBT_ORB_INVALID_ARGUMENT));
    header = load_header(request);
    header.magic = RBT_ORB_REQUEST_MAGIC;
    header.version += 1u;
    store_header(&request, header);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 64u,
                        RBT_ORB_INVALID_ARGUMENT));
    header = load_header(request);
    header.version = RBT_ORB_PROTOCOL_VERSION;
    header.reserved[1] = 1u;
    store_header(&request, header);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 64u,
                        RBT_ORB_INVALID_ARGUMENT));
    header = load_header(request);
    header.reserved[1] = 0u;
    header.args.words[3] = 1u;
    store_header(&request, header);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 64u,
                        RBT_ORB_INVALID_ARGUMENT));

    request = make_capabilities_request(64u);
    header = load_header(request);
    header.request_bytes -= 1u;
    memcpy(request.data(), &header, sizeof(header));
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 64u,
                        RBT_ORB_INVALID_ARGUMENT));
    request = make_capabilities_request(64u);
    header = load_header(request);
    header.operation = 99u;
    store_header(&request, header);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 64u,
                        RBT_ORB_INVALID_ARGUMENT));

    request = make_capabilities_request(64u);
    request.push_back(0u);
    header = load_header(request);
    header.request_bytes = static_cast<uint32_t>(request.size());
    store_header(&request, header);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 64u,
                        RBT_ORB_INVALID_ARGUMENT));

    request = make_capabilities_request(64u);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 65u,
                        RBT_ORB_INVALID_ARGUMENT));
    request = make_capabilities_request(40u);
    assert(expect_error(request, static_cast<uint32_t>(request.size()), 40u,
                        RBT_ORB_OUTPUT_CAPACITY));

    std::vector<uint8_t> match = make_match_request(0u, 0u, 32u);
    header = load_header(match);
    header.args.match.query_count = RBT_ORB_MAX_FEATURES + 1u;
    store_header(&match, header);
    assert(expect_error(match, static_cast<uint32_t>(match.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    match = make_match_request(0u, 0u, 32u);
    header = load_header(match);
    header.args.match.reserved[5] = 1u;
    store_header(&match, header);
    assert(expect_error(match, static_cast<uint32_t>(match.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    match = make_match_request(1u, 1u, 32u);
    match.resize(match.size() - 1u);
    header = load_header(match);
    header.request_bytes = static_cast<uint32_t>(match.size());
    store_header(&match, header);
    assert(expect_error(match, static_cast<uint32_t>(match.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    match = make_match_request(1u, 1u, 32u);
    assert(expect_error(match, static_cast<uint32_t>(match.size()), 32u,
                        RBT_ORB_OUTPUT_CAPACITY));
    match = make_match_request(0u, RBT_ORB_MAX_FEATURES + 1u, 32u);
    header = load_header(match);
    header.request_bytes = sizeof(RbtOrbRequest);
    store_header(&match, header);
    assert(expect_error(match, static_cast<uint32_t>(match.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));

    const RbtOrbConfig good_config = {64u, 2u, 1.2f, 10u};
    std::vector<uint8_t> extract =
        make_extract_request(96u, 88u, 101u, good_config, 32u);
    header = load_header(extract);
    header.args.extract.width = 0u;
    store_header(&extract, header);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    extract = make_extract_request(96u, 88u, 101u, good_config, 32u);
    header = load_header(extract);
    header.args.extract.width = RBT_ORB_MAX_WIDTH + 1u;
    store_header(&extract, header);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    extract = make_extract_request(96u, 88u, 101u, good_config, 32u);
    header = load_header(extract);
    header.args.extract.height = RBT_ORB_MAX_HEIGHT + 1u;
    store_header(&extract, header);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    extract = make_extract_request(96u, 88u, 101u, good_config, 32u);
    header = load_header(extract);
    header.args.extract.stride = 95u;
    store_header(&extract, header);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    extract = make_extract_request(96u, 88u, 101u, good_config, 32u);
    header = load_header(extract);
    header.args.extract.stride = RBT_ORB_MAX_WIDTH + 1u;
    store_header(&extract, header);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    extract = make_extract_request(96u, 88u, 101u, good_config, 32u);
    header = load_header(extract);
    header.args.extract.image_bytes += 1u;
    store_header(&extract, header);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    extract = make_extract_request(96u, 88u, 101u, good_config, 32u);
    header = load_header(extract);
    header.args.extract.config.scale_factor =
        std::numeric_limits<float>::quiet_NaN();
    store_header(&extract, header);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    RbtOrbConfig bad_feature_config = good_config;
    bad_feature_config.n_features = RBT_ORB_MAX_FEATURES + 1u;
    extract = make_extract_request(96u, 88u, 101u, bad_feature_config, 32u);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    RbtOrbConfig bad_level_config = good_config;
    bad_level_config.n_levels = RBT_ORB_MAX_LEVELS + 1u;
    extract = make_extract_request(96u, 88u, 101u, bad_level_config, 32u);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_INVALID_ARGUMENT));
    extract = make_extract_request(96u, 88u, 101u, good_config, 32u);
    assert(expect_error(extract, static_cast<uint32_t>(extract.size()), 32u,
                        RBT_ORB_OUTPUT_CAPACITY));

    std::vector<uint8_t> unaligned_request = make_match_request(1u, 1u, 48u);
    std::vector<uint8_t> unaligned_output(49u, kCanary);
    assert(rbt_orb_dispatch(unaligned_request.data(),
                            static_cast<uint32_t>(unaligned_request.size()),
                            unaligned_output.data() + 1u, 48u, nullptr, 0u) ==
           RBT_ORB_INVALID_ARGUMENT);
    assert(load_response(unaligned_output.data() + 1u).status ==
           RBT_ORB_INVALID_ARGUMENT);
    assert(unaligned_output[0] == kCanary);
    for (size_t index = 33u; index < 49u; ++index) {
        assert(unaligned_output[index] == kCanary);
    }

    std::vector<uint8_t> short_response(31u, kCanary);
    assert(rbt_orb_dispatch(request.data(),
                            static_cast<uint32_t>(request.size()),
                            short_response.data(), 31u, nullptr, 0u) ==
           RBT_ORB_OUTPUT_CAPACITY);
    for (uint8_t value : short_response) assert(value == kCanary);
    return true;
}

}  // namespace

int main() {
    assert(test_capabilities_and_unaligned_headers());
    assert(test_matching());
    assert(test_extract_serialization());
    assert(test_malformed_and_capacity());
    std::puts("orb_dispatch_test: all checks passed");
    return 0;
}
