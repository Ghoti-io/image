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
 * VP8L inverse transforms. Logic matches libwebp 1.5.0 src/dsp/lossless.c.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "webp_internal.h"

#define ARGB_BLACK 0xff000000u

static uint32_t vp8l_subsample_size(uint32_t size, uint32_t sampling_bits) {
  return (size + (1u << sampling_bits) - 1u) >> sampling_bits;
}

static uint32_t vp8l_add_pixels(uint32_t a, uint32_t b) {
  const uint32_t alpha_and_green = (a & 0xff00ff00u) + (b & 0xff00ff00u);
  const uint32_t red_and_blue = (a & 0x00ff00ffu) + (b & 0x00ff00ffu);
  return (alpha_and_green & 0xff00ff00u) | (red_and_blue & 0x00ff00ffu);
}

static uint32_t average2(uint32_t a0, uint32_t a1) {
  return (((a0 ^ a1) & 0xfefefefeu) >> 1) + (a0 & a1);
}

static uint32_t average3(uint32_t a0, uint32_t a1, uint32_t a2) {
  return average2(average2(a0, a2), a1);
}

static uint32_t average4(uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
  return average2(average2(a0, a1), average2(a2, a3));
}

static uint32_t clip255(uint32_t a) {
  if (a < 256u) {
    return a;
  }
  return ~a >> 24;
}

static int add_subtract_component_full(int a, int b, int c) {
  return (int)clip255((uint32_t)(a + b - c));
}

static uint32_t clamped_add_subtract_full(uint32_t c0, uint32_t c1,
    uint32_t c2) {
  const int a = add_subtract_component_full(
      (int)(c0 >> 24), (int)(c1 >> 24), (int)(c2 >> 24));
  const int r = add_subtract_component_full(
      (int)((c0 >> 16) & 0xff), (int)((c1 >> 16) & 0xff),
      (int)((c2 >> 16) & 0xff));
  const int g = add_subtract_component_full(
      (int)((c0 >> 8) & 0xff), (int)((c1 >> 8) & 0xff),
      (int)((c2 >> 8) & 0xff));
  const int b = add_subtract_component_full(
      (int)(c0 & 0xff), (int)(c1 & 0xff), (int)(c2 & 0xff));
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
      (uint32_t)b;
}

static int add_subtract_component_half(int a, int b) {
  return (int)clip255((uint32_t)(a + (a - b) / 2));
}

static uint32_t clamped_add_subtract_half(uint32_t c0, uint32_t c1,
    uint32_t c2) {
  const uint32_t ave = average2(c0, c1);
  const int a = add_subtract_component_half((int)(ave >> 24), (int)(c2 >> 24));
  const int r = add_subtract_component_half(
      (int)((ave >> 16) & 0xff), (int)((c2 >> 16) & 0xff));
  const int g = add_subtract_component_half(
      (int)((ave >> 8) & 0xff), (int)((c2 >> 8) & 0xff));
  const int b = add_subtract_component_half(
      (int)(ave & 0xff), (int)(c2 & 0xff));
  return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) |
      (uint32_t)b;
}

static int sub3(int a, int b, int c) {
  const int pb = b - c;
  const int pa = a - c;
  const int apb = pb < 0 ? -pb : pb;
  const int apa = pa < 0 ? -pa : pa;
  return apb - apa;
}

static uint32_t select_pred(uint32_t a, uint32_t b, uint32_t c) {
  const int pa_minus_pb =
      sub3((int)(a >> 24), (int)(b >> 24), (int)(c >> 24)) +
      sub3((int)((a >> 16) & 0xff), (int)((b >> 16) & 0xff),
          (int)((c >> 16) & 0xff)) +
      sub3((int)((a >> 8) & 0xff), (int)((b >> 8) & 0xff),
          (int)((c >> 8) & 0xff)) +
      sub3((int)(a & 0xff), (int)(b & 0xff), (int)(c & 0xff));
  return (pa_minus_pb <= 0) ? a : b;
}

static uint32_t predictor0(const uint32_t * left, const uint32_t * top) {
  (void)left;
  (void)top;
  return ARGB_BLACK;
}

static uint32_t predictor1(const uint32_t * left, const uint32_t * top) {
  (void)top;
  return *left;
}

static uint32_t predictor2(const uint32_t * left, const uint32_t * top) {
  (void)left;
  return top[0];
}

static uint32_t predictor3(const uint32_t * left, const uint32_t * top) {
  (void)left;
  return top[1];
}

static uint32_t predictor4(const uint32_t * left, const uint32_t * top) {
  (void)left;
  return top[-1];
}

static uint32_t predictor5(const uint32_t * left, const uint32_t * top) {
  return average3(*left, top[0], top[1]);
}

