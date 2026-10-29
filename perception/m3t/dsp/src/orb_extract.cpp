// SPDX-License-Identifier: MIT
// Portable deterministic ORB core. The learned BRIEF point table is
// reproduced under BSD-3-Clause terms in orb_pattern_31.h.
//
// The stage order follows OpenCV ORB (4.10.0): multiscale FAST with NMS,
// retain up to 2x each level quota by FAST response, retain each quota by
// Harris-7x7 response, intensity-centroid orientation, then blurred rotated
// BRIEF. This implementation intentionally has numeric differences from
// OpenCV: levels use repeated float scale multiplication and Q16 center-based
// bilinear resampling; Harris ranking uses the exact integer numerator for
// k=1/25; the 7x7 sigma-2 Gaussian uses the symmetric Q10 kernel below and
// one final Q20 rounding; angle and pattern rotation use Q30 CORDIC and
// round-to-nearest with half values away from zero. Level quotas use Q16
// reciprocal scale weights. No libm or OpenCV runtime is used by this file.

#include "rbt_orb_core.h"
#include "orb_pattern_31.h"

#include <stddef.h>
#include <stdint.h>

namespace {

const size_t kWorkspaceLimit = static_cast<size_t>(RBT_ORB_WORKSPACE_CAPACITY);
const size_t kWorkspaceAlignment = 128u;
const uint32_t kEdgeThreshold = 31u;
const uint32_t kPatchRadius = 15u;
const uint32_t kFastRadius = 3u;
const uint32_t kHarrisRadius = 3u;
const uint64_t kQ16One = 65536u;
const uint64_t kQ32One = 4294967296ull;
const int64_t kQ32HalfTurn = 2147483648ll;
const int64_t kQ32QuarterTurn = 1073741824ll;
const int64_t kQ30One = 1073741824ll;
const int64_t kQ30Half = 536870912ll;
const int64_t kMomentScaleQ30 = 1073741824ll;
const int64_t kCordicGainQ30 = 652032874ll;
const int kCordicAtanTurnsQ32[31] = {
    536870912, 316933406, 167458907, 85004756, 42667331, 21354465,
    10679838, 5340245, 2670163, 1335087, 667544, 333772, 166886,
    83443, 41722, 20861, 10430, 5215, 2608, 1304, 652, 326, 163,
    81, 41, 20, 10, 5, 3, 1, 1};
const int kGaussianQ10[7] = {72, 134, 195, 222, 195, 134, 72};
const int kUmax[16] = {15, 15, 15, 15, 14, 14, 14, 13,
                       13, 12, 11, 10, 9, 8, 6, 3};

struct Candidate {
  uint16_t x;
  uint16_t y;
  uint8_t fast_score;
  uint8_t reserved[3];
  int64_t harris_numerator;
};

struct WorkspaceLayout {
  size_t gray0;
  size_t gray1;
  size_t blurred;
  size_t fast_scores;
  size_t horizontal_q10;
  size_t candidates;
  size_t total;
};

bool valid_config(const RbtOrbConfig* config) {
  return config != nullptr && config->n_features >= 1u &&
         config->n_features <= RBT_ORB_MAX_FEATURES &&
         config->n_levels >= 1u && config->n_levels <= RBT_ORB_MAX_LEVELS &&
         config->scale_factor >= 1.05f && config->scale_factor <= 2.0f &&
         config->fast_threshold >= 1u && config->fast_threshold <= 255u;
}

bool round_up_alignment(size_t value, size_t* rounded) {
  const size_t mask = kWorkspaceAlignment - 1u;
  if (value > static_cast<size_t>(-1) - mask) return false;
  *rounded = (value + mask) & ~mask;
  return true;
}

bool add_segment(size_t bytes, size_t* offset, size_t* segment) {
  size_t aligned = 0u;
  if (!round_up_alignment(*offset, &aligned)) return false;
  if (bytes > static_cast<size_t>(-1) - aligned) return false;
  *segment = aligned;
  *offset = aligned + bytes;
  return true;
}

bool address_range_fits(const void* pointer, size_t bytes) {
  const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
  const uintptr_t maximum = static_cast<uintptr_t>(-1);
  return bytes <= static_cast<size_t>(maximum - address);
}

void clear_bytes(uint8_t* data, size_t count) {
  volatile uint8_t* output = data;
  for (size_t index = 0u; index < count; ++index) output[index] = 0u;
}

void clear_words(uint32_t* data, uint32_t count) {
  volatile uint32_t* output = data;
  for (uint32_t index = 0u; index < count; ++index) output[index] = 0u;
}

bool make_layout(uint32_t width, uint32_t height, const RbtOrbConfig* config,
                 WorkspaceLayout* layout) {
  if (layout == nullptr || width == 0u || height == 0u ||
      width > RBT_ORB_MAX_WIDTH || height > RBT_ORB_MAX_HEIGHT ||
      !valid_config(config)) {
    return false;
  }
  const size_t pixels = static_cast<size_t>(width) * height;
  if (pixels > static_cast<size_t>(-1) / sizeof(uint32_t)) return false;
  const size_t candidate_bytes =
      static_cast<size_t>(config->n_features) * sizeof(Candidate);
  size_t offset = 0u;
  if (!add_segment(pixels, &offset, &layout->gray0) ||
      !add_segment(pixels, &offset, &layout->gray1) ||
      !add_segment(pixels, &offset, &layout->blurred) ||
      !add_segment(pixels, &offset, &layout->fast_scores) ||
      !add_segment(pixels * sizeof(uint32_t), &offset,
                   &layout->horizontal_q10) ||
      !add_segment(candidate_bytes, &offset, &layout->candidates) ||
      !round_up_alignment(offset, &layout->total) ||
      layout->total > kWorkspaceLimit) {
    return false;
  }
  return true;
}

uint32_t round_positive_float(float value) {
  if (value <= 1.0f) return 1u;
  return static_cast<uint32_t>(value + 0.5f);
}

uint32_t level_dimension(uint32_t base, float scale) {
  const float inverse_scale = 1.0f / scale;
  const float dimension = static_cast<float>(base) * inverse_scale;
  return round_positive_float(dimension);
}

void compute_level_quotas(const RbtOrbConfig* config,
                          uint32_t quotas[RBT_ORB_MAX_LEVELS]) {
  uint32_t weights[RBT_ORB_MAX_LEVELS];
  const uint32_t q16 = 65536u;
  const float reciprocal = 1.0f / config->scale_factor;
  const uint32_t reciprocal_q16 =
      static_cast<uint32_t>(reciprocal * 65536.0f + 0.5f);
  weights[0] = q16;
  uint32_t weight_sum = q16;
  for (uint32_t level = 1u; level < config->n_levels; ++level) {
    weights[level] =
        (weights[level - 1u] * reciprocal_q16 + q16 / 2u) / q16;
    weight_sum += weights[level];
  }
  uint32_t assigned = 0u;
  for (uint32_t level = 0u; level + 1u < config->n_levels; ++level) {
    const uint32_t scaled = config->n_features * weights[level];
    uint32_t quota = (scaled + weight_sum / 2u) / weight_sum;
    if (quota > config->n_features - assigned) {
      quota = config->n_features - assigned;
    }
    quotas[level] = quota;
    assigned += quota;
  }
  quotas[config->n_levels - 1u] = config->n_features - assigned;
}

int64_t shift_right_floor(int64_t value, uint32_t bits) {
  if (bits == 0u) return value;
  if (value >= 0) return value >> bits;
  const uint64_t magnitude = static_cast<uint64_t>(-(value + 1)) + 1u;
  const uint64_t quotient = magnitude >> bits;
  const uint64_t remainder_mask = (static_cast<uint64_t>(1u) << bits) - 1u;
  return -static_cast<int64_t>(quotient + ((magnitude & remainder_mask) != 0u));
}

int64_t floor_q16(int64_t value) {
  return shift_right_floor(value, 16u);
}

int64_t center_coordinate_q16(uint32_t destination_index,
                               uint32_t source_length,
                               uint32_t destination_length) {
  const uint32_t numerator = (2u * destination_index + 1u) * source_length;
  const uint32_t whole = numerator / destination_length;
  const uint32_t remainder = numerator % destination_length;
  return static_cast<int64_t>(whole * 32768u +
                              (remainder * 32768u) / destination_length) -
         32768;
}

uint32_t reflect101(int64_t coordinate, uint32_t length) {
  if (length <= 1u) return 0u;
  const int64_t maximum = static_cast<int64_t>(length) - 1;
  while (coordinate < 0 || coordinate > maximum) {
    if (coordinate < 0) {
      coordinate = -coordinate;
    } else {
      coordinate = 2 * maximum - coordinate;
    }
  }
  return static_cast<uint32_t>(coordinate);
}

void copy_base_image(const uint8_t* source, uint32_t width, uint32_t height,
                     uint32_t stride, uint8_t* destination) {
  for (uint32_t y = 0u; y < height; ++y) {
    const uint8_t* row = source + static_cast<size_t>(y) * stride;
    uint8_t* output = destination + static_cast<size_t>(y) * width;
    for (uint32_t x = 0u; x < width; ++x) output[x] = row[x];
  }
}

void resize_bilinear_q16(const uint8_t* source, uint32_t source_width,
                         uint32_t source_height, uint8_t* destination,
                         uint32_t destination_width,
                         uint32_t destination_height) {
  if (source_width == destination_width &&
      source_height == destination_height) {
    const size_t bytes = static_cast<size_t>(source_width) * source_height;
    for (size_t index = 0u; index < bytes; ++index)
      destination[index] = source[index];
    return;
  }
  for (uint32_t y = 0u; y < destination_height; ++y) {
    const int64_t y_q16 =
        center_coordinate_q16(y, source_height, destination_height);
    const int64_t y0_signed = floor_q16(y_q16);
    const uint32_t y0 = reflect101(y0_signed, source_height);
    const uint32_t y1 = reflect101(y0_signed + 1, source_height);
    const uint32_t fy = static_cast<uint32_t>(y_q16 - y0_signed * 65536);
    const uint64_t wy0 = kQ16One - fy;
    const uint64_t wy1 = fy;
    for (uint32_t x = 0u; x < destination_width; ++x) {
      const int64_t x_q16 =
          center_coordinate_q16(x, source_width, destination_width);
      const int64_t x0_signed = floor_q16(x_q16);
      const uint32_t x0 = reflect101(x0_signed, source_width);
      const uint32_t x1 = reflect101(x0_signed + 1, source_width);
      const uint32_t fx = static_cast<uint32_t>(x_q16 - x0_signed * 65536);
      const uint64_t wx0 = kQ16One - fx;
      const uint64_t wx1 = fx;
      const uint64_t top =
          static_cast<uint64_t>(source[static_cast<size_t>(y0) * source_width +
                                       x0]) * wx0 +
          static_cast<uint64_t>(source[static_cast<size_t>(y0) * source_width +
                                       x1]) * wx1;
      const uint64_t bottom =
          static_cast<uint64_t>(source[static_cast<size_t>(y1) * source_width +
                                       x0]) * wx0 +
          static_cast<uint64_t>(source[static_cast<size_t>(y1) * source_width +
                                       x1]) * wx1;
      const uint64_t combined = top * wy0 + bottom * wy1;
      destination[static_cast<size_t>(y) * destination_width + x] =
          static_cast<uint8_t>((combined + (static_cast<uint64_t>(1u) << 31u)) >>
                               32u);
    }
  }
}

uint8_t fast9_score(const uint8_t* image, uint32_t width, uint32_t x,
                    uint32_t y, uint32_t threshold) {
  static const int8_t kRingX[16] = {0, 1, 2, 3, 3, 3, 2, 1,
                                    0, -1, -2, -3, -3, -3, -2, -1};
  static const int8_t kRingY[16] = {-3, -3, -2, -1, 0, 1, 2, 3,
                                    3, 3, 2, 1, 0, -1, -2, -3};
  const int center = image[static_cast<size_t>(y) * width + x];
  int16_t difference[16];
  for (uint32_t index = 0u; index < 16u; ++index) {
    const int value = image[static_cast<size_t>(
        static_cast<int64_t>(y) + kRingY[index]) * width +
                            static_cast<uint32_t>(static_cast<int64_t>(x) +
                                                  kRingX[index])];
    difference[index] = static_cast<int16_t>(value - center);
  }
  bool is_corner = false;
  int bright_run = 0;
  int dark_run = 0;
  for (uint32_t index = 0u; index < 24u; ++index) {
    const uint32_t ring_index = index & 15u;
    const int value = difference[ring_index];
    bright_run = value > static_cast<int>(threshold)
                     ? bright_run + 1
                     : 0;
    dark_run = -value > static_cast<int>(threshold) ? dark_run + 1 : 0;
    if (bright_run >= 9 || dark_run >= 9) {
      is_corner = true;
      break;
    }
  }
  if (!is_corner) return 0u;

  int best = 0;
  for (uint32_t start = 0u; start < 16u; ++start) {
    int bright_minimum = 256;
    int dark_minimum = 256;
    for (uint32_t offset = 0u; offset < 9u; ++offset) {
      const int value = difference[(start + offset) & 15u];
      const int bright = value;
      const int dark = -value;
      if (bright < bright_minimum) bright_minimum = bright;
      if (dark < dark_minimum) dark_minimum = dark;
    }
    if (bright_minimum > best) best = bright_minimum;
    if (dark_minimum > best) best = dark_minimum;
  }
  return static_cast<uint8_t>(best > 0 ? best : 0);
}

bool is_fast_nonmax(const uint8_t* scores, uint32_t width, uint32_t x,
                    uint32_t y) {
  const size_t center = static_cast<size_t>(y) * width + x;
  const uint8_t score = scores[center];
  if (score == 0u) return false;
  return score > scores[center - width - 1u] &&
         score > scores[center - width] &&
         score > scores[center - width + 1u] &&
         score > scores[center - 1u] && score > scores[center + 1u] &&
         score > scores[center + width - 1u] &&
         score > scores[center + width] &&
         score > scores[center + width + 1u];
}

int64_t harris_numerator(const uint8_t* image, uint32_t width, uint32_t x,
                         uint32_t y) {
  int64_t a = 0;
  int64_t b = 0;
  int64_t c = 0;
  for (int dy = -static_cast<int>(kHarrisRadius);
       dy <= static_cast<int>(kHarrisRadius); ++dy) {
    for (int dx = -static_cast<int>(kHarrisRadius);
         dx <= static_cast<int>(kHarrisRadius); ++dx) {
      const size_t center = static_cast<size_t>(
          static_cast<int64_t>(y) + dy) * width +
                            static_cast<uint32_t>(static_cast<int64_t>(x) + dx);
      const int ix = (static_cast<int>(image[center + 1u]) -
                      static_cast<int>(image[center - 1u])) *
                         2 +
                     static_cast<int>(image[center - width + 1u]) -
                     static_cast<int>(image[center - width - 1u]) +
                     static_cast<int>(image[center + width + 1u]) -
                     static_cast<int>(image[center + width - 1u]);
      const int iy = (static_cast<int>(image[center + width]) -
                      static_cast<int>(image[center - width])) *
                         2 +
                     static_cast<int>(image[center + width - 1u]) -
                     static_cast<int>(image[center - width - 1u]) +
                     static_cast<int>(image[center + width + 1u]) -
                     static_cast<int>(image[center - width + 1u]);
      a += static_cast<int64_t>(ix) * ix;
      b += static_cast<int64_t>(iy) * iy;
      c += static_cast<int64_t>(ix) * iy;
    }
  }
  const int64_t determinant = a * b - c * c;
  const int64_t trace = a + b;
  return 25 * determinant - trace * trace;
}

bool candidate_better(const Candidate& left, const Candidate& right) {
  if (left.harris_numerator != right.harris_numerator)
    return left.harris_numerator > right.harris_numerator;
  if (left.y != right.y) return left.y < right.y;
  return left.x < right.x;
}

void insert_harris_candidate(Candidate* candidates, uint32_t* count,
                             uint32_t quota, const Candidate& candidate) {
  uint32_t position = 0u;
  while (position < *count &&
         !candidate_better(candidate, candidates[position])) {
    ++position;
  }
  if (position >= quota) return;
  if (*count < quota) ++(*count);
  for (uint32_t index = *count - 1u; index > position; --index)
    candidates[index] = candidates[index - 1u];
  candidates[position] = candidate;
}

int64_t angle_turns_q32(int64_t x, int64_t y) {
  if (x == 0 && y == 0) return 0;
  // A 31x31 patch bounds either first moment by
  // 255 * 31 * 31 * 15 < 3.7e6. Q30 scaling and the CORDIC gain (<1.65)
  // therefore keep every vectoring intermediate below 6.6e15, within int64.
  // Scaling preserves direction while retaining low-moment precision.
  x *= kMomentScaleQ30;
  y *= kMomentScaleQ30;
  int64_t z = 0;
  if (x < 0) {
    z = y >= 0 ? kQ32HalfTurn : -kQ32HalfTurn;
    x = -x;
    y = -y;
  }
  for (uint32_t index = 0u; index < 31u; ++index) {
    const int64_t shifted_x = shift_right_floor(x, index);
    const int64_t shifted_y = shift_right_floor(y, index);
    if (y > 0) {
      x += shifted_y;
      y -= shifted_x;
      z += kCordicAtanTurnsQ32[index];
    } else if (y < 0) {
      x -= shifted_y;
      y += shifted_x;
      z -= kCordicAtanTurnsQ32[index];
    }
  }
  if (z < 0) z += static_cast<int64_t>(kQ32One);
  if (z >= static_cast<int64_t>(kQ32One)) z -= static_cast<int64_t>(kQ32One);
  return z;
}

void intensity_centroid(const uint8_t* image, uint32_t width, uint32_t x,
                        uint32_t y, int64_t* angle_turns) {
  int64_t m10 = 0;
  int64_t m01 = 0;
  const size_t center = static_cast<size_t>(y) * width + x;
  for (int u = -static_cast<int>(kPatchRadius);
       u <= static_cast<int>(kPatchRadius); ++u) {
    m10 += static_cast<int64_t>(u) * image[center + u];
  }
  for (int v = 1; v <= static_cast<int>(kPatchRadius); ++v) {
    int64_t row_difference = 0;
    const int d = kUmax[v];
    for (int u = -d; u <= d; ++u) {
      const int plus = image[static_cast<size_t>(
          static_cast<int64_t>(y) + v) * width +
                               static_cast<uint32_t>(static_cast<int64_t>(x) + u)];
      const int minus = image[static_cast<size_t>(
          static_cast<int64_t>(y) - v) * width +
                               static_cast<uint32_t>(static_cast<int64_t>(x) + u)];
      row_difference += plus - minus;
      m10 += static_cast<int64_t>(u) * (plus + minus);
    }
    m01 += static_cast<int64_t>(v) * row_difference;
  }
  *angle_turns = angle_turns_q32(m10, m01);
}

void cordic_sincos_q30(int64_t angle_turns, int64_t* cosine, int64_t* sine) {
  int64_t z = angle_turns;
  int64_t sign = 1;
  if (z >= kQ32HalfTurn) z -= static_cast<int64_t>(kQ32One);
  if (z > kQ32QuarterTurn) {
    z -= kQ32HalfTurn;
    sign = -1;
  } else if (z < -kQ32QuarterTurn) {
    z += kQ32HalfTurn;
    sign = -1;
  }
  int64_t x = kCordicGainQ30;
  int64_t y = 0;
  for (uint32_t index = 0u; index < 31u; ++index) {
    const int64_t shifted_x = shift_right_floor(x, index);
    const int64_t shifted_y = shift_right_floor(y, index);
    if (z >= 0) {
      x -= shifted_y;
      y += shifted_x;
      z -= kCordicAtanTurnsQ32[index];
    } else {
      x += shifted_y;
      y -= shifted_x;
      z += kCordicAtanTurnsQ32[index];
    }
  }
  *cosine = x * sign;
  *sine = y * sign;
}

int32_t round_q30_away(int64_t value) {
  const int64_t half = kQ30Half;
  if (value >= 0)
    return static_cast<int32_t>((value + half) / kQ30One);
  return -static_cast<int32_t>((-value + half) / kQ30One);
}

float angle_degrees(int64_t turns_q32) {
  float angle = static_cast<float>(turns_q32) * (360.0f / 4294967296.0f);
  if (angle >= 360.0f) angle = 0.0f;
  return angle;
}

void gaussian7x7_q10(const uint8_t* image, uint32_t width, uint32_t height,
                     uint8_t* blurred, uint32_t* horizontal_q10) {
  for (uint32_t y = 0u; y < height; ++y) {
    for (uint32_t x = 0u; x < width; ++x) {
      uint32_t sum = 0u;
      for (int k = -3; k <= 3; ++k) {
        const uint32_t sx = reflect101(static_cast<int64_t>(x) + k, width);
        sum += static_cast<uint32_t>(kGaussianQ10[k + 3]) *
               image[static_cast<size_t>(y) * width + sx];
      }
      horizontal_q10[static_cast<size_t>(y) * width + x] = sum;
    }
  }
  for (uint32_t y = 0u; y < height; ++y) {
    for (uint32_t x = 0u; x < width; ++x) {
      uint64_t sum = 0u;
      for (int k = -3; k <= 3; ++k) {
        const uint32_t sy = reflect101(static_cast<int64_t>(y) + k, height);
        sum += static_cast<uint64_t>(kGaussianQ10[k + 3]) *
               horizontal_q10[static_cast<size_t>(sy) * width + x];
      }
      blurred[static_cast<size_t>(y) * width + x] =
          static_cast<uint8_t>((sum + (1u << 19u)) >> 20u);
    }
  }
}

float harris_response_float(int64_t numerator, const uint8_t* image,
                            uint32_t width, uint32_t x, uint32_t y) {
  (void)numerator;
  int64_t a = 0;
  int64_t b = 0;
  int64_t c = 0;
  for (int dy = -static_cast<int>(kHarrisRadius);
       dy <= static_cast<int>(kHarrisRadius); ++dy) {
    for (int dx = -static_cast<int>(kHarrisRadius);
         dx <= static_cast<int>(kHarrisRadius); ++dx) {
      const size_t center = static_cast<size_t>(
          static_cast<int64_t>(y) + dy) * width +
                            static_cast<uint32_t>(static_cast<int64_t>(x) + dx);
      const int ix = (static_cast<int>(image[center + 1u]) -
                      static_cast<int>(image[center - 1u])) *
                         2 +
                     static_cast<int>(image[center - width + 1u]) -
                     static_cast<int>(image[center - width - 1u]) +
                     static_cast<int>(image[center + width + 1u]) -
                     static_cast<int>(image[center + width - 1u]);
      const int iy = (static_cast<int>(image[center + width]) -
                      static_cast<int>(image[center - width])) *
                         2 +
                     static_cast<int>(image[center + width - 1u]) -
                     static_cast<int>(image[center - width - 1u]) +
                     static_cast<int>(image[center + width + 1u]) -
                     static_cast<int>(image[center - width + 1u]);
      a += static_cast<int64_t>(ix) * ix;
      b += static_cast<int64_t>(iy) * iy;
      c += static_cast<int64_t>(ix) * iy;
    }
  }
  const float scale = 1.0f / (4.0f * 7.0f * 255.0f);
  const float scale_sq_sq = scale * scale * scale * scale;
  const float fa = static_cast<float>(a);
  const float fb = static_cast<float>(b);
  const float fc = static_cast<float>(c);
  const float trace = fa + fb;
  return (fa * fb - fc * fc - 0.04f * trace * trace) * scale_sq_sq;
}

void write_descriptor(const uint8_t* blurred, uint32_t width, uint32_t x,
                      uint32_t y, int64_t angle_turns, uint8_t* descriptor) {
  int64_t cosine = 0;
  int64_t sine = 0;
  cordic_sincos_q30(angle_turns, &cosine, &sine);
  for (uint32_t byte = 0u; byte < RBT_ORB_DESCRIPTOR_BYTES; ++byte) {
    uint8_t value = 0u;
    for (uint32_t bit = 0u; bit < 8u; ++bit) {
      const uint32_t point = byte * 16u + bit * 2u;
      const uint32_t pair = point / 2u;
      const int64_t ax = rbt_orb_pattern_31[pair][0];
      const int64_t ay = rbt_orb_pattern_31[pair][1];
      const int64_t bx = rbt_orb_pattern_31[pair][2];
      const int64_t by = rbt_orb_pattern_31[pair][3];
      const int32_t ax_rotated =
          round_q30_away(ax * cosine - ay * sine);
      const int32_t ay_rotated =
          round_q30_away(ax * sine + ay * cosine);
      const int32_t bx_rotated =
          round_q30_away(bx * cosine - by * sine);
      const int32_t by_rotated =
          round_q30_away(bx * sine + by * cosine);
      const uint8_t first = blurred[
          static_cast<size_t>(static_cast<int64_t>(y) + ay_rotated) * width +
          static_cast<uint32_t>(static_cast<int64_t>(x) + ax_rotated)];
      const uint8_t second = blurred[
          static_cast<size_t>(static_cast<int64_t>(y) + by_rotated) * width +
          static_cast<uint32_t>(static_cast<int64_t>(x) + bx_rotated)];
      if (first < second) value |= static_cast<uint8_t>(1u << bit);
    }
    descriptor[byte] = value;
  }
}

uint32_t process_level(const uint8_t* image, uint32_t width, uint32_t height,
                       float scale, uint32_t quota, uint32_t fast_threshold,
                       uint8_t* fast_scores, uint8_t* blurred,
                       uint32_t* horizontal_q10, Candidate* candidates,
                       RbtOrbKeypoint* keypoints, uint8_t* descriptors,
                       uint32_t output_offset) {
  if (quota == 0u || width <= 2u * kEdgeThreshold ||
      height <= 2u * kEdgeThreshold) {
    return 0u;
  }
  const size_t pixels = static_cast<size_t>(width) * height;
  clear_bytes(fast_scores, pixels);

  for (uint32_t y = kFastRadius; y + kFastRadius < height; ++y) {
    for (uint32_t x = kFastRadius; x + kFastRadius < width; ++x) {
      fast_scores[static_cast<size_t>(y) * width + x] =
          fast9_score(image, width, x, y, fast_threshold);
    }
  }

  uint32_t histogram[256];
  clear_words(histogram, 256u);
  uint32_t candidate_total = 0u;
  for (uint32_t y = kFastRadius; y + kFastRadius < height; ++y) {
    for (uint32_t x = kFastRadius; x + kFastRadius < width; ++x) {
      if (x < kEdgeThreshold || y < kEdgeThreshold ||
          x + kEdgeThreshold >= width || y + kEdgeThreshold >= height ||
          !is_fast_nonmax(fast_scores, width, x, y)) {
        continue;
      }
      const uint8_t score = fast_scores[static_cast<size_t>(y) * width + x];
      ++histogram[score];
      ++candidate_total;
    }
  }
  if (candidate_total == 0u) return 0u;

  const uint32_t fast_limit = quota * 2u;
  uint32_t cutoff = 0u;
  uint32_t take_at_cutoff = 0u;
  if (candidate_total <= fast_limit) {
    take_at_cutoff = histogram[0];
  } else {
    uint32_t greater_count = 0u;
    for (int score = 255; score >= 1; --score) {
      const uint32_t score_count = histogram[score];
      if (greater_count + score_count >= fast_limit) {
        cutoff = static_cast<uint32_t>(score);
        take_at_cutoff = fast_limit - greater_count;
        break;
      }
      greater_count += score_count;
    }
  }

  uint32_t tied_remaining = take_at_cutoff;
  uint32_t retained = 0u;
  for (uint32_t y = kEdgeThreshold; y + kEdgeThreshold < height; ++y) {
    for (uint32_t x = kEdgeThreshold; x + kEdgeThreshold < width; ++x) {
      if (!is_fast_nonmax(fast_scores, width, x, y)) continue;
      const uint8_t fast_score = fast_scores[static_cast<size_t>(y) * width + x];
      if (fast_score < cutoff) continue;
      if (fast_score == cutoff) {
        if (tied_remaining == 0u) continue;
        --tied_remaining;
      }
      Candidate candidate;
      candidate.x = static_cast<uint16_t>(x);
      candidate.y = static_cast<uint16_t>(y);
      candidate.fast_score = fast_score;
      candidate.reserved[0] = 0u;
      candidate.reserved[1] = 0u;
      candidate.reserved[2] = 0u;
      candidate.harris_numerator = harris_numerator(image, width, x, y);
      insert_harris_candidate(candidates, &retained, quota, candidate);
    }
  }
  if (retained == 0u) return 0u;

  gaussian7x7_q10(image, width, height, blurred, horizontal_q10);
  for (uint32_t index = 0u; index < retained; ++index) {
    const Candidate& candidate = candidates[index];
    const uint32_t x = candidate.x;
    const uint32_t y = candidate.y;
    int64_t angle = 0;
    intensity_centroid(image, width, x, y, &angle);
    RbtOrbKeypoint& keypoint = keypoints[output_offset + index];
    keypoint.x = static_cast<float>(x) * scale;
    keypoint.y = static_cast<float>(y) * scale;
    keypoint.size = 31.0f * scale;
    keypoint.angle = angle_degrees(angle);
    keypoint.response = harris_response_float(
        candidate.harris_numerator, image, width, x, y);
    keypoint.octave = static_cast<int32_t>(0);
    write_descriptor(blurred, width, x, y, angle,
                     descriptors + static_cast<size_t>(output_offset + index) *
                                      RBT_ORB_DESCRIPTOR_BYTES);
  }
  return retained;
}

}  // namespace

