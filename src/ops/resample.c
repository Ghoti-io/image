/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Resizing: one separable filtered resampler, run as a horizontal pass and
 * then a vertical one, parameterized by a kernel and a support radius.  Every
 * filter this library offers but NEAREST is a row in that table rather than
 * an implementation of its own.
 *
 * **The support scaling is the part that matters.** When an axis is being
 * reduced, the kernel is stretched by the reduction ratio so that it averages
 * over the whole source region mapping to one destination pixel.  Without it
 * a kernel keeps its original radius, reads one or two source pixels out of
 * however many it should have covered, and aliases - which is the difference
 * between a downscale and a decimation, and is the most common way a
 * resampler is wrong while still producing a plausible picture.
 *
 * NEAREST is deliberately *not* stretched, and is a separate path: its whole
 * purpose is to return a sample that was in the source, which is what a
 * caller wants when the values are labels rather than colours.  Averaging
 * would defeat it at exactly the ratios where the stretch would apply.
 *
 * **Fixed point, not floating point.**  Coefficients are quantized to
 * GIMG_RESAMPLE_PRECISION_BITS and accumulated in integers.  A float
 * accumulator gives different results under FMA contraction and at different
 * optimization levels, and a byte-exact comparison against an outside
 * resampler that only holds on one build is not a check.
 *
 * The arrangement of the coefficient computation - the half-pixel centre, the
 * bounds, the normalization and the rounding - follows Pillow's Resample.c,
 * because being able to compare byte for byte against a resampler nobody here
 * wrote is worth more than any different arrangement of the same arithmetic.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/color/color.h>
#include <ghoti.io/color/math.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "../core/safe_math_internal.h"
#include "../raster/raster_internal.h"
#include "ops_internal.h"

/**
 * Coefficients are held as integers scaled by 1 << this.  Twenty-two is what
 * Pillow uses for eight-bit input: a sample of 255 times a full-weight
 * coefficient is about 2^30, which leaves room in a signed 32-bit accumulator
 * for the negative lobes of a cubic or a Lanczos window.  The accumulator
 * here is 64-bit so that the same number of bits serves 16-bit samples too.
 */
#define GIMG_RESAMPLE_PRECISION_BITS 22

/** pi to more places than a double holds; M_PI is not ISO C. */
#define GIMG_RESAMPLE_PI 3.14159265358979323846

/** The coefficient table for one axis: who feeds each output pixel, and how
 *  much of each. */
typedef struct {
  /** weights[out * support_max + tap], fixed point. */
  int32_t * weights;
  /** First source index contributing to each output pixel. */
  int32_t * first;
  /** How many taps each output pixel actually uses. */
  int32_t * count;
  /** Row stride of @c weights: the most taps any output pixel can use. */
  int32_t support_max;
  /** How many output pixels this axis has. */
  uint32_t out_count;
} gimg_resample_axis;

static double gimg_resample_kernel_support(GIMG_Resample_Filter filter) {
  switch (filter) {
    case GIMG_FILTER_BOX: return 0.5;
    case GIMG_FILTER_TRIANGLE: return 1.0;
    case GIMG_FILTER_CATMULL_ROM: return 2.0;
    case GIMG_FILTER_LANCZOS3: return 3.0;
    default: return 0.0;
  }
}

static double gimg_resample_sinc(double x) {
  if (x == 0.0) {
    return 1.0;
  }
  x *= GIMG_RESAMPLE_PI;
  return sin(x) / x;
}

static double gimg_resample_kernel(GIMG_Resample_Filter filter, double x) {
  switch (filter) {
    case GIMG_FILTER_BOX:
      // Half-open on one side so that a sample exactly on the boundary
      // between two output pixels is counted once rather than twice.
      return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0;
    case GIMG_FILTER_TRIANGLE: {
      double t = fabs(x);
      return (t < 1.0) ? (1.0 - t) : 0.0;
    }
    case GIMG_FILTER_CATMULL_ROM: {
      // The a = -0.5 member of the Mitchell-Netravali family, which is the
      // cubic that interpolates its samples rather than approximating them.
      const double a = -0.5;
      double t = fabs(x);
      if (t < 1.0) {
        return ((a + 2.0) * t - (a + 3.0)) * t * t + 1.0;
      }
      if (t < 2.0) {
        return (((t - 5.0) * t + 8.0) * t - 4.0) * a;
      }
      return 0.0;
    }
    case GIMG_FILTER_LANCZOS3:
      if (x >= -3.0 && x < 3.0) {
        return gimg_resample_sinc(x) * gimg_resample_sinc(x / 3.0);
      }
      return 0.0;
    default:
      return 0.0;
  }
}

