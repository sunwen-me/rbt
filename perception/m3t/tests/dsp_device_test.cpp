// SPDX-License-Identifier: MIT
#include "rbt_m3t/dsp_orb.hpp"

#include "rbt_orb_core.h"

#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kFeatureCount = 300;
constexpr int kDescriptorBytes = 32;
constexpr size_t kWorkspaceBytes = RBT_ORB_WORKSPACE_CAPACITY;

alignas(128) std::array<uint8_t, kWorkspaceBytes> g_workspace{};

struct Harness {
    int failures = 0;
    std::vector<std::string> messages;

    void check(bool condition, const std::string& message) {
        if (condition) return;
        ++failures;
        messages.push_back(message);
        std::fprintf(stderr, "dsp_device_test: %s\n", message.c_str());
    }
};

struct CaseSummary {
    std::string name;
    int width = 0;
    int height = 0;
    size_t dsp_features = 0;
    size_t opencv_features = 0;
};

struct TimingSummary {
    double p50_ms = 0.0;
    double p95_ms = 0.0;
};

struct BenchmarkSummary {
    TimingSummary portable_extract;
    TimingSummary dsp_extract;
    TimingSummary opencv_orb;
    TimingSummary portable_match;
    TimingSummary dsp_match;
    TimingSummary opencv_bf_match;
};

struct Extracted {
    std::vector<cv::KeyPoint> keypoints;
    cv::Mat descriptors;
};

RbtOrbConfig core_config() {
    RbtOrbConfig config{};
    config.n_features = kFeatureCount;
    config.n_levels = 3u;
    config.scale_factor = 1.2f;
    config.fast_threshold = 20u;
    return config;
}

rbt_m3t::dsp::OrbConfig facade_config() {
    rbt_m3t::dsp::OrbConfig config;
    config.n_features = kFeatureCount;
    config.n_levels = 3;
    config.scale_factor = 1.2f;
    return config;
}

cv::Mat make_texture(int width, int height, uint32_t seed) {
    cv::Mat image(height, width, CV_8UC1);
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> pixel(0, 255);
    for (int row = 0; row < image.rows; ++row) {
        uint8_t* bytes = image.ptr<uint8_t>(row);
        for (int col = 0; col < image.cols; ++col)
            bytes[col] = static_cast<uint8_t>(pixel(generator));
    }
    return image;
}

cv::Mat translated(const cv::Mat& source) {
    const cv::Mat transform = (cv::Mat_<double>(2, 3) <<
        1.0, 0.0, 7.0, 0.0, 1.0, -5.0);
    cv::Mat destination;
    cv::warpAffine(source, destination, transform, source.size(), cv::INTER_LINEAR,
                   cv::BORDER_REFLECT_101);
    return destination;
}

cv::Mat rotated(const cv::Mat& source) {
    const cv::Mat transform = cv::getRotationMatrix2D(
        cv::Point2f(static_cast<float>(source.cols) / 2.0f,
                    static_cast<float>(source.rows) / 2.0f),
        11.0, 1.0);
    cv::Mat destination;
    cv::warpAffine(source, destination, transform, source.size(), cv::INTER_LINEAR,
                   cv::BORDER_REFLECT_101);
    return destination;
}

std::vector<std::pair<std::string, cv::Mat>> make_images() {
    std::vector<std::pair<std::string, cv::Mat>> images;
    for (const auto& dimensions :
         {std::pair<int, int>{200, 200}, std::pair<int, int>{320, 240}}) {
        const std::string suffix = std::to_string(dimensions.first) + "x" +
                                   std::to_string(dimensions.second);
        const cv::Mat base = make_texture(dimensions.first, dimensions.second,
                                          0x4d335400u +
                                              static_cast<uint32_t>(dimensions.first * 17 +
                                                                    dimensions.second));
        images.emplace_back("texture_" + suffix, base);
        images.emplace_back("translated_" + suffix, translated(base));
        images.emplace_back("rotated_" + suffix, rotated(base));
    }
    return images;
}

bool finite_keypoint(const cv::KeyPoint& point) {
    return std::isfinite(point.pt.x) && std::isfinite(point.pt.y) &&
           std::isfinite(point.size) && std::isfinite(point.angle) &&
           std::isfinite(point.response) && point.size > 0.0f &&
           point.angle >= 0.0f && point.angle < 360.0f && point.octave >= 0;
}

