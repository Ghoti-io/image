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
 * VP8 decode, written from RFC 6386. A keyframe is reconstructed for
 * every intra mode in sections 11 and 12. An interframe (sections 16
 * through 18) predicts from the last, golden, and altref pictures kept
 * across calls that share one sequence. Coefficient rows are packed
 * into the partitions of section 9.5. Segment adjustments (sections 9.3
 * and 10) select the quantizer and the loop-filter level, then section
 * 15 filters the frame. Chroma is upsampled with the 9-3-3-1 kernel
 * before the display conversion. The boolean coder is section 7.3, the
 * transforms are sections 14.3 and 14.4, and the quantizer factors are
 * section 14.1.
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
  VP8_MAX_DIM = 16383,
  /* Section 16.2. mv_nearest is num_ymodes so one value holds either. */
  VP8_MV_NEAREST = 5,
  VP8_MV_NEAR = 6,
  VP8_MV_ZERO = 7,
  VP8_MV_NEW = 8,
  VP8_MV_SPLIT = 9,
  VP8_LEFT4 = 10,
  VP8_ABOVE4 = 11,
  VP8_ZERO4 = 12,
  VP8_NEW4 = 13,
  VP8_PART_TOP = 0,
  VP8_PART_LEFT = 1,
  VP8_PART_QUARTERS = 2,
  VP8_PART_16 = 3,
  VP8_REF_INTRA = 0,
  VP8_REF_LAST = 1,
  VP8_REF_GOLDEN = 2,
  VP8_REF_ALT = 3
};

/* Section 11.3: a 16x16 mode stands in as one subblock mode for context. */
static const uint8_t k_ymode_as_bmode[4] = { 0, 2, 3, 1 };

typedef struct {
  uint8_t y;
  uint8_t uv;
  uint8_t nz; /* any nonzero coefficient in this macroblock */
  uint8_t seg;
  uint8_t ref; /* 0 intra, 1 last, 2 golden, 3 altref */
  uint8_t b[16];
  /* Eighth-pel luma vectors. The macroblock vector is subblock 15
   * when the mode is SPLITMV. */
  int mv_r;
  int mv_c;
  int sr[16];
  int sc[16];
} vp8_mb;

/* Section 10. Terminal 0 is segment 0, stored as 0 rather than -0. */
static const int8_t k_segment_tree[] = { 2, 4, 0, -1, -2, -3 };

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
} vp8_bool;

typedef uint8_t vp8_probs[4][8][3][11];

static int vp8_bool_init(vp8_bool * d, const uint8_t * data, size_t size) {
  memset(d, 0, sizeof(*d));
  d->end = data + size;
  d->range = 255u;
  /* Section 7.3 warms the register with two bytes. A partition shorter
   * than that supplies zeros for the bytes it does not have. */
  if (size >= 2u) {
    d->value = ((uint32_t)data[0] << 8) | (uint32_t)data[1];
    d->p = data + 2;
  }
  else if (size == 1u) {
    d->value = (uint32_t)data[0] << 8;
    d->p = data + 1;
  }
  else {
    d->p = data;
  }
  return 1;
}

/** Section 7.3. @a prob is the probability the bool is zero, in 1/256. */
static int vp8_bool_read(vp8_bool * d, int prob, int * bit) {
  uint32_t split;
  uint32_t split_shifted;
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
      /* Past the partition the missing byte is zero. The encoder's
       * stop sequence lives in the bytes that were written. */
      if (d->p < d->end) {
        d->value |= (uint32_t)(*d->p++);
      }
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
 */

/** Repeat the edge sample. The chroma plane is addressed in samples. */
static int chroma_at(const uint8_t * plane, int stride, int w, int h, int x,
    int y) {
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x >= w) {
    x = w - 1;
  }
  if (y >= h) {
    y = h - 1;
  }
  return plane[(size_t)y * (size_t)stride + (size_t)x];
}

/**
 * 4:2:0 fancy upsample. Each axis uses weights 3 and 1, so the four
 * samples are 9, 3, 3, and 1. Adding 8 rounds the divide by 16. An even
 * luma coordinate looks toward the previous chroma sample and an odd one
 * toward the next. RFC 6386 does not specify this display step.
 */
static int fancy_chroma(const uint8_t * plane, int stride, int w, int h,
    int x, int y) {
  int cx = x >> 1;
  int cy = y >> 1;
  int dx = (x & 1) ? 1 : -1;
  int dy = (y & 1) ? 1 : -1;
  int a = chroma_at(plane, stride, w, h, cx, cy);
  int b = chroma_at(plane, stride, w, h, cx + dx, cy);
  int c = chroma_at(plane, stride, w, h, cx, cy + dy);
  int d = chroma_at(plane, stride, w, h, cx + dx, cy + dy);
  return (9 * a + 3 * b + 3 * c + d + 8) >> 4;
}
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

/* Sections 9.3 and 9.4. A clear update flag leaves the previous value,
 * which is zero on a keyframe. The sign bit is 1 when negative. */
static int read_signed(vp8_bool * d, int nbits, int * delta) {
  int present;
  int mag;
  int sign;
  if (!vp8_bool_read(d, 128, &present)) {
    return 0;
  }
  if (!present) {
    return 1;
  }
  mag = vp8_read_literal(d, nbits);
  if (mag < 0 || !vp8_bool_read(d, 128, &sign)) {
    return 0;
  }
  *delta = sign ? -mag : mag;
  return 1;
}

/* Section 15.2. Clamp to a signed byte. */
static int vp8_c(int v) {
  if (v < -128) {
    return -128;
  }
  if (v > 127) {
    return 127;
  }
  return v;
}

/* The shifts in section 15 propagate the sign. C leaves that to the
 * implementation, so do it explicitly. */
static int vp8_shr(int v, int bits) {
  if (v >= 0) {
    return v >> bits;
  }
  return ~((~v) >> bits);
}

static int vp8_u2s(int pixel) {
  return pixel - 128;
}

static uint8_t vp8_s2u(int v) {
  return (uint8_t)(vp8_c(v) + 128);
}

static int vp8_abs(int v) {
  return (v < 0) ? -v : v;
}

/* Section 15.2. Returns the adjustment applied to the edge pixels. */
static int vp8_common_adjust(int use_outer, uint8_t * p1, uint8_t * p0,
    uint8_t * q0, uint8_t * q1) {
  int sp1 = vp8_u2s(*p1);
  int sp0 = vp8_u2s(*p0);
  int sq0 = vp8_u2s(*q0);
  int sq1 = vp8_u2s(*q1);
  int outer = use_outer ? vp8_c(sp1 - sq1) : 0;
  int a = vp8_c(outer + 3 * (sq0 - sp0));
  int b = vp8_shr(vp8_c(a + 3), 3);
  a = vp8_shr(vp8_c(a + 4), 3);
  *q0 = vp8_s2u(sq0 - a);
  *p0 = vp8_s2u(sp0 + b);
  return a;
}

/* Section 15.3. The signed conversion cancels in every difference. */
static int vp8_filter_yes(int interior, int edge, int p3, int p2, int p1,
    int p0, int q0, int q1, int q2, int q3) {
  if ((vp8_abs(p0 - q0) * 2 + vp8_abs(p1 - q1) / 2) > edge) {
    return 0;
  }
  if (vp8_abs(p3 - p2) > interior || vp8_abs(p2 - p1) > interior ||
      vp8_abs(p1 - p0) > interior) {
    return 0;
  }
  if (vp8_abs(q3 - q2) > interior || vp8_abs(q2 - q1) > interior ||
      vp8_abs(q1 - q0) > interior) {
    return 0;
  }
  return 1;
}

static int vp8_hev(int threshold, int p1, int p0, int q0, int q1) {
  return vp8_abs(p1 - p0) > threshold || vp8_abs(q1 - q0) > threshold;
}

/* One sample of a vertical edge (across = 1) or a horizontal edge
 * (across = the row stride). @a q0 is the first sample after the edge. */
static void vp8_simple_sample(uint8_t * q0, int across, int limit) {
  uint8_t * p0 = q0 - across;
  uint8_t * p1 = p0 - across;
  uint8_t * q1 = q0 + across;
  if ((vp8_abs(*p0 - *q0) * 2 + vp8_abs(*p1 - *q1) / 2) <= limit) {
    vp8_common_adjust(1, p1, p0, q0, q1);
  }
}

static void vp8_sub_sample(uint8_t * q0, int across, int hev_th,
    int interior, int edge) {
  uint8_t * p0 = q0 - across;
  uint8_t * p1 = p0 - across;
  uint8_t * p2 = p1 - across;
  uint8_t * p3 = p2 - across;
  uint8_t * q1 = q0 + across;
  uint8_t * q2 = q1 + across;
  uint8_t * q3 = q2 + across;
  int sp1 = vp8_u2s(*p1);
  int sp0 = vp8_u2s(*p0);
  int sq0 = vp8_u2s(*q0);
  int sq1 = vp8_u2s(*q1);
  int hv;
  int a;
  if (!vp8_filter_yes(interior, edge, *p3, *p2, *p1, *p0, *q0, *q1, *q2,
          *q3)) {
    return;
  }
  hv = vp8_hev(hev_th, sp1, sp0, sq0, sq1);
  a = vp8_shr(vp8_common_adjust(hv, p1, p0, q0, q1) + 1, 1);
  if (!hv) {
    *q1 = vp8_s2u(sq1 - a);
    *p1 = vp8_s2u(sp1 + a);
  }
}