static void gimg_resample_axis_free(
    const GIMG_Allocator * alloc, gimg_resample_axis * axis) {
  gimg_free(alloc, axis->weights);
  gimg_free(alloc, axis->first);
  gimg_free(alloc, axis->count);
  axis->weights = NULL;
  axis->first = NULL;
  axis->count = NULL;
}

/**
 * Work out, for each output pixel along one axis, which source pixels feed it
 * and with what weight.
 *
 * The centre of output pixel i sits at (i + 0.5) * scale in source
 * coordinates - the half-pixel is what makes a resize to the same size the
 * identity rather than a shift of half a pixel.  Weights are normalized to
 * sum to one before quantizing, so a kernel whose taps happen to fall
 * unevenly does not brighten or darken the picture: an unnormalized kernel is
 * the defect that a constant image failing to resize to the same constant
 * detects.
 */
static GIMG_Result gimg_resample_axis_build(const GIMG_Allocator * alloc,
    GIMG_Resample_Filter filter, uint32_t in_size, uint32_t out_size,
    gimg_resample_axis * out_axis) {
  memset(out_axis, 0, sizeof(*out_axis));

  const double scale = (double)in_size / (double)out_size;
  // Only a reduction stretches the kernel.  Enlarging leaves it alone: there
  // is no source detail to average away, only samples to interpolate between.
  const double filter_scale = (scale >= 1.0) ? scale : 1.0;
  const double support = gimg_resample_kernel_support(filter) * filter_scale;

  int32_t support_max = (int32_t)ceil(support) * 2 + 1;
  if (support_max < 1) {
    support_max = 1;
  }
  if ((uint32_t)support_max > in_size) {
    // Never plan for more taps than the axis has samples; the clamped bounds
    // below could not reach them anyway, and the allocation would be wasted.
    support_max = (int32_t)in_size;
  }

  size_t weight_count = 0u;
  if (!gcu_safe_mul_size((size_t)out_size, (size_t)support_max,
          &weight_count)) {
    return GIMG_ERR_LIMIT;
  }
  size_t weight_bytes = 0u;
  if (!gcu_safe_mul_size(weight_count, sizeof(int32_t), &weight_bytes)) {
    return GIMG_ERR_LIMIT;
  }

  out_axis->weights = (int32_t *)gimg_malloc(alloc, weight_bytes);
  out_axis->first = (int32_t *)gimg_malloc(
      alloc, (size_t)out_size * sizeof(int32_t));
  out_axis->count = (int32_t *)gimg_malloc(
      alloc, (size_t)out_size * sizeof(int32_t));
  double * scratch = (double *)gimg_malloc(
      alloc, (size_t)support_max * sizeof(double));
  if (!out_axis->weights || !out_axis->first || !out_axis->count || !scratch) {
    gimg_free(alloc, scratch);
    gimg_resample_axis_free(alloc, out_axis);
    return GIMG_ERR_OOM;
  }
  memset(out_axis->weights, 0, weight_bytes);
  out_axis->support_max = support_max;
  out_axis->out_count = out_size;

  const double inv_filter_scale = 1.0 / filter_scale;
  for (uint32_t i = 0; i < out_size; i++) {
    const double center = ((double)i + 0.5) * scale;
    int32_t xmin = (int32_t)(center - support + 0.5);
    if (xmin < 0) {
      xmin = 0;
    }
    int32_t xmax = (int32_t)(center + support + 0.5);
    if (xmax > (int32_t)in_size) {
      xmax = (int32_t)in_size;
    }
    int32_t taps = xmax - xmin;
    if (taps < 1) {
      // Can only happen through rounding at an extreme ratio; one tap at the
      // nearest sample is the sane reading, and leaving taps at zero would
      // write an uninitialized output pixel.
      xmin = (int32_t)(center);
      if (xmin >= (int32_t)in_size) {
        xmin = (int32_t)in_size - 1;
      }
      if (xmin < 0) {
        xmin = 0;
      }
      taps = 1;
    }
    if (taps > support_max) {
      taps = support_max;
    }

    double sum = 0.0;
    for (int32_t t = 0; t < taps; t++) {
      const double w = gimg_resample_kernel(
          filter, ((double)(xmin + t) - center + 0.5) * inv_filter_scale);
      scratch[t] = w;
      sum += w;
    }

    int32_t * row = out_axis->weights + ((size_t)i * (size_t)support_max);
    for (int32_t t = 0; t < taps; t++) {
      const double w = (sum != 0.0) ? (scratch[t] / sum) : 0.0;
      const double scaled = w * (double)(INT32_C(1)
          << GIMG_RESAMPLE_PRECISION_BITS);
      // Round away from zero, matching the reference implementation: rounding
      // negative lobes the other way biases the whole kernel.
      row[t] = (int32_t)(scaled >= 0.0 ? (scaled + 0.5) : (scaled - 0.5));
    }
    out_axis->first[i] = xmin;
    out_axis->count[i] = taps;
  }

  gimg_free(alloc, scratch);
  return GIMG_OK;
}

