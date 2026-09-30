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
 * VP8 keyframe decode, written from RFC 6386. A keyframe with no
 * segmentation, loop-filter level 0, and one coefficient partition is
 * reconstructed for every intra mode in sections 11 and 12. Other frames
 * return GIMG_ERR_UNSUPPORTED. Chroma is replicated onto each 2x2 luma
 * block (section 2). The boolean coder is section 7.3, the transforms
 * are sections 14.3 and 14.4, and the quantizer factors are section 14.1.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>
#include <stdint.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "webp_internal.h"
#include "webp_vp8_proba.inc"

int gimg_webp_peek_vp8_dims(const unsigned char * data, size_t size,
    uint32_t * out_w, uint32_t * out_h) {
  if (!data || !out_w || !out_h || size < 10u) {
    return 0;
  }
  /* Key frame, start-code 0x9d012a, then 14-bit width and height. */
  if ((data[0] & 1u) != 0u) {
    return 0;
  }
  if (data[3] != 0x9du || data[4] != 0x01u || data[5] != 0x2au) {
    return 0;
  }
  uint16_t raw_w =
      (uint16_t)((uint32_t)data[6] | ((uint32_t)data[7] << 8));
  uint16_t raw_h =
      (uint16_t)((uint32_t)data[8] | ((uint32_t)data[9] << 8));
  *out_w = (uint32_t)(raw_w & 0x3fffu);
  *out_h = (uint32_t)(raw_h & 0x3fffu);
  return (*out_w > 0u && *out_h > 0u) ? 1 : 0;
}

int gimg_webp_peek_vp8l_dims(const unsigned char * data, size_t size,
    uint32_t * out_w, uint32_t * out_h, int * out_alpha) {
  if (!data || !out_w || !out_h || size < 5u) {
    return 0;
  }
  if (data[0] != 0x2fu) {
    return 0;
  }
  uint32_t bits = (uint32_t)data[1] | ((uint32_t)data[2] << 8) |
      ((uint32_t)data[3] << 16) | ((uint32_t)data[4] << 24);
  *out_w = (bits & 0x3fffu) + 1u;
  *out_h = ((bits >> 14) & 0x3fffu) + 1u;
  if (out_alpha) {
    *out_alpha = (int)((bits >> 28) & 1u);
  }
  uint32_t version = (bits >> 29) & 7u;
  return (version == 0u && *out_w > 0u && *out_h > 0u) ? 1 : 0;
}

enum {
  VP8_DC_PRED = 0,
  VP8_V_PRED = 1,
  VP8_H_PRED = 2,
  VP8_TM_PRED = 3,
  VP8_B_PRED = 4,
  VP8_B_DC = 0,
  VP8_B_TM = 1,
  VP8_B_VE = 2,
  VP8_B_HE = 3,
  VP8_B_LD = 4,
  VP8_B_RD = 5,
  VP8_B_VR = 6,
  VP8_B_VL = 7,
  VP8_B_HD = 8,
  VP8_B_HU = 9,
  VP8_EOB = 11,
  VP8_MAX_DIM = 16383
};

/* Section 11.3: a 16x16 mode stands in as one subblock mode for context. */
static const uint8_t k_ymode_as_bmode[4] = { 0, 2, 3, 1 };

typedef struct {
  uint8_t y;
  uint8_t uv;
  uint8_t b[16];
} vp8_mb;

/* RFC 6386 section 8. Terminals are non-positive: 0 is DC_PRED. */
static const int8_t k_kf_ymode_tree[] = {
  -4, 2, 4, 6, 0, -1, -2, -3
};
static const uint8_t k_kf_ymode_prob[] = { 145, 156, 163, 128 };

static const int8_t k_uv_mode_tree[] = { 0, 2, -1, 4, -2, -3 };
static const uint8_t k_uv_mode_prob[] = { 142, 114, 183 };

/* Section 11.2. B_DC_PRED is 0, so its terminal is stored as 0. */
static const int8_t k_bmode_tree[] = {
  0, 2, -1, 4, -2, 6, 8, 12, -3, 10, -5, -6, -4, 14, -7, 16, -8, -9
};

static const int8_t k_coeff_tree[] = {
  -11, 2, 0, 4, -1, 6, 8, 12, -2, 10, -3, -4, 14, 16, -5, -6, 18, 20,
  -7, -8, -9, -10
};

