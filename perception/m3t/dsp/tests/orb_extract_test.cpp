// SPDX-License-Identifier: MIT

#include "../include/rbt_orb_core.h"
#include "../src/orb_pattern_31.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#if defined(RBT_ORB_TESTING)
extern "C" uint32_t rbt_orb_test_angle_turns_q32(int64_t x, int64_t y);
extern "C" void rbt_orb_test_sincos_q30(uint32_t angle_turns,
                                          int64_t* cosine, int64_t* sine);
#endif

namespace {

const uint32_t kWidth = 192u;
const uint32_t kHeight = 160u;
const uint32_t kStride = kWidth + 7u;
const uint32_t kFeatureCapacity = 320u;
const uint32_t kMaxFeatures = RBT_ORB_MAX_FEATURES;
const size_t kWorkspaceBytes = 1024u * 1024u;
const uint8_t kCanary = 0xa5u;

struct GuardedKeypoints {
  uint64_t before;
  RbtOrbKeypoint values[kMaxFeatures];
  uint64_t after;
};

alignas(128) uint8_t g_workspace[kWorkspaceBytes];
alignas(128) uint8_t g_misaligned_workspace[kWorkspaceBytes + 128u];
uint8_t g_image[kStride * kHeight];
uint8_t g_tiny_image[30u * 30u];
GuardedKeypoints g_keypoints;
GuardedKeypoints g_repeat_keypoints;
uint8_t g_descriptors[kMaxFeatures * RBT_ORB_DESCRIPTOR_BYTES + 32u];
uint8_t g_repeat_descriptors[kMaxFeatures * RBT_ORB_DESCRIPTOR_BYTES + 32u];
uint32_t g_reference_horizontal[kWidth * kHeight];
uint8_t g_reference_blurred[kWidth * kHeight];

bool require(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "orb_extract_test: %s\n", message);
  return false;
}

void fill_bytes(void* pointer, size_t bytes, uint8_t value) {
  uint8_t* output = static_cast<uint8_t*>(pointer);
  for (size_t index = 0u; index < bytes; ++index) output[index] = value;
}

void make_texture() {
  for (uint32_t y = 0u; y < kHeight; ++y) {
    for (uint32_t x = 0u; x < kStride; ++x) {
      if (x >= kWidth) {
        g_image[static_cast<size_t>(y) * kStride + x] = 0xeeu;
        continue;
      }
      const uint32_t checker = ((x / 9u + y / 7u) & 1u) * 97u;
      const uint32_t value = x * 37u + y * 53u + checker +
                             ((x * y) % 113u) * 3u + ((x ^ y) * 17u);
      g_image[static_cast<size_t>(y) * kStride + x] =
          static_cast<uint8_t>(value & 255u);
    }
  }
  for (uint32_t index = 0u; index < 30u * 30u; ++index)
    g_tiny_image[index] = static_cast<uint8_t>((index * 71u) & 255u);
}

RbtOrbConfig test_config() {
  RbtOrbConfig config = {};
  config.n_features = kFeatureCapacity;
  config.n_levels = 4u;
  config.scale_factor = 1.2f;
  config.fast_threshold = 12u;
  return config;
}

uint32_t reflect101_test(int coordinate, uint32_t length) {
  if (length <= 1u) return 0u;
  const int maximum = static_cast<int>(length) - 1;
  while (coordinate < 0 || coordinate > maximum) {
    coordinate = coordinate < 0 ? -coordinate : 2 * maximum - coordinate;
  }
  return static_cast<uint32_t>(coordinate);
}