/** Read one sample.  Eight-bit samples are bytes; 12- and 16-bit are uint16. */
static inline uint32_t gimg_resample_get(
    const unsigned char * pixel, bool wide, uint8_t channel) {
  return wide ? (uint32_t)((const uint16_t *)pixel)[channel]
              : (uint32_t)pixel[channel];
}

static inline void gimg_resample_put(
    unsigned char * pixel, bool wide, uint8_t channel, uint32_t value) {
  if (wide) {
    ((uint16_t *)pixel)[channel] = (uint16_t)value;
  }
  else {
    pixel[channel] = (unsigned char)value;
  }
}

/** Divide rounding to nearest, for non-negative arguments. */
static inline uint32_t gimg_resample_div_round(uint32_t value, uint32_t by) {
  return (value + (by / 2u)) / by;
}

/**
 * Turn an accumulator into a sample.
 *
 * Clamping below before the shift keeps the arithmetic inside what ISO C
 * defines: a right shift of a negative value is implementation-defined, and
 * every negative accumulator clamps to zero anyway, so nothing is lost by
 * settling it first.  A cubic or a Lanczos window really does produce
 * negative sums at a hard edge - that is the ringing they are known for -
 * so this is a path taken in ordinary use, not a guard against the
 * impossible.
 */
static inline uint32_t gimg_resample_finish(int64_t acc, uint32_t max) {
  if (acc < 0) {
    acc = 0;
  }
  const int64_t v = acc >> GIMG_RESAMPLE_PRECISION_BITS;
  return (v > (int64_t)max) ? max : (uint32_t)v;
}

/**
 * Horizontal pass: src (in_w x h) to dst (out_w x h).
 *
 * When @p premultiply is set the samples are multiplied by their own alpha on
 * the way in.  Filtering straight alpha lets a transparent pixel contribute
 * its colour to opaque neighbours, so every edge against transparency picks up
 * a halo of whatever colour was hiding underneath it - usually black, because
 * that is what a cleared buffer holds.
 */
static void gimg_resample_horizontal(const unsigned char * src,
    size_t src_stride, unsigned char * dst, size_t dst_stride, uint32_t height,
    const gimg_resample_axis * axis, uint8_t channels, bool wide, uint32_t max,
    bool premultiply) {
  const size_t src_bpp = (size_t)channels * (wide ? 2u : 1u);
  const size_t dst_bpp = src_bpp;
  for (uint32_t y = 0; y < height; y++) {
    const unsigned char * src_row = src + ((size_t)y * src_stride);
    unsigned char * dst_row = dst + ((size_t)y * dst_stride);
    for (uint32_t x = 0; x < axis->out_count; x++) {
      const int32_t first = axis->first[x];
      const int32_t taps = axis->count[x];
      const int32_t * w = axis->weights + ((size_t)x * (size_t)axis->support_max);
      unsigned char * out_pixel = dst_row + ((size_t)x * dst_bpp);
      for (uint8_t c = 0; c < channels; c++) {
        int64_t acc = INT64_C(1) << (GIMG_RESAMPLE_PRECISION_BITS - 1);
        for (int32_t t = 0; t < taps; t++) {
          const unsigned char * in_pixel =
              src_row + ((size_t)(first + t) * src_bpp);
          uint32_t v = gimg_resample_get(in_pixel, wide, c);
          if (premultiply && c < 3u) {
            const uint32_t a = gimg_resample_get(in_pixel, wide, 3u);
            v = gimg_resample_div_round(v * a, max);
          }
          acc += (int64_t)v * (int64_t)w[t];
        }
        gimg_resample_put(out_pixel, wide, c, gimg_resample_finish(acc, max));
      }
    }
  }
}

/**
 * Vertical pass: src (w x in_h) to dst (w x out_h).
 *
 * When @p unpremultiply is set the samples are divided back out by their
 * alpha on the way out, undoing what the horizontal pass did.
 */