bool equal_keypoint_field(Harness& harness, size_t index, const char* field,
                          float dsp_value, float core_value) {
    if (dsp_value == core_value) return true;
    std::ostringstream message;
    message << "portable core mismatch at keypoint " << index << " field "
            << field << ": DSP=" << std::setprecision(9) << dsp_value
            << " core=" << core_value;
    harness.check(false, message.str());
    return false;
}

Extracted run_core_extract(Harness& harness, const cv::Mat& image,
                           const std::string& label) {
    Extracted result;
    const RbtOrbConfig config = core_config();
    const size_t required = rbt_orb_workspace_size(
        static_cast<uint32_t>(image.cols), static_cast<uint32_t>(image.rows),
        &config);
    harness.check(required > 0u && required <= g_workspace.size(),
                  label + ": portable core workspace is valid and bounded");
    if (required == 0u || required > g_workspace.size()) return result;

    std::array<RbtOrbKeypoint, kFeatureCount> keypoints{};
    std::array<uint8_t, kFeatureCount * kDescriptorBytes> descriptors{};
    uint32_t count = 0u;
    const int status = rbt_orb_extract(
        image.ptr<uint8_t>(), static_cast<uint32_t>(image.cols),
        static_cast<uint32_t>(image.rows), static_cast<uint32_t>(image.step[0]),
        &config, keypoints.data(), descriptors.data(), kFeatureCount, &count,
        g_workspace.data(), required);
    harness.check(status == RBT_ORB_OK,
                  label + ": portable core extraction succeeds");
    if (status != RBT_ORB_OK) return result;
    harness.check(count <= static_cast<uint32_t>(kFeatureCount),
                  label + ": portable core feature count is bounded");
    result.keypoints.reserve(count);
    for (uint32_t index = 0u; index < count; ++index) {
        const RbtOrbKeypoint& point = keypoints[index];
        result.keypoints.emplace_back(cv::Point2f(point.x, point.y), point.size,
                                       point.angle, point.response, point.octave,
                                       -1);
    }
    if (count != 0u) {
        result.descriptors.create(static_cast<int>(count), kDescriptorBytes,
                                  CV_8UC1);
        for (uint32_t row = 0u; row < count; ++row) {
            std::memcpy(result.descriptors.ptr<uint8_t>(static_cast<int>(row)),
                        descriptors.data() +
                            static_cast<size_t>(row) * kDescriptorBytes,
                        kDescriptorBytes);
        }
    }
    return result;
}

void compare_extract(Harness& harness, const std::string& label,
                     const Extracted& dsp, const Extracted& core,
                     const cv::Mat& image) {
    harness.check(dsp.keypoints.size() ==
                          static_cast<size_t>(dsp.descriptors.rows) ||
                      (dsp.keypoints.empty() && dsp.descriptors.empty()),
                  label + ": DSP descriptor row count matches keypoints");
    harness.check(dsp.descriptors.empty() ||
                      (dsp.descriptors.type() == CV_8UC1 &&
                       dsp.descriptors.cols == kDescriptorBytes),
                  label + ": DSP descriptor rows are 32-byte CV_8UC1");
    harness.check(dsp.keypoints.size() == core.keypoints.size(),
                  label + ": DSP feature count exactly matches portable core");
    if (dsp.keypoints.size() != core.keypoints.size()) return;
    if (dsp.keypoints.empty()) {
        harness.check(dsp.descriptors.empty() && core.descriptors.empty(),
                      label + ": empty feature results have empty descriptors");
        return;
    }
    for (size_t index = 0; index < dsp.keypoints.size(); ++index) {
        const cv::KeyPoint& actual = dsp.keypoints[index];
        const cv::KeyPoint& expected = core.keypoints[index];
        harness.check(finite_keypoint(actual),
                      label + ": DSP keypoint fields are finite and valid");
        equal_keypoint_field(harness, index, "x", actual.pt.x, expected.pt.x);
        equal_keypoint_field(harness, index, "y", actual.pt.y, expected.pt.y);
        equal_keypoint_field(harness, index, "size", actual.size, expected.size);
        equal_keypoint_field(harness, index, "angle", actual.angle, expected.angle);
        equal_keypoint_field(harness, index, "response", actual.response,
                             expected.response);
        harness.check(actual.octave == expected.octave,
                      label + ": DSP octave exactly matches portable core");
        harness.check(actual.class_id == -1 && expected.class_id == -1,
                      label + ": class_id uses the facade sentinel");
    }
    harness.check(dsp.descriptors.rows == core.descriptors.rows &&
                      dsp.descriptors.cols == kDescriptorBytes &&
                      core.descriptors.cols == kDescriptorBytes,
                  label + ": DSP descriptor dimensions exactly match portable core");
    if (dsp.descriptors.rows != core.descriptors.rows ||
        dsp.descriptors.cols != kDescriptorBytes ||
        core.descriptors.cols != kDescriptorBytes) {
        return;
    }
    for (int row = 0; row < dsp.descriptors.rows; ++row) {
        if (std::memcmp(dsp.descriptors.ptr<uint8_t>(row),
                        core.descriptors.ptr<uint8_t>(row), kDescriptorBytes) != 0) {
            std::ostringstream message;
            message << label << ": descriptor row " << row
                    << " differs from portable core";
            harness.check(false, message.str());
            break;
        }
    }
    if (image.cols != 200 && image.cols != 320)
        harness.check(false, label + ": unexpected deterministic image width");
}