void build_independent_blur() {
  static const int weights[7] = {72, 134, 195, 222, 195, 134, 72};
  for (uint32_t y = 0u; y < kHeight; ++y) {
    for (uint32_t x = 0u; x < kWidth; ++x) {
      uint32_t sum = 0u;
      for (int dx = -3; dx <= 3; ++dx) {
        sum += static_cast<uint32_t>(weights[dx + 3]) *
               g_image[static_cast<size_t>(y) * kStride +
                       reflect101_test(static_cast<int>(x) + dx, kWidth)];
      }
      g_reference_horizontal[static_cast<size_t>(y) * kWidth + x] = sum;
    }
  }
  for (uint32_t y = 0u; y < kHeight; ++y) {
    for (uint32_t x = 0u; x < kWidth; ++x) {
      uint64_t sum = 0u;
      for (int dy = -3; dy <= 3; ++dy) {
        const uint32_t sy = reflect101_test(static_cast<int>(y) + dy, kHeight);
        sum += static_cast<uint64_t>(weights[dy + 3]) *
               g_reference_horizontal[static_cast<size_t>(sy) * kWidth + x];
      }
      g_reference_blurred[static_cast<size_t>(y) * kWidth + x] =
          static_cast<uint8_t>((sum + (1u << 19u)) >> 20u);
    }
  }
}

float independent_orientation(uint32_t x, uint32_t y) {
  static const int umax[16] = {15, 15, 15, 15, 14, 14, 14, 13,
                               13, 12, 11, 10, 9, 8, 6, 3};
  int64_t m10 = 0;
  int64_t m01 = 0;
  const size_t center = static_cast<size_t>(y) * kStride + x;
  for (int u = -15; u <= 15; ++u)
    m10 += static_cast<int64_t>(u) * g_image[center + u];
  for (int v = 1; v <= 15; ++v) {
    int row_difference = 0;
    for (int u = -umax[v]; u <= umax[v]; ++u) {
      const int plus = g_image[static_cast<size_t>(y + v) * kStride + x + u];
      const int minus = g_image[static_cast<size_t>(y - v) * kStride + x + u];
      row_difference += plus - minus;
      m10 += static_cast<int64_t>(u) * (plus + minus);
    }
    m01 += static_cast<int64_t>(v) * row_difference;
  }
  float angle = static_cast<float>(std::atan2(static_cast<double>(m01),
                                                static_cast<double>(m10)) *
                                   (180.0 / 3.14159265358979323846));
  if (angle < 0.0f) angle += 360.0f;
  return angle;
}

#if defined(RBT_ORB_TESTING)
bool check_cordic_reference() {
  struct Moment {
    int64_t x;
    int64_t y;
  };
  static const Moment moments[] = {
      {1, 2},          {2, 1},          {1, 1},          {2, 3},
      {3, 2},          {-1, 2},         {-2, 1},         {-1, 1},
      {-2, 3},         {-3, 2},         {-1, -2},        {-2, -1},
      {-1, -1},        {-2, -3},        {-3, -2},        {1, -2},
      {2, -1},         {1, -1},         {2, -3},         {3, -2},
      {0, 1},          {1, 0},          {0, -1},         {-1, 0},
      {1, 1000000},    {-1, 1000000},   {-1, -1000000},  {1, -1000000},
      {1000000, 1},    {-1000000, 1},   {-1000000, -1},  {1000000, -1}};
  for (size_t index = 0u; index < sizeof(moments) / sizeof(moments[0]);
       ++index) {
    const uint32_t actual_turns =
        rbt_orb_test_angle_turns_q32(moments[index].x, moments[index].y);
    const double actual_degrees =
        static_cast<double>(actual_turns) * (360.0 / 4294967296.0);
    double expected_degrees =
        std::atan2(static_cast<double>(moments[index].y),
                   static_cast<double>(moments[index].x)) *
        (180.0 / 3.14159265358979323846);
    if (expected_degrees < 0.0) expected_degrees += 360.0;
    double difference = std::fabs(actual_degrees - expected_degrees);
    if (difference > 180.0) difference = 360.0 - difference;
    if (!require(difference < 0.0002,
                 "CORDIC orientation matches atan2 across quadrants and axes"))
      return false;
  }

  static const uint32_t angles[] = {
      0x00000000u, 0x00000001u, 0x1fffffffu, 0x20000000u,
      0x40000000u, 0x60000000u, 0x80000000u, 0xa0000000u,
      0xc0000000u, 0xe0000000u, 0xf8e38e39u, 0xfffffff0u};
  for (size_t index = 0u; index < sizeof(angles) / sizeof(angles[0]);
       ++index) {
    int64_t actual_cosine = 0;
    int64_t actual_sine = 0;
    rbt_orb_test_sincos_q30(angles[index], &actual_cosine, &actual_sine);
    const double radians = static_cast<double>(angles[index]) *
                           (6.28318530717958647692 / 4294967296.0);
    const double expected_cosine = std::cos(radians) * 1073741824.0;
    const double expected_sine = std::sin(radians) * 1073741824.0;
    if (!require(std::fabs(static_cast<double>(actual_cosine) -
                           expected_cosine) < 128.0 &&
                     std::fabs(static_cast<double>(actual_sine) -
                               expected_sine) < 128.0,
                 "CORDIC sine/cosine matches reference around full turn"))
      return false;
  }
  return true;
}
#endif