static void gimg_resample_vertical(const unsigned char * src,
    size_t src_stride, unsigned char * dst, size_t dst_stride, uint32_t width,
    const gimg_resample_axis * axis, uint8_t channels, bool wide, uint32_t max,
    bool unpremultiply) {
  const size_t bpp = (size_t)channels * (wide ? 2u : 1u);
  for (uint32_t y = 0; y < axis->out_count; y++) {
    const int32_t first = axis->first[y];
    const int32_t taps = axis->count[y];
    const int32_t * w = axis->weights + ((size_t)y * (size_t)axis->support_max);
    unsigned char * dst_row = dst + ((size_t)y * dst_stride);
    for (uint32_t x = 0; x < width; x++) {
      unsigned char * out_pixel = dst_row + ((size_t)x * bpp);
      uint32_t alpha = max;
      for (uint8_t c = 0; c < channels; c++) {
        int64_t acc = INT64_C(1) << (GIMG_RESAMPLE_PRECISION_BITS - 1);
        for (int32_t t = 0; t < taps; t++) {
          const unsigned char * in_pixel = src +
              ((size_t)(first + t) * src_stride) + ((size_t)x * bpp);
          acc += (int64_t)gimg_resample_get(in_pixel, wide, c) *
              (int64_t)w[t];
        }
        uint32_t v = gimg_resample_finish(acc, max);
        if (unpremultiply) {
          // The alpha channel is written last of the four, so it is computed
          // here and used to undo the colour channels below.  Holding it
          // instead of re-reading keeps this independent of channel order
          // beyond RGBA's own.
          if (c == 3u) {
            alpha = v;
          }
        }
        gimg_resample_put(out_pixel, wide, c, v);
      }
      if (unpremultiply) {
        for (uint8_t c = 0; c < 3u; c++) {
          uint32_t v = gimg_resample_get(out_pixel, wide, c);
          if (alpha == 0u) {
            // Nothing is visible here and no colour can be recovered: the
            // premultiplied value is zero whatever the original was.
            v = 0u;
          }
          else {
            v = gimg_resample_div_round(v * max, alpha);
            if (v > max) {
              v = max;
            }
          }
          gimg_resample_put(out_pixel, wide, c, v);
        }
      }
    }
  }
}

/** NEAREST, which never averages and so never uses a coefficient table. */
static void gimg_resample_nearest(const unsigned char * src, size_t src_stride,
    uint32_t src_w, uint32_t src_h, unsigned char * dst, size_t dst_stride,
    uint32_t dst_w, uint32_t dst_h, size_t bpp) {
  // The scale is formed once per axis and then multiplied, which is what the
  // filtered path does when it computes a tap centre.  Dividing after
  // multiplying instead is the same arithmetic and not the same double: at
  // 8 -> 7 it puts destination pixel 3 exactly on the boundary at 4.0 and
  // takes source pixel 4, where the filtered path lands a hair below and
  // takes 3.  NEAREST and BOX then disagreed about which source pixel a
  // destination pixel *is*, which is worse than either answer.
  const double x_scale = (double)src_w / (double)dst_w;
  const double y_scale = (double)src_h / (double)dst_h;
  for (uint32_t y = 0; y < dst_h; y++) {
    uint32_t sy = (uint32_t)(((double)y + 0.5) * y_scale);
    if (sy >= src_h) {
      sy = src_h - 1u;
    }
    const unsigned char * src_row = src + ((size_t)sy * src_stride);
    unsigned char * dst_row = dst + ((size_t)y * dst_stride);
    for (uint32_t x = 0; x < dst_w; x++) {
      uint32_t sx = (uint32_t)(((double)x + 0.5) * x_scale);
      if (sx >= src_w) {
        sx = src_w - 1u;
      }
      memcpy(dst_row + ((size_t)x * bpp), src_row + ((size_t)sx * bpp), bpp);
    }
  }
}


/**
 * Transfer encode/decode as a pair of lookup tables, via libs/color.
 *
 * The forward table maps a sample to a linear value on 0..65535; the reverse
 * maps a linear value back to a sample.  The reverse is built by walking the
 * forward one rather than by calling gcol_transfer_encode, so that
 * rev[fwd[v]] == v for every v: independent rounding of the inverse loses a
 * count here and there in the darks, where curves are steepest and where a
 * resize that changes nothing would then change something.
 */
typedef struct {
  uint16_t * forward; /**< max + 1 entries. */
  uint16_t * reverse; /**< 65536 entries. */
} gimg_transfer_tables;

