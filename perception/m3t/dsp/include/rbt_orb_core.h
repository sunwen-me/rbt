// SPDX-License-Identifier: MIT
#ifndef RBT_M3T_DSP_ORB_CORE_H
#define RBT_M3T_DSP_ORB_CORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RBT_ORB_MAX_WIDTH 1024u
#define RBT_ORB_MAX_HEIGHT 1024u
#define RBT_ORB_MAX_FEATURES 2048u
#define RBT_ORB_MAX_LEVELS 8u
#define RBT_ORB_DESCRIPTOR_BYTES 32u
#define RBT_ORB_WORKSPACE_CAPACITY (16u * 1024u * 1024u)

typedef enum RbtOrbStatus {
    RBT_ORB_OK = 0,
    RBT_ORB_INVALID_ARGUMENT = 1,
    RBT_ORB_OUTPUT_CAPACITY = 2,
    RBT_ORB_WORKSPACE_TOO_SMALL = 3,
    RBT_ORB_INTERNAL_ERROR = 4
} RbtOrbStatus;

// ORB WTA_K=2, patchSize=31, edgeThreshold=31, firstLevel=0,
// Harris scoring. Other parameter combinations are deliberately rejected.
typedef struct RbtOrbConfig {
    uint32_t n_features;
    uint32_t n_levels;
    float scale_factor;
    uint32_t fast_threshold;
} RbtOrbConfig;

typedef struct RbtOrbKeypoint {
    float x;
    float y;
    float size;
    float angle;
    float response;
    int32_t octave;
} RbtOrbKeypoint;

typedef struct RbtOrbMatch {
    int32_t first_index;
    int32_t second_index;
    uint32_t first_distance;
    uint32_t second_distance;
} RbtOrbMatch;

// Returns zero for invalid dimensions/configuration. Workspace must be
// 128-byte aligned. No function allocates memory or retains caller pointers.
size_t rbt_orb_workspace_size(uint32_t width, uint32_t height,
                              const RbtOrbConfig* config);

// The descriptor for keypoint i is descriptors + i * 32. Coordinates are in
// original-image pixels; angle is degrees, size is pixels, octave is zero-based.
// count is reset to zero on failure. Empty feature sets are valid results.
int rbt_orb_extract(const uint8_t* image, uint32_t width, uint32_t height,
                    uint32_t stride, const RbtOrbConfig* config,
                    RbtOrbKeypoint* keypoints, uint8_t* descriptors,
                    uint32_t capacity, uint32_t* count,
                    void* workspace, size_t workspace_bytes);

// One complete batch: each query receives its two nearest train descriptors.
// Stable ties use the lower train index. Missing neighbors have index -1 and
// distance 257. Empty batches are valid. All descriptor rows are 32 bytes;
// strides may be larger. Bounds are enforced independently of the RPC layer.
int rbt_orb_match_2nn(const uint8_t* query, uint32_t query_count,
                      uint32_t query_stride, const uint8_t* train,
                      uint32_t train_count, uint32_t train_stride,
                      RbtOrbMatch* matches, uint32_t capacity);

#ifdef __cplusplus
}
#endif
#endif