static void vp8_mb_sample(uint8_t * q0, int across, int hev_th,
    int interior, int edge) {
  uint8_t * p0 = q0 - across;
  uint8_t * p1 = p0 - across;
  uint8_t * p2 = p1 - across;
  uint8_t * p3 = p2 - across;
  uint8_t * q1 = q0 + across;
  uint8_t * q2 = q1 + across;
  uint8_t * q3 = q2 + across;
  int sp2 = vp8_u2s(*p2);
  int sp1 = vp8_u2s(*p1);
  int sp0 = vp8_u2s(*p0);
  int sq0 = vp8_u2s(*q0);
  int sq1 = vp8_u2s(*q1);
  int sq2 = vp8_u2s(*q2);
  int w;
  int a;
  if (!vp8_filter_yes(interior, edge, *p3, *p2, *p1, *p0, *q0, *q1, *q2,
          *q3)) {
    return;
  }
  if (vp8_hev(hev_th, sp1, sp0, sq0, sq1)) {
    vp8_common_adjust(1, p1, p0, q0, q1);
    return;
  }
  w = vp8_c(vp8_c(sp1 - sq1) + 3 * (sq0 - sp0));
  a = vp8_c(vp8_shr(27 * w + 63, 7));
  *q0 = vp8_s2u(sq0 - a);
  *p0 = vp8_s2u(sp0 + a);
  a = vp8_c(vp8_shr(18 * w + 63, 7));
  *q1 = vp8_s2u(sq1 - a);
  *p1 = vp8_s2u(sp1 + a);
  a = vp8_c(vp8_shr(9 * w + 63, 7));
  *q2 = vp8_s2u(sq2 - a);
  *p2 = vp8_s2u(sp2 + a);
}

static void vp8_edge(uint8_t * q0, int along, int across, int count,
    int simple, int mb_edge, int hev_th, int interior, int limit) {
  int i;
  for (i = 0; i < count; ++i) {
    if (simple) {
      vp8_simple_sample(q0, across, limit);
    }
    else if (mb_edge) {
      vp8_mb_sample(q0, across, hev_th, interior, limit);
    }
    else {
      vp8_sub_sample(q0, across, hev_th, interior, limit);
    }
    q0 += along;
  }
}

/* Section 15.1. Each macroblock filters the edges above it and to its
 * left, then its interior. Prediction has already used the unfiltered
 * samples. A level of 0 skips the macroblock. The simple filter
 * (filter_type 1) leaves chroma alone. Interior edges are skipped when
 * the mode is not B_PRED or SPLITMV and no coefficient was nonzero. */
static void vp8_loop_filter(uint8_t * y, int y_stride, uint8_t * u,
    uint8_t * v, int uv_stride, const vp8_mb * modes, uint32_t mb_w,
    uint32_t mb_h, int simple, int base, int sharpness, int lf_adj,
    const int ref_lf[4], const int mode_lf[4], int seg_on, int seg_abs,
    const int seg_lf[4]) {
  uint32_t mb_y;
  uint32_t mb_x;
  for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      const vp8_mb * mb = &modes[mb_y * mb_w + mb_x];
      int level = base;
      int interior;
      int hev_th;
      int mb_limit;
      int sub_limit;
      int inner;
      int ox = (int)mb_x * 16;
      int oy = (int)mb_y * 16;
      int cx = (int)mb_x * 8;
      int cy = (int)mb_y * 8;
      uint8_t * yb = y + oy * y_stride + ox;
      uint8_t * ub = u + cy * uv_stride + cx;
      uint8_t * vb = v + cy * uv_stride + cx;
      int k;
      /* Section 10. Absolute mode replaces the frame level. Delta mode
       * adds to it. The section 19 note (0 = delta, 1 = absolute) is
       * the one that matches decoded frames. Clamp, then apply the
       * section 9.4 mode and reference deltas. */
      if (seg_on) {
        if (seg_abs) {
          level = seg_lf[mb->seg];
        }
        else {
          level += seg_lf[mb->seg];
        }
      }
      if (level > 63) {
        level = 63;
      }
      if (level < 0) {
        level = 0;
      }
      /* Mode and reference deltas do not raise a frame-header level of
       * 0. Segmentation still can. */
      if (lf_adj && base != 0) {
        level += ref_lf[mb->ref];
        /* Four mode deltas: B_PRED, zero motion, the other whole
         * macroblock vectors, and a split. Intra DC, V, H, and TM
         * take none. A keyframe macroblock is reference 0. */
        if (mb->ref == 0) {
          if (mb->y == VP8_B_PRED) {
            level += mode_lf[0];
          }
        }
        else if (mb->y == VP8_MV_ZERO) {
          level += mode_lf[1];
        }
        else if (mb->y == VP8_MV_SPLIT) {
          level += mode_lf[3];
        }
        else {
          level += mode_lf[2];
        }
      }
      if (level > 63) {
        level = 63;
      }
      if (level < 0) {
        level = 0;
      }
      if (level == 0) {
        continue;
      }
      interior = level;
      if (sharpness) {
        interior >>= (sharpness > 4) ? 2 : 1;
        if (interior > 9 - sharpness) {
          interior = 9 - sharpness;
        }
      }
      if (!interior) {
        interior = 1;
      }
      hev_th = 0;
      if (level >= 40) {
        hev_th = 2;
      }
      else if (level >= 15) {
        hev_th = 1;
      }
      mb_limit = ((level + 2) * 2) + interior;
      sub_limit = (level * 2) + interior;
      inner = mb->nz || mb->y == VP8_B_PRED || mb->y == VP8_MV_SPLIT;
      if (mb_x) {
        vp8_edge(yb, y_stride, 1, 16, simple, 1, hev_th, interior,
            mb_limit);
        if (!simple) {
          vp8_edge(ub, uv_stride, 1, 8, 0, 1, hev_th, interior, mb_limit);
          vp8_edge(vb, uv_stride, 1, 8, 0, 1, hev_th, interior, mb_limit);
        }
      }
      if (inner) {
        for (k = 4; k <= 12; k += 4) {
          vp8_edge(yb + k, y_stride, 1, 16, simple, 0, hev_th, interior,
              sub_limit);
        }
        if (!simple) {
          vp8_edge(ub + 4, uv_stride, 1, 8, 0, 0, hev_th, interior,
              sub_limit);
          vp8_edge(vb + 4, uv_stride, 1, 8, 0, 0, hev_th, interior,
              sub_limit);
        }
      }
      if (mb_y) {
        vp8_edge(yb, 1, y_stride, 16, simple, 1, hev_th, interior,
            mb_limit);
        if (!simple) {
          vp8_edge(ub, 1, uv_stride, 8, 0, 1, hev_th, interior, mb_limit);
          vp8_edge(vb, 1, uv_stride, 8, 0, 1, hev_th, interior, mb_limit);
        }
      }
      if (inner) {
        for (k = 4; k <= 12; k += 4) {
          vp8_edge(yb + k * y_stride, 1, y_stride, 16, simple, 0, hev_th,
              interior, sub_limit);
        }
        if (!simple) {
          vp8_edge(ub + 4 * uv_stride, 1, uv_stride, 8, 0, 0, hev_th,
              interior, sub_limit);
          vp8_edge(vb + 4 * uv_stride, 1, uv_stride, 8, 0, 0, hev_th,
              interior, sub_limit);
        }
      }
    }
  }
}

/* Section 16.1. Intra modes inside an interframe. DC_PRED is terminal 0. */
static const int8_t k_if_ymode_tree[] = {
  0, 2, 4, 6, -VP8_V_PRED, -VP8_H_PRED, -VP8_TM_PRED, -VP8_B_PRED
};
static const uint8_t k_if_bmode_prob[9] = {
  120, 90, 79, 133, 87, 85, 80, 111, 151
};

/* Section 16.2. zero = "0", nearest = "10", near = "110", new = "1110". */
static const int8_t k_mv_ref_tree[] = {
  -VP8_MV_ZERO, 2, -VP8_MV_NEAREST, 4, -VP8_MV_NEAR, 6, -VP8_MV_NEW,
  -VP8_MV_SPLIT
};

static const int k_mv_mode_ctx[6][4] = {
  { 7, 1, 1, 143 },
  { 14, 18, 14, 107 },
  { 135, 64, 57, 68 },
  { 60, 56, 128, 65 },
  { 159, 134, 128, 34 },
  { 234, 188, 128, 28 }
};

/* Section 16.4. MV_16 = "0", quarters = "10", top = "110", left = "111".
 * top_bottom is terminal 0. */
static const int8_t k_mv_part_tree[] = {
  -VP8_PART_16, 2, -VP8_PART_QUARTERS, 4, 0, -VP8_PART_LEFT
};
static const uint8_t k_mv_part_prob[3] = { 110, 111, 150 };