int round_away(double value) {
  return static_cast<int>(value >= 0.0 ? std::floor(value + 0.5)
                                       : std::ceil(value - 0.5));
}

void independent_descriptor(uint32_t x, uint32_t y, float angle,
                            uint8_t descriptor[RBT_ORB_DESCRIPTOR_BYTES]) {
  const double radians = static_cast<double>(angle) *
                         (3.14159265358979323846 / 180.0);
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  for (uint32_t byte = 0u; byte < RBT_ORB_DESCRIPTOR_BYTES; ++byte) {
    uint8_t value = 0u;
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
      const uint32_t point = byte * 16u + bit * 2u;
      const uint32_t pair = point / 2u;
      const int ax = rbt_orb_pattern_31[pair][0];
      const int ay = rbt_orb_pattern_31[pair][1];
      const int bx = rbt_orb_pattern_31[pair][2];
      const int by = rbt_orb_pattern_31[pair][3];
      const int ax_rotated = round_away(ax * cosine - ay * sine);
      const int ay_rotated = round_away(ax * sine + ay * cosine);
      const int bx_rotated = round_away(bx * cosine - by * sine);
      const int by_rotated = round_away(bx * sine + by * cosine);
      const uint8_t first = g_reference_blurred[
          static_cast<size_t>(static_cast<int>(y) + ay_rotated) * kWidth +
          static_cast<uint32_t>(static_cast<int>(x) + ax_rotated)];
      const uint8_t second = g_reference_blurred[
          static_cast<size_t>(static_cast<int>(y) + by_rotated) * kWidth +
          static_cast<uint32_t>(static_cast<int>(x) + bx_rotated)];
      if (first < second) value |= static_cast<uint8_t>(1u << bit);
    }
    descriptor[byte] = value;
  }
}

bool untouched_outputs(uint32_t count) {
  if (count != 0u) return false;
  const uint8_t* keypoint_bytes =
      reinterpret_cast<const uint8_t*>(g_keypoints.values);
  for (size_t index = 0u; index < sizeof(g_keypoints.values); ++index)
    if (keypoint_bytes[index] != kCanary) return false;
  for (uint32_t index = 0u;
       index < kFeatureCapacity * RBT_ORB_DESCRIPTOR_BYTES; ++index)
    if (g_descriptors[index] != kCanary) return false;
  return true;
}

bool prepare_outputs() {
  fill_bytes(&g_keypoints, sizeof(g_keypoints), kCanary);
  fill_bytes(g_descriptors, sizeof(g_descriptors), kCanary);
  g_keypoints.before = 0x1122334455667788ull;
  g_keypoints.after = 0x8877665544332211ull;
  return true;
}

}  // namespace