static uint32_t predictor6(const uint32_t * left, const uint32_t * top) {
  return average2(*left, top[-1]);
}

static uint32_t predictor7(const uint32_t * left, const uint32_t * top) {
  return average2(*left, top[0]);
}

static uint32_t predictor8(const uint32_t * left, const uint32_t * top) {
  (void)left;
  return average2(top[-1], top[0]);
}

static uint32_t predictor9(const uint32_t * left, const uint32_t * top) {
  (void)left;
  return average2(top[0], top[1]);
}

static uint32_t predictor10(const uint32_t * left, const uint32_t * top) {
  return average4(*left, top[-1], top[0], top[1]);
}

static uint32_t predictor11(const uint32_t * left, const uint32_t * top) {
  return select_pred(top[0], *left, top[-1]);
}

static uint32_t predictor12(const uint32_t * left, const uint32_t * top) {
  return clamped_add_subtract_full(*left, top[0], top[-1]);
}

static uint32_t predictor13(const uint32_t * left, const uint32_t * top) {
  return clamped_add_subtract_half(*left, top[0], top[-1]);
}

typedef uint32_t (*pred_fn)(const uint32_t * left, const uint32_t * top);

static const pred_fn k_predictors[14] = {
  predictor0, predictor1, predictor2, predictor3, predictor4, predictor5,
  predictor6, predictor7, predictor8, predictor9, predictor10, predictor11,
  predictor12, predictor13
};

static void predictor_add(pred_fn pred, const uint32_t * in,
    const uint32_t * upper, int num_pixels, uint32_t * out) {
  for (int x = 0; x < num_pixels; ++x) {
    const uint32_t p = pred(&out[x - 1], upper + x);
    out[x] = vp8l_add_pixels(in[x], p);
  }
}

static void predictor_add0(const uint32_t * in, int num_pixels,
    uint32_t * out) {
  for (int x = 0; x < num_pixels; ++x) {
    out[x] = vp8l_add_pixels(in[x], ARGB_BLACK);
  }
}

static void predictor_add1(const uint32_t * in, int num_pixels,
    uint32_t * out) {
  uint32_t left = out[-1];
  for (int i = 0; i < num_pixels; ++i) {
    left = vp8l_add_pixels(in[i], left);
    out[i] = left;
  }
}

static void predictor_inverse(const gimg_webp_vp8l_xform_t * transform,
    int y_start, int y_end, const uint32_t * in, uint32_t * out) {
  const int width = transform->xsize;
  if (y_start == 0) {
    predictor_add0(in, 1, out);
    predictor_add1(in + 1, width - 1, out + 1);
    in += width;
    out += width;
    ++y_start;
  }

  {
    int y = y_start;
    const int tile_width = 1 << transform->bits;
    const int mask = tile_width - 1;
    const int tiles_per_row = (int)vp8l_subsample_size(
        (uint32_t)width, (uint32_t)transform->bits);
    const uint32_t * pred_mode_base =
        transform->data + (y >> transform->bits) * tiles_per_row;

    while (y < y_end) {
      const uint32_t * pred_mode_src = pred_mode_base;
      int x = 1;
      predictor_add(predictor2, in, out - width, 1, out);
      while (x < width) {
        const int mode = (int)((*pred_mode_src++ >> 8) & 0xfu);
        const pred_fn pred =
            (mode < 14) ? k_predictors[mode] : predictor0;
        int x_end = (x & ~mask) + tile_width;
        if (x_end > width) {
          x_end = width;
        }
        predictor_add(pred, in + x, out + x - width, x_end - x, out + x);
        x = x_end;
      }
      in += width;
      out += width;
      ++y;
      if ((y & mask) == 0) {
        pred_mode_base += tiles_per_row;
      }
    }
  }
}

static void add_green_to_blue_and_red(const uint32_t * src, int num_pixels,
    uint32_t * dst) {
  for (int i = 0; i < num_pixels; ++i) {
    const uint32_t argb = src[i];
    const uint32_t green = (argb >> 8) & 0xffu;
    uint32_t red_blue = argb & 0x00ff00ffu;
    red_blue += (green << 16) | green;
    red_blue &= 0x00ff00ffu;
    dst[i] = (argb & 0xff00ff00u) | red_blue;
  }
}

static int color_transform_delta(int8_t color_pred, int8_t color) {
  return ((int)color_pred * (int)color) >> 5;
}