static const int8_t k_sub_mv_tree[] = {
  -VP8_LEFT4, 2, -VP8_ABOVE4, 4, -VP8_ZERO4, -VP8_NEW4
};
static const uint8_t k_sub_mv_prob[5][3] = {
  { 147, 136, 18 },
  { 106, 145, 1 },
  { 179, 121, 1 },
  { 223, 1, 34 },
  { 208, 1, 1 }
};

static const uint8_t k_part_blocks[4][16] = {
  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 },
  { 0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15 },
  { 0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15 },
  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 }
};
static const int k_part_count[4] = { 2, 2, 4, 16 };
static const int k_part_len[4] = { 8, 8, 4, 1 };

/* Section 17.1. 0 = "000" is stored as terminal 0. */
static const int8_t k_small_mvtree[] = {
  2, 8, 4, 6, 0, -1, -2, -3, 10, 12, -4, -5, -6, -7
};

static const uint8_t k_mv_update_prob[2][19] = {
  {
    237, 246, 253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
    250, 250, 252, 254, 254
  },
  {
    231, 243, 245, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
    251, 251, 254, 254, 254
  }
};

static const uint8_t k_default_mv[2][19] = {
  {
    162, 128, 225, 146, 172, 147, 214, 39, 156, 128, 129, 132, 75, 145,
    178, 206, 239, 254, 254
  },
  {
    164, 128, 204, 170, 119, 235, 140, 230, 228, 128, 130, 130, 74, 148,
    180, 203, 236, 254, 254
  }
};

/* Section 18.3. Taps sum to 128. */
static const int k_bilinear[8][6] = {
  { 0, 0, 128, 0, 0, 0 },
  { 0, 0, 112, 16, 0, 0 },
  { 0, 0, 96, 32, 0, 0 },
  { 0, 0, 80, 48, 0, 0 },
  { 0, 0, 64, 64, 0, 0 },
  { 0, 0, 48, 80, 0, 0 },
  { 0, 0, 32, 96, 0, 0 },
  { 0, 0, 16, 112, 0, 0 }
};
static const int k_bicubic[8][6] = {
  { 0, 0, 128, 0, 0, 0 },
  { 0, -6, 123, 12, -1, 0 },
  { 2, -11, 108, 36, -8, 1 },
  { 0, -9, 93, 50, -6, 0 },
  { 3, -16, 77, 77, -16, 3 },
  { 0, -6, 50, 93, -9, 0 },
  { 1, -8, 36, 108, -11, 2 },
  { 0, -1, 12, 123, -6, 0 }
};

struct gimg_vp8_seq {
  int ready;
  uint32_t width;
  uint32_t height;
  uint32_t mb_w;
  uint32_t mb_h;
  uint32_t y_stride;
  uint32_t uv_stride;
  size_t y_bytes;
  size_t uv_bytes;
  uint8_t * y[3];
  uint8_t * u[3];
  uint8_t * v[3];
  vp8_probs probs;
  uint8_t ymode_prob[4];
  uint8_t uv_mode_prob[3];
  uint8_t mv_prob[2][19];
  int seg_abs;
  int seg_q[4];
  int seg_lf[4];
  uint8_t seg_prob[3];
  uint8_t * seg_ids;
  size_t seg_count;
  int ref_lf[4];
  int mode_lf[4];
  int sign_bias[4];
};

static void vp8_seq_reset_entropy(gimg_vp8_seq * seq) {
  static const uint8_t ymode[4] = { 112, 86, 140, 37 };
  static const uint8_t uv[3] = { 162, 101, 204 };
  memcpy(seq->probs, gimg_vp8_coeffs_proba0, sizeof(seq->probs));
  memcpy(seq->ymode_prob, ymode, sizeof(ymode));
  memcpy(seq->uv_mode_prob, uv, sizeof(uv));
  memcpy(seq->mv_prob, k_default_mv, sizeof(k_default_mv));
  memset(seq->seg_q, 0, sizeof(seq->seg_q));
  memset(seq->seg_lf, 0, sizeof(seq->seg_lf));
  seq->seg_abs = 0;
  seq->seg_prob[0] = 255;
  seq->seg_prob[1] = 255;
  seq->seg_prob[2] = 255;
  memset(seq->ref_lf, 0, sizeof(seq->ref_lf));
  memset(seq->mode_lf, 0, sizeof(seq->mode_lf));
  memset(seq->sign_bias, 0, sizeof(seq->sign_bias));
  if (seq->seg_ids && seq->seg_count) {
    memset(seq->seg_ids, 0, seq->seg_count);
  }
}

static void vp8_seq_free_planes(const GIMG_Allocator * alloc, gimg_vp8_seq * seq) {
  int i;
  for (i = 0; i < 3; ++i) {
    gimg_free(alloc, seq->y[i]);
    gimg_free(alloc, seq->u[i]);
    gimg_free(alloc, seq->v[i]);
    seq->y[i] = NULL;
    seq->u[i] = NULL;
    seq->v[i] = NULL;
  }
  seq->y_bytes = 0;
  seq->uv_bytes = 0;
  seq->mb_w = 0;
  seq->mb_h = 0;
}

GIMG_Result gimg_vp8_seq_create(const GIMG_Allocator * alloc,
    gimg_vp8_seq ** out) {
  gimg_vp8_seq * seq;
  if (!out) {
    return GIMG_ERR_INTERNAL;
  }
  *out = NULL;
  seq = (gimg_vp8_seq *)gimg_malloc(alloc, sizeof(*seq));
  if (!seq) {
    return GIMG_ERR_OOM;
  }
  memset(seq, 0, sizeof(*seq));
  vp8_seq_reset_entropy(seq);
  *out = seq;
  return GIMG_OK;
}

void gimg_vp8_seq_destroy(const GIMG_Allocator * alloc, gimg_vp8_seq * seq) {
  if (!seq) {
    return;
  }
  vp8_seq_free_planes(alloc, seq);
  gimg_free(alloc, seq->seg_ids);
  gimg_free(alloc, seq);
}

void gimg_vp8_seq_reset(const GIMG_Allocator * alloc, gimg_vp8_seq * seq) {
  if (!seq) {
    return;
  }
  vp8_seq_free_planes(alloc, seq);
  seq->ready = 0;
  seq->width = 0;
  seq->height = 0;
  vp8_seq_reset_entropy(seq);
}

static int vp8_seq_alloc_planes(gimg_vp8_seq * seq, const GIMG_Allocator * alloc,
    uint32_t mb_w, uint32_t mb_h) {
  size_t y_bytes = (size_t)mb_w * 16u * (size_t)mb_h * 16u;
  size_t uv_bytes = (size_t)mb_w * 8u * (size_t)mb_h * 8u;
  int i;
  if (seq->y[0] && seq->mb_w == mb_w && seq->mb_h == mb_h) {
    return 1;
  }
  vp8_seq_free_planes(alloc, seq);
  for (i = 0; i < 3; ++i) {
    seq->y[i] = (uint8_t *)gimg_malloc(alloc, y_bytes);
    seq->u[i] = (uint8_t *)gimg_malloc(alloc, uv_bytes);
    seq->v[i] = (uint8_t *)gimg_malloc(alloc, uv_bytes);
    if (!seq->y[i] || !seq->u[i] || !seq->v[i]) {
      vp8_seq_free_planes(alloc, seq);
      return 0;
    }
    memset(seq->y[i], 0, y_bytes);
    memset(seq->u[i], 0, uv_bytes);
    memset(seq->v[i], 0, uv_bytes);
  }
  seq->y_bytes = y_bytes;
  seq->uv_bytes = uv_bytes;
  seq->mb_w = mb_w;
  seq->mb_h = mb_h;
  seq->y_stride = mb_w * 16u;
  seq->uv_stride = mb_w * 8u;
  return 1;
}

static int vp8_seq_alloc_seg(gimg_vp8_seq * seq, const GIMG_Allocator * alloc,
    size_t count) {
  uint8_t * ids;
  if (seq->seg_ids && seq->seg_count == count) {
    return 1;
  }
  ids = (uint8_t *)gimg_malloc(alloc, count ? count : 1u);
  if (!ids) {
    return 0;
  }
  memset(ids, 0, count ? count : 1u);
  gimg_free(alloc, seq->seg_ids);
  seq->seg_ids = ids;
  seq->seg_count = count;
  return 1;
}

static void vp8_copy_ref(uint8_t * dy, uint8_t * du, uint8_t * dv,
    const uint8_t * sy, const uint8_t * su, const uint8_t * sv, size_t y_bytes,
    size_t uv_bytes) {
  memcpy(dy, sy, y_bytes);
  memcpy(du, su, uv_bytes);
  memcpy(dv, sv, uv_bytes);
}

/* -4096..4095 full pixels, in eighth-pel units. */
static int vp8_clamp_full(int v) {
  if (v < -32768) {
    return -32768;
  }
  if (v > 32760) {
    return 32760;
  }
  return v;
}

/* Section 16.3. One macroblock of zero vectors borders the picture.
 * The limits are the edge, one macroblock out, plus the 128 eighth-pel
 * margin from the clamp formula. */