int main() {
#if defined(RBT_ORB_TESTING)
  if (!check_cordic_reference()) return 1;
#endif
  make_texture();
  RbtOrbConfig config = test_config();
  const size_t required = rbt_orb_workspace_size(kWidth, kHeight, &config);
  if (!require(required > 0u && required <= kWorkspaceBytes,
               "workspace size is valid and bounded"))
    return 1;
  RbtOrbConfig maximum = {RBT_ORB_MAX_FEATURES, RBT_ORB_MAX_LEVELS, 1.05f,
                          255u};
  const size_t maximum_workspace = rbt_orb_workspace_size(
      RBT_ORB_MAX_WIDTH, RBT_ORB_MAX_HEIGHT, &maximum);
  if (!require(maximum_workspace > 0u &&
                   maximum_workspace <= RBT_ORB_WORKSPACE_CAPACITY,
               "maximum workspace remains below the public limit"))
    return 1;

  fill_bytes(g_image, sizeof(g_image), 0u);
  uint32_t count = 77u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kStride, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count, g_workspace,
                               kWorkspaceBytes) == RBT_ORB_OK &&
                   count == 0u,
               "flat image returns a valid empty result"))
    return 1;
  make_texture();

  if (!prepare_outputs()) return 1;
  count = 0u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kStride, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count, g_workspace,
                               kWorkspaceBytes) == RBT_ORB_OK,
               "textured image extraction succeeds"))
    return 1;
  if (!require(count > 0u && count <= config.n_features,
               "textured image produces a bounded feature set"))
    return 1;
  if (!require(g_keypoints.before == 0x1122334455667788ull &&
                   g_keypoints.after == 0x8877665544332211ull,
               "keypoint output canaries remain intact"))
    return 1;
  for (uint32_t index = count; index < kFeatureCapacity; ++index) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(
        &g_keypoints.values[index]);
    for (size_t byte = 0u; byte < sizeof(RbtOrbKeypoint); ++byte)
      if (!require(bytes[byte] == kCanary,
                   "keypoint writes stop at returned count"))
        return 1;
  }
  for (uint32_t index = count * RBT_ORB_DESCRIPTOR_BYTES;
       index < kFeatureCapacity * RBT_ORB_DESCRIPTOR_BYTES; ++index) {
    if (!require(g_descriptors[index] == kCanary,
                 "descriptor writes stop at returned count"))
      return 1;
  }
  for (uint32_t index = 0u; index < count; ++index) {
    const RbtOrbKeypoint& keypoint = g_keypoints.values[index];
    if (!require(std::isfinite(keypoint.x) && std::isfinite(keypoint.y) &&
                     std::isfinite(keypoint.size) &&
                     std::isfinite(keypoint.angle) &&
                     std::isfinite(keypoint.response) && keypoint.x >= 0.0f &&
                     keypoint.y >= 0.0f && keypoint.x < kWidth &&
                     keypoint.y < kHeight && keypoint.size > 0.0f &&
                     keypoint.angle >= 0.0f && keypoint.angle < 360.0f &&
                     keypoint.octave >= 0 &&
                     keypoint.octave < static_cast<int32_t>(config.n_levels),
                 "all keypoint fields are finite and in range"))
      return 1;
  }
  uint32_t octave_mask = 0u;
  uint32_t octave_zero_count = 0u;
  for (uint32_t index = 0u; index < count; ++index) {
    octave_mask |= 1u << static_cast<uint32_t>(g_keypoints.values[index].octave);
    if (g_keypoints.values[index].octave == 0) ++octave_zero_count;
  }
  if (!require((octave_mask & (octave_mask - 1u)) != 0u,
               "features span multiple pyramid levels"))
    return 1;
  if (!require(octave_zero_count > 1u,
               "multiple level-zero features are available for independent checks"))
    return 1;

  build_independent_blur();
  uint32_t checked_octave_zero = 0u;
  for (uint32_t index = 0u; index < count; ++index) {
    const RbtOrbKeypoint& reference = g_keypoints.values[index];
    if (reference.octave != 0) continue;
    const uint32_t reference_x = static_cast<uint32_t>(reference.x + 0.5f);
    const uint32_t reference_y = static_cast<uint32_t>(reference.y + 0.5f);
    const float expected_angle =
        independent_orientation(reference_x, reference_y);
    float angle_error = std::fabs(reference.angle - expected_angle);
    if (angle_error > 180.0f) angle_error = 360.0f - angle_error;
    if (!require(angle_error < 0.01f,
                 "intensity-centroid orientation matches independent reference"))
      return 1;
    uint8_t expected_descriptor[RBT_ORB_DESCRIPTOR_BYTES];
    independent_descriptor(reference_x, reference_y, expected_angle,
                           expected_descriptor);
    if (!require(std::memcmp(expected_descriptor,
                             g_descriptors +
                                 static_cast<size_t>(index) *
                                     RBT_ORB_DESCRIPTOR_BYTES,
                             RBT_ORB_DESCRIPTOR_BYTES) == 0,
                 "octave-zero rotated BRIEF matches independent reference"))
      return 1;
    ++checked_octave_zero;
  }
  if (!require(checked_octave_zero == octave_zero_count,
               "all octave-zero descriptors were checked independently"))
    return 1;

  fill_bytes(&g_repeat_keypoints, sizeof(g_repeat_keypoints), kCanary);
  fill_bytes(g_repeat_descriptors, sizeof(g_repeat_descriptors), kCanary);
  g_repeat_keypoints.before = 0x1122334455667788ull;
  g_repeat_keypoints.after = 0x8877665544332211ull;
  uint32_t repeat_count = 0u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kStride, &config,
                               g_repeat_keypoints.values,
                               g_repeat_descriptors, kFeatureCapacity,
                               &repeat_count, g_workspace, kWorkspaceBytes) ==
                       RBT_ORB_OK &&
                   repeat_count == count &&
                   std::memcmp(g_keypoints.values, g_repeat_keypoints.values,
                               count * sizeof(RbtOrbKeypoint)) == 0 &&
                   std::memcmp(g_descriptors, g_repeat_descriptors,
                               count * RBT_ORB_DESCRIPTOR_BYTES) == 0,
               "repeated calls produce identical keypoints and descriptors"))
    return 1;

  config.n_features = 0u;
  if (!require(rbt_orb_workspace_size(kWidth, kHeight, &config) == 0u,
               "invalid configuration has no workspace size"))
    return 1;
  if (!prepare_outputs()) return 1;
  count = 77u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kStride, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count, g_workspace,
                               kWorkspaceBytes) == RBT_ORB_INVALID_ARGUMENT &&
                   untouched_outputs(count),
               "invalid configuration fails before output writes"))
    return 1;
  config = test_config();
  if (!prepare_outputs()) return 1;
  count = 77u;
  if (!require(rbt_orb_extract(g_image, 0u, kHeight, kStride, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count, g_workspace,
                               kWorkspaceBytes) == RBT_ORB_INVALID_ARGUMENT &&
                   untouched_outputs(count),
               "invalid dimensions fail before output writes"))
    return 1;
  if (!prepare_outputs()) return 1;
  count = 77u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kWidth - 1u,
                               &config, g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count, g_workspace,
                               kWorkspaceBytes) == RBT_ORB_INVALID_ARGUMENT &&
                   untouched_outputs(count),
               "short stride fails before output writes"))
    return 1;
  if (!prepare_outputs()) return 1;
  count = 77u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kStride, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity - 1u, &count, g_workspace,
                               kWorkspaceBytes) == RBT_ORB_OUTPUT_CAPACITY &&
                   untouched_outputs(count),
               "insufficient output capacity fails before output writes"))
    return 1;
  if (!prepare_outputs()) return 1;
  count = 77u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kStride, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count, g_workspace,
                               required - 1u) == RBT_ORB_WORKSPACE_TOO_SMALL &&
                   untouched_outputs(count),
               "undersized workspace fails before output writes"))
    return 1;
  if (!prepare_outputs()) return 1;
  count = 77u;
  if (!require(rbt_orb_extract(g_image, kWidth, kHeight, kStride, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count,
                               g_misaligned_workspace + 1u,
                               kWorkspaceBytes) == RBT_ORB_INVALID_ARGUMENT &&
                   untouched_outputs(count),
               "misaligned workspace fails before output writes"))
    return 1;
  if (!prepare_outputs()) return 1;
  count = 77u;
  if (!require(rbt_orb_extract(g_tiny_image, 30u, 30u, 30u, &config,
                               g_keypoints.values, g_descriptors,
                               kFeatureCapacity, &count, g_workspace,
                               kWorkspaceBytes) == RBT_ORB_OK &&
                   count == 0u,
               "levels smaller than the patch produce a valid empty result"))
    return 1;

  std::puts("orb_extract_test: PASS");
  return 0;
}
