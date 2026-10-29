// SPDX-License-Identifier: MIT
#include "rbt_orb_core.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <random>
#include <vector>

namespace {

uint32_t reference_distance(const uint8_t* lhs, const uint8_t* rhs) {
    uint32_t distance = 0;
    for (uint32_t byte_index = 0; byte_index < RBT_ORB_DESCRIPTOR_BYTES;
         ++byte_index) {
        uint8_t bits = static_cast<uint8_t>(lhs[byte_index] ^ rhs[byte_index]);
        while (bits != 0) {
            distance += bits & 1u;
            bits = static_cast<uint8_t>(bits >> 1);
        }
    }
    return distance;
}

std::vector<RbtOrbMatch> reference_matches(const uint8_t* query,
                                           uint32_t query_count,
                                           uint32_t query_stride,
                                           const uint8_t* train,
                                           uint32_t train_count,
                                           uint32_t train_stride) {
    std::vector<RbtOrbMatch> expected(query_count);
    for (uint32_t query_index = 0; query_index < query_count; ++query_index) {
        RbtOrbMatch result = {-1, -1, 257u, 257u};
        const uint8_t* query_row = query + query_index * query_stride;
        for (uint32_t train_index = 0; train_index < train_count;
             ++train_index) {
            const uint8_t* train_row = train + train_index * train_stride;
            const uint32_t distance = reference_distance(query_row, train_row);
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
        expected[query_index] = result;
    }
    return expected;
}

void assert_match_equal(const RbtOrbMatch& actual,
                        const RbtOrbMatch& expected) {
    assert(actual.first_index == expected.first_index);
    assert(actual.second_index == expected.second_index);
    assert(actual.first_distance == expected.first_distance);
    assert(actual.second_distance == expected.second_distance);
}

void assert_matches_equal(const std::vector<RbtOrbMatch>& actual,
                          const std::vector<RbtOrbMatch>& expected) {
    assert(actual.size() == expected.size());
    for (size_t index = 0; index < actual.size(); ++index) {
        assert_match_equal(actual[index], expected[index]);
    }
}

void fill_descriptors(std::vector<uint8_t>* storage, uint32_t count,
                      uint32_t stride, std::mt19937* random) {
    storage->assign(static_cast<size_t>(count) * stride, 0xA5u);
    for (uint32_t row = 0; row < count; ++row) {
        for (uint32_t byte_index = 0;
             byte_index < RBT_ORB_DESCRIPTOR_BYTES; ++byte_index) {
            (*storage)[static_cast<size_t>(row) * stride + byte_index] =
                static_cast<uint8_t>((*random)());
        }
    }
}

void test_random_batches() {
    std::mt19937 random(0x4F524231u);
    for (uint32_t iteration = 0; iteration < 100; ++iteration) {
        const uint32_t query_count = iteration % 9;
        const uint32_t train_count = (iteration * 7) % 12;
        const uint32_t query_stride = 32 + (iteration % 7);
        const uint32_t train_stride = 32 + ((iteration * 3) % 9);
        std::vector<uint8_t> query_storage;
        std::vector<uint8_t> train_storage;
        fill_descriptors(&query_storage, query_count, query_stride, &random);
        fill_descriptors(&train_storage, train_count, train_stride, &random);

        const uint8_t* query =
            query_count == 0 ? nullptr : query_storage.data();
        const uint8_t* train =
            train_count == 0 ? nullptr : train_storage.data();
        std::vector<RbtOrbMatch> actual(query_count);
        const int status = rbt_orb_match_2nn(
            query, query_count, query_stride, train, train_count, train_stride,
            actual.empty() ? nullptr : actual.data(), query_count);
        assert(status == RBT_ORB_OK);

        const std::vector<RbtOrbMatch> expected = reference_matches(
            query, query_count, query_stride, train, train_count, train_stride);
        assert_matches_equal(actual, expected);

        for (uint32_t row = 0; row < query_count; ++row) {
            for (uint32_t byte_index = RBT_ORB_DESCRIPTOR_BYTES;
                 byte_index < query_stride; ++byte_index) {
                assert(query_storage[static_cast<size_t>(row) * query_stride +
                                    byte_index] == 0xA5u);
            }
        }
        for (uint32_t row = 0; row < train_count; ++row) {
            for (uint32_t byte_index = RBT_ORB_DESCRIPTOR_BYTES;
                 byte_index < train_stride; ++byte_index) {
                assert(train_storage[static_cast<size_t>(row) * train_stride +
                                    byte_index] == 0xA5u);
            }
        }
    }
}

void test_ties_and_identical_descriptors() {
    std::array<uint8_t, 32> query{};
    std::array<uint8_t, 4 * 32> train{};
    train[0] = 1u;
    train[32] = 1u;
    train[64] = 0u;
    train[96] = 2u;
    std::array<RbtOrbMatch, 1> result{};

    assert(rbt_orb_match_2nn(query.data(), 1, 32, train.data(), 4, 32,
                             result.data(), 1) == RBT_ORB_OK);
    assert(result[0].first_index == 2);
    assert(result[0].first_distance == 0);
    assert(result[0].second_index == 0);
    assert(result[0].second_distance == 1);

    train.fill(0u);
    assert(rbt_orb_match_2nn(query.data(), 1, 32, train.data(), 4, 32,
                             result.data(), 1) == RBT_ORB_OK);
    assert(result[0].first_index == 0);
    assert(result[0].second_index == 1);
    assert(result[0].first_distance == 0);
    assert(result[0].second_distance == 0);
}

void test_zero_and_one_train() {
    std::array<uint8_t, 2 * 32> query{};
    std::array<uint8_t, 32> train{};
    std::array<RbtOrbMatch, 2> result{};

    assert(rbt_orb_match_2nn(query.data(), 2, 32, nullptr, 0, 0,
                             result.data(), 2) == RBT_ORB_OK);
    for (const RbtOrbMatch& match : result) {
        assert(match.first_index == -1);
        assert(match.second_index == -1);
        assert(match.first_distance == 257u);
        assert(match.second_distance == 257u);
    }

    train[0] = 1u;
    assert(rbt_orb_match_2nn(query.data(), 2, 32, train.data(), 1, 32,
                             result.data(), 2) == RBT_ORB_OK);
    for (const RbtOrbMatch& match : result) {
        assert(match.first_index == 0);
        assert(match.first_distance == 1u);
        assert(match.second_index == -1);
        assert(match.second_distance == 257u);
    }

    assert(rbt_orb_match_2nn(nullptr, 0, 0, nullptr, 0, 0, nullptr, 0) ==
           RBT_ORB_OK);
}

void test_canaries_and_validation() {
    std::array<uint8_t, 2 * 32> query{};
    std::array<uint8_t, 2 * 32> train{};
    RbtOrbMatch marker = {91, 92, 93u, 94u};
    std::array<RbtOrbMatch, 4> output = {marker, marker, marker, marker};
    RbtOrbMatch* const matches = output.data() + 1;

    assert(rbt_orb_match_2nn(query.data(), 2, 32, train.data(), 2, 32,
                             matches, 2) == RBT_ORB_OK);
    assert_match_equal(output.front(), marker);
    assert_match_equal(output.back(), marker);

    output = {marker, marker, marker, marker};
    assert(rbt_orb_match_2nn(query.data(), 2, 32, train.data(), 2, 32,
                             matches, 1) == RBT_ORB_OUTPUT_CAPACITY);
    for (const RbtOrbMatch& entry : output) {
        assert_match_equal(entry, marker);
    }

    assert(rbt_orb_match_2nn(nullptr, 1, 32, train.data(), 2, 32,
                             matches, 2) == RBT_ORB_INVALID_ARGUMENT);
    assert(rbt_orb_match_2nn(query.data(), 1, 31, train.data(), 2, 32,
                             matches, 2) == RBT_ORB_INVALID_ARGUMENT);
    assert(rbt_orb_match_2nn(query.data(), 1, 32, nullptr, 2, 32,
                             matches, 2) == RBT_ORB_INVALID_ARGUMENT);
    assert(rbt_orb_match_2nn(query.data(), 1, 32, train.data(), 2, 31,
                             matches, 2) == RBT_ORB_INVALID_ARGUMENT);
    assert(rbt_orb_match_2nn(query.data(), RBT_ORB_MAX_FEATURES + 1u, 32,
                             train.data(), 1, 32, matches,
                             RBT_ORB_MAX_FEATURES + 1u) ==
           RBT_ORB_INVALID_ARGUMENT);
    assert(rbt_orb_match_2nn(query.data(), 1, 32, train.data(),
                             RBT_ORB_MAX_FEATURES + 1u, 32, matches, 1) ==
           RBT_ORB_INVALID_ARGUMENT);
    assert(rbt_orb_match_2nn(query.data(), 1, 32, train.data(), 1, 32,
                             nullptr, 1) == RBT_ORB_INVALID_ARGUMENT);

    const uintptr_t address_max = std::numeric_limits<uintptr_t>::max();
    const uint8_t* wrapping_query = reinterpret_cast<const uint8_t*>(
        address_max - 8u);
    assert(rbt_orb_match_2nn(wrapping_query, 1, 32, train.data(), 1, 32,
                             matches, 1) == RBT_ORB_INVALID_ARGUMENT);
    RbtOrbMatch* wrapping_matches = reinterpret_cast<RbtOrbMatch*>(
        address_max - 4u);
    assert(rbt_orb_match_2nn(query.data(), 2, 32, train.data(), 1, 32,
                             wrapping_matches, 2) ==
           RBT_ORB_INVALID_ARGUMENT);
}

}  // namespace

int main() {
    test_random_batches();
    test_ties_and_identical_descriptors();
    test_zero_and_one_train();
    test_canaries_and_validation();
    std::cout << "orb_match_test: all checks passed\n";
    return 0;
}
