// SPDX-License-Identifier: MIT
#include "rbt_orb_core.h"

#include <string.h>

namespace {

bool valid_row_span(const void* data, uint32_t count, uint32_t stride,
                    size_t row_bytes) {
    if (count == 0) {
        return true;
    }
    if (data == nullptr || static_cast<size_t>(stride) < row_bytes) {
        return false;
    }

    const size_t size_max = static_cast<size_t>(-1);
    const size_t rows_before_last = static_cast<size_t>(count - 1);
    if (rows_before_last != 0 &&
        static_cast<size_t>(stride) > size_max / rows_before_last) {
        return false;
    }
    const size_t last_row_offset = rows_before_last * stride;
    if (last_row_offset > size_max - row_bytes) {
        return false;
    }

    const size_t span_bytes = last_row_offset + row_bytes;
    const uintmax_t address_max = static_cast<uintmax_t>(
        static_cast<uintptr_t>(-1));
    const uintmax_t address = static_cast<uintmax_t>(
        reinterpret_cast<uintptr_t>(data));
    const uintmax_t last_byte_offset = static_cast<uintmax_t>(span_bytes - 1);
    return last_byte_offset <= address_max &&
           address <= address_max - last_byte_offset;
}

uint32_t popcount64(uint64_t value) {
#if defined(__hexagon__)
    return static_cast<uint32_t>(__builtin_popcountll(value));
#else
    value -= (value >> 1) & UINT64_C(0x5555555555555555);
    value = (value & UINT64_C(0x3333333333333333)) +
            ((value >> 2) & UINT64_C(0x3333333333333333));
    value = (value + (value >> 4)) & UINT64_C(0x0f0f0f0f0f0f0f0f);
    return static_cast<uint32_t>(
        (value * UINT64_C(0x0101010101010101)) >> 56);
#endif
}

#if defined(__hexagon__)
inline uint32_t hamming_distance_aligned(const uint8_t* lhs,
                                         const uint8_t* rhs) {
    const uint8_t* aligned_lhs = static_cast<const uint8_t*>(
        __builtin_assume_aligned(lhs, 8));
    const uint8_t* aligned_rhs = static_cast<const uint8_t*>(
        __builtin_assume_aligned(rhs, 8));
    uint64_t lhs_words[4];
    uint64_t rhs_words[4];
    __builtin_memcpy(&lhs_words[0], aligned_lhs, sizeof(uint64_t));
    __builtin_memcpy(&lhs_words[1], aligned_lhs + 8, sizeof(uint64_t));
    __builtin_memcpy(&lhs_words[2], aligned_lhs + 16, sizeof(uint64_t));
    __builtin_memcpy(&lhs_words[3], aligned_lhs + 24, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[0], aligned_rhs, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[1], aligned_rhs + 8, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[2], aligned_rhs + 16, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[3], aligned_rhs + 24, sizeof(uint64_t));

    return popcount64(lhs_words[0] ^ rhs_words[0]) +
           popcount64(lhs_words[1] ^ rhs_words[1]) +
           popcount64(lhs_words[2] ^ rhs_words[2]) +
           popcount64(lhs_words[3] ^ rhs_words[3]);
}
#endif

uint32_t hamming_distance(const uint8_t* lhs, const uint8_t* rhs) {
#if defined(__hexagon__)
    if (((reinterpret_cast<uintptr_t>(lhs) |
          reinterpret_cast<uintptr_t>(rhs)) & 7u) == 0) {
        return hamming_distance_aligned(lhs, rhs);
    }
#endif

    uint64_t lhs_words[4];
    uint64_t rhs_words[4];
#if defined(__hexagon__)
    __builtin_memcpy(&lhs_words[0], lhs, sizeof(uint64_t));
    __builtin_memcpy(&lhs_words[1], lhs + 8, sizeof(uint64_t));
    __builtin_memcpy(&lhs_words[2], lhs + 16, sizeof(uint64_t));
    __builtin_memcpy(&lhs_words[3], lhs + 24, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[0], rhs, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[1], rhs + 8, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[2], rhs + 16, sizeof(uint64_t));
    __builtin_memcpy(&rhs_words[3], rhs + 24, sizeof(uint64_t));
#else
    memcpy(&lhs_words[0], lhs, sizeof(uint64_t));
    memcpy(&lhs_words[1], lhs + 8, sizeof(uint64_t));
    memcpy(&lhs_words[2], lhs + 16, sizeof(uint64_t));
    memcpy(&lhs_words[3], lhs + 24, sizeof(uint64_t));
    memcpy(&rhs_words[0], rhs, sizeof(uint64_t));
    memcpy(&rhs_words[1], rhs + 8, sizeof(uint64_t));
    memcpy(&rhs_words[2], rhs + 16, sizeof(uint64_t));
    memcpy(&rhs_words[3], rhs + 24, sizeof(uint64_t));
#endif

    return popcount64(lhs_words[0] ^ rhs_words[0]) +
           popcount64(lhs_words[1] ^ rhs_words[1]) +
           popcount64(lhs_words[2] ^ rhs_words[2]) +
           popcount64(lhs_words[3] ^ rhs_words[3]);
}

}  // namespace

extern "C" int rbt_orb_match_2nn(const uint8_t* query,
                                uint32_t query_count,
                                uint32_t query_stride,
                                const uint8_t* train,
                                uint32_t train_count,
                                uint32_t train_stride,
                                RbtOrbMatch* matches,
                                uint32_t capacity) {
    if (query_count > RBT_ORB_MAX_FEATURES ||
        train_count > RBT_ORB_MAX_FEATURES) {
        return RBT_ORB_INVALID_ARGUMENT;
    }
    if (!valid_row_span(query, query_count, query_stride,
                        RBT_ORB_DESCRIPTOR_BYTES) ||
        !valid_row_span(train, train_count, train_stride,
                        RBT_ORB_DESCRIPTOR_BYTES)) {
        return RBT_ORB_INVALID_ARGUMENT;
    }
    if (capacity < query_count) {
        return RBT_ORB_OUTPUT_CAPACITY;
    }
    if (query_count == 0) {
        return RBT_ORB_OK;
    }
    if (!valid_row_span(matches, query_count,
                        static_cast<uint32_t>(sizeof(RbtOrbMatch)),
                        sizeof(RbtOrbMatch))) {
        return RBT_ORB_INVALID_ARGUMENT;
    }

    for (uint32_t query_index = 0; query_index < query_count; ++query_index) {
        RbtOrbMatch result = {-1, -1, 257u, 257u};
        const uint8_t* query_row =
            query + static_cast<size_t>(query_index) * query_stride;

        for (uint32_t train_index = 0; train_index < train_count;
             ++train_index) {
            const uint8_t* train_row =
                train + static_cast<size_t>(train_index) * train_stride;
            const uint32_t distance = hamming_distance(query_row, train_row);
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

        matches[query_index] = result;
    }
    return RBT_ORB_OK;
}
