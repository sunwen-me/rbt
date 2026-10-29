// SPDX-License-Identifier: MIT
#include "rbt_orb_protocol.h"

#include <string.h>

namespace {

bool checked_add(size_t lhs, size_t rhs, size_t* result) {
    const size_t size_max = static_cast<size_t>(-1);
    if (lhs > size_max - rhs) return false;
    *result = lhs + rhs;
    return true;
}

bool checked_multiply(size_t lhs, size_t rhs, size_t* result) {
    const size_t size_max = static_cast<size_t>(-1);
    if (lhs != 0u && rhs > size_max / lhs) return false;
    *result = lhs * rhs;
    return true;
}

bool valid_address_span(const void* pointer, size_t bytes) {
    if (bytes == 0u) return true;
    if (pointer == nullptr) return false;
    const uintmax_t address_max =
        static_cast<uintmax_t>(static_cast<uintptr_t>(-1));
    const uintmax_t address =
        static_cast<uintmax_t>(reinterpret_cast<uintptr_t>(pointer));
    const uintmax_t last_byte_offset = static_cast<uintmax_t>(bytes - 1u);
    return last_byte_offset <= address_max &&
           address <= address_max - last_byte_offset;
}

bool is_aligned(const void* pointer, size_t alignment) {
    return (reinterpret_cast<uintptr_t>(pointer) % alignment) == 0u;
}

bool zero_words(const uint32_t* words, size_t count) {
    for (size_t index = 0u; index < count; ++index) {
        if (words[index] != 0u) return false;
    }
    return true;
}

void write_response(uint8_t* response, uint32_t operation,
                    uint32_t response_bytes, int status, uint32_t count,
                    uint32_t implemented_features) {
    RbtOrbResponse header = {};
    header.magic = RBT_ORB_RESPONSE_MAGIC;
    header.version = RBT_ORB_PROTOCOL_VERSION;
    header.operation = operation;
    header.response_bytes = response_bytes;
    header.status = status;
    header.count = count;
    header.implemented_features = implemented_features;
    memcpy(response, &header, sizeof(header));
}

int fail(uint8_t* response, uint32_t operation, int status) {
    write_response(response, operation, sizeof(RbtOrbResponse), status, 0u,
                   0u);
    return status;
}

bool valid_config(const RbtOrbConfig& config) {
    return config.n_features >= 1u &&
           config.n_features <= RBT_ORB_MAX_FEATURES &&
           config.n_levels >= 1u && config.n_levels <= RBT_ORB_MAX_LEVELS &&
           config.scale_factor >= 1.05f && config.scale_factor <= 2.0f &&
           config.fast_threshold >= 1u && config.fast_threshold <= 255u;
}

}  // namespace