static void vp8_clamp_margin(int * row, int * col, int mb_x, int mb_y,
    int mb_w, int mb_h) {
  int left = -((mb_x + 1) << 7);
  int right = (mb_w - mb_x) << 7;
  int top = -((mb_y + 1) << 7);
  int bottom = (mb_h - mb_y) << 7;
  if (*col < left) {
    *col = left;
  }
  else if (*col > right) {
    *col = right;
  }
  if (*row < top) {
    *row = top;
  }
  else if (*row > bottom) {
    *row = bottom;
  }
}

static int vp8_read_mvcomponent(vp8_bool * d, const uint8_t * p, int * out) {
  int is_long;
  int a = 0;
  if (!vp8_bool_read(d, p[0], &is_long)) {
    return 0;
  }
  if (is_long) {
    int i;
    for (i = 0; i < 3; ++i) {
      int bit;
      if (!vp8_bool_read(d, p[9 + i], &bit)) {
        return 0;
      }
      a += bit << i;
    }
    for (i = 9; i > 3; --i) {
      int bit;
      if (!vp8_bool_read(d, p[9 + i], &bit)) {
        return 0;
      }
      a += bit << i;
    }
    /* Bit 3 is implicit when the value is still below 16. */
    if (!(a & 0xfff0)) {
      a += 8;
    }
    else {
      int bit;
      if (!vp8_bool_read(d, p[12], &bit)) {
        return 0;
      }
      if (bit) {
        a += 8;
      }
    }
  }
  else {
    a = vp8_tree_read(d, k_small_mvtree, p + 2, 0);
    if (a < 0) {
      return 0;
    }
  }
  if (a) {
    int sign;
    if (!vp8_bool_read(d, p[1], &sign)) {
      return 0;
    }
    if (sign) {
      a = -a;
    }
  }
  *out = a;
  return 1;
}

/* Quarter-pel components, doubled into eighth-pel before the caller
 * adds them to an already-doubled predictor. */
static int vp8_read_mv(vp8_bool * d, uint8_t mv_prob[2][19], int * row,
    int * col) {
  int r;
  int c;
  if (!vp8_read_mvcomponent(d, mv_prob[0], &r) ||
      !vp8_read_mvcomponent(d, mv_prob[1], &c)) {
    return 0;
  }
  *row = r * 2;
  *col = c * 2;
  return 1;
}

static int vp8_update_mvprobs(vp8_bool * d, uint8_t mv_prob[2][19]) {
  int comp;
  int i;
  for (comp = 0; comp < 2; ++comp) {
    for (i = 0; i < 19; ++i) {
      int update;
      int value;
      if (!vp8_bool_read(d, k_mv_update_prob[comp][i], &update)) {
        return 0;
      }
      if (!update) {
        continue;
      }
      value = vp8_read_literal(d, 7);
      if (value < 0) {
        return 0;
      }
      mv_prob[comp][i] = (uint8_t)(value ? (value << 1) : 1);
    }
  }
  return 1;
}

static void vp8_neighbor(const vp8_mb * modes, int mb_w, int mb_h, int x,
    int y, int * ref, int * mode, int * row, int * col) {
  /* Outside the picture: inter, zero vector, not split. */
  *ref = VP8_REF_LAST;
  *mode = VP8_MV_ZERO;
  *row = 0;
  *col = 0;
  if (x < 0 || y < 0 || x >= mb_w || y >= mb_h) {
    return;
  }
  {
    const vp8_mb * mb = &modes[(size_t)y * (size_t)mb_w + (size_t)x];
    *ref = mb->ref;
    *mode = mb->y;
    *row = mb->mv_r;
    *col = mb->mv_c;
  }
}

static void vp8_find_near(const vp8_mb * modes, int mb_w, int mb_h, int mb_x,
    int mb_y, int refframe, const int sign_bias[4], int * nearest_r,
    int * nearest_c, int * near_r, int * near_c, int * best_r, int * best_c,
    int cnt[4]) {
  int rows[4] = { 0, 0, 0, 0 };
  int cols[4] = { 0, 0, 0, 0 };
  int slot = 0;
  int cx = 0;
  int above_ref, above_mode, above_r, above_c;
  int left_ref, left_mode, left_r, left_c;
  int al_ref, al_mode, al_r, al_c;
  cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0;
  vp8_neighbor(modes, mb_w, mb_h, mb_x, mb_y - 1, &above_ref, &above_mode,
      &above_r, &above_c);
  vp8_neighbor(modes, mb_w, mb_h, mb_x - 1, mb_y, &left_ref, &left_mode,
      &left_r, &left_c);
  vp8_neighbor(modes, mb_w, mb_h, mb_x - 1, mb_y - 1, &al_ref, &al_mode, &al_r,
      &al_c);
  if (above_ref != VP8_REF_INTRA) {
    if (above_r || above_c) {
      if (sign_bias[above_ref] != sign_bias[refframe]) {
        above_r = -above_r;
        above_c = -above_c;
      }
      slot++;
      cx++;
      rows[slot] = above_r;
      cols[slot] = above_c;
    }
    cnt[cx] += 2;
  }
  if (left_ref != VP8_REF_INTRA) {
    if (left_r || left_c) {
      if (sign_bias[left_ref] != sign_bias[refframe]) {
        left_r = -left_r;
        left_c = -left_c;
      }
      if (left_r != rows[slot] || left_c != cols[slot]) {
        slot++;
        cx++;
        rows[slot] = left_r;
        cols[slot] = left_c;
      }
      cnt[cx] += 2;
    }
    else {
      cnt[0] += 2;
    }
  }
  if (al_ref != VP8_REF_INTRA) {
    if (al_r || al_c) {
      if (sign_bias[al_ref] != sign_bias[refframe]) {
        al_r = -al_r;
        al_c = -al_c;
      }
      if (al_r != rows[slot] || al_c != cols[slot]) {
        slot++;
        cx++;
        rows[slot] = al_r;
        cols[slot] = al_c;
      }
      cnt[cx] += 1;
    }
    else {
      cnt[0] += 1;
    }
  }
  /* The listing consults the split census before assigning it, so a
   * third distinct vector is not merged. The census is filled here. */
  cnt[3] = ((above_mode == VP8_MV_SPLIT) + (left_mode == VP8_MV_SPLIT)) * 2 +
      (al_mode == VP8_MV_SPLIT);
  if (cnt[2] > cnt[1]) {
    int tmp = cnt[1];
    cnt[1] = cnt[2];
    cnt[2] = tmp;
    tmp = rows[1];
    rows[1] = rows[2];
    rows[2] = tmp;
    tmp = cols[1];
    cols[1] = cols[2];
    cols[2] = tmp;
  }
  if (cnt[1] >= cnt[0]) {
    rows[0] = rows[1];
    cols[0] = cols[1];
  }
  vp8_clamp_margin(&rows[1], &cols[1], mb_x, mb_y, mb_w, mb_h);
  vp8_clamp_margin(&rows[2], &cols[2], mb_x, mb_y, mb_w, mb_h);
  vp8_clamp_margin(&rows[0], &cols[0], mb_x, mb_y, mb_w, mb_h);
  rows[0] = vp8_clamp_full(rows[0]);
  cols[0] = vp8_clamp_full(cols[0]);
  rows[1] = vp8_clamp_full(rows[1]);
  cols[1] = vp8_clamp_full(cols[1]);
  rows[2] = vp8_clamp_full(rows[2]);
  cols[2] = vp8_clamp_full(cols[2]);
  *best_r = rows[0];
  *best_c = cols[0];
  *nearest_r = rows[1];
  *nearest_c = cols[1];
  *near_r = rows[2];
  *near_c = cols[2];
}

static void vp8_set_uniform_mv(vp8_mb * mb, int row, int col) {
  int j;
  row = vp8_clamp_full(row);
  col = vp8_clamp_full(col);
  mb->mv_r = row;
  mb->mv_c = col;
  for (j = 0; j < 16; ++j) {
    mb->sr[j] = row;
    mb->sc[j] = col;
  }
}

static void vp8_sub_mv(const vp8_mb * modes, int mb_w, int mb_h, int mb_x,
    int mb_y, int sx, int sy, int * row, int * col) {
  if (sx < 0) {
    mb_x -= 1;
    sx += 4;
  }
  if (sy < 0) {
    mb_y -= 1;
    sy += 4;
  }
  *row = 0;
  *col = 0;
  if (mb_x < 0 || mb_y < 0 || mb_x >= mb_w || mb_y >= mb_h) {
    return;
  }
  {
    const vp8_mb * mb = &modes[(size_t)mb_y * (size_t)mb_w + (size_t)mb_x];
    if (mb->ref == VP8_REF_INTRA) {
      return;
    }
    *row = mb->sr[sy * 4 + sx];
    *col = mb->sc[sy * 4 + sx];
  }
}

static int vp8_mv_context(int lr, int lc, int ar, int ac) {
  int lez = (lr == 0 && lc == 0);
  int aez = (ar == 0 && ac == 0);
  int lea = (lr == ar && lc == ac);
  if (lea && lez) {
    return 4;
  }
  if (lea) {
    return 3;
  }
  if (aez) {
    return 2;
  }
  if (lez) {
    return 1;
  }
  return 0;
}