/** The linear working range: sixteen bits, whatever the source width. */
#define GIMG_LINEAR_MAX 65535u

static void gimg_transfer_free(
    const GIMG_Allocator * alloc, gimg_transfer_tables * t) {
  gimg_free(alloc, t->forward);
  gimg_free(alloc, t->reverse);
  t->forward = NULL;
  t->reverse = NULL;
}

/**
 * True when @p info names a transfer color can decode and encode.
 *
 * UNKNOWN is not usable here: the caller substitutes sRGB for that case.
 * GAMMA needs a positive gamma_value; PARAMETRIC needs a positive g term.
 */
static bool gimg_transfer_info_usable(const GCOL_Color_Info * info) {
  if (!info) {
    return false;
  }
  switch (info->transfer) {
  case GCOL_TRANSFER_LINEAR:
  case GCOL_TRANSFER_SRGB:
  case GCOL_TRANSFER_BT1886:
  case GCOL_TRANSFER_PQ:
  case GCOL_TRANSFER_HLG:
    return true;
  case GCOL_TRANSFER_GAMMA:
    return info->gamma_value > 0.0 && isfinite(info->gamma_value);
  case GCOL_TRANSFER_PARAMETRIC:
    return info->transfer_params[0] > 0.0 &&
        isfinite(info->transfer_params[0]);
  default:
    return false;
  }
}

static GIMG_Result gimg_transfer_build(const GIMG_Allocator * alloc,
    uint32_t max, const GCOL_Color_Info * info, gimg_transfer_tables * out) {
  memset(out, 0, sizeof(*out));
  if (!gimg_transfer_info_usable(info)) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const size_t count = (size_t)max + 1u;
  out->forward = (uint16_t *)gimg_malloc(alloc, count * sizeof(uint16_t));
  out->reverse = (uint16_t *)gimg_malloc(
      alloc, ((size_t)GIMG_LINEAR_MAX + 1u) * sizeof(uint16_t));
  if (!out->forward || !out->reverse) {
    gimg_transfer_free(alloc, out);
    return GIMG_ERR_OOM;
  }
  for (size_t v = 0; v < count; v++) {
    const double c = (double)v / (double)max;
    const double linear = gcol_transfer_decode(info, c);
    double scaled = linear * (double)GIMG_LINEAR_MAX + 0.5;
    if (scaled < 0.0) {
      scaled = 0.0;
    }
    if (scaled > (double)GIMG_LINEAR_MAX) {
      scaled = (double)GIMG_LINEAR_MAX;
    }
    out->forward[v] = (uint16_t)scaled;
  }
  // Fill the reverse table by handing each linear value to whichever sample
  // it is nearest, walking the forward table once.  It is monotonic, so the
  // boundary between two samples is the midpoint between their linear values.
  size_t v = 0;
  for (size_t l = 0; l <= GIMG_LINEAR_MAX; l++) {
    while (v + 1u < count) {
      const uint32_t here = out->forward[v];
      const uint32_t next = out->forward[v + 1u];
      if ((uint32_t)l * 2u >= (uint32_t)here + next) {
        v++;
      }
      else {
        break;
      }
    }
    out->reverse[l] = (uint16_t)v;
  }
  return GIMG_OK;
}


/**
 * Run both passes, source bytes in and destination bytes out.
 *
 * The intermediate is at the same width as the destination and the same
 * height as the source, and holds samples at the width they were read at, so
 * the horizontal pass rounds before the vertical one reads.  Keeping it wider
 * would be more accurate and would stop the result being comparable, byte for
 * byte, with the resampler this arrangement follows.
 */