std::vector<cv::KeyPoint> run_opencv_orb(Harness& harness,
                                         const cv::Mat& image,
                                         cv::Mat* descriptors,
                                         const std::string& label) {
    auto orb = cv::ORB::create(kFeatureCount, 1.2f, 3, 31, 0, 2,
                               cv::ORB::HARRIS_SCORE, 31, 20);
    std::vector<cv::KeyPoint> keypoints;
    orb->detectAndCompute(image, cv::noArray(), keypoints, *descriptors);
    harness.check(keypoints.size() == static_cast<size_t>(descriptors->rows),
                  label + ": OpenCV ORB descriptor count matches keypoints");
    harness.check(descriptors->empty() ||
                      (descriptors->type() == CV_8UC1 &&
                       descriptors->cols == kDescriptorBytes),
                  label + ": independent OpenCV ORB emits 32-byte descriptors");
    for (const cv::KeyPoint& point : keypoints)
        harness.check(finite_keypoint(point),
                      label + ": OpenCV ORB keypoint fields are finite");
    return keypoints;
}

void run_extraction_checks(Harness& harness, rbt_m3t::dsp::Orb& orb,
                           std::vector<CaseSummary>* summaries) {
    cv::Mat flat(200, 200, CV_8UC1, cv::Scalar(0));
    Extracted core_flat = run_core_extract(harness, flat, "flat_200x200");
    Extracted dsp_flat;
    orb.extract(flat, dsp_flat.keypoints, dsp_flat.descriptors);
    compare_extract(harness, "flat_200x200", dsp_flat, core_flat, flat);
    harness.check(dsp_flat.keypoints.empty() && dsp_flat.descriptors.empty(),
                  "flat_200x200: zero-feature result is valid");

    cv::Mat opencv_flat_descriptors;
    const auto opencv_flat = run_opencv_orb(
        harness, flat, &opencv_flat_descriptors, "flat_200x200");
    harness.check(opencv_flat.empty() && opencv_flat_descriptors.empty(),
                  "flat_200x200: independent OpenCV ORB also returns no features");
    summaries->push_back({"flat_200x200", 200, 200, 0u, 0u});

    for (const auto& item : make_images()) {
        const std::string& label = item.first;
        const cv::Mat& image = item.second;
        Extracted core = run_core_extract(harness, image, label);
        Extracted dsp;
        orb.extract(image, dsp.keypoints, dsp.descriptors);
        compare_extract(harness, label, dsp, core, image);
        harness.check(!dsp.keypoints.empty(),
                      label + ": deterministic textured image yields features");

        cv::Mat opencv_descriptors;
        const std::vector<cv::KeyPoint> opencv =
            run_opencv_orb(harness, image, &opencv_descriptors, label);
        harness.check(!opencv.empty(),
                      label + ": independent OpenCV ORB yields features");
        summaries->push_back({label, image.cols, image.rows,
                              dsp.keypoints.size(), opencv.size()});
    }
}

std::string exception_text(const std::exception& error) {
    return error.what() == nullptr ? std::string() : std::string(error.what());
}