static int vp8_read_split(vp8_bool * d, vp8_mb * mb, const vp8_mb * modes,
    int mb_w, int mb_h,     int mb_x, int mb_y, int best_r, int best_c, uint8_t mv_prob[2][19]) {
  int part = vp8_tree_read(d, k_mv_part_tree, k_mv_part_prob, 0);
  int n;
  int len;
  int s;
  const uint8_t * blocks;
  if (part < 0 || part > 3) {
    return 0;
  }
  n = k_part_count[part];
  len = k_part_len[part];
  if (part == VP8_PART_TOP) {
    blocks = k_part_blocks[0];
  }
  else if (part == VP8_PART_LEFT) {
    blocks = k_part_blocks[1];
  }
  else if (part == VP8_PART_QUARTERS) {
    blocks = k_part_blocks[2];
  }
  else {
    blocks = k_part_blocks[3];
  }
  for (s = 0; s < n; ++s) {
    int ul = blocks[s * len];
    int sx = ul & 3;
    int sy = ul >> 2;
    int lr, lc, ar, ac;
    int ctx;
    int sub;
    int row = 0;
    int col = 0;
    int k;
    vp8_sub_mv(modes, mb_w, mb_h, mb_x, mb_y, sx - 1, sy, &lr, &lc);
    vp8_sub_mv(modes, mb_w, mb_h, mb_x, mb_y, sx, sy - 1, &ar, &ac);
    ctx = vp8_mv_context(lr, lc, ar, ac);
    sub = vp8_tree_read(d, k_sub_mv_tree, k_sub_mv_prob[ctx], 0);
    if (sub < 0) {
      return 0;
    }
    if (sub == VP8_LEFT4) {
      row = lr;
      col = lc;
    }
    else if (sub == VP8_ABOVE4) {
      row = ar;
      col = ac;
    }
    else if (sub == VP8_ZERO4) {
      row = 0;
      col = 0;
    }
    else {
      int dr, dc;
      if (!vp8_read_mv(d, mv_prob, &dr, &dc)) {
        return 0;
      }
      /* SPLITMV skips the second margin clamp. */
      row = vp8_clamp_full(best_r + dr);
      col = vp8_clamp_full(best_c + dc);
    }
    for (k = 0; k < len; ++k) {
      int bi = blocks[s * len + k];
      mb->sr[bi] = row;
      mb->sc[bi] = col;
    }
  }
  mb->mv_r = mb->sr[15];
  mb->mv_c = mb->sc[15];
  return 1;
}

static int vp8_read_inter_mb(vp8_bool * d, vp8_mb * mb, const vp8_mb * modes,
    int mb_w, int mb_h, int mb_x, int mb_y, int prob_intra, int prob_last,
    int prob_gf, const uint8_t ymode_prob[4], const uint8_t uv_mode_prob[3],
    uint8_t mv_prob[2][19], const int sign_bias[4]) {
  int inter;
  int use_last;
  int ref;
  int mode;
  int nearest_r, nearest_c, near_r, near_c, best_r, best_c;
  int cnt[4];
  uint8_t mv_ref_p[4];
  int j;
  if (!vp8_bool_read(d, prob_intra, &inter)) {
    return 0;
  }
  if (!inter) {
    int ymode = vp8_tree_read(d, k_if_ymode_tree, ymode_prob, 0);
    int uvmode;
    mb->ref = VP8_REF_INTRA;
    vp8_set_uniform_mv(mb, 0, 0);
    if (ymode < 0) {
      return 0;
    }
    mb->y = (uint8_t)ymode;
    if (ymode == VP8_B_PRED) {
      for (j = 0; j < 16; ++j) {
        int bmode = vp8_tree_read(d, k_bmode_tree, k_if_bmode_prob, 0);
        if (bmode < 0) {
          return 0;
        }
        mb->b[j] = (uint8_t)bmode;
      }
    }
    uvmode = vp8_tree_read(d, k_uv_mode_tree, uv_mode_prob, 0);
    if (uvmode < 0) {
      return 0;
    }
    mb->uv = (uint8_t)uvmode;
    return 1;
  }
  if (!vp8_bool_read(d, prob_last, &use_last)) {
    return 0;
  }
  if (!use_last) {
    ref = VP8_REF_LAST;
  }
  else {
    int gf;
    if (!vp8_bool_read(d, prob_gf, &gf)) {
      return 0;
    }
    ref = gf ? VP8_REF_ALT : VP8_REF_GOLDEN;
  }
  mb->ref = (uint8_t)ref;
  vp8_find_near(modes, mb_w, mb_h, mb_x, mb_y, ref, sign_bias, &nearest_r,
      &nearest_c, &near_r, &near_c, &best_r, &best_c, cnt);
  mv_ref_p[0] = (uint8_t)k_mv_mode_ctx[cnt[0]][0];
  mv_ref_p[1] = (uint8_t)k_mv_mode_ctx[cnt[1]][1];
  mv_ref_p[2] = (uint8_t)k_mv_mode_ctx[cnt[2]][2];
  mv_ref_p[3] = (uint8_t)k_mv_mode_ctx[cnt[3]][3];
  mode = vp8_tree_read(d, k_mv_ref_tree, mv_ref_p, 0);
  if (mode < 0) {
    return 0;
  }
  mb->y = (uint8_t)mode;
  if (mode == VP8_MV_NEAREST) {
    vp8_set_uniform_mv(mb, nearest_r, nearest_c);
  }
  else if (mode == VP8_MV_NEAR) {
    vp8_set_uniform_mv(mb, near_r, near_c);
  }
  else if (mode == VP8_MV_ZERO) {
    vp8_set_uniform_mv(mb, 0, 0);
  }
  else if (mode == VP8_MV_NEW) {
    int dr, dc;
    if (!vp8_read_mv(d, mv_prob, &dr, &dc)) {
      return 0;
    }
    {
      int row = best_r + dr;
      int col = best_c + dc;
      vp8_clamp_margin(&row, &col, mb_x, mb_y, mb_w, mb_h);
      vp8_set_uniform_mv(mb, row, col);
    }
  }
  else if (!vp8_read_split(d, mb, modes, mb_w, mb_h, mb_x, mb_y, best_r,
               best_c, mv_prob)) {
    return 0;
  }
  return 1;
}

static int vp8_pred_sample(const uint8_t * plane, int stride, int w, int h,
    int x, int y) {
  if (x < 0) {
    x = 0;
  }
  if (y < 0) {
    y = 0;
  }
  if (x >= w) {
    x = w - 1;
  }
  if (y >= h) {
    y = h - 1;
  }
  return plane[(size_t)y * (size_t)stride + (size_t)x];
}

static int vp8_interp_pix(const int fil[6], const uint8_t * plane, int stride,
    int w, int h, int x, int y, int step_x, int step_y) {
  int a = 0;
  int i;
  for (i = 0; i < 6; ++i) {
    a += vp8_pred_sample(plane, stride, w, h, x + (i - 2) * step_x,
             y + (i - 2) * step_y) *
        fil[i];
  }
  return clamp255(vp8_shr(a + 64, 7));
}

/* Section 18. The listing's horizontal pass is nine rows. The prose
 * puts two of those above the 4x4 and three below it, which is what
 * the vertical pass consumes when it steps two samples back. */
static void vp8_predict_4(uint8_t * dst, int dst_stride, const uint8_t * ref,
    int ref_stride, int ref_w, int ref_h, int x, int y, int mv_r, int mv_c,
    int bicubic, int fullpel) {
  int full_r;
  int full_c;
  int vfrac;
  int hfrac;
  int ox;
  int oy;
  int r;
  int c;
  if (fullpel) {
    mv_r &= ~7;
    mv_c &= ~7;
  }
  full_r = vp8_shr(mv_r, 3);
  full_c = vp8_shr(mv_c, 3);
  vfrac = mv_r - (full_r << 3);
  hfrac = mv_c - (full_c << 3);
  ox = x + full_c;
  oy = y + full_r;
  if (hfrac == 0 && vfrac == 0) {
    for (r = 0; r < 4; ++r) {
      for (c = 0; c < 4; ++c) {
        dst[(y + r) * dst_stride + (x + c)] = (uint8_t)vp8_pred_sample(
            ref, ref_stride, ref_w, ref_h, ox + c, oy + r);
      }
    }
    return;
  }
  {
    uint8_t temp[9][4];
    const int * hfil = bicubic ? k_bicubic[hfrac] : k_bilinear[hfrac];
    const int * vfil = bicubic ? k_bicubic[vfrac] : k_bilinear[vfrac];
    for (r = 0; r < 9; ++r) {
      int sy = oy - 2 + r;
      for (c = 0; c < 4; ++c) {
        temp[r][c] = (uint8_t)vp8_interp_pix(
            hfil, ref, ref_stride, ref_w, ref_h, ox + c, sy, 1, 0);
      }
    }
    for (r = 0; r < 4; ++r) {
      for (c = 0; c < 4; ++c) {
        int a = 0;
        int i;
        for (i = 0; i < 6; ++i) {
          a += (int)temp[r + i][c] * vfil[i];
        }
        dst[(y + r) * dst_stride + (x + c)] =
            (uint8_t)clamp255(vp8_shr(a + 64, 7));
      }
    }
  }
}