extern "C" int rbt_orb_dispatch(const uint8_t* request,
                                uint32_t request_bytes,
                                uint8_t* response,
                                uint32_t response_capacity,
                                void* workspace,
                                size_t workspace_bytes) {
    if (response == nullptr) return RBT_ORB_INVALID_ARGUMENT;
    if (response_capacity < sizeof(RbtOrbResponse)) {
        return RBT_ORB_OUTPUT_CAPACITY;
    }
    if (!valid_address_span(response, sizeof(RbtOrbResponse))) {
        return RBT_ORB_INVALID_ARGUMENT;
    }

    uint32_t operation = 0u;
    if (request == nullptr || request_bytes < sizeof(RbtOrbRequest) ||
        !valid_address_span(request, sizeof(RbtOrbRequest))) {
        return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
    }

    RbtOrbRequest parsed_request;
    memcpy(&parsed_request, request, sizeof(parsed_request));
    operation = parsed_request.operation;

    if (parsed_request.magic != RBT_ORB_REQUEST_MAGIC ||
        parsed_request.version != RBT_ORB_PROTOCOL_VERSION ||
        parsed_request.request_bytes != request_bytes ||
        parsed_request.response_capacity != response_capacity ||
        !zero_words(parsed_request.reserved, 3u)) {
        return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
    }

    size_t expected_request_bytes = sizeof(RbtOrbRequest);
    size_t expected_response_bytes = sizeof(RbtOrbResponse);
    uint32_t response_count = 0u;
    uint32_t implemented_features = RBT_ORB_REQUIRED_FEATURES;
    size_t workspace_required = 0u;

    switch (operation) {
        case RBT_ORB_CAPABILITIES:
            if (!zero_words(parsed_request.args.words, 8u)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }
            if (!checked_add(sizeof(RbtOrbResponse),
                             sizeof(RbtOrbCapabilities),
                             &expected_response_bytes)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }
            response_count = 1u;
            break;

        case RBT_ORB_EXTRACT: {
            const uint32_t width = parsed_request.args.extract.width;
            const uint32_t height = parsed_request.args.extract.height;
            const uint32_t stride = parsed_request.args.extract.stride;
            const RbtOrbConfig config = parsed_request.args.extract.config;
            if (width == 0u || width > RBT_ORB_MAX_WIDTH || height == 0u ||
                height > RBT_ORB_MAX_HEIGHT || stride < width ||
                stride > RBT_ORB_MAX_WIDTH || !valid_config(config)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }

            size_t image_bytes = 0u;
            if (!checked_multiply(stride, height, &image_bytes) ||
                image_bytes > 1024u * 1024u ||
                image_bytes != parsed_request.args.extract.image_bytes) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }
            if (!checked_add(sizeof(RbtOrbRequest), image_bytes,
                             &expected_request_bytes) ||
                expected_request_bytes != request_bytes ||
                !valid_address_span(request, expected_request_bytes)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }

            size_t keypoint_bytes = 0u;
            size_t descriptor_bytes = 0u;
            if (!checked_multiply(config.n_features,
                                  sizeof(RbtOrbKeypoint), &keypoint_bytes) ||
                !checked_multiply(config.n_features,
                                  RBT_ORB_DESCRIPTOR_BYTES,
                                  &descriptor_bytes) ||
                !checked_add(sizeof(RbtOrbResponse), keypoint_bytes,
                             &expected_response_bytes) ||
                !checked_add(expected_response_bytes, descriptor_bytes,
                             &expected_response_bytes)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }
            if (response_capacity < expected_response_bytes) {
                return fail(response, operation, RBT_ORB_OUTPUT_CAPACITY);
            }
            if (!valid_address_span(response, expected_response_bytes) ||
                !is_aligned(response, alignof(RbtOrbKeypoint))) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }

            workspace_required =
                rbt_orb_workspace_size(width, height, &config);
            if (workspace_required == 0u ||
                workspace_bytes > RBT_ORB_WORKSPACE_CAPACITY ||
                workspace == nullptr || !is_aligned(workspace, 128u) ||
                !valid_address_span(workspace, workspace_required)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }
            if (workspace_bytes < workspace_required) {
                return fail(response, operation, RBT_ORB_WORKSPACE_TOO_SMALL);
            }
            break;
        }

        case RBT_ORB_MATCH: {
            const uint32_t query_count = parsed_request.args.match.query_count;
            const uint32_t train_count = parsed_request.args.match.train_count;
            if (query_count > RBT_ORB_MAX_FEATURES ||
                train_count > RBT_ORB_MAX_FEATURES ||
                !zero_words(parsed_request.args.match.reserved, 6u)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }

            size_t query_bytes = 0u;
            size_t train_bytes = 0u;
            if (!checked_multiply(query_count, RBT_ORB_DESCRIPTOR_BYTES,
                                  &query_bytes) ||
                !checked_multiply(train_count, RBT_ORB_DESCRIPTOR_BYTES,
                                  &train_bytes) ||
                !checked_add(sizeof(RbtOrbRequest), query_bytes,
                             &expected_request_bytes) ||
                !checked_add(expected_request_bytes, train_bytes,
                             &expected_request_bytes) ||
                expected_request_bytes != request_bytes ||
                !valid_address_span(request, expected_request_bytes)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }

            size_t matches_bytes = 0u;
            if (!checked_multiply(query_count, sizeof(RbtOrbMatch),
                                  &matches_bytes) ||
                !checked_add(sizeof(RbtOrbResponse), matches_bytes,
                             &expected_response_bytes)) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }
            if (response_capacity < expected_response_bytes) {
                return fail(response, operation, RBT_ORB_OUTPUT_CAPACITY);
            }
            if (!valid_address_span(response, expected_response_bytes) ||
                !is_aligned(response, alignof(RbtOrbMatch))) {
                return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
            }
            response_count = query_count;
            break;
        }

        default:
            return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
    }

    if (expected_request_bytes != request_bytes ||
        !valid_address_span(request, expected_request_bytes)) {
        return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
    }
    if (response_capacity < expected_response_bytes) {
        return fail(response, operation, RBT_ORB_OUTPUT_CAPACITY);
    }
    if (!valid_address_span(response, expected_response_bytes)) {
        return fail(response, operation, RBT_ORB_INVALID_ARGUMENT);
    }

    uint8_t* const response_payload = response + sizeof(RbtOrbResponse);
    switch (operation) {
        case RBT_ORB_CAPABILITIES: {
            RbtOrbCapabilities capabilities = {};
            capabilities.max_width = RBT_ORB_MAX_WIDTH;
            capabilities.max_height = RBT_ORB_MAX_HEIGHT;
            capabilities.max_features = RBT_ORB_MAX_FEATURES;
            capabilities.max_levels = RBT_ORB_MAX_LEVELS;
            capabilities.workspace_capacity =
                RBT_ORB_WORKSPACE_CAPACITY;
            capabilities.implementation_version =
                RBT_ORB_IMPLEMENTATION_VERSION;
            memcpy(response_payload, &capabilities, sizeof(capabilities));
            break;
        }

        case RBT_ORB_EXTRACT: {
            const uint32_t width = parsed_request.args.extract.width;
            const uint32_t height = parsed_request.args.extract.height;
            const uint32_t stride = parsed_request.args.extract.stride;
            const RbtOrbConfig config = parsed_request.args.extract.config;
            const size_t keypoint_bytes =
                static_cast<size_t>(config.n_features) *
                sizeof(RbtOrbKeypoint);
            const size_t descriptor_bytes =
                static_cast<size_t>(config.n_features) *
                RBT_ORB_DESCRIPTOR_BYTES;
            RbtOrbKeypoint* const keypoints =
                reinterpret_cast<RbtOrbKeypoint*>(response_payload);
            uint8_t* const descriptors = response_payload + keypoint_bytes;
            memset(response_payload, 0, keypoint_bytes + descriptor_bytes);

            uint32_t count = 0u;
            const int status = rbt_orb_extract(
                request + sizeof(RbtOrbRequest), width, height, stride,
                &config, keypoints, descriptors, config.n_features, &count,
                workspace, workspace_bytes);
            if (status != RBT_ORB_OK) {
                write_response(response, operation,
                               static_cast<uint32_t>(expected_response_bytes),
                               status, 0u, 0u);
                return status;
            }
            response_count = count;
            break;
        }

        case RBT_ORB_MATCH: {
            const uint32_t query_count = parsed_request.args.match.query_count;
            const uint32_t train_count = parsed_request.args.match.train_count;
            const uint8_t* const query = request + sizeof(RbtOrbRequest);
            const uint8_t* const train = query +
                static_cast<size_t>(query_count) * RBT_ORB_DESCRIPTOR_BYTES;
            RbtOrbMatch* const matches =
                reinterpret_cast<RbtOrbMatch*>(response_payload);
            const int status = rbt_orb_match_2nn(
                query, query_count, RBT_ORB_DESCRIPTOR_BYTES, train,
                train_count, RBT_ORB_DESCRIPTOR_BYTES, matches, query_count);
            if (status != RBT_ORB_OK) {
                write_response(response, operation,
                               static_cast<uint32_t>(expected_response_bytes),
                               status, 0u, 0u);
                return status;
            }
            break;
        }

        default:
            return fail(response, operation, RBT_ORB_INTERNAL_ERROR);
    }

    write_response(response, operation,
                   static_cast<uint32_t>(expected_response_bytes),
                   RBT_ORB_OK, response_count, implemented_features);
    return RBT_ORB_OK;
}
