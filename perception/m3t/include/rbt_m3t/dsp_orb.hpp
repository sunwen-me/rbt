// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <memory>
#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>
#include <string>
#include <vector>

namespace rbt_m3t::dsp {

struct OrbConfig {
    int n_features = 300;
    float scale_factor = 1.2f;
    int n_levels = 3;
};

// One shared FastRPC session per process, serialized by the implementation.
// Every extract and match batch executes in the DSP module. Errors throw;
// selecting this backend never silently invokes CPU ORB or a CPU matcher.
class Orb {
public:
    explicit Orb(OrbConfig config);
    ~Orb();
    Orb(const Orb&) = delete;
    Orb& operator=(const Orb&) = delete;

    void extract(const cv::Mat& gray, std::vector<cv::KeyPoint>& keypoints,
                 cv::Mat& descriptors);
    std::vector<std::vector<cv::DMatch>> match_2nn(
        const cv::Mat& query, const cv::Mat& train);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rbt_m3t::dsp