template <typename Function>
void expect_runtime_error(Harness& harness, Function&& function,
                          const std::string& expected_text,
                          const std::string& label) {
    try {
        function();
        harness.check(false, label + ": expected a runtime_error");
    } catch (const std::runtime_error& error) {
        const std::string message = exception_text(error);
        harness.check(message.find(expected_text) != std::string::npos,
                      label + ": clear error message was " + message);
    } catch (const std::exception& error) {
        harness.check(false, label + ": unexpected error type: " +
                                  exception_text(error));
    } catch (...) {
        harness.check(false, label + ": unexpected non-standard exception");
    }
}

void run_invalid_input_checks(Harness& harness, rbt_m3t::dsp::Orb& orb) {
    cv::Mat invalid_color(16, 16, CV_8UC3, cv::Scalar(0));
    std::vector<cv::KeyPoint> keypoints;
    keypoints.emplace_back(cv::Point2f(1.0f, 2.0f), 3.0f);
    cv::Mat descriptors(1, kDescriptorBytes, CV_8UC1, cv::Scalar(0xff));
    expect_runtime_error(harness,
        [&] { orb.extract(invalid_color, keypoints, descriptors); },
        "requires a nonempty CV_8UC1 image", "invalid color input");
    harness.check(keypoints.empty() && descriptors.empty(),
                  "invalid color input clears extraction outputs");

    cv::Mat oversized(1, 1025, CV_8UC1, cv::Scalar(0));
    expect_runtime_error(harness,
        [&] { orb.extract(oversized, keypoints, descriptors); },
        "no larger than 1024x1024", "oversized image input");
    harness.check(keypoints.empty() && descriptors.empty(),
                  "oversized image input clears extraction outputs");

    cv::Mat invalid_descriptors(2, 31, CV_8UC1, cv::Scalar(0));
    expect_runtime_error(harness,
        [&] { (void)orb.match_2nn(invalid_descriptors,
                                  cv::Mat(2, kDescriptorBytes, CV_8UC1)); },
        "must be CV_8UC1 with 32 columns", "invalid descriptor dimensions");

    rbt_m3t::dsp::OrbConfig invalid_config = facade_config();
    invalid_config.n_levels = 0;
    expect_runtime_error(harness,
        [&] { rbt_m3t::dsp::Orb invalid(invalid_config); },
        "configuration is outside the supported bounds", "invalid ORB config");
}

const uint8_t* row_ptr(const std::vector<uint8_t>& bytes, int row) {
    return bytes.data() + static_cast<size_t>(row) * kDescriptorBytes;
}

cv::Mat descriptor_matrix(int rows, std::vector<uint8_t>* bytes) {
    if (rows == 0) return cv::Mat(0, kDescriptorBytes, CV_8UC1);
    return cv::Mat(rows, kDescriptorBytes, CV_8UC1, bytes->data());
}

int popcount_byte(uint8_t byte) {
    int count = 0;
    while (byte != 0u) {
        count += byte & 1u;
        byte = static_cast<uint8_t>(byte >> 1u);
    }
    return count;
}

int hamming(const uint8_t* lhs, const uint8_t* rhs) {
    int distance = 0;
    for (int byte = 0; byte < kDescriptorBytes; ++byte)
        distance += popcount_byte(static_cast<uint8_t>(lhs[byte] ^ rhs[byte]));
    return distance;
}

struct MatchSummary {
    int queries = 0;
    int train = 0;
    size_t neighbors = 0;
};