/* Scan index to raster index. RFC 6386, the coefficient zigzag. */
static const uint8_t k_zigzag[16] = {
  0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

static const int k_cat_base[6] = { 5, 7, 11, 19, 35, 67 };

typedef struct {
  const uint8_t * p;
  const uint8_t * end;
  uint32_t range;
  uint32_t value;
  int bit_count;
  int error;
} vp8_bool;

typedef uint8_t vp8_probs[4][8][3][11];

static int vp8_bool_init(vp8_bool * d, const uint8_t * data, size_t size) {
  memset(d, 0, sizeof(*d));
  if (size < 2u) {
    d->error = 1;
    return 0;
  }
  d->value = ((uint32_t)data[0] << 8) | (uint32_t)data[1];
  d->p = data + 2;
  d->end = data + size;
  d->range = 255u;
  return 1;
}

/** Section 7.3. @a prob is the probability the bool is zero, in 1/256. */
static int vp8_bool_read(vp8_bool * d, int prob, int * bit) {
  uint32_t split;
  uint32_t split_shifted;
  if (d->error) {
    return 0;
  }
  split = 1u + (((d->range - 1u) * (uint32_t)prob) >> 8);
  split_shifted = split << 8;
  if (d->value >= split_shifted) {
    *bit = 1;
    d->range -= split;
    d->value -= split_shifted;
  }
  else {
    *bit = 0;
    d->range = split;
  }
  while (d->range < 128u) {
    d->value <<= 1;
    d->range <<= 1;
    d->bit_count++;
    if (d->bit_count == 8) {
      d->bit_count = 0;
      if (d->p >= d->end) {
        d->error = 1;
        return 0;
      }
      d->value |= (uint32_t)(*d->p++);
    }
  }
  return 1;
}

static int vp8_read_literal(vp8_bool * d, int nbits) {
  int value = 0;
  while (nbits-- > 0) {
    int bit;
    if (!vp8_bool_read(d, 128, &bit)) {
      return -1;
    }
    value = (value << 1) | bit;
  }
  return value;
}

static int vp8_tree_read(vp8_bool * d, const int8_t * tree,
    const uint8_t * prob, int node) {
  for (;;) {
    int bit;
    if (!vp8_bool_read(d, prob[node >> 1], &bit)) {
      return -1;
    }
    node = tree[node + bit];
    if (node <= 0) {
      return -node;
    }
  }
}

static int clamp255(int v) {
  if (v < 0) {
    return 0;
  }
  if (v > 255) {
    return 255;
  }
  return v;
}

static int clamp_q(int q) {
  if (q < 0) {
    return 0;
  }
  if (q > 127) {
    return 127;
  }
  return q;
}

/** Section 14.4. @a in and @a out are 16 coefficients in raster order. */
static void idct_4x4(const int16_t * in, int16_t * out) {
  int16_t tmp[16];
  int i;
  const int16_t * ip = in;
  int16_t * op = tmp;
  for (i = 0; i < 4; ++i) {
    int a1 = ip[0] + ip[8];
    int b1 = ip[0] - ip[8];
    int t1 = (ip[4] * 35468) >> 16;
    int t2 = ip[12] + ((ip[12] * 20091) >> 16);
    int c1 = t1 - t2;
    int d1;
    t1 = ip[4] + ((ip[4] * 20091) >> 16);
    t2 = (ip[12] * 35468) >> 16;
    d1 = t1 + t2;
    op[0] = (int16_t)(a1 + d1);
    op[12] = (int16_t)(a1 - d1);
    op[4] = (int16_t)(b1 + c1);
    op[8] = (int16_t)(b1 - c1);
    ip++;
    op++;
  }
  ip = tmp;
  op = out;
  for (i = 0; i < 4; ++i) {
    int a1 = ip[0] + ip[2];
    int b1 = ip[0] - ip[2];
    int t1 = (ip[1] * 35468) >> 16;
    int t2 = ip[3] + ((ip[3] * 20091) >> 16);
    int c1 = t1 - t2;
    int d1;
    t1 = ip[1] + ((ip[1] * 20091) >> 16);
    t2 = (ip[3] * 35468) >> 16;
    d1 = t1 + t2;
    op[0] = (int16_t)((a1 + d1 + 4) >> 3);
    op[3] = (int16_t)((a1 - d1 + 4) >> 3);
    op[1] = (int16_t)((b1 + c1 + 4) >> 3);
    op[2] = (int16_t)((b1 - c1 + 4) >> 3);
    ip += 4;
    op += 4;
  }
}

/** Section 14.3. */
static void iwht_4x4(const int16_t * in, int16_t * out) {
  int16_t tmp[16];
  int i;
  const int16_t * ip = in;
  int16_t * op = tmp;
  for (i = 0; i < 4; ++i) {
    int a1 = ip[0] + ip[12];
    int b1 = ip[4] + ip[8];
    int c1 = ip[4] - ip[8];
    int d1 = ip[0] - ip[12];
    op[0] = (int16_t)(a1 + b1);
    op[4] = (int16_t)(c1 + d1);
    op[8] = (int16_t)(a1 - b1);
    op[12] = (int16_t)(d1 - c1);
    ip++;
    op++;
  }
  ip = tmp;
  op = out;
  for (i = 0; i < 4; ++i) {
    int a1 = ip[0] + ip[3];
    int b1 = ip[1] + ip[2];
    int c1 = ip[1] - ip[2];
    int d1 = ip[0] - ip[3];
    int a2 = a1 + b1;
    int b2 = c1 + d1;
    int c2 = a1 - b1;
    int d2 = d1 - c1;
    op[0] = (int16_t)((a2 + 3) >> 3);
    op[1] = (int16_t)((b2 + 3) >> 3);
    op[2] = (int16_t)((c2 + 3) >> 3);
    op[3] = (int16_t)((d2 + 3) >> 3);
    ip += 4;
    op += 4;
  }
}

static int nz_at(const uint8_t * nz, int stride, int x, int y) {
  int ctx = 0;
  if (y > 0 && nz[(y - 1) * stride + x]) {
    ctx++;
  }
  if (x > 0 && nz[y * stride + (x - 1)]) {
    ctx++;
  }
  return ctx;
}

static int cat_extra(vp8_bool * d, const uint8_t * prob) {
  int value = 0;
  while (*prob) {
    int bit;
    if (!vp8_bool_read(d, *prob, &bit)) {
      return -1;
    }
    value = (value << 1) | bit;
    prob++;
  }
  return value;
}

/** Decode one coefficient block. @a had_nz is set when any token is nonzero. */
static int decode_coeffs(vp8_bool * d, uint8_t probs[8][3][11],
    int first, int ctx, int q_dc, int q_ac, int16_t * coeff, int * had_nz) {
  static const uint8_t * const cats[6] = {
    gimg_vp8_pcat1, gimg_vp8_pcat2, gimg_vp8_pcat3, gimg_vp8_pcat4,
    gimg_vp8_pcat5, gimg_vp8_pcat6
  };
  int i;
  int prev_zero = 0;
  memset(coeff, 0, 16u * sizeof(int16_t));
  *had_nz = 0;
  for (i = first; i < 16; ++i) {
    int start = prev_zero ? 2 : 0;
    int token;
    int mag;
    int sign;
    int q;
    const uint8_t * row = probs[gimg_vp8_bands[i]][ctx];
    token = vp8_tree_read(d, k_coeff_tree, row, start);
    if (token < 0) {
      return 0;
    }
    if (token == VP8_EOB) {
      return 1;
    }
    if (token <= 4) {
      mag = token;
    }
    else {
      int extra = cat_extra(d, cats[token - 5]);
      if (extra < 0) {
        return 0;
      }
      mag = k_cat_base[token - 5] + extra;
    }
    prev_zero = (mag == 0);
    if (mag == 0) {
      ctx = 0;
      continue;
    }
    if (!vp8_bool_read(d, 128, &sign)) {
      return 0;
    }
    q = (i == 0) ? q_dc : q_ac;
    coeff[k_zigzag[i]] = (int16_t)(sign ? -(mag * q) : (mag * q));
    *had_nz = 1;
    ctx = (mag == 1) ? 1 : 2;
  }
  return 1;
}

static void add_residue(uint8_t * plane, int stride, int x, int y,
    const int16_t * residue) {
  int row;
  int col;
  for (row = 0; row < 4; ++row) {
    for (col = 0; col < 4; ++col) {
      int v = (int)plane[(y + row) * stride + (x + col)] +
          residue[row * 4 + col];
      plane[(y + row) * stride + (x + col)] = (uint8_t)clamp255(v);
    }
  }
}

static void paint_b4(uint8_t * plane, int stride, int x, int y,
    const uint8_t * pred, const int16_t * residue) {
  int row;
  int col;
  for (row = 0; row < 4; ++row) {
    for (col = 0; col < 4; ++col) {
      int v = pred[row * 4 + col];
      if (residue) {
        v += residue[row * 4 + col];
      }
      plane[(y + row) * stride + (x + col)] = (uint8_t)clamp255(v);
    }
  }
}

static void fill_rect(uint8_t * plane, int stride, int x, int y, int n,
    int pred) {
  int row;
  int col;
  uint8_t value = (uint8_t)pred;
  for (row = 0; row < n; ++row) {
    for (col = 0; col < n; ++col) {
      plane[(y + row) * stride + (x + col)] = value;
    }
  }
}

/** Section 12. DC prediction. @a n is 16 for luma and 8 for chroma. */
static int dc_pred(const uint8_t * plane, int stride, int x, int y, int n) {
  int sum = 0;
  int i;
  int shift = (n == 16) ? 4 : 3;
  if (x == 0 && y == 0) {
    return 128;
  }
  if (y == 0) {
    for (i = 0; i < n; ++i) {
      sum += plane[(x - 1) + (y + i) * stride];
    }
    return (sum + (1 << (shift - 1))) >> shift;
  }
  if (x == 0) {
    for (i = 0; i < n; ++i) {
      sum += plane[(x + i) + (y - 1) * stride];
    }
    return (sum + (1 << (shift - 1))) >> shift;
  }
  for (i = 0; i < n; ++i) {
    sum += plane[(x + i) + (y - 1) * stride];
    sum += plane[(x - 1) + (y + i) * stride];
  }
  return (sum + (1 << shift)) >> (shift + 1);
}

static int avg2(int a, int b) {
  return (a + b + 1) >> 1;
}

static int avg3(int a, int b, int c) {
  return (a + b + b + c + 2) >> 2;
}

/**
 * Section 12. Pixels outside the frame are 127 above and 129 to the left,
 * including the corner above the left edge of a row that is not the top
 * row. The corner of the frame itself is 127.
 */
static void load_mb_edge(const uint8_t * plane, int stride, int x, int y,
    int n, uint8_t * above, uint8_t * left, int * p_out) {
  int i;
  if (y == 0) {
    for (i = 0; i < n; ++i) {
      above[i] = 127;
    }
    *p_out = 127;
  }
  else {
    for (i = 0; i < n; ++i) {
      above[i] = plane[(y - 1) * stride + x + i];
    }
    *p_out = (x == 0) ? 129 : plane[(y - 1) * stride + (x - 1)];
  }
  if (x == 0) {
    for (i = 0; i < n; ++i) {
      left[i] = 129;
    }
  }
  else {
    for (i = 0; i < n; ++i) {
      left[i] = plane[(y + i) * stride + (x - 1)];
    }
  }
}

/** Sections 12.2 and 12.3. DC uses dc_pred and omits out-of-frame samples. */
static void predict_mb(uint8_t * plane, int stride, int x, int y, int n,
    int mode) {
  uint8_t above[16];
  uint8_t left[16];
  int p;
  int r;
  int c;
  if (mode == VP8_DC_PRED) {
    fill_rect(plane, stride, x, y, n, dc_pred(plane, stride, x, y, n));
    return;
  }
  load_mb_edge(plane, stride, x, y, n, above, left, &p);
  if (mode == VP8_V_PRED) {
    for (r = 0; r < n; ++r) {
      memcpy(plane + (size_t)((y + r) * stride + x), above, (size_t)n);
    }
    return;
  }
  if (mode == VP8_H_PRED) {
    for (r = 0; r < n; ++r) {
      for (c = 0; c < n; ++c) {
        plane[(y + r) * stride + x + c] = left[r];
      }
    }
    return;
  }
  for (r = 0; r < n; ++r) {
    for (c = 0; c < n; ++c) {
      plane[(y + r) * stride + x + c] =
          (uint8_t)clamp255((int)left[r] + (int)above[c] - p);
    }
  }
}

/**
 * Section 12.3. The right-edge subblocks borrow the four samples above and
 * to the right of subblock 3. On the rightmost macroblock those four repeat
 * the sample at (-1, 15). On the top row they are 127.
 */
static void load_b_edge(const uint8_t * plane, int stride, int mb_w,
    int mb_x, int mb_y, int sx, int x, int y, uint8_t * above,
    uint8_t * left, int * p_out) {
  int i;
  int ox = mb_x * 16;
  int oy = mb_y * 16;
  if (y == 0) {
    for (i = 0; i < 8; ++i) {
      above[i] = 127;
    }
    *p_out = 127;
  }
  else {
    for (i = 0; i < 4; ++i) {
      above[i] = plane[(y - 1) * stride + x + i];
    }
    *p_out = (x == 0) ? 129 : plane[(y - 1) * stride + (x - 1)];
    if (sx < 3) {
      for (i = 0; i < 4; ++i) {
        above[4 + i] = plane[(y - 1) * stride + x + 4 + i];
      }
    }
    else if (mb_y == 0) {
      for (i = 0; i < 4; ++i) {
        above[4 + i] = 127;
      }
    }
    else if (mb_x + 1 >= mb_w) {
      uint8_t edge = plane[(oy - 1) * stride + ox + 15];
      for (i = 0; i < 4; ++i) {
        above[4 + i] = edge;
      }
    }
    else {
      for (i = 0; i < 4; ++i) {
        above[4 + i] = plane[(oy - 1) * stride + ox + 16 + i];
      }
    }
  }
  if (x == 0) {
    for (i = 0; i < 4; ++i) {
      left[i] = 129;
    }
  }
  else {
    for (i = 0; i < 4; ++i) {
      left[i] = plane[(y + i) * stride + (x - 1)];
    }
  }
}

static int b_dc(const uint8_t * above, const uint8_t * left) {
  int i;
  int sum = 4;
  /* Section 12.3 averages all eight samples. A missing edge is the 127 or
   * 129 fill from load_b_edge, which is what makes the top-left block 128. */
  for (i = 0; i < 4; ++i) {
    sum += above[i] + left[i];
  }
  return sum >> 3;
}

/**
 * Section 12.3. The listing writes svg2p in B_HD_PRED; that name is not
 * defined, and the same line's neighbours are avg2p, so this is avg2.
 */
static void pred_b4(uint8_t * dst, const uint8_t * above, const uint8_t * left,
    int p, int mode) {
  int a0 = above[0];
  int a1 = above[1];
  int a2 = above[2];
  int a3 = above[3];
  int a4 = above[4];
  int a5 = above[5];
  int a6 = above[6];
  int a7 = above[7];
  int l0 = left[0];
  int l1 = left[1];
  int l2 = left[2];
  int l3 = left[3];
  int e0 = l3;
  int e1 = l2;
  int e2 = l1;
  int e3 = l0;
  int e4 = p;
  int e5 = a0;
  int e6 = a1;
  int e7 = a2;
  int e8 = a3;
  int r;
  int c;
  int v;
  if (mode == VP8_B_DC) {
    v = b_dc(above, left);
    for (r = 0; r < 16; ++r) {
      dst[r] = (uint8_t)v;
    }
    return;
  }
  if (mode == VP8_B_TM) {
    for (r = 0; r < 4; ++r) {
      for (c = 0; c < 4; ++c) {
        dst[r * 4 + c] = (uint8_t)clamp255(left[r] + above[c] - p);
      }
    }
    return;
  }
  if (mode == VP8_B_VE) {
    int col[4];
    col[0] = avg3(p, a0, a1);
    col[1] = avg3(a0, a1, a2);
    col[2] = avg3(a1, a2, a3);
    col[3] = avg3(a2, a3, a4);
    for (r = 0; r < 4; ++r) {
      for (c = 0; c < 4; ++c) {
        dst[r * 4 + c] = (uint8_t)col[c];
      }
    }
    return;
  }
  if (mode == VP8_B_HE) {
    int rowv[4];
    rowv[3] = avg3(l2, l3, l3);
    rowv[2] = avg3(l1, l2, l3);
    rowv[1] = avg3(l0, l1, l2);
    rowv[0] = avg3(p, l0, l1);
    for (r = 0; r < 4; ++r) {
      for (c = 0; c < 4; ++c) {
        dst[r * 4 + c] = (uint8_t)rowv[r];
      }
    }
    return;
  }
  if (mode == VP8_B_LD) {
    dst[0] = (uint8_t)avg3(a0, a1, a2);
    dst[1] = dst[4] = (uint8_t)avg3(a1, a2, a3);
    dst[2] = dst[5] = dst[8] = (uint8_t)avg3(a2, a3, a4);
    dst[3] = dst[6] = dst[9] = dst[12] = (uint8_t)avg3(a3, a4, a5);
    dst[7] = dst[10] = dst[13] = (uint8_t)avg3(a4, a5, a6);
    dst[11] = dst[14] = (uint8_t)avg3(a5, a6, a7);
    dst[15] = (uint8_t)avg3(a6, a7, a7);
    return;
  }
  if (mode == VP8_B_RD) {
    dst[12] = (uint8_t)avg3(e0, e1, e2);
    dst[13] = dst[8] = (uint8_t)avg3(e1, e2, e3);
    dst[14] = dst[9] = dst[4] = (uint8_t)avg3(e2, e3, e4);
    dst[15] = dst[10] = dst[5] = dst[0] = (uint8_t)avg3(e3, e4, e5);
    dst[11] = dst[6] = dst[1] = (uint8_t)avg3(e4, e5, e6);
    dst[7] = dst[2] = (uint8_t)avg3(e5, e6, e7);
    dst[3] = (uint8_t)avg3(e6, e7, e8);
    return;
  }
  if (mode == VP8_B_VR) {
    dst[12] = (uint8_t)avg3(e1, e2, e3);
    dst[8] = (uint8_t)avg3(e2, e3, e4);
    dst[13] = dst[4] = (uint8_t)avg3(e3, e4, e5);
    dst[9] = dst[0] = (uint8_t)avg2(e4, e5);
    dst[14] = dst[5] = (uint8_t)avg3(e4, e5, e6);
    dst[10] = dst[1] = (uint8_t)avg2(e5, e6);
    dst[15] = dst[6] = (uint8_t)avg3(e5, e6, e7);
    dst[11] = dst[2] = (uint8_t)avg2(e6, e7);
    dst[7] = (uint8_t)avg3(e6, e7, e8);
    dst[3] = (uint8_t)avg2(e7, e8);
    return;
  }
  if (mode == VP8_B_VL) {
    dst[0] = (uint8_t)avg2(a0, a1);
    dst[4] = (uint8_t)avg3(a0, a1, a2);
    dst[8] = dst[1] = (uint8_t)avg2(a1, a2);
    dst[5] = dst[12] = (uint8_t)avg3(a1, a2, a3);
    dst[9] = dst[2] = (uint8_t)avg2(a2, a3);
    dst[13] = dst[6] = (uint8_t)avg3(a2, a3, a4);
    dst[10] = dst[3] = (uint8_t)avg2(a3, a4);
    dst[14] = dst[7] = (uint8_t)avg3(a3, a4, a5);
    dst[11] = (uint8_t)avg3(a4, a5, a6);
    dst[15] = (uint8_t)avg3(a5, a6, a7);
    return;
  }
  if (mode == VP8_B_HD) {
    dst[12] = (uint8_t)avg2(e0, e1);
    dst[13] = (uint8_t)avg3(e0, e1, e2);
    dst[8] = dst[14] = (uint8_t)avg2(e1, e2);
    dst[9] = dst[15] = (uint8_t)avg3(e1, e2, e3);
    dst[10] = dst[4] = (uint8_t)avg2(e2, e3);
    dst[11] = dst[5] = (uint8_t)avg3(e2, e3, e4);
    dst[6] = dst[0] = (uint8_t)avg2(e3, e4);
    dst[7] = dst[1] = (uint8_t)avg3(e3, e4, e5);
    dst[2] = (uint8_t)avg3(e4, e5, e6);
    dst[3] = (uint8_t)avg3(e5, e6, e7);
    return;
  }
  dst[0] = (uint8_t)avg2(l0, l1);
  dst[1] = (uint8_t)avg3(l0, l1, l2);
  dst[2] = dst[4] = (uint8_t)avg2(l1, l2);
  dst[3] = dst[5] = (uint8_t)avg3(l1, l2, l3);
  dst[6] = dst[8] = (uint8_t)avg2(l2, l3);
  dst[7] = dst[9] = (uint8_t)avg3(l2, l3, l3);
  dst[10] = dst[11] = dst[12] = dst[13] = dst[14] = dst[15] = left[3];
}

static int mulhi(int sample, int coeff) {
  return (sample * coeff) >> 8;
}

/** Descale by 6. A value outside 0..16383 clamps. */
static int clip_fix6(int v) {
  if ((v & ~16383) == 0) {
    return v >> 6;
  }
  return (v < 0) ? 0 : 255;
}

/**
 * Rec. ITU-R BT.601, 14-bit fixed point. RFC 6386 reconstructs YUV and
 * leaves the display conversion unspecified. The factors are 255/219 and
 * the recommendation's chroma gains, scaled by 2^14 (19077, 26149, 6419,
 * 13320, 33050). Each product keeps eight fraction bits, the offsets fold
 * in Y's bias of 16 and chroma's bias of 128, and the sum descales by 6.
 * One chroma sample covers each 2x2 luma block.
 */
static void yuv_to_rgba(int y, int u, int v, uint8_t * px) {
  int y_term = mulhi(y, 19077);
  px[0] = (uint8_t)clip_fix6(y_term + mulhi(v, 26149) - 14234);
  px[1] = (uint8_t)clip_fix6(
      y_term - mulhi(u, 6419) - mulhi(v, 13320) + 8708);
  px[2] = (uint8_t)clip_fix6(y_term + mulhi(u, 33050) - 17685);
  px[3] = 255;
}

static int read_delta_q(vp8_bool * d, int * delta) {
  int present;
  int mag;
  int sign;
  if (!vp8_bool_read(d, 128, &present)) {
    return 0;
  }
  if (!present) {
    *delta = 0;
    return 1;
  }
  mag = vp8_read_literal(d, 4);
  if (mag < 0 || !vp8_bool_read(d, 128, &sign)) {
    return 0;
  }
  *delta = sign ? -mag : mag;
  return 1;
}

GIMG_Result gimg_webp_vp8_decode(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, GIMG_Raster ** out_raster) {
  uint32_t tag;
  uint32_t part0_size;
  uint32_t width;
  uint32_t height;
  uint32_t mb_w;
  uint32_t mb_h;
  uint32_t y_stride;
  uint32_t uv_stride;
  int q_index;
  int y1_dc_delta = 0;
  int y2_dc_delta = 0;
  int y2_ac_delta = 0;
  int uv_dc_delta = 0;
  int uv_ac_delta = 0;
  int y1_dc;
  int y1_ac;
  int y2_dc;
  int y2_ac;
  int uv_dc;
  int uv_ac;
  int skip_enabled = 0;
  int skip_prob = 0;
  int t;
  int b;
  int c;
  int p;
  vp8_bool hdr;
  vp8_bool tokens;
  vp8_probs probs;
  uint8_t * y_plane = NULL;
  uint8_t * u_plane = NULL;
  uint8_t * v_plane = NULL;
  uint8_t * y_nz = NULL;
  uint8_t * u_nz = NULL;
  uint8_t * v_nz = NULL;
  uint8_t * y2_above = NULL;
  uint8_t * skip_mb = NULL;
  uint8_t * above_b = NULL;
  vp8_mb * modes = NULL;
  GIMG_Raster * raster = NULL;
  GIMG_Result r = GIMG_OK;
  const uint8_t * part0;
  const uint8_t * tok;
  size_t tok_size;
  uint32_t mb_y;
  uint32_t mb_x;

  if (!out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (!data || size < 10u) {
    return GIMG_ERR_CORRUPT;
  }
  /* Interframes are a later slice. Bit 0 set means not a keyframe. */
  if ((data[0] & 1u) != 0u) {
    return GIMG_ERR_UNSUPPORTED;
  }
  tag = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
      ((uint32_t)data[2] << 16);
  part0_size = tag >> 5;
  if (data[3] != 0x9du || data[4] != 0x01u || data[5] != 0x2au) {
    return GIMG_ERR_CORRUPT;
  }
  {
    uint32_t raw_w = (uint32_t)data[6] | ((uint32_t)data[7] << 8);
    uint32_t raw_h = (uint32_t)data[8] | ((uint32_t)data[9] << 8);
    /* Nonzero scale is the optional upscaler. Not in this slice. */
    if ((raw_w >> 14) != 0u || (raw_h >> 14) != 0u) {
      return GIMG_ERR_UNSUPPORTED;
    }
    width = raw_w & 0x3fffu;
    height = raw_h & 0x3fffu;
  }
  if (width == 0u || height == 0u || width > VP8_MAX_DIM ||
      height > VP8_MAX_DIM) {
    return GIMG_ERR_CORRUPT;
  }
  if ((size_t)10u + (size_t)part0_size > size || part0_size < 2u) {
    return GIMG_ERR_CORRUPT;
  }
  part0 = data + 10;
  tok = part0 + part0_size;
  tok_size = size - (10u + (size_t)part0_size);
  if (!vp8_bool_init(&hdr, part0, part0_size)) {
    return GIMG_ERR_CORRUPT;
  }

  /* Section 19.2, keyframe arm. */
  {
    int color;
    int clamp_bit;
    int seg;
    int filter_type;
    int filter_level;
    int sharp;
    int lf_adj;
    int parts;
    int refresh;
    if (!vp8_bool_read(&hdr, 128, &color) ||
        !vp8_bool_read(&hdr, 128, &clamp_bit)) {
      return GIMG_ERR_CORRUPT;
    }
    (void)clamp_bit;
    if (color != 0) {
      return GIMG_ERR_UNSUPPORTED;
    }
    if (!vp8_bool_read(&hdr, 128, &seg)) {
      return GIMG_ERR_CORRUPT;
    }
    if (seg) {
      return GIMG_ERR_UNSUPPORTED;
    }
    filter_type = vp8_read_literal(&hdr, 1);
    filter_level = vp8_read_literal(&hdr, 6);
    sharp = vp8_read_literal(&hdr, 3);
    if (filter_type < 0 || filter_level < 0 || sharp < 0 ||
        !vp8_bool_read(&hdr, 128, &lf_adj)) {
      return GIMG_ERR_CORRUPT;
    }
    (void)filter_type;
    (void)sharp;
    /* A nonzero filter changes the reconstructed pixels. */
    if (filter_level != 0 || lf_adj) {
      return GIMG_ERR_UNSUPPORTED;
    }
    parts = vp8_read_literal(&hdr, 2);
    if (parts < 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (parts != 0) {
      return GIMG_ERR_UNSUPPORTED;
    }
    q_index = vp8_read_literal(&hdr, 7);
    if (q_index < 0 || !read_delta_q(&hdr, &y1_dc_delta) ||
        !read_delta_q(&hdr, &y2_dc_delta) ||
        !read_delta_q(&hdr, &y2_ac_delta) ||
        !read_delta_q(&hdr, &uv_dc_delta) ||
        !read_delta_q(&hdr, &uv_ac_delta) ||
        !vp8_bool_read(&hdr, 128, &refresh)) {
      return GIMG_ERR_CORRUPT;
    }
    (void)refresh;
  }

  memcpy(probs, gimg_vp8_coeffs_proba0, sizeof(probs));
  for (t = 0; t < 4; ++t) {
    for (b = 0; b < 8; ++b) {
      for (c = 0; c < 3; ++c) {
        for (p = 0; p < 11; ++p) {
          int update;
          int value;
          if (!vp8_bool_read(&hdr, gimg_vp8_coeffs_update_proba[t][b][c][p],
                  &update)) {
            return GIMG_ERR_CORRUPT;
          }
          if (!update) {
            continue;
          }
          value = vp8_read_literal(&hdr, 8);
          if (value < 0) {
            return GIMG_ERR_CORRUPT;
          }
          probs[t][b][c][p] = (uint8_t)value;
        }
      }
    }
  }
  if (!vp8_bool_read(&hdr, 128, &skip_enabled)) {
    return GIMG_ERR_CORRUPT;
  }
  if (skip_enabled) {
    skip_prob = vp8_read_literal(&hdr, 8);
    if (skip_prob < 0) {
      return GIMG_ERR_CORRUPT;
    }
  }

  mb_w = (width + 15u) >> 4;
  mb_h = (height + 15u) >> 4;
  y_stride = mb_w * 16u;
  uv_stride = mb_w * 8u;
  if (skip_enabled) {
    skip_mb = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * mb_h);
    if (!skip_mb) {
      return GIMG_ERR_OOM;
    }
    memset(skip_mb, 0, (size_t)mb_w * mb_h);
  }
  y1_dc = gimg_vp8_dc_qlookup[clamp_q(q_index + y1_dc_delta)];
  y1_ac = (int)gimg_vp8_ac_qlookup[clamp_q(q_index)];
  y2_dc = gimg_vp8_dc_qlookup[clamp_q(q_index + y2_dc_delta)] * 2;
  y2_ac = (int)gimg_vp8_ac_qlookup[clamp_q(q_index + y2_ac_delta)] * 155 / 100;
  if (y2_ac < 8) {
    y2_ac = 8;
  }
  uv_dc = gimg_vp8_dc_qlookup[clamp_q(q_index + uv_dc_delta)];
  if (uv_dc > 132) {
    uv_dc = 132;
  }
  uv_ac = (int)gimg_vp8_ac_qlookup[clamp_q(q_index + uv_ac_delta)];

  modes = (vp8_mb *)gimg_malloc(
      alloc, (size_t)mb_w * (size_t)mb_h * sizeof(vp8_mb));
  above_b = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * 4u);
  if (!modes || !above_b) {
    gimg_free(alloc, modes);
    gimg_free(alloc, above_b);
    gimg_free(alloc, skip_mb);
    return GIMG_ERR_OOM;
  }
  memset(modes, 0, (size_t)mb_w * (size_t)mb_h * sizeof(vp8_mb));
  memset(above_b, 0, (size_t)mb_w * 4u);

  for (mb_y = 0; mb_y < mb_h && r == GIMG_OK; ++mb_y) {
    uint8_t left_b[4] = { 0, 0, 0, 0 };
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      vp8_mb * mb = &modes[mb_y * mb_w + mb_x];
      int ymode;
      int uvmode;
      int j;
      /* Section 11.1: the skip flag is in the first partition, before the
       * mode. It is absent when the frame did not enable it. */
      if (skip_enabled) {
        int skip_bit;
        if (!vp8_bool_read(&hdr, skip_prob, &skip_bit)) {
          r = GIMG_ERR_CORRUPT;
          break;
        }
        skip_mb[mb_y * mb_w + mb_x] = (uint8_t)skip_bit;
      }
      ymode = vp8_tree_read(&hdr, k_kf_ymode_tree, k_kf_ymode_prob, 0);
      if (ymode < 0) {
        r = GIMG_ERR_CORRUPT;
        break;
      }
      mb->y = (uint8_t)ymode;
      if (ymode == VP8_B_PRED) {
        for (j = 0; j < 16; ++j) {
          int row = j >> 2;
          int col = j & 3;
          int above_mode = (row == 0) ? above_b[mb_x * 4u + (uint32_t)col]
                                      : mb->b[(row - 1) * 4 + col];
          int left_mode = (col == 0) ? left_b[row] : mb->b[row * 4 + col - 1];
          int bmode = vp8_tree_read(&hdr, k_bmode_tree,
              gimg_vp8_kf_bmode_prob[above_mode][left_mode], 0);
          if (bmode < 0) {
            r = GIMG_ERR_CORRUPT;
            break;
          }
          mb->b[j] = (uint8_t)bmode;
        }
        if (r != GIMG_OK) {
          break;
        }
        for (j = 0; j < 4; ++j) {
          above_b[mb_x * 4u + (uint32_t)j] = mb->b[12 + j];
          left_b[j] = mb->b[j * 4 + 3];
        }
      }
      else {
        uint8_t mapped = k_ymode_as_bmode[ymode];
        for (j = 0; j < 4; ++j) {
          above_b[mb_x * 4u + (uint32_t)j] = mapped;
          left_b[j] = mapped;
        }
      }
      uvmode = vp8_tree_read(&hdr, k_uv_mode_tree, k_uv_mode_prob, 0);
      if (uvmode < 0) {
        r = GIMG_ERR_CORRUPT;
        break;
      }
      mb->uv = (uint8_t)uvmode;
    }
  }
  gimg_free(alloc, above_b);
  above_b = NULL;
  if (r != GIMG_OK) {
    gimg_free(alloc, modes);
    gimg_free(alloc, skip_mb);
    return r;
  }

  y_plane = (uint8_t *)gimg_malloc(alloc, (size_t)y_stride * mb_h * 16u);
  u_plane = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
  v_plane = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
  y_nz = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * 4u * mb_h * 4u);
  u_nz = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * 2u * mb_h * 2u);
  v_nz = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * 2u * mb_h * 2u);
  y2_above = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w);
  if (!y_plane || !u_plane || !v_plane || !y_nz || !u_nz || !v_nz ||
      !y2_above) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  memset(y_nz, 0, (size_t)mb_w * 4u * mb_h * 4u);
  memset(u_nz, 0, (size_t)mb_w * 2u * mb_h * 2u);
  memset(v_nz, 0, (size_t)mb_w * 2u * mb_h * 2u);
  memset(y2_above, 0, (size_t)mb_w);
  if (!vp8_bool_init(&tokens, tok, tok_size)) {
    r = GIMG_ERR_CORRUPT;
    goto Done;
  }

  for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    int y2_left = 0;
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      const vp8_mb * mb = &modes[mb_y * mb_w + mb_x];
      int skip = skip_mb ? skip_mb[mb_y * mb_w + mb_x] : 0;
      int sy;
      int sx;
      int had;
      const int y4_stride = (int)mb_w * 4;
      const int uv4_stride = (int)mb_w * 2;
      const int ox = (int)mb_x * 16;
      const int oy = (int)mb_y * 16;
      const int cx = (int)mb_x * 8;
      const int cy = (int)mb_y * 8;
      if (mb->y == VP8_B_PRED) {
        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            uint8_t pred[16];
            uint8_t edge_a[8];
            uint8_t edge_l[4];
            int edge_p;
            int px = ox + sx * 4;
            int py = oy + sy * 4;
            int fx = (int)mb_x * 4 + sx;
            int fy = (int)mb_y * 4 + sy;
            load_b_edge(y_plane, (int)y_stride, (int)mb_w, (int)mb_x,
                (int)mb_y, sx, px, py, edge_a, edge_l, &edge_p);
            pred_b4(pred, edge_a, edge_l, edge_p, mb->b[sy * 4 + sx]);
            if (skip) {
              paint_b4(y_plane, (int)y_stride, px, py, pred, NULL);
              continue;
            }
            {
              int16_t block[16];
              int16_t residue[16];
              if (!decode_coeffs(&tokens, probs[3], 0,
                      nz_at(y_nz, y4_stride, fx, fy), y1_dc, y1_ac, block,
                      &had)) {
                r = GIMG_ERR_CORRUPT;
                goto Done;
              }
              y_nz[fy * y4_stride + fx] = (uint8_t)had;
              idct_4x4(block, residue);
              paint_b4(y_plane, (int)y_stride, px, py, pred, residue);
            }
          }
        }
      }
      else {
        int16_t y2_coeff[16];
        int16_t y2_spatial[16];
        predict_mb(y_plane, (int)y_stride, ox, oy, 16, mb->y);
        if (skip) {
          y2_left = 0;
          y2_above[mb_x] = 0;
        }
        else {
          int ctx = (y2_left ? 1 : 0) + (y2_above[mb_x] ? 1 : 0);
          if (!decode_coeffs(&tokens, probs[1], 0, ctx, y2_dc, y2_ac,
                  y2_coeff, &had)) {
            r = GIMG_ERR_CORRUPT;
            goto Done;
          }
          y2_left = had;
          y2_above[mb_x] = (uint8_t)had;
          iwht_4x4(y2_coeff, y2_spatial);
          for (sy = 0; sy < 4; ++sy) {
            for (sx = 0; sx < 4; ++sx) {
              int16_t block[16];
              int16_t residue[16];
              int fx = (int)mb_x * 4 + sx;
              int fy = (int)mb_y * 4 + sy;
              if (!decode_coeffs(&tokens, probs[0], 1,
                      nz_at(y_nz, y4_stride, fx, fy), y1_dc, y1_ac, block,
                      &had)) {
                r = GIMG_ERR_CORRUPT;
                goto Done;
              }
              y_nz[fy * y4_stride + fx] = (uint8_t)had;
              block[0] = y2_spatial[sy * 4 + sx];
              idct_4x4(block, residue);
              add_residue(y_plane, (int)y_stride, ox + sx * 4, oy + sy * 4,
                  residue);
            }
          }
        }
      }
      predict_mb(u_plane, (int)uv_stride, cx, cy, 8, mb->uv);
      predict_mb(v_plane, (int)uv_stride, cx, cy, 8, mb->uv);
      if (skip) {
        continue;
      }
      for (p = 0; p < 2; ++p) {
        uint8_t * plane = (p == 0) ? u_plane : v_plane;
        uint8_t * nz = (p == 0) ? u_nz : v_nz;
        for (sy = 0; sy < 2; ++sy) {
          for (sx = 0; sx < 2; ++sx) {
            int16_t block[16];
            int16_t residue[16];
            int fx = (int)mb_x * 2 + sx;
            int fy = (int)mb_y * 2 + sy;
            if (!decode_coeffs(&tokens, probs[2], 0,
                    nz_at(nz, uv4_stride, fx, fy), uv_dc, uv_ac, block,
                    &had)) {
              r = GIMG_ERR_CORRUPT;
              goto Done;
            }
            nz[fy * uv4_stride + fx] = (uint8_t)had;
            idct_4x4(block, residue);
            add_residue(plane, (int)uv_stride, cx + sx * 4, cy + sy * 4,
                residue);
          }
        }
      }
    }
  }

  r = gimg_raster_create_with_allocator(alloc, width, height, &GIMG_PIXEL_RGBA8,
      GIMG_RASTER_OWNED, NULL, 0, &raster);
  if (r != GIMG_OK) {
    goto Done;
  }
  {
    uint8_t * dst = (uint8_t *)gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    uint32_t y;
    uint32_t x;
    for (y = 0; y < height; ++y) {
      for (x = 0; x < width; ++x) {
        int yy = y_plane[y * y_stride + x];
        int uu = u_plane[(y / 2u) * uv_stride + (x / 2u)];
        int vv = v_plane[(y / 2u) * uv_stride + (x / 2u)];
        yuv_to_rgba(yy, uu, vv, dst + y * stride + (size_t)x * 4u);
      }
    }
  }
  *out_raster = raster;
  raster = NULL;

Done:
  gimg_raster_destroy(raster);
  gimg_free(alloc, y_plane);
  gimg_free(alloc, u_plane);
  gimg_free(alloc, v_plane);
  gimg_free(alloc, y_nz);
  gimg_free(alloc, u_nz);
  gimg_free(alloc, v_nz);
  gimg_free(alloc, y2_above);
  gimg_free(alloc, skip_mb);
  gimg_free(alloc, above_b);
  gimg_free(alloc, modes);
  if (r != GIMG_OK) {
    *out_raster = NULL;
  }
  return r;
}