static int vp8_chroma_avg(int c1, int c2, int c3, int c4) {
  int s = c1 + c2 + c3 + c4;
  if (s >= 0) {
    return (s + 4) >> 3;
  }
  return -(((-s) + 4) >> 3);
}

static int vp8_predict_inter_mb(uint8_t * y, int y_stride, uint8_t * u,
    uint8_t * v, int uv_stride, const gimg_vp8_seq * seq, const vp8_mb * mb,
    int mb_x, int mb_y, int fullpel, int bicubic) {
  const uint8_t * ry;
  const uint8_t * ru;
  const uint8_t * rv;
  int ref_w;
  int ref_h;
  int uv_w;
  int uv_h;
  int sy;
  int sx;
  static const int k_chroma_blocks[4] = { 0, 2, 8, 10 };
  if (mb->ref < VP8_REF_LAST || mb->ref > VP8_REF_ALT) {
    return 0;
  }
  ry = seq->y[mb->ref - 1];
  ru = seq->u[mb->ref - 1];
  rv = seq->v[mb->ref - 1];
  if (!ry || !ru || !rv) {
    return 0;
  }
  ref_w = (int)seq->mb_w * 16;
  ref_h = (int)seq->mb_h * 16;
  uv_w = (int)seq->mb_w * 8;
  uv_h = (int)seq->mb_h * 8;
  for (sy = 0; sy < 4; ++sy) {
    for (sx = 0; sx < 4; ++sx) {
      int bi = sy * 4 + sx;
      vp8_predict_4(y, y_stride, ry, (int)seq->y_stride, ref_w, ref_h,
          mb_x * 16 + sx * 4, mb_y * 16 + sy * 4, mb->sr[bi], mb->sc[bi],
          bicubic, fullpel);
    }
  }
  for (sx = 0; sx < 4; ++sx) {
    int bi = k_chroma_blocks[sx];
    int cr = vp8_chroma_avg(mb->sr[bi], mb->sr[bi + 1], mb->sr[bi + 4],
        mb->sr[bi + 5]);
    int cc = vp8_chroma_avg(mb->sc[bi], mb->sc[bi + 1], mb->sc[bi + 4],
        mb->sc[bi + 5]);
    int bx = (sx & 1) * 4;
    int by = (sx >> 1) * 4;
    if (fullpel) {
      cr &= ~7;
      cc &= ~7;
    }
    vp8_predict_4(u, uv_stride, ru, (int)seq->uv_stride, uv_w, uv_h,
        mb_x * 8 + bx, mb_y * 8 + by, cr, cc, bicubic, fullpel);
    vp8_predict_4(v, uv_stride, rv, (int)seq->uv_stride, uv_w, uv_h,
        mb_x * 8 + bx, mb_y * 8 + by, cr, cc, bicubic, fullpel);
  }
  return 1;
}