MatchSummary run_match_case(Harness& harness, rbt_m3t::dsp::Orb& orb,
                            const std::string& label,
                            const std::vector<uint8_t>& query_bytes,
                            int query_count,
                            const std::vector<uint8_t>& train_bytes,
                            int train_count, bool use_opencv_oracle,
                            std::vector<std::vector<cv::DMatch>>* captured = nullptr) {
    cv::Mat query = descriptor_matrix(query_count,
                                      const_cast<std::vector<uint8_t>*>(&query_bytes));
    cv::Mat train = descriptor_matrix(train_count,
                                      const_cast<std::vector<uint8_t>*>(&train_bytes));
    std::vector<RbtOrbMatch> core(static_cast<size_t>(query_count));
    const int core_status = rbt_orb_match_2nn(
        query_bytes.empty() ? nullptr : query_bytes.data(),
        static_cast<uint32_t>(query_count), kDescriptorBytes,
        train_bytes.empty() ? nullptr : train_bytes.data(),
        static_cast<uint32_t>(train_count), kDescriptorBytes,
        core.empty() ? nullptr : core.data(), static_cast<uint32_t>(core.size()));
    harness.check(core_status == RBT_ORB_OK,
                  label + ": portable core batch match succeeds");
    const auto dsp = orb.match_2nn(query, train);
    if (captured != nullptr) *captured = dsp;
    harness.check(dsp.size() == static_cast<size_t>(query_count),
                  label + ": DSP match returns one result row per query");
    if (dsp.size() != static_cast<size_t>(query_count))
        return {query_count, train_count, 0u};

    size_t neighbors = 0u;
    for (int query_index = 0; query_index < query_count; ++query_index) {
        const RbtOrbMatch& expected = core[static_cast<size_t>(query_index)];
        const std::vector<cv::DMatch>& actual = dsp[static_cast<size_t>(query_index)];
        const size_t expected_count = train_count == 0 ? 0u :
                                      (train_count == 1 ? 1u : 2u);
        harness.check(actual.size() == expected_count,
                      label + ": neighbor count matches portable core");
        if (actual.size() != expected_count) continue;
        if (expected_count >= 1u) {
            ++neighbors;
            harness.check(actual[0].queryIdx == query_index &&
                              actual[0].trainIdx == expected.first_index &&
                              actual[0].imgIdx == 0 &&
                              actual[0].distance ==
                                  static_cast<float>(expected.first_distance),
                          label + ": first result exactly matches portable core");
            if (train_count > 0) {
                harness.check(expected.first_index >= 0 &&
                                  expected.first_index < train_count &&
                                  hamming(row_ptr(query_bytes, query_index),
                                          row_ptr(train_bytes,
                                                  expected.first_index)) ==
                                      static_cast<int>(expected.first_distance),
                              label + ": first distance matches bytewise Hamming");
            }
        } else {
            harness.check(expected.first_index == -1 &&
                              expected.second_index == -1 &&
                              expected.first_distance == 257u &&
                              expected.second_distance == 257u,
                          label + ": missing neighbors use protocol sentinels");
        }
        if (expected_count >= 2u) {
            ++neighbors;
            harness.check(actual[1].queryIdx == query_index &&
                              actual[1].trainIdx == expected.second_index &&
                              actual[1].imgIdx == 0 &&
                              actual[1].distance ==
                                  static_cast<float>(expected.second_distance),
                          label + ": second result exactly matches portable core");
            harness.check(expected.first_distance <= expected.second_distance &&
                              (expected.first_distance != expected.second_distance ||
                               expected.first_index < expected.second_index),
                          label + ": stable distance/index ordering is preserved");
            harness.check(hamming(row_ptr(query_bytes, query_index),
                                  row_ptr(train_bytes, expected.second_index)) ==
                              static_cast<int>(expected.second_distance),
                          label + ": second distance matches bytewise Hamming");
        }
    }

    if (use_opencv_oracle && query_count > 0 && train_count > 0) {
        cv::BFMatcher matcher(cv::NORM_HAMMING, false);
        std::vector<std::vector<cv::DMatch>> opencv_matches;
        matcher.knnMatch(query, train, opencv_matches, 2);
        harness.check(opencv_matches.size() == static_cast<size_t>(query_count),
                      label + ": OpenCV BFMatcher returns every query row");
        for (int query_index = 0;
             query_index < query_count &&
             query_index < static_cast<int>(opencv_matches.size());
             ++query_index) {
            const auto& actual = dsp[static_cast<size_t>(query_index)];
            const auto& independent =
                opencv_matches[static_cast<size_t>(query_index)];
            const size_t expected_count = std::min<size_t>(2u, train_count);
            harness.check(independent.size() == expected_count,
                          label + ": OpenCV BFMatcher returns the expected k");
            if (independent.size() != expected_count ||
                actual.size() != expected_count) continue;
            for (size_t neighbor = 0; neighbor < expected_count; ++neighbor) {
                harness.check(std::isfinite(independent[neighbor].distance) &&
                                  independent[neighbor].distance ==
                                      actual[neighbor].distance,
                              label + ": DSP Hamming distance matches OpenCV BFMatcher");
                const int index = actual[neighbor].trainIdx;
                harness.check(index >= 0 && index < train_count &&
                                  hamming(row_ptr(query_bytes, query_index),
                                          row_ptr(train_bytes, index)) ==
                                      static_cast<int>(independent[neighbor].distance),
                              label + ": DSP neighbor is an OpenCV-oracle Hamming candidate");
            }
        }
    }
    return {query_count, train_count, neighbors};
}