static void transform_color_inverse(uint8_t green_to_red,
    uint8_t green_to_blue, uint8_t red_to_blue, const uint32_t * src,
    int num_pixels, uint32_t * dst) {
  for (int i = 0; i < num_pixels; ++i) {
    const uint32_t argb = src[i];
    const int8_t green = (int8_t)(argb >> 8);
    const uint32_t red = argb >> 16;
    int new_red = (int)(red & 0xffu);
    int new_blue = (int)(argb & 0xffu);
    new_red += color_transform_delta((int8_t)green_to_red, green);
    new_red &= 0xff;
    new_blue += color_transform_delta((int8_t)green_to_blue, green);
    new_blue +=
        color_transform_delta((int8_t)red_to_blue, (int8_t)new_red);
    new_blue &= 0xff;
    dst[i] = (argb & 0xff00ff00u) | ((uint32_t)new_red << 16) |
        (uint32_t)new_blue;
  }
}

static void color_space_inverse(const gimg_webp_vp8l_xform_t * transform,
    int y_start, int y_end, const uint32_t * src, uint32_t * dst) {
  const int width = transform->xsize;
  const int tile_width = 1 << transform->bits;
  const int mask = tile_width - 1;
  const int safe_width = width & ~mask;
  const int remaining_width = width - safe_width;
  const int tiles_per_row = (int)vp8l_subsample_size(
      (uint32_t)width, (uint32_t)transform->bits);
  int y = y_start;
  const uint32_t * pred_row =
      transform->data + (y >> transform->bits) * tiles_per_row;

  while (y < y_end) {
    const uint32_t * pred = pred_row;
    const uint32_t * src_safe_end = src + safe_width;
    const uint32_t * src_end = src + width;
    while (src < src_safe_end) {
      const uint32_t code = *pred++;
      transform_color_inverse((uint8_t)(code & 0xffu),
          (uint8_t)((code >> 8) & 0xffu), (uint8_t)((code >> 16) & 0xffu),
          src, tile_width, dst);
      src += tile_width;
      dst += tile_width;
    }
    if (src < src_end) {
      const uint32_t code = *pred++;
      transform_color_inverse((uint8_t)(code & 0xffu),
          (uint8_t)((code >> 8) & 0xffu), (uint8_t)((code >> 16) & 0xffu),
          src, remaining_width, dst);
      src += remaining_width;
      dst += remaining_width;
    }
    ++y;
    if ((y & mask) == 0) {
      pred_row += tiles_per_row;
    }
  }
}

static void color_index_inverse(const gimg_webp_vp8l_xform_t * transform,
    int y_start, int y_end, const uint32_t * src, uint32_t * dst) {
  const int bits_per_pixel = 8 >> transform->bits;
  const int width = transform->xsize;
  const uint32_t * const color_map = transform->data;

  if (bits_per_pixel < 8) {
    const int pixels_per_byte = 1 << transform->bits;
    const int count_mask = pixels_per_byte - 1;
    const uint32_t bit_mask = (1u << bits_per_pixel) - 1u;
    for (int y = y_start; y < y_end; ++y) {
      uint32_t packed_pixels = 0;
      for (int x = 0; x < width; ++x) {
        if ((x & count_mask) == 0) {
          packed_pixels = (*src++ >> 8) & 0xffu;
        }
        *dst++ = color_map[packed_pixels & bit_mask];
        packed_pixels >>= bits_per_pixel;
      }
    }
  }
  else {
    for (int y = y_start; y < y_end; ++y) {
      for (int x = 0; x < width; ++x) {
        *dst++ = color_map[(*src++ >> 8) & 0xffu];
      }
    }
  }
}

void gimg_webp_vp8l_inverse_xform(const gimg_webp_vp8l_xform_t * xform,
    int row_start, int row_end, const uint32_t * in, uint32_t * out) {
  const int width = xform->xsize;
  switch (xform->type) {
  case GIMG_WEBP_VP8L_SUBTRACT_GREEN:
    add_green_to_blue_and_red(in, (row_end - row_start) * width, out);
    break;
  case GIMG_WEBP_VP8L_PREDICTOR:
    predictor_inverse(xform, row_start, row_end, in, out);
    break;
  case GIMG_WEBP_VP8L_CROSS_COLOR:
    color_space_inverse(xform, row_start, row_end, in, out);
    break;
  case GIMG_WEBP_VP8L_COLOR_INDEXING:
    if (in == out && xform->bits > 0) {
      const int out_stride = (row_end - row_start) * width;
      const int in_stride = (row_end - row_start) *
          (int)vp8l_subsample_size(
              (uint32_t)xform->xsize, (uint32_t)xform->bits);
      uint32_t * const src = out + out_stride - in_stride;
      memmove(src, out, (size_t)in_stride * sizeof(*src));
      color_index_inverse(xform, row_start, row_end, src, out);
    }
    else {
      color_index_inverse(xform, row_start, row_end, in, out);
    }
    break;
  default:
    break;
  }
}