GIMG_Result gimg_webp_vp8_decode_frame(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, gimg_vp8_seq * seq, GIMG_Raster ** out_raster) {
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
  int y1_dc_s[4];
  int y1_ac_s[4];
  int y2_dc_s[4];
  int y2_ac_s[4];
  int uv_dc_s[4];
  int uv_ac_s[4];
  int seg_on = 0;
  int seg_abs = 0;
  int seg_map = 0;
  int seg_q[4] = { 0, 0, 0, 0 };
  int seg_lf[4] = { 0, 0, 0, 0 };
  uint8_t seg_prob[3] = { 255, 255, 255 };
  int skip_enabled = 0;
  int skip_prob = 0;
  int filter_simple = 0;
  int filter_level = 0;
  int sharp = 0;
  int lf_adj = 0;
  int ref_lf[4] = { 0, 0, 0, 0 };
  int mode_lf[4] = { 0, 0, 0, 0 };
  int t;
  int b;
  int c;
  int p;
  vp8_bool hdr;
  vp8_bool part_dec[8];
  int npart = 1;
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
  uint32_t hdr_bytes;
  int is_key;
  int version;
  int fullpel;
  int bicubic;
  int refresh_entropy = 1;
  int refresh_gf = 1;
  int refresh_arf = 1;
  int refresh_last = 1;
  int copy_gf = 0;
  int copy_arf = 0;
  int prob_intra = 0;
  int prob_last = 0;
  int prob_gf = 0;
  vp8_probs saved_probs;

  if (!out_raster || !seq) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (!data || size < 3u) {
    return GIMG_ERR_CORRUPT;
  }
  is_key = (data[0] & 1u) == 0u;
  version = (int)((data[0] >> 1) & 7u);
  if (version > 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  /* Version 0 is bicubic. Versions 1 and 2 are bilinear. Version 3
   * keeps the integer sample (section 9.1, reconstruction filter
   * "None") and truncates chroma motion to a whole chroma pixel. */
  bicubic = (version == 0);
  fullpel = (version == 3);
  tag = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
      ((uint32_t)data[2] << 16);
  part0_size = tag >> 5;
  if (is_key) {
    if (size < 10u) {
      return GIMG_ERR_CORRUPT;
    }
    if (data[3] != 0x9du || data[4] != 0x01u || data[5] != 0x2au) {
      return GIMG_ERR_CORRUPT;
    }
    {
      uint32_t raw_w = (uint32_t)data[6] | ((uint32_t)data[7] << 8);
      uint32_t raw_h = (uint32_t)data[8] | ((uint32_t)data[9] << 8);
      /* Section 9.1. The top two bits are a display scale. Reconstruction
       * stays at the coded size, which is also what dwebp writes. */
      width = raw_w & 0x3fffu;
      height = raw_h & 0x3fffu;
    }
    hdr_bytes = 10u;
  }
  else {
    /* An interframe has no width of its own. The preceding keyframe in
     * this sequence supplied it. */
    if (!seq->ready || seq->width == 0u || seq->height == 0u) {
      return GIMG_ERR_CORRUPT;
    }
    width = seq->width;
    height = seq->height;
    hdr_bytes = 3u;
  }
  if (width == 0u || height == 0u || width > VP8_MAX_DIM ||
      height > VP8_MAX_DIM) {
    return GIMG_ERR_CORRUPT;
  }
  if ((size_t)hdr_bytes + (size_t)part0_size > size || part0_size < 2u) {
    return GIMG_ERR_CORRUPT;
  }
  part0 = data + hdr_bytes;
  tok = part0 + part0_size;
  tok_size = size - ((size_t)hdr_bytes + (size_t)part0_size);
  if (!vp8_bool_init(&hdr, part0, part0_size)) {
    return GIMG_ERR_CORRUPT;
  }

  /* A keyframe restores the default probabilities and segment state.
   * An interframe continues from the previous frame in this sequence.
   * A clear update flag leaves that previous value. */
  if (is_key) {
    vp8_seq_reset_entropy(seq);
  }
  seg_abs = seq->seg_abs;
  memcpy(seg_q, seq->seg_q, sizeof(seg_q));
  memcpy(seg_lf, seq->seg_lf, sizeof(seg_lf));
  memcpy(seg_prob, seq->seg_prob, sizeof(seg_prob));
  memcpy(ref_lf, seq->ref_lf, sizeof(ref_lf));
  memcpy(mode_lf, seq->mode_lf, sizeof(mode_lf));

  /* Section 19.2. Color space and clamping are keyframe fields. */
  {
    int color;
    int clamp_bit;
    int parts;
    int i;
    if (is_key) {
      if (!vp8_bool_read(&hdr, 128, &color) ||
          !vp8_bool_read(&hdr, 128, &clamp_bit)) {
        return GIMG_ERR_CORRUPT;
      }
      (void)clamp_bit;
      if (color != 0) {
        return GIMG_ERR_UNSUPPORTED;
      }
    }
    if (!vp8_bool_read(&hdr, 128, &seg_on)) {
      return GIMG_ERR_CORRUPT;
    }
    /* Section 9.3, in the order of the section 19 table. Feature mode
     * 0 adds a delta and 1 replaces the frame value. A clear update
     * flag leaves the keyframe default of 0. Probabilities stay 255
     * unless the map update replaces them. */
    if (seg_on) {
      int update_map;
      int update_data;
      if (!vp8_bool_read(&hdr, 128, &update_map) ||
          !vp8_bool_read(&hdr, 128, &update_data)) {
        return GIMG_ERR_CORRUPT;
      }
      seg_map = update_map;
      if (update_data) {
        if (!vp8_bool_read(&hdr, 128, &seg_abs)) {
          return GIMG_ERR_CORRUPT;
        }
        for (i = 0; i < 4; ++i) {
          if (!read_signed(&hdr, 7, &seg_q[i])) {
            return GIMG_ERR_CORRUPT;
          }
        }
        for (i = 0; i < 4; ++i) {
          if (!read_signed(&hdr, 6, &seg_lf[i])) {
            return GIMG_ERR_CORRUPT;
          }
        }
      }
      if (update_map) {
        for (i = 0; i < 3; ++i) {
          int update;
          if (!vp8_bool_read(&hdr, 128, &update)) {
            return GIMG_ERR_CORRUPT;
          }
          if (!update) {
            continue;
          }
          {
            int value = vp8_read_literal(&hdr, 8);
            if (value < 0) {
              return GIMG_ERR_CORRUPT;
            }
            seg_prob[i] = (uint8_t)value;
          }
        }
      }
    }
    filter_simple = vp8_read_literal(&hdr, 1);
    filter_level = vp8_read_literal(&hdr, 6);
    sharp = vp8_read_literal(&hdr, 3);
    if (filter_simple < 0 || filter_level < 0 || sharp < 0 ||
        !vp8_bool_read(&hdr, 128, &lf_adj)) {
      return GIMG_ERR_CORRUPT;
    }
    /* Section 9.4. The enable bit is followed by the delta record even
     * when this frame does not update the values. */
    if (lf_adj) {
      int update;
      if (!vp8_bool_read(&hdr, 128, &update)) {
        return GIMG_ERR_CORRUPT;
      }
      if (update) {
        for (i = 0; i < 4; ++i) {
          if (!read_signed(&hdr, 6, &ref_lf[i])) {
            return GIMG_ERR_CORRUPT;
          }
        }
        for (i = 0; i < 4; ++i) {
          if (!read_signed(&hdr, 6, &mode_lf[i])) {
            return GIMG_ERR_CORRUPT;
          }
        }
      }
    }
    parts = vp8_read_literal(&hdr, 2);
    if (parts < 0) {
      return GIMG_ERR_CORRUPT;
    }
    /* Section 9.5: L(2) is log2 of the coefficient-partition count. */
    npart = 1 << parts;
    q_index = vp8_read_literal(&hdr, 7);
    if (q_index < 0 || !read_delta_q(&hdr, &y1_dc_delta) ||
        !read_delta_q(&hdr, &y2_dc_delta) ||
        !read_delta_q(&hdr, &y2_ac_delta) ||
        !read_delta_q(&hdr, &uv_dc_delta) ||
        !read_delta_q(&hdr, &uv_ac_delta)) {
      return GIMG_ERR_CORRUPT;
    }
    /* Section 19.2, the interframe arm, then refresh_entropy. Copies
     * of one reference onto another happen before the current frame
     * replaces a buffer. */
    if (is_key) {
      refresh_gf = 1;
      refresh_arf = 1;
      refresh_last = 1;
      if (!vp8_bool_read(&hdr, 128, &refresh_entropy)) {
        return GIMG_ERR_CORRUPT;
      }
    }
    else {
      int bit;
      if (!vp8_bool_read(&hdr, 128, &refresh_gf) ||
          !vp8_bool_read(&hdr, 128, &refresh_arf)) {
        return GIMG_ERR_CORRUPT;
      }
      if (!refresh_gf) {
        copy_gf = vp8_read_literal(&hdr, 2);
        if (copy_gf < 0) {
          return GIMG_ERR_CORRUPT;
        }
      }
      if (!refresh_arf) {
        copy_arf = vp8_read_literal(&hdr, 2);
        if (copy_arf < 0) {
          return GIMG_ERR_CORRUPT;
        }
      }
      if (!vp8_bool_read(&hdr, 128, &bit)) {
        return GIMG_ERR_CORRUPT;
      }
      seq->sign_bias[VP8_REF_GOLDEN] = bit;
      if (!vp8_bool_read(&hdr, 128, &bit)) {
        return GIMG_ERR_CORRUPT;
      }
      seq->sign_bias[VP8_REF_ALT] = bit;
      if (!vp8_bool_read(&hdr, 128, &refresh_entropy) ||
          !vp8_bool_read(&hdr, 128, &refresh_last)) {
        return GIMG_ERR_CORRUPT;
      }
    }
  }

  memcpy(saved_probs, seq->probs, sizeof(saved_probs));
  memcpy(probs, seq->probs, sizeof(probs));
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
  if (!is_key) {
    int update;
    int i;
    prob_intra = vp8_read_literal(&hdr, 8);
    prob_last = vp8_read_literal(&hdr, 8);
    prob_gf = vp8_read_literal(&hdr, 8);
    if (prob_intra < 0 || prob_last < 0 || prob_gf < 0 ||
        !vp8_bool_read(&hdr, 128, &update)) {
      return GIMG_ERR_CORRUPT;
    }
    if (update) {
      for (i = 0; i < 4; ++i) {
        int value = vp8_read_literal(&hdr, 8);
        if (value < 0) {
          return GIMG_ERR_CORRUPT;
        }
        seq->ymode_prob[i] = (uint8_t)value;
      }
    }
    if (!vp8_bool_read(&hdr, 128, &update)) {
      return GIMG_ERR_CORRUPT;
    }
    if (update) {
      for (i = 0; i < 3; ++i) {
        int value = vp8_read_literal(&hdr, 8);
        if (value < 0) {
          return GIMG_ERR_CORRUPT;
        }
        seq->uv_mode_prob[i] = (uint8_t)value;
      }
    }
    if (!vp8_update_mvprobs(&hdr, seq->mv_prob)) {
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
  if (!vp8_seq_alloc_planes(seq, alloc, mb_w, mb_h) ||
      !vp8_seq_alloc_seg(seq, alloc, (size_t)mb_w * (size_t)mb_h)) {
    gimg_free(alloc, skip_mb);
    return GIMG_ERR_OOM;
  }
  if (is_key) {
    memset(seq->seg_ids, 0, (size_t)mb_w * (size_t)mb_h);
  }
  {
    int s;
    for (s = 0; s < 4; ++s) {
      int q = q_index;
      int y2ac;
      if (seg_on) {
        if (seg_abs) {
          q = seg_q[s];
        }
        else {
          q += seg_q[s];
        }
      }
      y1_dc_s[s] = gimg_vp8_dc_qlookup[clamp_q(q + y1_dc_delta)];
      y1_ac_s[s] = (int)gimg_vp8_ac_qlookup[clamp_q(q)];
      y2_dc_s[s] = gimg_vp8_dc_qlookup[clamp_q(q + y2_dc_delta)] * 2;
      y2ac = (int)gimg_vp8_ac_qlookup[clamp_q(q + y2_ac_delta)] * 155 / 100;
      if (y2ac < 8) {
        y2ac = 8;
      }
      y2_ac_s[s] = y2ac;
      uv_dc_s[s] = gimg_vp8_dc_qlookup[clamp_q(q + uv_dc_delta)];
      if (uv_dc_s[s] > 132) {
        uv_dc_s[s] = 132;
      }
      uv_ac_s[s] = (int)gimg_vp8_ac_qlookup[clamp_q(q + uv_ac_delta)];
    }
  }

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
      /* Section 10: the segment id precedes the skip flag, and only when
       * this frame updates the map. A segmentation that stays on without
       * a new map reuses the previous frame's ids. */
      if (seg_on && seg_map) {
        int id = vp8_tree_read(&hdr, k_segment_tree, seg_prob, 0);
        if (id < 0) {
          r = GIMG_ERR_CORRUPT;
          break;
        }
        mb->seg = (uint8_t)id;
        seq->seg_ids[mb_y * mb_w + mb_x] = (uint8_t)id;
      }
      else if (seg_on && !is_key) {
        mb->seg = seq->seg_ids[mb_y * mb_w + mb_x];
      }
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
      if (!is_key) {
        if (!vp8_read_inter_mb(&hdr, mb, modes, (int)mb_w, (int)mb_h,
                (int)mb_x, (int)mb_y, prob_intra, prob_last, prob_gf,
                seq->ymode_prob, seq->uv_mode_prob, seq->mv_prob,
                seq->sign_bias)) {
          r = GIMG_ERR_CORRUPT;
          break;
        }
        continue;
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
  /* Section 9.5. The sizes of every coefficient partition except the
   * last are 3 little-endian bytes immediately after the first
   * partition. The last partition is whatever remains. Rows use those
   * partitions in turn, starting again at the first. */
  {
    const uint8_t * cursor = tok;
    size_t remain = tok_size;
    size_t psz[8];
    int pi;
    size_t off = 0;
    if (npart > 1) {
      if (remain < (size_t)(npart - 1) * 3u) {
        r = GIMG_ERR_CORRUPT;
        goto Done;
      }
      for (pi = 0; pi < npart - 1; ++pi) {
        psz[pi] = (size_t)cursor[0] | ((size_t)cursor[1] << 8) |
            ((size_t)cursor[2] << 16);
        cursor += 3;
        remain -= 3u;
      }
      for (pi = 0; pi < npart - 1; ++pi) {
        if (psz[pi] > remain - off) {
          r = GIMG_ERR_CORRUPT;
          goto Done;
        }
        off += psz[pi];
      }
      psz[npart - 1] = remain - off;
    }
    else {
      psz[0] = remain;
    }
    off = 0;
    for (pi = 0; pi < npart; ++pi) {
      if (!vp8_bool_init(&part_dec[pi], cursor + off, psz[pi])) {
        r = GIMG_ERR_CORRUPT;
        goto Done;
      }
      off += psz[pi];
    }
  }

  for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    int y2_left = 0;
    vp8_bool * row_tok = &part_dec[mb_y & (uint32_t)(npart - 1)];
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      vp8_mb * mb = &modes[mb_y * mb_w + mb_x];
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
      const int y1_dc = y1_dc_s[mb->seg];
      const int y1_ac = y1_ac_s[mb->seg];
      const int y2_dc = y2_dc_s[mb->seg];
      const int y2_ac = y2_ac_s[mb->seg];
      const int uv_dc = uv_dc_s[mb->seg];
      const int uv_ac = uv_ac_s[mb->seg];
      if (mb->ref != VP8_REF_INTRA) {
        if (!vp8_predict_inter_mb(y_plane, (int)y_stride, u_plane, v_plane,
                (int)uv_stride, seq, mb, (int)mb_x, (int)mb_y, fullpel,
                bicubic)) {
          r = GIMG_ERR_CORRUPT;
          goto Done;
        }
      }
      if (mb->y == VP8_MV_SPLIT) {
        if (!skip) {
          for (sy = 0; sy < 4; ++sy) {
            for (sx = 0; sx < 4; ++sx) {
              int16_t block[16];
              int16_t residue[16];
              int fx = (int)mb_x * 4 + sx;
              int fy = (int)mb_y * 4 + sy;
              if (!decode_coeffs(row_tok, probs[3], 0,
                      nz_at(y_nz, y4_stride, fx, fy), y1_dc, y1_ac, block,
                      &had)) {
                r = GIMG_ERR_CORRUPT;
                goto Done;
              }
              y_nz[fy * y4_stride + fx] = (uint8_t)had;
              if (had) {
                mb->nz = 1;
              }
              idct_4x4(block, residue);
              add_residue(y_plane, (int)y_stride, ox + sx * 4, oy + sy * 4,
                  residue);
            }
          }
        }
      }
      else if (mb->y == VP8_B_PRED) {
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
              if (!decode_coeffs(row_tok, probs[3], 0,
                      nz_at(y_nz, y4_stride, fx, fy), y1_dc, y1_ac, block,
                      &had)) {
                r = GIMG_ERR_CORRUPT;
                goto Done;
              }
              y_nz[fy * y4_stride + fx] = (uint8_t)had;
              if (had) {
                mb->nz = 1;
              }
              idct_4x4(block, residue);
              paint_b4(y_plane, (int)y_stride, px, py, pred, residue);
            }
          }
        }
      }
      else {
        int16_t y2_coeff[16];
        int16_t y2_spatial[16];
        if (mb->ref == VP8_REF_INTRA) {
          predict_mb(y_plane, (int)y_stride, ox, oy, 16, mb->y);
        }
        if (skip) {
          y2_left = 0;
          y2_above[mb_x] = 0;
        }
        else {
          int ctx = (y2_left ? 1 : 0) + (y2_above[mb_x] ? 1 : 0);
          if (!decode_coeffs(row_tok, probs[1], 0, ctx, y2_dc, y2_ac,
                  y2_coeff, &had)) {
            r = GIMG_ERR_CORRUPT;
            goto Done;
          }
          y2_left = had;
          y2_above[mb_x] = (uint8_t)had;
          if (had) {
            mb->nz = 1;
          }
          iwht_4x4(y2_coeff, y2_spatial);
          for (sy = 0; sy < 4; ++sy) {
            for (sx = 0; sx < 4; ++sx) {
              int16_t block[16];
              int16_t residue[16];
              int fx = (int)mb_x * 4 + sx;
              int fy = (int)mb_y * 4 + sy;
              if (!decode_coeffs(row_tok, probs[0], 1,
                      nz_at(y_nz, y4_stride, fx, fy), y1_dc, y1_ac, block,
                      &had)) {
                r = GIMG_ERR_CORRUPT;
                goto Done;
              }
              y_nz[fy * y4_stride + fx] = (uint8_t)had;
              if (had) {
                mb->nz = 1;
              }
              block[0] = y2_spatial[sy * 4 + sx];
              idct_4x4(block, residue);
              add_residue(y_plane, (int)y_stride, ox + sx * 4, oy + sy * 4,
                  residue);
            }
          }
        }
      }
      if (mb->ref == VP8_REF_INTRA) {
        predict_mb(u_plane, (int)uv_stride, cx, cy, 8, mb->uv);
        predict_mb(v_plane, (int)uv_stride, cx, cy, 8, mb->uv);
      }
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
            if (!decode_coeffs(row_tok, probs[2], 0,
                    nz_at(nz, uv4_stride, fx, fy), uv_dc, uv_ac, block,
                    &had)) {
              r = GIMG_ERR_CORRUPT;
              goto Done;
            }
            nz[fy * uv4_stride + fx] = (uint8_t)had;
            if (had) {
              mb->nz = 1;
            }
            idct_4x4(block, residue);
            add_residue(plane, (int)uv_stride, cx + sx * 4, cy + sy * 4,
                residue);
          }
        }
      }
    }
  }

  /* A frame-header level of 0 skips the filter, including when the
   * adjustment deltas would raise it, unless segmentation supplies a
   * level. The records were parsed above so the header stays aligned.
   * A macroblock whose adjusted level lands on 0 is skipped inside. */
  if (filter_level != 0 || seg_on) {
    vp8_loop_filter(y_plane, (int)y_stride, u_plane, v_plane, (int)uv_stride,
        modes, mb_w, mb_h, filter_simple, filter_level, sharp, lf_adj,
        ref_lf, mode_lf, seg_on, seg_abs, seg_lf);
  }

  memcpy(seq->probs, probs, sizeof(probs));
  if (!refresh_entropy) {
    memcpy(seq->probs, saved_probs, sizeof(saved_probs));
  }
  seq->seg_abs = seg_abs;
  memcpy(seq->seg_q, seg_q, sizeof(seg_q));
  memcpy(seq->seg_lf, seg_lf, sizeof(seg_lf));
  memcpy(seq->seg_prob, seg_prob, sizeof(seg_prob));
  memcpy(seq->ref_lf, ref_lf, sizeof(ref_lf));
  memcpy(seq->mode_lf, mode_lf, sizeof(mode_lf));
  /* Section 9.7. Copy another reference first, then let a refresh
   * replace that buffer with the filtered current frame. */
  if (copy_arf == 1) {
    vp8_copy_ref(seq->y[2], seq->u[2], seq->v[2], seq->y[0], seq->u[0],
        seq->v[0], seq->y_bytes, seq->uv_bytes);
  }
  else if (copy_arf == 2) {
    vp8_copy_ref(seq->y[2], seq->u[2], seq->v[2], seq->y[1], seq->u[1],
        seq->v[1], seq->y_bytes, seq->uv_bytes);
  }
  if (copy_gf == 1) {
    vp8_copy_ref(seq->y[1], seq->u[1], seq->v[1], seq->y[0], seq->u[0],
        seq->v[0], seq->y_bytes, seq->uv_bytes);
  }
  else if (copy_gf == 2) {
    vp8_copy_ref(seq->y[1], seq->u[1], seq->v[1], seq->y[2], seq->u[2],
        seq->v[2], seq->y_bytes, seq->uv_bytes);
  }
  if (refresh_gf) {
    vp8_copy_ref(seq->y[1], seq->u[1], seq->v[1], y_plane, u_plane, v_plane,
        seq->y_bytes, seq->uv_bytes);
  }
  if (refresh_arf) {
    vp8_copy_ref(seq->y[2], seq->u[2], seq->v[2], y_plane, u_plane, v_plane,
        seq->y_bytes, seq->uv_bytes);
  }
  if (refresh_last) {
    vp8_copy_ref(seq->y[0], seq->u[0], seq->v[0], y_plane, u_plane, v_plane,
        seq->y_bytes, seq->uv_bytes);
  }
  seq->width = width;
  seq->height = height;
  seq->ready = 1;

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
    {
      const int cw = (int)((width + 1u) / 2u);
      const int ch = (int)((height + 1u) / 2u);
      for (y = 0; y < height; ++y) {
        for (x = 0; x < width; ++x) {
          int yy = y_plane[y * y_stride + x];
          int uu = fancy_chroma(u_plane, (int)uv_stride, cw, ch, (int)x,
              (int)y);
          int vv = fancy_chroma(v_plane, (int)uv_stride, cw, ch, (int)x,
              (int)y);
          yuv_to_rgba(yy, uu, vv, dst + y * stride + (size_t)x * 4u);
        }
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

GIMG_Result gimg_webp_vp8_decode(const unsigned char * data, size_t size,
    const GIMG_Allocator * alloc, GIMG_Raster ** out_raster) {
  gimg_vp8_seq * seq = NULL;
  GIMG_Result r = gimg_vp8_seq_create(alloc, &seq);
  if (r != GIMG_OK) {
    if (out_raster) {
      *out_raster = NULL;
    }
    return r;
  }
  r = gimg_webp_vp8_decode_frame(data, size, alloc, seq, out_raster);
  gimg_vp8_seq_destroy(alloc, seq);
  return r;
}