std::pair<std::vector<uint8_t>, std::vector<uint8_t>> seeded_descriptors() {
    std::mt19937 generator(0x4d33544du);
    std::uniform_int_distribution<int> byte(0, 255);
    std::vector<uint8_t> query(static_cast<size_t>(kFeatureCount) *
                               kDescriptorBytes);
    std::vector<uint8_t> train(query.size());
    for (uint8_t& value : query) value = static_cast<uint8_t>(byte(generator));
    for (uint8_t& value : train) value = static_cast<uint8_t>(byte(generator));
    return {std::move(query), std::move(train)};
}

void run_match_checks(Harness& harness, rbt_m3t::dsp::Orb& orb,
                      MatchSummary* summary) {
    auto [query, train] = seeded_descriptors();
    *summary = run_match_case(harness, orb, "seeded_300x300", query,
                              kFeatureCount, train, kFeatureCount, true);

    std::vector<uint8_t> tie_query(kDescriptorBytes, 0u);
    std::vector<uint8_t> tie_train(4u * kDescriptorBytes, 0u);
    tie_train[0] = 1u;
    tie_train[2 * kDescriptorBytes] = 0u;
    tie_train[3 * kDescriptorBytes] = 3u;
    std::vector<std::vector<cv::DMatch>> tie_matches;
    const MatchSummary ties = run_match_case(harness, orb, "stable_tie_1x4",
                                              tie_query, 1, tie_train, 4, true,
                                              &tie_matches);
    harness.check(ties.neighbors == 2u,
                  "stable_tie_1x4: both tied nearest neighbors are returned");
    harness.check(tie_matches.size() == 1u && tie_matches[0].size() == 2u &&
                      tie_matches[0][0].trainIdx == 1 &&
                      tie_matches[0][0].distance == 0.0f &&
                      tie_matches[0][1].trainIdx == 2 &&
                      tie_matches[0][1].distance == 0.0f,
                  "stable_tie_1x4: equal distances choose ascending train index");

    std::vector<uint8_t> small_query(3u * kDescriptorBytes, 0u);
    std::vector<uint8_t> empty_train;
    const MatchSummary no_train = run_match_case(
        harness, orb, "empty_train_3x0", small_query, 3, empty_train, 0, false);
    harness.check(no_train.neighbors == 0u,
                  "empty_train_3x0: all query rows have no neighbors");

    std::vector<uint8_t> one_train(kDescriptorBytes, 0x5au);
    const MatchSummary one = run_match_case(
        harness, orb, "one_train_3x1", small_query, 3, one_train, 1, true);
    harness.check(one.neighbors == 3u,
                  "one_train_3x1: every query gets exactly one neighbor");

    cv::Mat empty_queries(0, kDescriptorBytes, CV_8UC1);
    const int before = rbt_orb_match_2nn(nullptr, 0u, kDescriptorBytes,
                                         one_train.data(), 1u, kDescriptorBytes,
                                         nullptr, 0u);
    harness.check(before == RBT_ORB_OK &&
                      orb.match_2nn(empty_queries,
                                    descriptor_matrix(1, &one_train)).empty(),
                  "empty_query_0x1 is a valid no-op batch");
}

std::vector<double> durations_ms(int warmups, int samples,
                                 const std::function<void()>& operation) {
    for (int index = 0; index < warmups; ++index) operation();
    std::vector<double> durations;
    durations.reserve(static_cast<size_t>(samples));
    for (int index = 0; index < samples; ++index) {
        const auto start = std::chrono::steady_clock::now();
        operation();
        const auto end = std::chrono::steady_clock::now();
        durations.push_back(std::chrono::duration<double, std::milli>(end - start)
                                .count());
    }
    return durations;
}

TimingSummary summarize(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const size_t p50 = static_cast<size_t>(std::ceil(0.50 * values.size())) - 1u;
    const size_t p95 = static_cast<size_t>(std::ceil(0.95 * values.size())) - 1u;
    return {values[p50], values[p95]};
}