#if defined(RBT_ORB_TESTING)
// Private test hooks intentionally stay out of the frozen public API.
extern "C" uint32_t rbt_orb_test_angle_turns_q32(int64_t x, int64_t y) {
  return static_cast<uint32_t>(angle_turns_q32(x, y));
}

extern "C" void rbt_orb_test_sincos_q30(uint32_t angle_turns,
                                          int64_t* cosine, int64_t* sine) {
  if (cosine == nullptr || sine == nullptr) return;
  cordic_sincos_q30(static_cast<int64_t>(angle_turns), cosine, sine);
}
#endif

extern "C" size_t rbt_orb_workspace_size(uint32_t width, uint32_t height,
                                           const RbtOrbConfig* config) {
  WorkspaceLayout layout;
  return make_layout(width, height, config, &layout) ? layout.total : 0u;
}

extern "C" int rbt_orb_extract(const uint8_t* image, uint32_t width,
                               uint32_t height, uint32_t stride,
                               const RbtOrbConfig* config,
                               RbtOrbKeypoint* keypoints,
                               uint8_t* descriptors, uint32_t capacity,
                               uint32_t* count, void* workspace,
                               size_t workspace_bytes) {
  if (count != nullptr) *count = 0u;
  if (count == nullptr || image == nullptr || keypoints == nullptr ||
      descriptors == nullptr || workspace == nullptr ||
      width == 0u || height == 0u || width > RBT_ORB_MAX_WIDTH ||
      height > RBT_ORB_MAX_HEIGHT || stride < width ||
      !valid_config(config)) {
    return RBT_ORB_INVALID_ARGUMENT;
  }
  const uint64_t final_row_offset =
      static_cast<uint64_t>(height - 1u) * stride;
  if (final_row_offset >
      static_cast<uint64_t>(static_cast<size_t>(-1) - width)) {
    return RBT_ORB_INVALID_ARGUMENT;
  }
  if ((reinterpret_cast<uintptr_t>(workspace) &
       static_cast<uintptr_t>(kWorkspaceAlignment - 1u)) != 0u ||
      workspace_bytes > kWorkspaceLimit) {
    return RBT_ORB_INVALID_ARGUMENT;
  }
  if (capacity < config->n_features) return RBT_ORB_OUTPUT_CAPACITY;

  WorkspaceLayout layout;
  if (!make_layout(width, height, config, &layout))
    return RBT_ORB_INVALID_ARGUMENT;
  if (workspace_bytes < layout.total) return RBT_ORB_WORKSPACE_TOO_SMALL;
  const size_t image_span = static_cast<size_t>(final_row_offset) + width;
  const size_t keypoint_span =
      static_cast<size_t>(config->n_features) * sizeof(RbtOrbKeypoint);
  const size_t descriptor_span =
      static_cast<size_t>(config->n_features) * RBT_ORB_DESCRIPTOR_BYTES;
  if (!address_range_fits(image, image_span) ||
      !address_range_fits(keypoints, keypoint_span) ||
      !address_range_fits(descriptors, descriptor_span) ||
      !address_range_fits(count, sizeof(*count)) ||
      !address_range_fits(workspace, layout.total)) {
    return RBT_ORB_INVALID_ARGUMENT;
  }

  uint8_t* workspace_bytes_ptr = static_cast<uint8_t*>(workspace);
  uint8_t* gray0 = workspace_bytes_ptr + layout.gray0;
  uint8_t* gray1 = workspace_bytes_ptr + layout.gray1;
  uint8_t* blurred = workspace_bytes_ptr + layout.blurred;
  uint8_t* fast_scores = workspace_bytes_ptr + layout.fast_scores;
  uint32_t* horizontal_q10 = reinterpret_cast<uint32_t*>(
      workspace_bytes_ptr + layout.horizontal_q10);
  Candidate* candidates = reinterpret_cast<Candidate*>(
      workspace_bytes_ptr + layout.candidates);

  copy_base_image(image, width, height, stride, gray0);
  uint8_t* current = gray0;
  uint8_t* next = gray1;
  uint32_t current_width = width;
  uint32_t current_height = height;
  float scale = 1.0f;
  uint32_t quotas[RBT_ORB_MAX_LEVELS];
  compute_level_quotas(config, quotas);
  uint32_t output_count = 0u;
  for (uint32_t level = 0u; level < config->n_levels; ++level) {
    if (level > 0u) {
      scale = scale * config->scale_factor;
      const uint32_t next_width = level_dimension(width, scale);
      const uint32_t next_height = level_dimension(height, scale);
      resize_bilinear_q16(current, current_width, current_height, next,
                          next_width, next_height);
      uint8_t* old_current = current;
      current = next;
      next = old_current;
      current_width = next_width;
      current_height = next_height;
    }
    const uint32_t produced = process_level(
        current, current_width, current_height, scale, quotas[level],
        config->fast_threshold, fast_scores, blurred, horizontal_q10,
        candidates, keypoints, descriptors, output_count);
    for (uint32_t index = 0u; index < produced; ++index)
      keypoints[output_count + index].octave = static_cast<int32_t>(level);
    output_count += produced;
  }
  *count = output_count;
  return RBT_ORB_OK;
}