static GIMG_Result gimg_resample_two_pass(const GIMG_Allocator * alloc,
    const unsigned char * src, size_t src_stride, uint32_t src_w,
    uint32_t src_h, unsigned char * dst, size_t dst_stride, uint32_t dst_w,
    uint32_t dst_h, GIMG_Resample_Filter filter, uint8_t channels, bool wide,
    uint32_t max, bool premultiply) {
  const size_t bpp = (size_t)channels * (wide ? 2u : 1u);
  size_t mid_stride = 0u;
  if (!gcu_safe_mul_size((size_t)dst_w, bpp, &mid_stride)) {
    return GIMG_ERR_LIMIT;
  }
  size_t mid_bytes = 0u;
  if (!gcu_safe_mul_size(mid_stride, (size_t)src_h, &mid_bytes)) {
    return GIMG_ERR_LIMIT;
  }
  unsigned char * mid = (unsigned char *)gimg_malloc(alloc, mid_bytes);
  if (!mid) {
    return GIMG_ERR_OOM;
  }

  gimg_resample_axis horizontal;
  gimg_resample_axis vertical;
  GIMG_Result r =
      gimg_resample_axis_build(alloc, filter, src_w, dst_w, &horizontal);
  if (r != GIMG_OK) {
    gimg_free(alloc, mid);
    return r;
  }
  r = gimg_resample_axis_build(alloc, filter, src_h, dst_h, &vertical);
  if (r != GIMG_OK) {
    gimg_resample_axis_free(alloc, &horizontal);
    gimg_free(alloc, mid);
    return r;
  }

  gimg_resample_horizontal(src, src_stride, mid, mid_stride, src_h,
      &horizontal, channels, wide, max, premultiply);
  gimg_resample_vertical(mid, mid_stride, dst, dst_stride, dst_w, &vertical,
      channels, wide, max, premultiply);

  gimg_resample_axis_free(alloc, &horizontal);
  gimg_resample_axis_free(alloc, &vertical);
  gimg_free(alloc, mid);
  return GIMG_OK;
}

/**
 * Resample with the samples linearized first and re-encoded afterwards.
 *
 * Averaging transfer-encoded values averages the wrong quantity: an encoding
 * that is roughly a power curve makes the mean of two encoded values darker
 * than the encoding of their mean, and a reduction of a high-contrast picture
 * comes out visibly murkier than it should.
 *
 * The work is done at sixteen bits whatever the source width, because
 * linearizing an eight-bit sample and rounding it straight back to eight bits
 * throws away most of the dark end.
 *
 * The curve comes from @p transfer via libs/color. Unknown transfers are
 * resolved to sRGB by the caller before this is entered.
 *
 * **Alpha is not transferred.**  It is a coverage fraction, not a light
 * level, and there is nothing non-linear about it to undo; running it through
 * the curve would make every partial transparency wrong.
 */
static GIMG_Result gimg_resample_linear_light(const GIMG_Allocator * alloc,
    const GIMG_Raster * src, GIMG_Raster * dst, GIMG_Resample_Filter filter,
    uint8_t channels, uint8_t bits, uint32_t max, bool has_alpha,
    const GCOL_Color_Info * transfer) {
  (void)bits;
  gimg_transfer_tables tables;
  GIMG_Result r = gimg_transfer_build(alloc, max, transfer, &tables);
  if (r != GIMG_OK) {
    return r;
  }

  const uint32_t src_w = gimg_raster_width(src);
  const uint32_t src_h = gimg_raster_height(src);
  const uint32_t dst_w = gimg_raster_width(dst);
  const uint32_t dst_h = gimg_raster_height(dst);
  const size_t src_bpp = gimg_raster_bytes_per_pixel(gimg_raster_format(src));
  const bool src_wide = (max > 255u);
  const size_t lin_bpp = (size_t)channels * 2u;

  size_t lin_src_stride = 0u;
  size_t lin_src_bytes = 0u;
  size_t lin_dst_stride = 0u;
  size_t lin_dst_bytes = 0u;
  if (!gcu_safe_mul_size((size_t)src_w, lin_bpp, &lin_src_stride) ||
      !gcu_safe_mul_size(lin_src_stride, (size_t)src_h, &lin_src_bytes) ||
      !gcu_safe_mul_size((size_t)dst_w, lin_bpp, &lin_dst_stride) ||
      !gcu_safe_mul_size(lin_dst_stride, (size_t)dst_h, &lin_dst_bytes)) {
    gimg_transfer_free(alloc, &tables);
    return GIMG_ERR_LIMIT;
  }
  unsigned char * lin_src = (unsigned char *)gimg_malloc(alloc, lin_src_bytes);
  unsigned char * lin_dst = (unsigned char *)gimg_malloc(alloc, lin_dst_bytes);
  if (!lin_src || !lin_dst) {
    gimg_free(alloc, lin_src);
    gimg_free(alloc, lin_dst);
    gimg_transfer_free(alloc, &tables);
    return GIMG_ERR_OOM;
  }

  const unsigned char * sp = (const unsigned char *)gimg_raster_pixels_const(src);
  const size_t sp_stride = gimg_raster_stride_bytes(src);
  for (uint32_t y = 0; y < src_h; y++) {
    const unsigned char * srow = sp + ((size_t)y * sp_stride);
    uint16_t * lrow = (uint16_t *)(lin_src + ((size_t)y * lin_src_stride));
    for (uint32_t x = 0; x < src_w; x++) {
      const unsigned char * spx = srow + ((size_t)x * src_bpp);
      uint16_t * lpx = lrow + ((size_t)x * channels);
      for (uint8_t c = 0; c < channels; c++) {
        const uint32_t v = gimg_resample_get(spx, src_wide, c);
        if (has_alpha && c == 3u) {
          lpx[c] = (uint16_t)gimg_resample_div_round(v * GIMG_LINEAR_MAX, max);
        }
        else {
          lpx[c] = tables.forward[(v > max) ? max : v];
        }
      }
    }
  }

  r = gimg_resample_two_pass(alloc, lin_src, lin_src_stride, src_w, src_h,
      lin_dst, lin_dst_stride, dst_w, dst_h, filter, channels, true,
      GIMG_LINEAR_MAX, has_alpha);
  if (r != GIMG_OK) {
    gimg_free(alloc, lin_src);
    gimg_free(alloc, lin_dst);
    gimg_transfer_free(alloc, &tables);
    return r;
  }

  unsigned char * dp = (unsigned char *)gimg_raster_pixels(dst);
  const size_t dp_stride = gimg_raster_stride_bytes(dst);
  const size_t dst_bpp = gimg_raster_bytes_per_pixel(gimg_raster_format(dst));
  for (uint32_t y = 0; y < dst_h; y++) {
    const uint16_t * lrow =
        (const uint16_t *)(lin_dst + ((size_t)y * lin_dst_stride));
    unsigned char * drow = dp + ((size_t)y * dp_stride);
    for (uint32_t x = 0; x < dst_w; x++) {
      const uint16_t * lpx = lrow + ((size_t)x * channels);
      unsigned char * dpx = drow + ((size_t)x * dst_bpp);
      for (uint8_t c = 0; c < channels; c++) {
        uint32_t v;
        if (has_alpha && c == 3u) {
          v = gimg_resample_div_round((uint32_t)lpx[c] * max, GIMG_LINEAR_MAX);
        }
        else {
          v = tables.reverse[lpx[c]];
        }
        gimg_resample_put(dpx, src_wide, c, (v > max) ? max : v);
      }
    }
  }

  gimg_free(alloc, lin_src);
  gimg_free(alloc, lin_dst);
  gimg_transfer_free(alloc, &tables);
  return GIMG_OK;
}