BenchmarkSummary run_benchmark(Harness& harness, const char* stub_path,
                               const char* module_path) {
    const std::string previous_audit = std::getenv("RBT_M3T_DSP_AUDIT") == nullptr
        ? std::string() : std::getenv("RBT_M3T_DSP_AUDIT");
    const bool had_audit = std::getenv("RBT_M3T_DSP_AUDIT") != nullptr;
    unsetenv("RBT_M3T_DSP_AUDIT");
    if (setenv("RBT_M3T_DSP_STUB", stub_path, 1) != 0 ||
        setenv("RBT_M3T_DSP_MODULE", module_path, 1) != 0) {
        throw std::runtime_error("could not restore DSP paths for benchmark");
    }

    const cv::Mat image = make_texture(200, 200, 0x4d334242u);
    const RbtOrbConfig config = core_config();
    const size_t workspace = rbt_orb_workspace_size(200u, 200u, &config);
    std::array<RbtOrbKeypoint, kFeatureCount> core_keypoints{};
    std::array<uint8_t, kFeatureCount * kDescriptorBytes> core_descriptors{};
    std::array<RbtOrbMatch, kFeatureCount> core_matches{};
    auto core_orb = cv::ORB::create(kFeatureCount, 1.2f, 3, 31, 0, 2,
                                    cv::ORB::HARRIS_SCORE, 31, 20);
    std::vector<cv::KeyPoint> opencv_keypoints;
    cv::Mat opencv_descriptors;
    cv::BFMatcher matcher(cv::NORM_HAMMING, false);
    const auto [query_bytes, train_bytes] = seeded_descriptors();
    cv::Mat query = descriptor_matrix(kFeatureCount,
                                      const_cast<std::vector<uint8_t>*>(&query_bytes));
    cv::Mat train = descriptor_matrix(kFeatureCount,
                                      const_cast<std::vector<uint8_t>*>(&train_bytes));
    std::vector<std::vector<cv::DMatch>> opencv_matches;
    rbt_m3t::dsp::Orb dsp(facade_config());

    constexpr int kWarmups = 5;
    constexpr int kSamples = 20;
    const auto portable_extract = durations_ms(kWarmups, kSamples, [&] {
        uint32_t count = 0u;
        const int status = rbt_orb_extract(
            image.ptr<uint8_t>(), 200u, 200u, static_cast<uint32_t>(image.step[0]),
            &config, core_keypoints.data(), core_descriptors.data(),
            kFeatureCount, &count, g_workspace.data(), workspace);
        if (status != RBT_ORB_OK)
            throw std::runtime_error("portable core benchmark extract failed");
    });
    const auto dsp_extract = durations_ms(kWarmups, kSamples, [&] {
        std::vector<cv::KeyPoint> keypoints;
        cv::Mat descriptors;
        dsp.extract(image, keypoints, descriptors);
    });
    const auto opencv_orb = durations_ms(kWarmups, kSamples, [&] {
        core_orb->detectAndCompute(image, cv::noArray(), opencv_keypoints,
                                   opencv_descriptors);
    });
    const auto portable_match = durations_ms(kWarmups, kSamples, [&] {
        const int status = rbt_orb_match_2nn(
            query_bytes.data(), kFeatureCount, kDescriptorBytes,
            train_bytes.data(), kFeatureCount, kDescriptorBytes,
            core_matches.data(), kFeatureCount);
        if (status != RBT_ORB_OK)
            throw std::runtime_error("portable core benchmark match failed");
    });
    const auto dsp_match = durations_ms(kWarmups, kSamples, [&] {
        (void)dsp.match_2nn(query, train);
    });
    const auto opencv_bf = durations_ms(kWarmups, kSamples, [&] {
        matcher.knnMatch(query, train, opencv_matches, 2);
    });
    if (had_audit) setenv("RBT_M3T_DSP_AUDIT", previous_audit.c_str(), 1);

    harness.check(workspace > 0u && workspace <= g_workspace.size(),
                  "benchmark portable workspace is valid");
    return {summarize(portable_extract), summarize(dsp_extract),
            summarize(opencv_orb), summarize(portable_match),
            summarize(dsp_match), summarize(opencv_bf)};
}

std::string json_escape(const std::string& value) {
    std::ostringstream output;
    for (const unsigned char byte : value) {
        switch (byte) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20u)
                output << "\\u" << std::hex << std::setw(4)
                       << std::setfill('0') << static_cast<int>(byte) << std::dec;
            else
                output << static_cast<char>(byte);
        }
    }
    return output.str();
}

void print_timing(const TimingSummary& timing) {
    std::cout << "{\"p50_ms\":" << timing.p50_ms
              << ",\"p95_ms\":" << timing.p95_ms << "}";
}

void print_summary(int failures, const std::vector<CaseSummary>& cases,
                   const MatchSummary& matches,
                   const std::vector<std::string>& messages,
                   const BenchmarkSummary* benchmark) {
    std::cout << "{\"status\":\"" << (failures == 0 ? "PASS" : "FAIL")
              << "\",\"mode\":\"DSP_DEVICE\",\"cases\":[";
    for (size_t index = 0; index < cases.size(); ++index) {
        if (index != 0u) std::cout << ',';
        const CaseSummary& item = cases[index];
        std::cout << "{\"name\":\"" << json_escape(item.name)
                  << "\",\"width\":" << item.width
                  << ",\"height\":" << item.height
                  << ",\"dsp_features\":" << item.dsp_features
                  << ",\"opencv_features\":" << item.opencv_features << '}';
    }
    std::cout << "],\"batch_match\":{\"queries\":" << matches.queries
              << ",\"train\":" << matches.train
              << ",\"neighbors\":" << matches.neighbors
              << ",\"rpc_per_nonempty_matrix\":1}"
              << ",\"checks_failed\":" << failures << ",\"errors\":[";
    for (size_t index = 0; index < messages.size(); ++index) {
        if (index != 0u) std::cout << ',';
        std::cout << '"' << json_escape(messages[index]) << '"';
    }
    std::cout << ']';
    if (benchmark != nullptr) {
        std::cout << ",\"benchmark\":{\"label\":\"synthetic_microbenchmark\","
                     "\"warmups\":5,\"samples\":20,\"units\":\"milliseconds\","
                     "\"portable_core_extract\":";
        print_timing(benchmark->portable_extract);
        std::cout << ",\"dsp_facade_extract\":";
        print_timing(benchmark->dsp_extract);
        std::cout << ",\"opencv_cpu_orb\":";
        print_timing(benchmark->opencv_orb);
        std::cout << ",\"portable_core_match\":";
        print_timing(benchmark->portable_match);
        std::cout << ",\"dsp_facade_match\":";
        print_timing(benchmark->dsp_match);
        std::cout << ",\"opencv_cpu_bf_match\":";
        print_timing(benchmark->opencv_bf_match);
        std::cout << '}';
    }
    std::cout << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    bool benchmark = false;
    if (argc != 3 && argc != 4) {
        std::fprintf(stderr,
                     "usage: rbt_m3t_dsp_device_test STUB MODULE [--benchmark]\n");
        return 2;
    }
    if (argc == 4) {
        if (std::string(argv[3]) != "--benchmark") {
            std::fprintf(stderr,
                         "usage: rbt_m3t_dsp_device_test STUB MODULE [--benchmark]\n");
            return 2;
        }
        benchmark = true;
    }

    Harness harness;
    std::vector<CaseSummary> cases;
    MatchSummary matches;
    BenchmarkSummary benchmark_summary;
    bool benchmark_ran = false;
    try {
        if (setenv("RBT_M3T_DSP_STUB", argv[1], 1) != 0 ||
            setenv("RBT_M3T_DSP_MODULE", argv[2], 1) != 0 ||
            setenv("RBT_M3T_DSP_AUDIT", "1", 1) != 0) {
            throw std::runtime_error("could not set DSP device-test environment");
        }
        {
            rbt_m3t::dsp::Orb orb(facade_config());
            run_extraction_checks(harness, orb, &cases);
            run_invalid_input_checks(harness, orb);
            run_match_checks(harness, orb, &matches);
        }
        if (benchmark) {
            benchmark_summary = run_benchmark(harness, argv[1], argv[2]);
            benchmark_ran = true;
        }
    } catch (const std::exception& error) {
        harness.check(false, "device test could not complete: " +
                                  exception_text(error));
    } catch (...) {
        harness.check(false, "device test could not complete: unknown exception");
    }
    print_summary(harness.failures, cases, matches, harness.messages,
                  benchmark_ran ? &benchmark_summary : nullptr);
    return harness.failures == 0 ? 0 : 1;
}