GIMG_API void gimg_resize_options_default(GIMG_Resize_Options * options) {
  if (!options) {
    return;
  }
  // memset first: setting the fields one at a time leaves whatever a later
  // field is added holding the caller's stack garbage, which is a defect that
  // appears only once somebody extends the struct.
  memset(options, 0, sizeof(*options));
  options->filter = GIMG_FILTER_AUTO;
  options->space = GIMG_RESAMPLE_SPACE_ENCODED;
}

/**
 * Resolve AUTO to the filter it names.
 *
 * This is a pinned alias, not a judgement free to drift.  The library
 * promises the same bytes for the same input and options; an AUTO that
 * quietly changed kernel between versions would break that where only a
 * comparison against an old output would find it.
 */
static GIMG_Resample_Filter gimg_resample_resolve(GIMG_Resample_Filter f) {
  return (f == GIMG_FILTER_AUTO) ? GIMG_FILTER_CATMULL_ROM : f;
}

GIMG_API GIMG_Result gimg_ops_resize(const GIMG_Raster * src,
    uint32_t dst_width, uint32_t dst_height,
    const GIMG_Resize_Options * options, GIMG_Raster ** out_raster) {
  if (!src || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (dst_width == 0u || dst_height == 0u) {
    return GIMG_ERR_INTERNAL;
  }

  GIMG_Resize_Options defaults;
  if (!options) {
    gimg_resize_options_default(&defaults);
    options = &defaults;
  }
  if (options->filter < 0 || options->filter >= GIMG_FILTER_COUNT) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (options->space != GIMG_RESAMPLE_SPACE_ENCODED &&
      options->space != GIMG_RESAMPLE_SPACE_LINEAR) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const GIMG_Resample_Filter filter = gimg_resample_resolve(options->filter);

  const GIMG_Pixel_Format * fmt = gimg_raster_format(src);
  if (fmt->layout != GIMG_LAYOUT_INTERLEAVED) {
    // Nothing in this library produces a planar raster, so a planar path
    // could not be tested against anything.
    return GIMG_ERR_UNSUPPORTED;
  }
  if (fmt->channel_model != GIMG_CHANNEL_GRAY &&
      fmt->channel_model != GIMG_CHANNEL_RGBA &&
      fmt->channel_model != GIMG_CHANNEL_CMYK &&
      fmt->channel_model != GIMG_CHANNEL_UNKNOWN) {
    // INDEXED above all: the average of two palette indices is not a palette
    // index.  A caller resizes the colour form and calls gimg_ops_quantize()
    // afterwards, which keeps the decision about which colours to keep where
    // the other palette work already puts it.
    return GIMG_ERR_UNSUPPORTED;
  }
  const uint8_t channels = fmt->channel_count;
  if (channels == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const uint8_t bits = fmt->bits_per_channel[0];
  for (uint8_t i = 1; i < channels && i < 8u; i++) {
    if (fmt->bits_per_channel[i] != bits) {
      // A format whose channels are different widths has no single max value
      // to clamp against and no single reader; none exists in this library.
      return GIMG_ERR_UNSUPPORTED;
    }
  }
  if (bits != 8u && bits != 12u && bits != 16u) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const bool wide = (bits != 8u);
  const uint32_t max = (bits >= 32u) ? UINT32_MAX
                                     : ((UINT32_C(1) << bits) - 1u);
  const bool has_alpha =
      (fmt->channel_model == GIMG_CHANNEL_RGBA && channels == 4u);

  if (options->space == GIMG_RESAMPLE_SPACE_LINEAR &&
      fmt->channel_model != GIMG_CHANNEL_GRAY &&
      fmt->channel_model != GIMG_CHANNEL_RGBA) {
    // Transfer linearisation says something about light. CMYK samples are ink
    // amounts and GIMG_CHANNEL_UNKNOWN samples are whatever the file happened
    // to carry; running either through a display-referred curve would be
    // arithmetic with no meaning behind it.
    return GIMG_ERR_UNSUPPORTED;
  }

  // LINEAR uses libs/color to honour the raster's stated transfer. An
  // unstated one is still the caller's assertion of sRGB. Primaries are not
  // consulted: linearisation is per-channel and depends on the curve alone.
  GCOL_Color_Info srgb_fallback;
  const GCOL_Color_Info * transfer = NULL;
  if (options->space == GIMG_RESAMPLE_SPACE_LINEAR) {
    const GCOL_Color_Info * info = gimg_raster_color_info_const(src);
    if (!info || info->transfer == GCOL_TRANSFER_UNKNOWN) {
      gcol_color_info_default(&srgb_fallback);
      srgb_fallback.transfer = GCOL_TRANSFER_SRGB;
      transfer = &srgb_fallback;
    }
    else if (!gimg_transfer_info_usable(info)) {
      return GIMG_ERR_UNSUPPORTED;
    }
    else {
      transfer = info;
    }
  }

  const void * src_pixels = gimg_raster_pixels_const(src);
  if (!src_pixels) {
    return GIMG_ERR_INTERNAL;
  }
  const uint32_t src_w = gimg_raster_width(src);
  const uint32_t src_h = gimg_raster_height(src);
  const size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  if (bpp == 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Result r = gimg_raster_create_with_allocator(gimg_raster_allocator(src),
      dst_width, dst_height, fmt, GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    return r;
  }

  if (filter == GIMG_FILTER_NEAREST) {
    gimg_resample_nearest((const unsigned char *)src_pixels,
        gimg_raster_stride_bytes(src), src_w, src_h,
        (unsigned char *)gimg_raster_pixels(*out_raster),
        gimg_raster_stride_bytes(*out_raster), dst_width, dst_height, bpp);
    r = gimg_ops_carry_color(src, *out_raster);
    if (r != GIMG_OK) {
      gimg_raster_destroy(*out_raster);
      *out_raster = NULL;
      return r;
    }
    return GIMG_OK;
  }

  const GIMG_Allocator * alloc = gimg_raster_allocator(src);
  if (options->space == GIMG_RESAMPLE_SPACE_LINEAR) {
    r = gimg_resample_linear_light(alloc, src, *out_raster, filter, channels,
        bits, max, has_alpha, transfer);
  }
  else {
    r = gimg_resample_two_pass(alloc,
        (const unsigned char *)src_pixels, gimg_raster_stride_bytes(src),
        src_w, src_h, (unsigned char *)gimg_raster_pixels(*out_raster),
        gimg_raster_stride_bytes(*out_raster), dst_width, dst_height, filter,
        channels, wide, max, has_alpha);
  }
  if (r != GIMG_OK) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
    return r;
  }

  r = gimg_ops_carry_color(src, *out_raster);
  if (r != GIMG_OK) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
    return r;
  }
  return GIMG_OK;
}
