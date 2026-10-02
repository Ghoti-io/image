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
 *
 * The boolean coder follows RFC 6386 section 7. Its range is one less
 * than the decoder range, and a range below 127 shifts until the span
 * is at least 128. The coefficient probabilities, category
 * probabilities, coefficient bands, and the DC dequantization table
 * are generated from that RFC; see webp_vp8_proba.inc. RGB to YUV is
 * the ITU-R BT.601 studio-swing matrix that RFC cites, at scale 2^16.
 */

/**
 * @file
 *
 * VP8 keyframe encoder. Every macroblock is one of the four Intra16
 * predictors, or sixteen 4×4 predictors when that scores better. The
 * score is reconstructed error plus the bool-coder cost of the modes
 * and the tokens; one bit is priced at the luma AC quantizer step, and
 * an equal score stays with the earlier choice. After the deadzone
 * quantizer, a backward pass drops a luma AC coefficient when its
 * tokens cost more than the error it removes. A macroblock the 16×16
 * predictors cannot explain takes a finer segment quantizer
 * (section 9.3). The Intra16
 * residual is the section 14 Walsh-Hadamard of the sixteen DC
 * coefficients and the 4×4 DCT of everything else. A 4×4 macroblock has
 * no Y2 block. A macroblock whose coefficients are all zero is skipped
 * when that flag costs less than the zero tokens (section 9.1). The
 * coefficient probabilities are then replaced where the tokens save
 * more than the section 13.4 update, and the trellis is priced with
 * that table at the neighbor context the bool coder will use. The
 * section 15 normal filter then runs at the level
 * whose filtered reconstruction is closest to the source. The level
 * is a fixed-width field, so an equal error stays unfiltered.
 * Prediction keeps the unfiltered samples. dwebp accepts the output.
 * make webp-rd is the comparison against cwebp.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "webp_internal.h"
#include "webp_vp8_proba.inc"

enum {
  YUV_FIX = 16,
  YUV_HALF = 1 << (YUV_FIX - 1),
  VP8_MAX_DIM = 16383
};

/* ITU-R BT.601 studio swing. Weights 0.299, 0.587, 0.114 are 299, 587,
 * and 114 over 1000. Luma is 16 + (219/255) times that combination.
 * Chroma is 128 plus (224/255) times (B - Y) / (2 (1 - Kb)) and the
 * matching V formula. Each product is rounded at scale 2^16. U and V
 * give any leftover rounding to the coefficient that completes the
 * triple, so the three sum to 0 and a neutral colour stays 128. */
/* The products exceed 2^31, so the rounding multiply is done in
 * long long and the quotient comes back as int. */
#define VP8_DIV_ROUND(n, d) \
  ((int)(((n) + (long long)((d) / 2)) / (long long)(d)))
#define VP8_KR 299LL
#define VP8_KG 587LL
#define VP8_KB 114LL
#define VP8_KDEN 1000LL
#define VP8_Y_DEN ((VP8_KDEN) * 255LL)
#define VP8_Y_R VP8_DIV_ROUND((VP8_KR) * 219LL * (1LL << YUV_FIX), VP8_Y_DEN)
#define VP8_Y_G VP8_DIV_ROUND((VP8_KG) * 219LL * (1LL << YUV_FIX), VP8_Y_DEN)
#define VP8_Y_B VP8_DIV_ROUND((VP8_KB) * 219LL * (1LL << YUV_FIX), VP8_Y_DEN)
#define VP8_C_SCALE (224LL * (1LL << YUV_FIX))
#define VP8_U_DEN (255LL * 2LL * ((VP8_KDEN) - (VP8_KB)))
#define VP8_U_R (-VP8_DIV_ROUND((VP8_KR) * VP8_C_SCALE, VP8_U_DEN))
#define VP8_U_G (-VP8_DIV_ROUND((VP8_KG) * VP8_C_SCALE, VP8_U_DEN))
#define VP8_U_B0 VP8_DIV_ROUND(((VP8_KDEN) - (VP8_KB)) * VP8_C_SCALE, VP8_U_DEN)
#define VP8_U_B ((VP8_U_B0) - ((VP8_U_R) + (VP8_U_G) + (VP8_U_B0)))
#define VP8_V_DEN (255LL * 2LL * ((VP8_KDEN) - (VP8_KR)))
#define VP8_V_R0 VP8_DIV_ROUND(((VP8_KDEN) - (VP8_KR)) * VP8_C_SCALE, VP8_V_DEN)
#define VP8_V_G (-VP8_DIV_ROUND((VP8_KG) * VP8_C_SCALE, VP8_V_DEN))
#define VP8_V_B (-VP8_DIV_ROUND((VP8_KB) * VP8_C_SCALE, VP8_V_DEN))
#define VP8_V_R ((VP8_V_R0) - ((VP8_V_R0) + (VP8_V_G) + (VP8_V_B)))

/* Pcat3..Pcat6, in the order put_large_value walks. The rows themselves
 * are generated from RFC 6386 section 13.2. */
static const uint8_t * const k_cat3456[] = {
  gimg_vp8_pcat3, gimg_vp8_pcat4, gimg_vp8_pcat5, gimg_vp8_pcat6
};

/* effort 0..9 → base quantizer index (higher is coarser). Each entry is
 * the single-segment index from libwebp's QualityToCompression at cwebp
 * qualities 15, 35, 50, 65, 75, 80, 85, 90, 95 and 100. Effort 4 is
 * index 26, which is what cwebp -q 75 writes when the frame has one
 * segment. */
static const int k_effort_q[10] = {
  68, 48, 38, 30, 26, 19, 14, 9, 4, 0
};

/* Finer index for a macroblock the 16×16 predictors cannot explain.
 * Susceptibility 96 on the same curve, at strength 50. Effort 4 is 18. */
static const int k_fine_q[10] = {
  50, 34, 27, 21, 18, 13, 9, 6, 2, 0
};

/* Residual sum of squares, over the macroblock, above which the finer
 * index is worth offering. A smooth ramp and a flat gray field stay on
 * the frame quantizer. */
enum { VP8_FINE_SSE = 80000 };

typedef struct {
  int range;
  int32_t value;
  int run;
  int nb_bits;
  uint8_t * buf;
  size_t pos;
  size_t max_pos;
  const GIMG_Allocator * alloc;
  int error;
} bool_writer_t;

static int bw_resize(bool_writer_t * bw, size_t extra) {
  size_t need = bw->pos + extra;
  size_t ncap;
  uint8_t * nbuf;
  if (bw->error) {
    return 0;
  }
  if (need <= bw->max_pos) {
    return 1;
  }
  ncap = bw->max_pos ? bw->max_pos * 2u : 1024u;
  if (ncap < need) {
    ncap = need;
  }
  nbuf = (uint8_t *)gimg_realloc(bw->alloc, bw->buf, ncap);
  if (!nbuf) {
    bw->error = 1;
    return 0;
  }
  bw->buf = nbuf;
  bw->max_pos = ncap;
  return 1;
}

static void bw_flush(bool_writer_t * bw) {
  const int s = 8 + bw->nb_bits;
  const int32_t bits = bw->value >> s;
  bw->value -= bits << s;
  bw->nb_bits -= 8;
  if ((bits & 0xff) != 0xff) {
    size_t pos = bw->pos;
    if (!bw_resize(bw, (size_t)bw->run + 1u)) {
      return;
    }
    if (bits & 0x100) {
      if (pos > 0u) {
        bw->buf[pos - 1u]++;
      }
    }
    if (bw->run > 0) {
      const int value = (bits & 0x100) ? 0x00 : 0xff;
      for (; bw->run > 0; --bw->run) {
        bw->buf[pos++] = (uint8_t)value;
      }
    }
    bw->buf[pos++] = (uint8_t)(bits & 0xff);
    bw->pos = pos;
  }
  else {
    bw->run++;
  }
}

/* Shift until (range + 1) is at least 128. The new range is that
 * span minus one, which is the encoder side of the section 7 range. */
static void bw_renorm(bool_writer_t * bw) {
  int span = bw->range + 1;
  int shift = 0;
  while (span < 128) {
    span <<= 1;
    shift++;
  }
  bw->range = span - 1;
  bw->value <<= shift;
  bw->nb_bits += shift;
  if (bw->nb_bits > 0) {
    bw_flush(bw);
  }
}

static void bw_init(bool_writer_t * bw, const GIMG_Allocator * alloc) {
  memset(bw, 0, sizeof(*bw));
  bw->range = 255 - 1;
  bw->nb_bits = -8;
  bw->alloc = alloc;
}

static int bw_put_bit(bool_writer_t * bw, int bit, int prob) {
  const int split = (bw->range * prob) >> 8;
  if (bit) {
    bw->value += split + 1;
    bw->range -= split + 1;
  }
  else {
    bw->range = split;
  }
  if (bw->range < 127) {
    bw_renorm(bw);
  }
  return bit;
}

static int bw_put_bit_uniform(bool_writer_t * bw, int bit) {
  const int split = bw->range >> 1;
  if (bit) {
    bw->value += split + 1;
    bw->range -= split + 1;
  }
  else {
    bw->range = split;
  }
  if (bw->range < 127) {
    bw_renorm(bw);
  }
  return bit;
}

static void bw_put_bits(bool_writer_t * bw, uint32_t value, int nb_bits) {
  uint32_t mask;
  for (mask = 1u << (nb_bits - 1); mask; mask >>= 1) {
    bw_put_bit_uniform(bw, (value & mask) != 0);
  }
}

static int bw_finish(bool_writer_t * bw) {
  bw_put_bits(bw, 0, 9 - bw->nb_bits);
  bw->nb_bits = 0;
  bw_flush(bw);
  return !bw->error;
}

static int clip127(int v) {
  return v < 0 ? 0 : v > 127 ? 127 : v;
}

static int clip8(int v) {
  return ((v & ~0xff) == 0) ? v : (v < 0) ? 0 : 255;
}

static int rgb_to_y(int r, int g, int b) {
  const int luma = VP8_Y_R * r + VP8_Y_G * g + VP8_Y_B * b;
  return clip8((luma + YUV_HALF + (16 << YUV_FIX)) >> YUV_FIX);
}

/* r, g, b are the sum of n samples. n is 1, 2, or 4. The extra shift
 * past 2^16 is that average. */
static int rgb_to_uv(int cr, int cg, int cb, int r, int g, int b, int n) {
  int shift = YUV_FIX;
  int samples = n;
  int uv;
  int rounding;
  while (samples > 1) {
    shift++;
    samples >>= 1;
  }
  uv = cr * r + cg * g + cb * b;
  rounding = YUV_HALF << (shift - YUV_FIX);
  return clip8((uv + rounding + (128 << shift)) >> shift);
}

static void put_large_value(bool_writer_t * bw, const uint8_t * p, int v) {
  if (v == 2) {
    bw_put_bit(bw, 0, p[3]);
    bw_put_bit(bw, 0, p[4]);
  }
  else if (v == 3 || v == 4) {
    bw_put_bit(bw, 0, p[3]);
    bw_put_bit(bw, 1, p[4]);
    bw_put_bit(bw, v - 3, p[5]);
  }
  else if (v == 5 || v == 6) {
    bw_put_bit(bw, 1, p[3]);
    bw_put_bit(bw, 0, p[6]);
    bw_put_bit(bw, 0, p[7]);
    bw_put_bit(bw, v - 5, gimg_vp8_pcat1[0]);
  }
  else if (v >= 7 && v <= 10) {
    int rem = v - 7;
    bw_put_bit(bw, 1, p[3]);
    bw_put_bit(bw, 0, p[6]);
    bw_put_bit(bw, 1, p[7]);
    bw_put_bit(bw, (rem >> 1) & 1, gimg_vp8_pcat2[0]);
    bw_put_bit(bw, rem & 1, gimg_vp8_pcat2[1]);
  }
  else {
    int cat;
    int base;
    int bits;
    int i;
    const uint8_t * tab;
    bw_put_bit(bw, 1, p[3]);
    bw_put_bit(bw, 1, p[6]);
    if (v < 19) {
      cat = 0;
      base = 11;
      bits = 3;
    }
    else if (v < 35) {
      cat = 1;
      base = 19;
      bits = 4;
    }
    else if (v < 67) {
      cat = 2;
      base = 35;
      bits = 5;
    }
    else {
      cat = 3;
      base = 67;
      bits = 11;
    }
    bw_put_bit(bw, (cat >> 1) & 1, p[8]);
    bw_put_bit(bw, cat & 1, p[9 + ((cat >> 1) & 1)]);
    tab = k_cat3456[cat];
    {
      int rem = v - base;
      for (i = 0; i < bits; ++i) {
        const int shift = bits - 1 - i;
        bw_put_bit(bw, (rem >> shift) & 1, tab[i]);
      }
    }
  }
}

static int quantize(int coeff, int q) {
  int abs_c;
  int level;
  if (q <= 0) {
    q = 1;
  }
  abs_c = coeff < 0 ? -coeff : coeff;
  level = (abs_c + (q >> 1)) / q;
  if (level > 2047) {
    level = 2047;
  }
  return coeff < 0 ? -level : level;
}

/* Scan position to raster position. The same order section 14's inverse
 * reads back. */
static const uint8_t k_zigzag[16] = {
  0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
};

/** Pointer to a [type][band][ctx][prob] coefficient table. */
typedef uint8_t (*vp8_prob_set)[GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
    [GIMG_VP8_NUM_PROBAS];

typedef uint8_t vp8_prob_table[GIMG_VP8_NUM_TYPES][GIMG_VP8_NUM_BANDS]
    [GIMG_VP8_NUM_CTX][GIMG_VP8_NUM_PROBAS];

/** Counts of the section 13 bools. Category extra bits and the sign
 *  stay on their fixed probabilities and are not counted. */
typedef struct {
  uint32_t n0[GIMG_VP8_NUM_TYPES][GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
      [GIMG_VP8_NUM_PROBAS];
  uint32_t n1[GIMG_VP8_NUM_TYPES][GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
      [GIMG_VP8_NUM_PROBAS];
} vp8_counts;

/* One dimension of the section 14.4 inverse butterfly, run backwards.
 * The two unscaled passes grow by sixteen; the inverse's final shift of
 * three is an eight, so the samples are shifted up by three first and
 * the coefficients are rounded down by four. */
static void fwd_1d(int o0, int o1, int o2, int o3, int * i0, int * i1,
    int * i2, int * i3) {
  const int a1 = o0 + o3;
  const int d1 = o0 - o3;
  const int b1 = o1 + o2;
  const int c1 = o1 - o2;
  *i0 = a1 + b1;
  *i2 = a1 - b1;
  *i1 = ((c1 * 35468) >> 16) + d1 + ((d1 * 20091) >> 16);
  *i3 = ((d1 * 35468) >> 16) - (c1 + ((c1 * 20091) >> 16));
}

static void fdct4(const int * sp, int * out) {
  int src[16];
  int tmp[16];
  int r;
  int c;
  for (r = 0; r < 16; ++r) {
    src[r] = sp[r] << 3;
  }
  for (r = 0; r < 4; ++r) {
    fwd_1d(src[r * 4], src[r * 4 + 1], src[r * 4 + 2], src[r * 4 + 3],
        &tmp[r * 4], &tmp[r * 4 + 1], &tmp[r * 4 + 2], &tmp[r * 4 + 3]);
  }
  for (c = 0; c < 4; ++c) {
    int i0, i1, i2, i3;
    fwd_1d(tmp[c], tmp[4 + c], tmp[8 + c], tmp[12 + c], &i0, &i1, &i2, &i3);
    out[c] = (i0 + 8) >> 4;
    out[4 + c] = (i1 + 8) >> 4;
    out[8 + c] = (i2 + 8) >> 4;
    out[12 + c] = (i3 + 8) >> 4;
  }
}

/** Section 14.4, so a reconstructed block is what the decoder adds. */
static void idct4(const int * in, int * out) {
  int tmp[16];
  int i;
  for (i = 0; i < 4; ++i) {
    const int i0 = in[i];
    const int i1 = in[4 + i];
    const int i2 = in[8 + i];
    const int i3 = in[12 + i];
    const int a1 = i0 + i2;
    const int b1 = i0 - i2;
    int t1 = (i1 * 35468) >> 16;
    int t2 = i3 + ((i3 * 20091) >> 16);
    const int c1 = t1 - t2;
    int d1;
    t1 = i1 + ((i1 * 20091) >> 16);
    t2 = (i3 * 35468) >> 16;
    d1 = t1 + t2;
    tmp[i] = a1 + d1;
    tmp[12 + i] = a1 - d1;
    tmp[4 + i] = b1 + c1;
    tmp[8 + i] = b1 - c1;
  }
  for (i = 0; i < 4; ++i) {
    const int i0 = tmp[i * 4];
    const int i1 = tmp[i * 4 + 1];
    const int i2 = tmp[i * 4 + 2];
    const int i3 = tmp[i * 4 + 3];
    const int a1 = i0 + i2;
    const int b1 = i0 - i2;
    int t1 = (i1 * 35468) >> 16;
    int t2 = i3 + ((i3 * 20091) >> 16);
    const int c1 = t1 - t2;
    int d1;
    t1 = i1 + ((i1 * 20091) >> 16);
    t2 = (i3 * 35468) >> 16;
    d1 = t1 + t2;
    out[i * 4] = (a1 + d1 + 4) >> 3;
    out[i * 4 + 3] = (a1 - d1 + 4) >> 3;
    out[i * 4 + 1] = (b1 + c1 + 4) >> 3;
    out[i * 4 + 2] = (b1 - c1 + 4) >> 3;
  }
}

/* Section 14.3's butterfly. Two passes multiply by sixteen and the
 * inverse shifts by three, so the forward shifts by one. */
static void fwht4(const int * in, int * out) {
  int tmp[16];
  int i;
  for (i = 0; i < 4; ++i) {
    const int i0 = in[i];
    const int i1 = in[4 + i];
    const int i2 = in[8 + i];
    const int i3 = in[12 + i];
    const int a1 = i0 + i3;
    const int b1 = i1 + i2;
    const int c1 = i1 - i2;
    const int d1 = i0 - i3;
    tmp[i] = a1 + b1;
    tmp[4 + i] = c1 + d1;
    tmp[8 + i] = a1 - b1;
    tmp[12 + i] = d1 - c1;
  }
  for (i = 0; i < 4; ++i) {
    const int i0 = tmp[i * 4];
    const int i1 = tmp[i * 4 + 1];
    const int i2 = tmp[i * 4 + 2];
    const int i3 = tmp[i * 4 + 3];
    const int a1 = i0 + i3;
    const int b1 = i1 + i2;
    const int c1 = i1 - i2;
    const int d1 = i0 - i3;
    out[i * 4] = (a1 + b1) >> 1;
    out[i * 4 + 1] = (c1 + d1) >> 1;
    out[i * 4 + 2] = (a1 - b1) >> 1;
    out[i * 4 + 3] = (d1 - c1) >> 1;
  }
}

/** Section 14.3. */
static void iwht4(const int * in, int * out) {
  int tmp[16];
  int i;
  for (i = 0; i < 4; ++i) {
    const int i0 = in[i];
    const int i1 = in[4 + i];
    const int i2 = in[8 + i];
    const int i3 = in[12 + i];
    const int a1 = i0 + i3;
    const int b1 = i1 + i2;
    const int c1 = i1 - i2;
    const int d1 = i0 - i3;
    tmp[i] = a1 + b1;
    tmp[4 + i] = c1 + d1;
    tmp[8 + i] = a1 - b1;
    tmp[12 + i] = d1 - c1;
  }
  for (i = 0; i < 4; ++i) {
    const int i0 = tmp[i * 4];
    const int i1 = tmp[i * 4 + 1];
    const int i2 = tmp[i * 4 + 2];
    const int i3 = tmp[i * 4 + 3];
    const int a1 = i0 + i3;
    const int b1 = i1 + i2;
    const int c1 = i1 - i2;
    const int d1 = i0 - i3;
    const int a2 = a1 + b1;
    const int b2 = c1 + d1;
    const int c2 = a1 - b1;
    const int d2 = d1 - c1;
    out[i * 4] = (a2 + 3) >> 3;
    out[i * 4 + 1] = (b2 + 3) >> 3;
    out[i * 4 + 2] = (c2 + 3) >> 3;
    out[i * 4 + 3] = (d2 + 3) >> 3;
  }
}

static int src_at(const uint8_t * plane, int stride, int x, int y, int w,
    int h) {
  if (w <= 0 || h <= 0) {
    return 128;
  }
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
  return plane[y * stride + x];
}

/** Section 12. DC prediction, reading the reconstructed plane. */
static int dc_pred(const uint8_t * plane, int stride, int x, int y, int n) {
  int sum = 0;
  int i;
  const int shift = (n == 16) ? 4 : 3;
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

/* Section 12. Outside the frame: 127 above, 129 to the left. The corner
 * above a left edge is 129 except at the top-left of the frame, which
 * is 127. */
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

/** Sections 12.2 and 12.3. @a pred is n by n, row-major. */
static void fill_pred(const uint8_t * rec, int stride, int x, int y, int n,
    int mode, uint8_t * pred) {
  uint8_t above[16];
  uint8_t left[16];
  int p;
  int r;
  int c;
  if (mode == 0) {
    const int dc = dc_pred(rec, stride, x, y, n);
    for (r = 0; r < n * n; ++r) {
      pred[r] = (uint8_t)dc;
    }
    return;
  }
  load_mb_edge(rec, stride, x, y, n, above, left, &p);
  for (r = 0; r < n; ++r) {
    for (c = 0; c < n; ++c) {
      int v;
      if (mode == 1) {
        v = above[c];
      }
      else if (mode == 2) {
        v = left[r];
      }
      else {
        v = (int)left[r] + (int)above[c] - p;
      }
      pred[r * n + c] = (uint8_t)clip8(v);
    }
  }
}

/** Minimum squared error of the DC, vertical and horizontal predictors,
 *  read from the source. Padding outside the frame counts as zero. */
static int pred_energy(const uint8_t * src, int stride, int bx, int by,
    int lim_w, int lim_h) {
  int mode;
  int best = -1;
  for (mode = 0; mode < 3; ++mode) {
    uint8_t pred[256];
    int sum = 0;
    int r;
    int c;
    fill_pred(src, stride, bx, by, 16, mode, pred);
    for (r = 0; r < 16; ++r) {
      for (c = 0; c < 16; ++c) {
        const int yy = by + r;
        const int xx = bx + c;
        const int s = (yy < lim_h && xx < lim_w) ? src[yy * stride + xx] : 0;
        const int d = s - (int)pred[r * 16 + c];
        sum += d * d;
      }
    }
    if (best < 0 || sum < best) {
      best = sum;
    }
  }
  return best;
}

static int64_t sse_rect(const uint8_t * rec, int rec_stride,
    const uint8_t * src, int src_stride, int x, int y, int n, int lim_w,
    int lim_h) {
  int64_t sum = 0;
  int r;
  int c;
  for (r = 0; r < n; ++r) {
    const int yy = y + r;
    if (yy >= lim_h) {
      break;
    }
    for (c = 0; c < n; ++c) {
      const int xx = x + c;
      int d;
      if (xx >= lim_w) {
        break;
      }
      d = (int)rec[yy * rec_stride + xx] - (int)src[yy * src_stride + xx];
      sum += (int64_t)d * (int64_t)d;
    }
  }
  return sum;
}

/* Keyframe luma tree, section 8. Bit 0 at 145 is B_PRED. DC and V share
 * the 156/163 branch; H and TM share the other. */
static void write_ymode(bool_writer_t * bw, int mode) {
  bw_put_bit(bw, 1, 145);
  if (mode == 0 || mode == 1) {
    bw_put_bit(bw, 0, 156);
    bw_put_bit(bw, mode == 1, 163);
  }
  else {
    bw_put_bit(bw, 1, 156);
    bw_put_bit(bw, mode == 3, 128);
  }
}

/* Section 11.2 subblock tree. The walk matches the decoder's
 * k_bmode_tree: 0 DC, 1 TM, 2 VE, then HE/RD/VR on one branch and
 * LD/VL/HD/HU on the other. */
static void write_bmode(bool_writer_t * bw, int mode, const uint8_t * p) {
  if (mode == 0) {
    bw_put_bit(bw, 0, p[0]);
    return;
  }
  bw_put_bit(bw, 1, p[0]);
  if (mode == 1) {
    bw_put_bit(bw, 0, p[1]);
    return;
  }
  bw_put_bit(bw, 1, p[1]);
  if (mode == 2) {
    bw_put_bit(bw, 0, p[2]);
    return;
  }
  bw_put_bit(bw, 1, p[2]);
  if (mode == 3) {
    bw_put_bit(bw, 0, p[3]);
    bw_put_bit(bw, 0, p[4]);
    return;
  }
  if (mode == 5 || mode == 6) {
    bw_put_bit(bw, 0, p[3]);
    bw_put_bit(bw, 1, p[4]);
    bw_put_bit(bw, mode == 6, p[5]);
    return;
  }
  bw_put_bit(bw, 1, p[3]);
  if (mode == 4) {
    bw_put_bit(bw, 0, p[6]);
    return;
  }
  bw_put_bit(bw, 1, p[6]);
  if (mode == 7) {
    bw_put_bit(bw, 0, p[7]);
    return;
  }
  bw_put_bit(bw, 1, p[7]);
  bw_put_bit(bw, mode == 9, p[8]);
}

/* Section 8 chroma tree. 0 DC, 1 V, 2 H, 3 TM. */
static void write_uvmode(bool_writer_t * bw, int mode) {
  if (mode == 0) {
    bw_put_bit(bw, 0, 142);
    return;
  }
  bw_put_bit(bw, 1, 142);
  if (mode == 1) {
    bw_put_bit(bw, 0, 114);
    return;
  }
  bw_put_bit(bw, 1, 114);
  bw_put_bit(bw, mode == 3, 183);
}

static void put_eob(bool_writer_t * bw, const uint8_t * p) {
  bw_put_bit(bw, 0, p[0]);
}

static void put_coeff(bool_writer_t * bw, const uint8_t * p, int level,
    int skip_eob) {
  int mag;
  if (!skip_eob) {
    bw_put_bit(bw, 1, p[0]);
  }
  if (level == 0) {
    bw_put_bit(bw, 0, p[1]);
    return;
  }
  bw_put_bit(bw, 1, p[1]);
  mag = level < 0 ? -level : level;
  if (mag == 1) {
    bw_put_bit(bw, 0, p[2]);
  }
  else {
    bw_put_bit(bw, 1, p[2]);
    put_large_value(bw, p, mag);
  }
  bw_put_bit_uniform(bw, level < 0);
}

/** Levels are raster order. @a first is 0, or 1 when the DC lives in Y2.
 *  Returns 1 when any emitted level is nonzero. */
static int put_block(bool_writer_t * bw,
    uint8_t bands[GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
        [GIMG_VP8_NUM_PROBAS],
    int ctx, int first, const int * levels) {
  int last = first - 1;
  int i;
  int skip_eob = 0;
  for (i = first; i < 16; ++i) {
    if (levels[k_zigzag[i]] != 0) {
      last = i;
    }
  }
  if (last < first) {
    put_eob(bw, bands[gimg_vp8_bands[first]][ctx]);
    return 0;
  }
  for (i = first; i <= last; ++i) {
    const int level = levels[k_zigzag[i]];
    const uint8_t * p = bands[gimg_vp8_bands[i]][ctx];
    put_coeff(bw, p, level, skip_eob);
    if (level == 0) {
      skip_eob = 1;
      ctx = 0;
    }
    else {
      skip_eob = 0;
      ctx = (level == 1 || level == -1) ? 1 : 2;
    }
  }
  if (last < 15) {
    put_eob(bw, bands[gimg_vp8_bands[last + 1]][ctx]);
  }
  return 1;
}

/* Cost of one section 7 bool, in 1/256 of a bit. @a prob is P(0) on
 * the 0..255 scale the coder uses. The table is round(-256*log2(i/256));
 * index 0 is unused because a probability of 0 is not a legal split. */
static const uint16_t k_bool_cost[256] = {
       0, 2048, 1792, 1642, 1536, 1454, 1386, 1329, 1280, 1236, 1198, 1162, 1130, 1101, 1073, 1048,
    1024, 1002,  980,  961,  942,  924,  906,  890,  874,  859,  845,  831,  817,  804,  792,  780,
     768,  757,  746,  735,  724,  714,  705,  695,  686,  676,  668,  659,  650,  642,  634,  626,
     618,  611,  603,  596,  589,  582,  575,  568,  561,  555,  548,  542,  536,  530,  524,  518,
     512,  506,  501,  495,  490,  484,  479,  474,  468,  463,  458,  453,  449,  444,  439,  434,
     430,  425,  420,  416,  412,  407,  403,  399,  394,  390,  386,  382,  378,  374,  370,  366,
     362,  358,  355,  351,  347,  343,  340,  336,  333,  329,  326,  322,  319,  315,  312,  309,
     305,  302,  299,  296,  292,  289,  286,  283,  280,  277,  274,  271,  268,  265,  262,  259,
     256,  253,  250,  247,  245,  242,  239,  236,  234,  231,  228,  226,  223,  220,  218,  215,
     212,  210,  207,  205,  202,  200,  197,  195,  193,  190,  188,  185,  183,  181,  178,  176,
     174,  171,  169,  167,  164,  162,  160,  158,  156,  153,  151,  149,  147,  145,  143,  140,
     138,  136,  134,  132,  130,  128,  126,  124,  122,  120,  118,  116,  114,  112,  110,  108,
     106,  104,  102,  101,   99,   97,   95,   93,   91,   89,   87,   86,   84,   82,   80,   78,
      77,   75,   73,   71,   70,   68,   66,   64,   63,   61,   59,   58,   56,   54,   53,   51,
      49,   48,   46,   44,   43,   41,   40,   38,   36,   35,   33,   32,   30,   28,   27,   25,
      24,   22,   21,   19,   18,   16,   15,   13,   12,   10,    9,    7,    6,    4,    3,    1
};

static int bool_cost(int bit, int prob) {
  int p = bit ? 256 - prob : prob;
  if (p < 1) {
    p = 1;
  }
  if (p > 255) {
    p = 255;
  }
  return (int)k_bool_cost[p];
}

static int bmode_cost(int mode, const uint8_t * p) {
  int cost = 0;
  if (mode == 0) {
    return bool_cost(0, p[0]);
  }
  cost = bool_cost(1, p[0]);
  if (mode == 1) {
    return cost + bool_cost(0, p[1]);
  }
  cost += bool_cost(1, p[1]);
  if (mode == 2) {
    return cost + bool_cost(0, p[2]);
  }
  cost += bool_cost(1, p[2]);
  if (mode == 3) {
    return cost + bool_cost(0, p[3]) + bool_cost(0, p[4]);
  }
  if (mode == 5 || mode == 6) {
    return cost + bool_cost(0, p[3]) + bool_cost(1, p[4]) +
        bool_cost(mode == 6, p[5]);
  }
  cost += bool_cost(1, p[3]);
  if (mode == 4) {
    return cost + bool_cost(0, p[6]);
  }
  cost += bool_cost(1, p[6]);
  if (mode == 7) {
    return cost + bool_cost(0, p[7]);
  }
  return cost + bool_cost(1, p[7]) + bool_cost(mode == 9, p[8]);
}

static int large_cost(const uint8_t * p, int v) {
  int cost;
  if (v == 2) {
    return bool_cost(0, p[3]) + bool_cost(0, p[4]);
  }
  if (v == 3 || v == 4) {
    return bool_cost(0, p[3]) + bool_cost(1, p[4]) + bool_cost(v - 3, p[5]);
  }
  if (v == 5 || v == 6) {
    return bool_cost(1, p[3]) + bool_cost(0, p[6]) + bool_cost(0, p[7]) +
        bool_cost(v - 5, gimg_vp8_pcat1[0]);
  }
  if (v >= 7 && v <= 10) {
    const int rem = v - 7;
    return bool_cost(1, p[3]) + bool_cost(0, p[6]) + bool_cost(1, p[7]) +
        bool_cost((rem >> 1) & 1, gimg_vp8_pcat2[0]) +
        bool_cost(rem & 1, gimg_vp8_pcat2[1]);
  }
  {
    int cat;
    int base;
    int bits;
    int i;
    const uint8_t * tab;
    int rem;
    cost = bool_cost(1, p[3]) + bool_cost(1, p[6]);
    if (v < 19) {
      cat = 0;
      base = 11;
      bits = 3;
    }
    else if (v < 35) {
      cat = 1;
      base = 19;
      bits = 4;
    }
    else if (v < 67) {
      cat = 2;
      base = 35;
      bits = 5;
    }
    else {
      cat = 3;
      base = 67;
      bits = 11;
    }
    cost += bool_cost((cat >> 1) & 1, p[8]);
    cost += bool_cost(cat & 1, p[9 + ((cat >> 1) & 1)]);
    tab = k_cat3456[cat];
    rem = v - base;
    for (i = 0; i < bits; ++i) {
      const int shift = bits - 1 - i;
      cost += bool_cost((rem >> shift) & 1, tab[i]);
    }
  }
  return cost;
}

static int coeff_cost(const uint8_t * p, int level, int skip_eob) {
  int cost = 0;
  int mag;
  if (!skip_eob) {
    cost += bool_cost(1, p[0]);
  }
  if (level == 0) {
    return cost + bool_cost(0, p[1]);
  }
  cost += bool_cost(1, p[1]);
  mag = level < 0 ? -level : level;
  if (mag == 1) {
    cost += bool_cost(0, p[2]);
  }
  else {
    cost += bool_cost(1, p[2]);
    cost += large_cost(p, mag);
  }
  /* The sign is a uniform bool. */
  cost += bool_cost(level < 0, 128);
  return cost;
}

/** Same walk as put_block. Cost is in 1/256 of a bit. */
static int block_bit_cost(
    uint8_t bands[GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
        [GIMG_VP8_NUM_PROBAS],
    int ctx, int first, const int * levels) {
  int last = first - 1;
  int i;
  int skip_eob = 0;
  int cost = 0;
  for (i = first; i < 16; ++i) {
    if (levels[k_zigzag[i]] != 0) {
      last = i;
    }
  }
  if (last < first) {
    return bool_cost(0, bands[gimg_vp8_bands[first]][ctx][0]);
  }
  for (i = first; i <= last; ++i) {
    const int level = levels[k_zigzag[i]];
    const uint8_t * p = bands[gimg_vp8_bands[i]][ctx];
    cost += coeff_cost(p, level, skip_eob);
    if (level == 0) {
      skip_eob = 1;
      ctx = 0;
    }
    else {
      skip_eob = 0;
      ctx = (level == 1 || level == -1) ? 1 : 2;
    }
  }
  if (last < 15) {
    cost += bool_cost(0, bands[gimg_vp8_bands[last + 1]][ctx][0]);
  }
  return cost;
}

static int ymode_cost(int mode) {
  int cost = bool_cost(1, 145);
  if (mode == 0 || mode == 1) {
    cost += bool_cost(0, 156);
    cost += bool_cost(mode == 1, 163);
  }
  else {
    cost += bool_cost(1, 156);
    cost += bool_cost(mode == 3, 128);
  }
  return cost;
}

static int uvmode_cost(int mode) {
  if (mode == 0) {
    return bool_cost(0, 142);
  }
  if (mode == 1) {
    return bool_cost(1, 142) + bool_cost(0, 114);
  }
  return bool_cost(1, 142) + bool_cost(1, 114) + bool_cost(mode == 3, 183);
}

static int block_nz(const int * levels, int first) {
  int i;
  for (i = first; i < 16; ++i) {
    if (levels[k_zigzag[i]] != 0) {
      return 1;
    }
  }
  return 0;
}

/** Pixel error of one inverse, ignoring the clip the painter applies. */
static int trellis_sse(const int * residual, int first, int dc_override,
    int dc_q, int ac_q, const int * levels) {
  int deq[16];
  int pix[16];
  int i;
  int sse = 0;
  deq[0] = (first == 0) ? levels[0] * dc_q : dc_override;
  for (i = 1; i < 16; ++i) {
    deq[i] = levels[i] * ac_q;
  }
  idct4(deq, pix);
  for (i = 0; i < 16; ++i) {
    const int d = residual[i] - pix[i];
    sse += d * d;
  }
  return sse;
}

/** One backward pass. Each coefficient may stay, shorten by one, or
 *  become zero. A tie keeps the deadzone level, which was tried first. */
static void trellis_dct(const int * residual, int dc_override, int dc_q,
    int ac_q, int first,
    uint8_t bands[GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
        [GIMG_VP8_NUM_PROBAS],
    int ctx, int lambda, int * levels) {
  int pos;
  for (pos = 15; pos >= first; --pos) {
    const int idx = (int)k_zigzag[pos];
    const int cur = levels[idx];
    const int mag = cur < 0 ? -cur : cur;
    const int sign = cur < 0 ? -1 : 1;
    int cand[3];
    int n = 0;
    int best_level = cur;
    int64_t best_score = -1;
    int k;
    if (mag == 0) {
      continue;
    }
    cand[n++] = cur;
    cand[n++] = 0;
    if (mag > 1) {
      cand[n++] = sign * (mag - 1);
    }
    for (k = 0; k < n; ++k) {
      int64_t score;
      levels[idx] = cand[k];
      score = (int64_t)trellis_sse(residual, first, dc_override, dc_q, ac_q,
                   levels) *
              256 +
          (int64_t)block_bit_cost(bands, ctx, first, levels) * lambda;
      if (best_score < 0 || score < best_score) {
        best_score = score;
        best_level = cand[k];
      }
    }
    levels[idx] = best_level;
  }
}

/** Token cost of one Intra16 macroblock, given the neighbors already chosen. */
static int i16_residual_cost(int ac_levels[16][16], const int * y2_level,
    const uint8_t * y_nz, int y4_stride, int mb_x, int mb_y, int y2_left,
    int y2_above, vp8_prob_set probs) {
  int local[16];
  int cost;
  int sy;
  int sx;
  int ctx = (y2_left ? 1 : 0) + (y2_above ? 1 : 0);
  cost = block_bit_cost(probs[1], ctx, 0, y2_level);
  for (sy = 0; sy < 4; ++sy) {
    for (sx = 0; sx < 4; ++sx) {
      const int fx = mb_x * 4 + sx;
      const int fy = mb_y * 4 + sy;
      const int bi = sy * 4 + sx;
      ctx = 0;
      if (sy > 0) {
        ctx += local[(sy - 1) * 4 + sx];
      }
      else if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
        ctx++;
      }
      if (sx > 0) {
        ctx += local[sy * 4 + (sx - 1)];
      }
      else if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
        ctx++;
      }
      local[bi] = block_nz(ac_levels[bi], 1);
      cost += block_bit_cost(probs[0], ctx, 1, ac_levels[bi]);
    }
  }
  return cost;
}

static int uv_residual_cost(int levels_u[4][16], int levels_v[4][16],
    const uint8_t * u_nz, const uint8_t * v_nz, int uv4_stride, int mb_x,
    int mb_y, vp8_prob_set probs) {
  int cost = 0;
  int ch;
  for (ch = 0; ch < 2; ++ch) {
    const uint8_t * nz_plane = (ch == 0) ? u_nz : v_nz;
    int (*levels)[16] = (ch == 0) ? levels_u : levels_v;
    int local[4];
    int sy;
    int sx;
    for (sy = 0; sy < 2; ++sy) {
      for (sx = 0; sx < 2; ++sx) {
        const int fx = mb_x * 2 + sx;
        const int fy = mb_y * 2 + sy;
        const int bi = sy * 2 + sx;
        int ctx = 0;
        if (sy > 0) {
          ctx += local[(sy - 1) * 2 + sx];
        }
        else if (fy > 0 && nz_plane[(fy - 1) * uv4_stride + fx]) {
          ctx++;
        }
        if (sx > 0) {
          ctx += local[sy * 2 + (sx - 1)];
        }
        else if (fx > 0 && nz_plane[fy * uv4_stride + (fx - 1)]) {
          ctx++;
        }
        local[bi] = block_nz(levels[bi], 0);
        cost += block_bit_cost(probs[2], ctx, 0, levels[bi]);
      }
    }
  }
  return cost;
}

/** Section 14.1 steps for one quantizer index, including the decoder's
 *  Y2 AC floor and UV DC cap. */
static void mb_quants(int q_index, int * y1_dc, int * y1_ac, int * y2_dc,
    int * y2_ac, int * uv_dc, int * uv_ac) {
  const int q = clip127(q_index);
  int y2ac;
  int uvdc;
  *y1_dc = gimg_vp8_dc_qlookup[q];
  *y1_ac = (int)gimg_vp8_ac_qlookup[q];
  *y2_dc = gimg_vp8_dc_qlookup[q] * 2;
  y2ac = (int)gimg_vp8_ac_qlookup[q] * 155 / 100;
  if (y2ac < 8) {
    y2ac = 8;
  }
  *y2_ac = y2ac;
  uvdc = gimg_vp8_dc_qlookup[q];
  if (uvdc > 132) {
    uvdc = 132;
  }
  *uv_dc = uvdc;
  *uv_ac = (int)gimg_vp8_ac_qlookup[q];
}

/** Section 9.3 signed literal. A zero omits the magnitude. The sign bit
 *  is 1 when the value is negative. */
static void write_signed_value(bool_writer_t * bw, int value, int nbits) {
  int mag;
  if (value == 0) {
    bw_put_bit_uniform(bw, 0);
    return;
  }
  bw_put_bit_uniform(bw, 1);
  mag = value < 0 ? -value : value;
  bw_put_bits(bw, (uint32_t)mag, nbits);
  bw_put_bit_uniform(bw, value < 0);
}

/** Section 10 tree. @a prob is P(zero) for the two branch levels. */
static void write_segment_id(bool_writer_t * bw, int id, const int prob[3]) {
  if (id < 2) {
    bw_put_bit(bw, 0, prob[0]);
    bw_put_bit(bw, id == 1, prob[1]);
  }
  else {
    bw_put_bit(bw, 1, prob[0]);
    bw_put_bit(bw, id == 3, prob[2]);
  }
}

static void write_frame_header(bool_writer_t * part0, int base_q, int seg_on,
    int q_delta, const int seg_prob[3], int filter_level, int skip_on,
    int skip_prob, vp8_prob_set probs) {
  int t, b, c, p;
  int s;
  /* keyframe: colorspace=0 (YUV), clamp=0 */
  bw_put_bit_uniform(part0, 0);
  bw_put_bit_uniform(part0, 0);
  bw_put_bit_uniform(part0, seg_on ? 1 : 0);
  if (seg_on) {
    bw_put_bit_uniform(part0, 1); /* update the map */
    bw_put_bit_uniform(part0, 1); /* update the quantizers */
    bw_put_bit_uniform(part0, 0); /* deltas, added to the frame index */
    write_signed_value(part0, 0, 7);
    write_signed_value(part0, q_delta, 7);
    write_signed_value(part0, 0, 7);
    write_signed_value(part0, 0, 7);
    for (s = 0; s < 4; ++s) {
      write_signed_value(part0, 0, 6); /* loop filter left at 0 */
    }
    for (s = 0; s < 3; ++s) {
      bw_put_bit_uniform(part0, 1);
      bw_put_bits(part0, (uint32_t)seg_prob[s], 8);
    }
  }
  /* Section 15. Normal filter, sharpness 0, no mode or reference
   * delta. The level is fixed width, so 0 costs the same as any other. */
  bw_put_bit_uniform(part0, 0);
  bw_put_bits(part0, (uint32_t)filter_level, 6);
  bw_put_bits(part0, 0, 3);
  bw_put_bit_uniform(part0, 0);
  /* one coeff partition */
  bw_put_bits(part0, 0, 2);
  /* quantizer */
  bw_put_bits(part0, (uint32_t)base_q, 7);
  bw_put_bit_uniform(part0, 0); /* y1_dc */
  bw_put_bit_uniform(part0, 0); /* y2_dc */
  bw_put_bit_uniform(part0, 0); /* y2_ac */
  bw_put_bit_uniform(part0, 0); /* uv_dc */
  bw_put_bit_uniform(part0, 0); /* uv_ac */
  /* update_proba (ignored on keyframes) then keep default coeff probas */
  bw_put_bit_uniform(part0, 0);
  for (t = 0; t < GIMG_VP8_NUM_TYPES; ++t) {
    for (b = 0; b < GIMG_VP8_NUM_BANDS; ++b) {
      for (c = 0; c < GIMG_VP8_NUM_CTX; ++c) {
        for (p = 0; p < GIMG_VP8_NUM_PROBAS; ++p) {
          const int changed =
              probs[t][b][c][p] != gimg_vp8_coeffs_proba0[t][b][c][p];
          bw_put_bit(part0, changed, gimg_vp8_coeffs_update_proba[t][b][c][p]);
          if (changed) {
            bw_put_bits(part0, probs[t][b][c][p], 8);
          }
        }
      }
    }
  }
  /* Section 9.1. The literal is P(not skipped), which is P(bit is 0). */
  bw_put_bit_uniform(part0, skip_on ? 1 : 0);
  if (skip_on) {
    bw_put_bits(part0, (uint32_t)skip_prob, 8);
  }
}

/** Quantize one Intra16 macroblock into @a rec and return the visible SSE. */
static int64_t build_i16(const uint8_t * src, int src_stride, int src_w,
    int src_h, uint8_t * rec, int rec_stride, int bx, int by, int mode,
    int y1_ac_q, int y2_dc_q, int y2_ac_q, int ac_levels[16][16],
    int y2_level[16], const uint8_t * y_nz, int y4_stride,
    vp8_prob_set probs) {
  uint8_t pred[256];
  int dc_coeff[16];
  int wht[16];
  int spatial[16];
  int sy;
  int sx;
  int i;
  fill_pred(rec, rec_stride, bx, by, 16, mode, pred);
  for (sy = 0; sy < 4; ++sy) {
    for (sx = 0; sx < 4; ++sx) {
      int residual[16];
      int coeff[16];
      int dy;
      int dx;
      const int bi = sy * 4 + sx;
      for (dy = 0; dy < 4; ++dy) {
        for (dx = 0; dx < 4; ++dx) {
          const int py = sy * 4 + dy;
          const int px = sx * 4 + dx;
          residual[dy * 4 + dx] =
              src_at(src, src_stride, bx + px, by + py, src_w, src_h) -
              (int)pred[py * 16 + px];
        }
      }
      fdct4(residual, coeff);
      dc_coeff[bi] = coeff[0];
      ac_levels[bi][0] = 0;
      for (i = 1; i < 16; ++i) {
        ac_levels[bi][i] = quantize(coeff[i], y1_ac_q);
      }
    }
  }
  fwht4(dc_coeff, wht);
  for (i = 0; i < 16; ++i) {
    y2_level[i] = quantize(wht[i], i == 0 ? y2_dc_q : y2_ac_q);
  }
  for (i = 0; i < 16; ++i) {
    wht[i] = y2_level[i] * (i == 0 ? y2_dc_q : y2_ac_q);
  }
  iwht4(wht, spatial);
  {
    int local_nz[16];
    for (sy = 0; sy < 4; ++sy) {
    for (sx = 0; sx < 4; ++sx) {
      const int bi = sy * 4 + sx;
      const int fx = bx / 4 + sx;
      const int fy = by / 4 + sy;
      int ctx = 0;
      if (sy > 0) {
        ctx += local_nz[(sy - 1) * 4 + sx];
      }
      else if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
        ctx++;
      }
      if (sx > 0) {
        ctx += local_nz[sy * 4 + (sx - 1)];
      }
      else if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
        ctx++;
      }
      int block[16];
      int residue[16];
      int dy;
      int dx;
      {
        int residual[16];
        for (dy = 0; dy < 4; ++dy) {
          for (dx = 0; dx < 4; ++dx) {
            const int py = sy * 4 + dy;
            const int px = sx * 4 + dx;
            residual[dy * 4 + dx] =
                src_at(src, src_stride, bx + px, by + py, src_w, src_h) -
                (int)pred[py * 16 + px];
          }
        }
        trellis_dct(residual, spatial[bi], 0, y1_ac_q, 1, probs[0], ctx,
            y1_ac_q, ac_levels[bi]);
        local_nz[bi] = block_nz(ac_levels[bi], 1);
      }
      block[0] = spatial[bi];
      for (i = 1; i < 16; ++i) {
        block[i] = ac_levels[bi][i] * y1_ac_q;
      }
      idct4(block, residue);
      for (dy = 0; dy < 4; ++dy) {
        for (dx = 0; dx < 4; ++dx) {
          const int py = sy * 4 + dy;
          const int px = sx * 4 + dx;
          const int v = (int)pred[py * 16 + px] + residue[dy * 4 + dx];
          rec[(by + py) * rec_stride + bx + px] = (uint8_t)clip8(v);
        }
      }
    }
  }
  }
  return sse_rect(rec, rec_stride, src, src_stride, bx, by, 16, src_w, src_h);
}

/** Quantize both chroma planes with one mode. Returns U SSE plus V SSE. */
static int64_t build_uv(const uint8_t * src_u, const uint8_t * src_v,
    uint8_t * rec_u, uint8_t * rec_v, int stride, int bx, int by, int lim_w,
    int lim_h, int mode, int uv_dc_q, int uv_ac_q, int levels_u[4][16],
    int levels_v[4][16], const uint8_t * u_nz, const uint8_t * v_nz,
    int uv4_stride, vp8_prob_set probs) {
  const uint8_t * srcs[2] = { src_u, src_v };
  uint8_t * recs[2] = { rec_u, rec_v };
  int (*levels[2])[16] = { levels_u, levels_v };
  int64_t sse = 0;
  int ch;
  for (ch = 0; ch < 2; ++ch) {
    uint8_t pred[64];
    int local_nz[4];
    int sy;
    int sx;
    int i;
    const uint8_t * nz_plane = (ch == 0) ? u_nz : v_nz;
    fill_pred(recs[ch], stride, bx, by, 8, mode, pred);
    for (sy = 0; sy < 2; ++sy) {
      for (sx = 0; sx < 2; ++sx) {
        int residual[16];
        int coeff[16];
        int block[16];
        int residue[16];
        int dy;
        int dx;
        const int bi = sy * 2 + sx;
        for (dy = 0; dy < 4; ++dy) {
          for (dx = 0; dx < 4; ++dx) {
            const int py = sy * 4 + dy;
            const int px = sx * 4 + dx;
            residual[dy * 4 + dx] =
                src_at(srcs[ch], stride, bx + px, by + py, lim_w, lim_h) -
                (int)pred[py * 8 + px];
          }
        }
        fdct4(residual, coeff);
        for (i = 0; i < 16; ++i) {
          levels[ch][bi][i] = quantize(coeff[i], i == 0 ? uv_dc_q : uv_ac_q);
        }
        {
          const int fx = bx / 4 + sx;
          const int fy = by / 4 + sy;
          int ctx = 0;
          if (sy > 0) {
            ctx += local_nz[bi - 2];
          }
          else if (fy > 0 && nz_plane[(fy - 1) * uv4_stride + fx]) {
            ctx++;
          }
          if (sx > 0) {
            ctx += local_nz[bi - 1];
          }
          else if (fx > 0 && nz_plane[fy * uv4_stride + (fx - 1)]) {
            ctx++;
          }
          trellis_dct(residual, 0, uv_dc_q, uv_ac_q, 0, probs[2], ctx, uv_ac_q,
              levels[ch][bi]);
          local_nz[bi] = block_nz(levels[ch][bi], 0);
        }
        for (i = 0; i < 16; ++i) {
          block[i] = levels[ch][bi][i] * (i == 0 ? uv_dc_q : uv_ac_q);
        }
        idct4(block, residue);
        for (dy = 0; dy < 4; ++dy) {
          for (dx = 0; dx < 4; ++dx) {
            const int py = sy * 4 + dy;
            const int px = sx * 4 + dx;
            const int v = (int)pred[py * 8 + px] + residue[dy * 4 + dx];
            recs[ch][(by + py) * stride + bx + px] = (uint8_t)clip8(v);
          }
        }
      }
    }
    sse += sse_rect(recs[ch], stride, srcs[ch], stride, bx, by, 8, lim_w,
        lim_h);
  }
  return sse;
}

/** One 4×4 of a B_PRED macroblock. DC uses the luma DC quantizer. */
static int64_t build_b4(const uint8_t * src, int src_stride, int src_w,
    int src_h, uint8_t * rec, int rec_stride, int mb_w, int mb_x, int mb_y,
    int sx, int sy, int mode,     int y1_dc_q, int y1_ac_q, int levels[16], int ctx,
    vp8_prob_set probs) {
  uint8_t above[8];
  uint8_t left[4];
  uint8_t pred[16];
  int edge_p;
  int residual[16];
  int coeff[16];
  int block[16];
  int residue[16];
  int i;
  int dy;
  int dx;
  const int bx = mb_x * 16 + sx * 4;
  const int by = mb_y * 16 + sy * 4;
  gimg_vp8_load_b_edge(rec, rec_stride, mb_w, mb_x, mb_y, sx, bx, by, above,
      left, &edge_p);
  gimg_vp8_pred_b4(pred, above, left, edge_p, mode);
  for (dy = 0; dy < 4; ++dy) {
    for (dx = 0; dx < 4; ++dx) {
      residual[dy * 4 + dx] =
          src_at(src, src_stride, bx + dx, by + dy, src_w, src_h) -
          (int)pred[dy * 4 + dx];
    }
  }
  fdct4(residual, coeff);
  for (i = 0; i < 16; ++i) {
    const int q = (i == 0) ? y1_dc_q : y1_ac_q;
    levels[i] = quantize(coeff[i], q);
  }
  trellis_dct(residual, 0, y1_dc_q, y1_ac_q, 0, probs[3], ctx, y1_ac_q,
      levels);
  for (i = 0; i < 16; ++i) {
    const int q = (i == 0) ? y1_dc_q : y1_ac_q;
    block[i] = levels[i] * q;
  }
  idct4(block, residue);
  for (dy = 0; dy < 4; ++dy) {
    for (dx = 0; dx < 4; ++dx) {
      const int v = (int)pred[dy * 4 + dx] + residue[dy * 4 + dx];
      rec[(by + dy) * rec_stride + bx + dx] = (uint8_t)clip8(v);
    }
  }
  return sse_rect(rec, rec_stride, src, src_stride, bx, by, 4, src_w, src_h);
}

/* Section 11.3. A 16×16 mode stands in as one subblock mode for the
 * neighbours of a B_PRED block. DC, V, H, TM map to B_DC, B_VE, B_HE, B_TM. */
static const uint8_t k_ymode_as_bmode[4] = { 0, 2, 3, 1 };

/**
 * Greedy 4×4 search. Returns 1 when its score beats @a i16_score, leaving
 * the reconstruction and the nonzero map in place and copying the sixteen
 * modes to @a out_modes. Otherwise restores both.
 */
static int try_bpred(const uint8_t * src, int src_stride, int src_w, int src_h,
    uint8_t * rec, int rec_stride, int mb_w, int mb_x, int mb_y, uint8_t * y_nz,
    int y4_stride, const uint8_t * above_b, const uint8_t * left_b,
    int y1_dc_q, int y1_ac_q, int64_t i16_score, uint8_t * out_modes,
    vp8_prob_set probs) {
  uint8_t saved[256];
  uint8_t saved_nz[16];
  uint8_t chosen[16] = { 0 };
  int64_t sse_sum = 0;
  int bits = bool_cost(0, 145);
  int bi;
  int r;
  for (r = 0; r < 16; ++r) {
    memcpy(saved + r * 16, rec + (mb_y * 16 + r) * rec_stride + mb_x * 16, 16);
  }
  for (bi = 0; bi < 16; ++bi) {
    const int sy = bi >> 2;
    const int sx = bi & 3;
    const int fx = mb_x * 4 + sx;
    const int fy = mb_y * 4 + sy;
    const int above_mode =
        (sy == 0) ? above_b[mb_x * 4 + sx] : chosen[bi - 4];
    const int left_mode = (sx == 0) ? left_b[sy] : chosen[bi - 1];
    const uint8_t * prob = gimg_vp8_kf_bmode_prob[above_mode][left_mode];
    int ctx = 0;
    int mode;
    int best = 0;
    int64_t best_sc = -1;
    int levels[16];
    saved_nz[bi] = y_nz[fy * y4_stride + fx];
    if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
      ctx++;
    }
    if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
      ctx++;
    }
    for (mode = 0; mode < 10; ++mode) {
      const int64_t sse = build_b4(src, src_stride, src_w, src_h, rec,
          rec_stride, mb_w, mb_x, mb_y, sx, sy, mode, y1_dc_q, y1_ac_q,
          levels, ctx, probs);
      const int cost = bmode_cost(mode, prob) +
          block_bit_cost(probs[3], ctx, 0, levels);
      const int64_t sc = sse * 256 + (int64_t)cost * y1_ac_q;
      if (best_sc < 0 || sc < best_sc) {
        best = mode;
        best_sc = sc;
      }
    }
    {
      const int64_t sse = build_b4(src, src_stride, src_w, src_h, rec,
          rec_stride, mb_w, mb_x, mb_y, sx, sy, best, y1_dc_q, y1_ac_q,
          levels, ctx, probs);
      sse_sum += sse;
      bits += bmode_cost(best, prob) +
          block_bit_cost(probs[3], ctx, 0, levels);
      chosen[bi] = (uint8_t)best;
      y_nz[fy * y4_stride + fx] = (uint8_t)block_nz(levels, 0);
    }
  }
  if (sse_sum * 256 + (int64_t)bits * y1_ac_q < i16_score) {
    memcpy(out_modes, chosen, 16);
    return 1;
  }
  for (r = 0; r < 16; ++r) {
    memcpy(rec + (mb_y * 16 + r) * rec_stride + mb_x * 16, saved + r * 16, 16);
  }
  for (bi = 0; bi < 16; ++bi) {
    const int fx = mb_x * 4 + (bi & 3);
    const int fy = mb_y * 4 + (bi >> 2);
    y_nz[fy * y4_stride + fx] = saved_nz[bi];
  }
  return 0;
}

static int levels_zero(const int * levels, int count) {
  int i;
  for (i = 0; i < count; ++i) {
    if (levels[i] != 0) {
      return 0;
    }
  }
  return 1;
}

static void tally_bit(vp8_counts * st, int type, int band, int ctx, int pi,
    int bit) {
  if (bit) {
    st->n1[type][band][ctx][pi]++;
  }
  else {
    st->n0[type][band][ctx][pi]++;
  }
}

/** The branches put_large_value codes with the 11-entry row. The
 *  category extra bits live in the fixed Pcat tables. */
static void tally_large(vp8_counts * st, int type, int band, int ctx, int v) {
  if (v == 2) {
    tally_bit(st, type, band, ctx, 3, 0);
    tally_bit(st, type, band, ctx, 4, 0);
    return;
  }
  if (v == 3 || v == 4) {
    tally_bit(st, type, band, ctx, 3, 0);
    tally_bit(st, type, band, ctx, 4, 1);
    tally_bit(st, type, band, ctx, 5, v - 3);
    return;
  }
  if (v == 5 || v == 6) {
    tally_bit(st, type, band, ctx, 3, 1);
    tally_bit(st, type, band, ctx, 6, 0);
    tally_bit(st, type, band, ctx, 7, 0);
    return;
  }
  if (v >= 7 && v <= 10) {
    tally_bit(st, type, band, ctx, 3, 1);
    tally_bit(st, type, band, ctx, 6, 0);
    tally_bit(st, type, band, ctx, 7, 1);
    return;
  }
  {
    int cat;
    tally_bit(st, type, band, ctx, 3, 1);
    tally_bit(st, type, band, ctx, 6, 1);
    if (v < 19) {
      cat = 0;
    }
    else if (v < 35) {
      cat = 1;
    }
    else if (v < 67) {
      cat = 2;
    }
    else {
      cat = 3;
    }
    tally_bit(st, type, band, ctx, 8, (cat >> 1) & 1);
    tally_bit(st, type, band, ctx, 9 + ((cat >> 1) & 1), cat & 1);
  }
}

static void tally_coeff(vp8_counts * st, int type, int band, int ctx, int level,
    int skip_eob) {
  int mag;
  if (!skip_eob) {
    tally_bit(st, type, band, ctx, 0, 1);
  }
  if (level == 0) {
    tally_bit(st, type, band, ctx, 1, 0);
    return;
  }
  tally_bit(st, type, band, ctx, 1, 1);
  mag = level < 0 ? -level : level;
  if (mag == 1) {
    tally_bit(st, type, band, ctx, 2, 0);
  }
  else {
    tally_bit(st, type, band, ctx, 2, 1);
    tally_large(st, type, band, ctx, mag);
  }
}

/** Same walk as put_block. */
static void tally_block(vp8_counts * st, int type, int ctx, int first,
    const int * levels) {
  int last = first - 1;
  int i;
  int skip_eob = 0;
  for (i = first; i < 16; ++i) {
    if (levels[k_zigzag[i]] != 0) {
      last = i;
    }
  }
  if (last < first) {
    tally_bit(st, type, gimg_vp8_bands[first], ctx, 0, 0);
    return;
  }
  for (i = first; i <= last; ++i) {
    const int level = levels[k_zigzag[i]];
    const int band = gimg_vp8_bands[i];
    tally_coeff(st, type, band, ctx, level, skip_eob);
    if (level == 0) {
      skip_eob = 1;
      ctx = 0;
    }
    else {
      skip_eob = 0;
      ctx = (level == 1 || level == -1) ? 1 : 2;
    }
  }
  if (last < 15) {
    tally_bit(st, type, gimg_vp8_bands[last + 1], ctx, 0, 0);
  }
}

/** Section 13.4. Keep a new probability when the tokens it saves cost
 *  more than the update bool plus the eight-bit literal. */
static void fit_probs(const vp8_counts * counts, vp8_prob_table dst) {
  int t, b, c, p;
  memcpy(dst, gimg_vp8_coeffs_proba0, sizeof(vp8_prob_table));
  for (t = 0; t < GIMG_VP8_NUM_TYPES; ++t) {
    for (b = 0; b < GIMG_VP8_NUM_BANDS; ++b) {
      for (c = 0; c < GIMG_VP8_NUM_CTX; ++c) {
        for (p = 0; p < GIMG_VP8_NUM_PROBAS; ++p) {
          const uint32_t n0 = counts->n0[t][b][c][p];
          const uint32_t n1 = counts->n1[t][b][c][p];
          const uint32_t n = n0 + n1;
          const int old = gimg_vp8_coeffs_proba0[t][b][c][p];
          const int upd = gimg_vp8_coeffs_update_proba[t][b][c][p];
          int neu;
          int64_t cost_old;
          int64_t cost_new;
          int64_t extra;
          if (n == 0u) {
            continue;
          }
          neu = (int)((n0 * 256u + n / 2u) / n);
          if (neu < 1) {
            neu = 1;
          }
          if (neu > 255) {
            neu = 255;
          }
          if (neu == old) {
            continue;
          }
          cost_old = (int64_t)n0 * bool_cost(0, old) +
              (int64_t)n1 * bool_cost(1, old);
          cost_new = (int64_t)n0 * bool_cost(0, neu) +
              (int64_t)n1 * bool_cost(1, neu);
          extra = (int64_t)bool_cost(1, upd) + 8 * 256 -
              (int64_t)bool_cost(0, upd);
          if (cost_old > cost_new + extra) {
            dst[t][b][c][p] = (uint8_t)neu;
          }
        }
      }
    }
  }
}

/** Count the tokens build would emit under @a probs. Skipped
 *  macroblocks contribute none. */
static void tally_frame(const uint8_t * y_plane, int y_stride, int width,
    int height, uint8_t * rec_y, const uint8_t * u_plane,
    const uint8_t * v_plane, uint8_t * rec_u, uint8_t * rec_v, int uv_stride,
    int uv_w, int uv_h, int mb_w, int mb_h, int base_q, int q_delta,
    const uint8_t * y_mode,
    const uint8_t * uv_mode, const uint8_t * b_mode, const uint8_t * seg,
    uint8_t * y_nz, uint8_t * u_nz, uint8_t * v_nz, uint8_t * y2_above,
    int y4_stride, int uv4_stride, int skip_on, const uint8_t * mb_skip,
    vp8_prob_set probs, uint8_t * mb_nz, vp8_counts * counts) {
  int mb_y;
  int mb_x;
  int y2_left;
  memset(y_nz, 0, (size_t)y4_stride * (size_t)mb_h * 4u);
  memset(u_nz, 0, (size_t)uv4_stride * (size_t)mb_h * 2u);
  memset(v_nz, 0, (size_t)uv4_stride * (size_t)mb_h * 2u);
  memset(y2_above, 0, (size_t)mb_w);
  if (mb_nz) {
    memset(mb_nz, 0, (size_t)mb_w * (size_t)mb_h);
  }
  for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    y2_left = 0;
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      const int mi = mb_y * mb_w + mb_x;
      int ac_levels[16][16];
      int y2_level[16];
      int levels_u[4][16];
      int levels_v[4][16];
      int y1_dc_m, y1_ac_m, y2_dc_m, y2_ac_m, uv_dc_m, uv_ac_m;
      int sy;
      int sx;
      int any = 0;
      mb_quants(base_q + (seg[mi] ? q_delta : 0), &y1_dc_m, &y1_ac_m,
          &y2_dc_m, &y2_ac_m, &uv_dc_m, &uv_ac_m);
      if (skip_on && mb_skip[mi]) {
        int zi;
        if (y_mode[mi] != 4) {
          y2_left = 0;
          y2_above[mb_x] = 0;
        }
        for (zi = 0; zi < 16; ++zi) {
          y_nz[(mb_y * 4 + (zi >> 2)) * y4_stride + mb_x * 4 + (zi & 3)] = 0;
        }
        for (zi = 0; zi < 4; ++zi) {
          const int fx = mb_x * 2 + (zi & 1);
          const int fy = mb_y * 2 + (zi >> 1);
          u_nz[fy * uv4_stride + fx] = 0;
          v_nz[fy * uv4_stride + fx] = 0;
        }
        continue;
      }
      if (y_mode[mi] == 4) {
        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            int levels[16];
            const int fx = mb_x * 4 + sx;
            const int fy = mb_y * 4 + sy;
            int ctx = 0;
            if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
              ctx++;
            }
            if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
              ctx++;
            }
            build_b4(y_plane, y_stride, width, height, rec_y, y_stride, mb_w,
                mb_x, mb_y, sx, sy, b_mode[mi * 16 + sy * 4 + sx], y1_dc_m,
                y1_ac_m, levels, ctx, probs);
            tally_block(counts, 3, ctx, 0, levels);
            y_nz[fy * y4_stride + fx] = (uint8_t)block_nz(levels, 0);
            if (y_nz[fy * y4_stride + fx]) {
              any = 1;
            }
          }
        }
      }
      else {
        int ctx = (y2_left ? 1 : 0) + (y2_above[mb_x] ? 1 : 0);
        build_i16(y_plane, y_stride, width, height, rec_y, y_stride, mb_x * 16,
            mb_y * 16, y_mode[mi], y1_ac_m, y2_dc_m, y2_ac_m, ac_levels,
            y2_level, y_nz, y4_stride, probs);
        tally_block(counts, 1, ctx, 0, y2_level);
        y2_left = block_nz(y2_level, 0);
        y2_above[mb_x] = (uint8_t)y2_left;
        if (y2_left) {
          any = 1;
        }
        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            const int fx = mb_x * 4 + sx;
            const int fy = mb_y * 4 + sy;
            const int bi = sy * 4 + sx;
            ctx = 0;
            if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
              ctx++;
            }
            if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
              ctx++;
            }
            tally_block(counts, 0, ctx, 1, ac_levels[bi]);
            y_nz[fy * y4_stride + fx] = (uint8_t)block_nz(ac_levels[bi], 1);
            if (y_nz[fy * y4_stride + fx]) {
              any = 1;
            }
          }
        }
      }
      build_uv(u_plane, v_plane, rec_u, rec_v, uv_stride, mb_x * 8, mb_y * 8,
          uv_w, uv_h, uv_mode[mi], uv_dc_m, uv_ac_m, levels_u, levels_v, u_nz,
          v_nz, uv4_stride, probs);
      for (sy = 0; sy < 2; ++sy) {
        for (sx = 0; sx < 2; ++sx) {
          int ch;
          for (ch = 0; ch < 2; ++ch) {
            uint8_t * nz_plane = (ch == 0) ? u_nz : v_nz;
            int (*levels)[16] = (ch == 0) ? levels_u : levels_v;
            const int fx = mb_x * 2 + sx;
            const int fy = mb_y * 2 + sy;
            const int bi = sy * 2 + sx;
            int ctx = 0;
            if (fy > 0 && nz_plane[(fy - 1) * uv4_stride + fx]) {
              ctx++;
            }
            if (fx > 0 && nz_plane[fy * uv4_stride + (fx - 1)]) {
              ctx++;
            }
            tally_block(counts, 2, ctx, 0, levels[bi]);
            nz_plane[fy * uv4_stride + fx] = (uint8_t)block_nz(levels[bi], 0);
            if (nz_plane[fy * uv4_stride + fx]) {
              any = 1;
            }
          }
        }
      }
      if (mb_nz) {
        mb_nz[mi] = (uint8_t)any;
      }
    }
  }
}

/** Section 9.1. Mark a macroblock when its coefficients are all zero.
 *  The flag is used only when the omitted tokens cost more than one
 *  skip bool on every macroblock plus the 8-bit probability. @a skip_prob
 *  is P(not skipped), the probability the bool coder assigns to zero. */
static void choose_skips(const uint8_t * y_plane, int y_stride, int width,
    int height, uint8_t * rec_y, const uint8_t * u_plane,
    const uint8_t * v_plane, uint8_t * rec_u, uint8_t * rec_v, int uv_stride,
    int uv_w, int uv_h, int mb_w, int mb_h, int base_q, int q_delta,
    const uint8_t * y_mode,
    const uint8_t * uv_mode, const uint8_t * b_mode, const uint8_t * seg,
    uint8_t * y_nz, uint8_t * u_nz, uint8_t * v_nz, uint8_t * y2_above,
    int y4_stride, int uv4_stride, uint8_t * mb_skip, int * skip_on,
    int * skip_prob, vp8_prob_set probs) {
  int64_t savings = 0;
  int64_t overhead;
  int n_skip = 0;
  int n_mb = mb_w * mb_h;
  int prob;
  int mb_y;
  int mb_x;
  int y2_left;
  memset(y_nz, 0, (size_t)y4_stride * (size_t)mb_h * 4u);
  memset(u_nz, 0, (size_t)uv4_stride * (size_t)mb_h * 2u);
  memset(v_nz, 0, (size_t)uv4_stride * (size_t)mb_h * 2u);
  memset(y2_above, 0, (size_t)mb_w);
  memset(mb_skip, 0, (size_t)n_mb);
  for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    y2_left = 0;
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      const int mi = mb_y * mb_w + mb_x;
      int ac_levels[16][16];
      int y2_level[16];
      int levels_u[4][16];
      int levels_v[4][16];
      int y1_dc_m, y1_ac_m, y2_dc_m, y2_ac_m, uv_dc_m, uv_ac_m;
      int empty = 1;
      int sy;
      int sx;
      int cost = 0;
      mb_quants(base_q + (seg[mi] ? q_delta : 0), &y1_dc_m, &y1_ac_m,
          &y2_dc_m, &y2_ac_m, &uv_dc_m, &uv_ac_m);
      if (y_mode[mi] == 4) {
        int b_nz[16];
        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            int levels[16];
            const int bi = sy * 4 + sx;
            const int fx = mb_x * 4 + sx;
            const int fy = mb_y * 4 + sy;
            int ctx = 0;
            if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
              ctx++;
            }
            if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
              ctx++;
            }
            build_b4(y_plane, y_stride, width, height, rec_y, y_stride, mb_w,
                mb_x, mb_y, sx, sy, b_mode[mi * 16 + bi], y1_dc_m, y1_ac_m,
                levels, ctx, probs);
            b_nz[bi] = block_nz(levels, 0);
            if (!levels_zero(levels, 16)) {
              empty = 0;
            }
            cost += block_bit_cost(probs[3], ctx, 0, levels);
            y_nz[fy * y4_stride + fx] = (uint8_t)b_nz[bi];
          }
        }
      }
      else {
        build_i16(y_plane, y_stride, width, height, rec_y, y_stride,
            mb_x * 16, mb_y * 16, y_mode[mi], y1_ac_m, y2_dc_m, y2_ac_m,
            ac_levels, y2_level, y_nz, y4_stride, probs);
        if (!levels_zero(y2_level, 16)) {
          empty = 0;
        }
        for (sy = 0; sy < 16; ++sy) {
          if (!levels_zero(ac_levels[sy], 16)) {
            empty = 0;
          }
        }
        cost = i16_residual_cost(ac_levels, y2_level, y_nz, y4_stride, mb_x,
            mb_y, y2_left, y2_above[mb_x], probs);
      }
      build_uv(u_plane, v_plane, rec_u, rec_v, uv_stride, mb_x * 8, mb_y * 8,
          uv_w, uv_h, uv_mode[mi], uv_dc_m, uv_ac_m, levels_u, levels_v, u_nz,
          v_nz, uv4_stride, probs);
      for (sy = 0; sy < 4; ++sy) {
        if (!levels_zero(levels_u[sy], 16) || !levels_zero(levels_v[sy], 16)) {
          empty = 0;
        }
      }
      cost += uv_residual_cost(levels_u, levels_v, u_nz, v_nz, uv4_stride,
          mb_x, mb_y, probs);
      if (empty) {
        mb_skip[mi] = 1;
        n_skip++;
        savings += cost;
      }
      if (y_mode[mi] == 4) {
        if (empty) {
          for (sy = 0; sy < 4; ++sy) {
            for (sx = 0; sx < 4; ++sx) {
              const int fx = mb_x * 4 + sx;
              const int fy = mb_y * 4 + sy;
              y_nz[fy * y4_stride + fx] = 0;
            }
          }
        }
      }
      else if (empty) {
        y2_left = 0;
        y2_above[mb_x] = 0;
        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            const int fx = mb_x * 4 + sx;
            const int fy = mb_y * 4 + sy;
            y_nz[fy * y4_stride + fx] = 0;
          }
        }
      }
      else {
        y2_left = block_nz(y2_level, 0);
        y2_above[mb_x] = (uint8_t)y2_left;
        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            const int fx = mb_x * 4 + sx;
            const int fy = mb_y * 4 + sy;
            y_nz[fy * y4_stride + fx] =
                (uint8_t)block_nz(ac_levels[sy * 4 + sx], 1);
          }
        }
      }
      for (sy = 0; sy < 2; ++sy) {
        for (sx = 0; sx < 2; ++sx) {
          const int fx = mb_x * 2 + sx;
          const int fy = mb_y * 2 + sy;
          const int bi = sy * 2 + sx;
          if (empty) {
            u_nz[fy * uv4_stride + fx] = 0;
            v_nz[fy * uv4_stride + fx] = 0;
          }
          else {
            u_nz[fy * uv4_stride + fx] = (uint8_t)block_nz(levels_u[bi], 0);
            v_nz[fy * uv4_stride + fx] = (uint8_t)block_nz(levels_v[bi], 0);
          }
        }
      }
    }
  }
  *skip_on = 0;
  *skip_prob = 0;
  if (n_skip == 0) {
    return;
  }
  prob = (int)(((n_mb - n_skip) * 256 + n_mb / 2) / n_mb);
  if (prob < 1) {
    prob = 1;
  }
  if (prob > 255) {
    prob = 255;
  }
  overhead = 8 * 256;
  for (mb_y = 0; mb_y < n_mb; ++mb_y) {
    overhead += bool_cost(mb_skip[mb_y] ? 1 : 0, prob);
  }
  if (savings <= overhead) {
    memset(mb_skip, 0, (size_t)n_mb);
    return;
  }
  *skip_on = 1;
  *skip_prob = prob;
}

/** Sum of squared error over the visible samples. The macroblock pad
 *  past the picture edge is not part of the picture. */
static int64_t frame_sse(const uint8_t * rec, int rec_stride,
    const uint8_t * src, int src_stride, int w, int h) {
  int64_t sum = 0;
  int y;
  for (y = 0; y < h; ++y) {
    int x;
    const uint8_t * rr = rec + (size_t)y * (size_t)rec_stride;
    const uint8_t * ss = src + (size_t)y * (size_t)src_stride;
    for (x = 0; x < w; ++x) {
      const int d = (int)rr[x] - (int)ss[x];
      sum += (int64_t)d * (int64_t)d;
    }
  }
  return sum;
}

/** Error of one filter level on copies of the unfiltered reconstruction.
 *  Returns -1 when the filter cannot allocate. */
static int64_t filtered_sse(uint8_t * ty, uint8_t * tu, uint8_t * tv,
    const uint8_t * rec_y, const uint8_t * rec_u, const uint8_t * rec_v,
    size_t y_bytes, size_t uv_bytes, const uint8_t * y_src, int y_stride,
    int width, int height, const uint8_t * u_src, const uint8_t * v_src,
    int uv_stride, int uv_w, int uv_h, int mb_w, int mb_h,
    const uint8_t * y_mode, const uint8_t * mb_nz, int level,
    const GIMG_Allocator * alloc) {
  memcpy(ty, rec_y, y_bytes);
  memcpy(tu, rec_u, uv_bytes);
  memcpy(tv, rec_v, uv_bytes);
  if (gimg_vp8_loop_filter(ty, y_stride, tu, tv, uv_stride, y_mode, mb_nz,
          (uint32_t)mb_w, (uint32_t)mb_h, level, alloc) != GIMG_OK) {
    return -1;
  }
  return frame_sse(ty, y_stride, y_src, y_stride, width, height) +
      frame_sse(tu, uv_stride, u_src, uv_stride, uv_w, uv_h) +
      frame_sse(tv, uv_stride, v_src, uv_stride, uv_w, uv_h);
}

/** Section 15. The level field is six bits whatever the value, so the
 *  choice is the filtered reconstruction closest to the source. An
 *  equal error stays with the lower level, and a flat picture stays
 *  off. Levels 8, 16, …, 56 and 63 are scored first, then the four
 *  neighbors on each side of the best of those, then the level walks
 *  left while the error does not rise. On the pictures this was measured
 *  against, that finds the same level as trying all 63. The filter runs
 *  on a copy: prediction uses the unfiltered samples. */
static int choose_filter_level(const uint8_t * y_src, int y_stride, int width,
    int height, const uint8_t * rec_y, const uint8_t * u_src,
    const uint8_t * v_src, const uint8_t * rec_u, const uint8_t * rec_v,
    int uv_stride, int uv_w, int uv_h, int mb_w, int mb_h,
    const uint8_t * y_mode, const uint8_t * mb_nz, const GIMG_Allocator * alloc,
    GIMG_Result * err) {
  const size_t y_bytes = (size_t)y_stride * (size_t)mb_h * 16u;
  const size_t uv_bytes = (size_t)uv_stride * (size_t)mb_h * 8u;
  uint8_t * ty = NULL;
  uint8_t * tu = NULL;
  uint8_t * tv = NULL;
  int64_t got[64];
  uint8_t have[64];
  int64_t best;
  int best_level = 0;
  int level;
  int i;
  *err = GIMG_OK;
  best = frame_sse(rec_y, y_stride, y_src, y_stride, width, height) +
      frame_sse(rec_u, uv_stride, u_src, uv_stride, uv_w, uv_h) +
      frame_sse(rec_v, uv_stride, v_src, uv_stride, uv_w, uv_h);
  if (best == 0) {
    return 0;
  }
  memset(have, 0, sizeof have);
  got[0] = best;
  have[0] = 1;
  ty = (uint8_t *)gimg_malloc(alloc, y_bytes);
  tu = (uint8_t *)gimg_malloc(alloc, uv_bytes);
  tv = (uint8_t *)gimg_malloc(alloc, uv_bytes);
  if (!ty || !tu || !tv) {
    *err = GIMG_ERR_OOM;
    goto Done;
  }
  for (i = 0; i < 8; ++i) {
    const int candidate = (i < 7) ? (i + 1) * 8 : 63;
    int64_t sse;
    if (have[candidate]) {
      sse = got[candidate];
    }
    else {
      sse = filtered_sse(ty, tu, tv, rec_y, rec_u, rec_v, y_bytes, uv_bytes,
          y_src, y_stride, width, height, u_src, v_src, uv_stride, uv_w, uv_h,
          mb_w, mb_h, y_mode, mb_nz, candidate, alloc);
      if (sse < 0) {
        *err = GIMG_ERR_OOM;
        best_level = 0;
        goto Done;
      }
      got[candidate] = sse;
      have[candidate] = 1;
    }
    if (sse < best) {
      best = sse;
      best_level = candidate;
    }
  }
  {
    const int center = best_level;
    for (level = center - 4; level <= center + 4; ++level) {
    int64_t sse;
    if (level < 1 || level > 63) {
      continue;
    }
    if (have[level]) {
      sse = got[level];
    }
    else {
      sse = filtered_sse(ty, tu, tv, rec_y, rec_u, rec_v, y_bytes, uv_bytes,
          y_src, y_stride, width, height, u_src, v_src, uv_stride, uv_w, uv_h,
          mb_w, mb_h, y_mode, mb_nz, level, alloc);
      if (sse < 0) {
        *err = GIMG_ERR_OOM;
        best_level = 0;
        goto Done;
      }
      got[level] = sse;
      have[level] = 1;
    }
    if (sse < best) {
      best = sse;
      best_level = level;
    }
    }
  }
  while (best_level > 0) {
    int64_t sse;
    const int left = best_level - 1;
    if (have[left]) {
      sse = got[left];
    }
    else {
      sse = filtered_sse(ty, tu, tv, rec_y, rec_u, rec_v, y_bytes, uv_bytes,
          y_src, y_stride, width, height, u_src, v_src, uv_stride, uv_w, uv_h,
          mb_w, mb_h, y_mode, mb_nz, left, alloc);
      if (sse < 0) {
        *err = GIMG_ERR_OOM;
        best_level = 0;
        goto Done;
      }
      got[left] = sse;
      have[left] = 1;
    }
    if (sse > best) {
      break;
    }
    best = sse;
    best_level = left;
  }
Done:
  gimg_free(alloc, ty);
  gimg_free(alloc, tu);
  gimg_free(alloc, tv);
  return best_level;
}

GIMG_Result gimg_webp_vp8_encode(const uint8_t * rgba, uint32_t width,
    uint32_t height, size_t stride, int effort,
    const GIMG_Allocator * alloc, unsigned char ** out_bytes,
    size_t * out_size) {
  uint32_t mb_w;
  uint32_t mb_h;
  uint32_t y_stride;
  uint32_t uv_stride;
  uint32_t uv_w;
  uint32_t uv_h;
  uint8_t * y_plane = NULL;
  uint8_t * u_plane = NULL;
  uint8_t * v_plane = NULL;
  bool_writer_t part0;
  bool_writer_t tokens;
  unsigned char * out = NULL;
  size_t out_cap = 0;
  size_t out_len = 0;
  int base_q;
  uint8_t * rec_y = NULL;
  uint8_t * rec_u = NULL;
  uint8_t * rec_v = NULL;
  uint8_t * y_mode = NULL;
  uint8_t * uv_mode = NULL;
  uint8_t * b_mode = NULL;
  uint8_t * seg = NULL;
  uint8_t * mb_skip = NULL;
  uint8_t * mb_nz = NULL;
  int filter_level = 0;
  int seg_on = 0;
  int q_delta = 0;
  int seg_prob[3] = { 255, 255, 255 };
  int skip_on = 0;
  int skip_prob = 0;
  int prob_pass;
  vp8_prob_table coeff_prob;
  vp8_prob_table write_prob;
  uint8_t * above_b = NULL;
  uint8_t * y_nz = NULL;
  uint8_t * u_nz = NULL;
  uint8_t * v_nz = NULL;
  uint8_t * y2_above = NULL;
  int y4_stride = 0;
  int uv4_stride = 0;
  GIMG_Result r = GIMG_OK;
  uint32_t mb_y;
  uint32_t mb_x;

  *out_bytes = NULL;
  *out_size = 0;
  if (!rgba || !alloc || width == 0u || height == 0u || width > VP8_MAX_DIM ||
      height > VP8_MAX_DIM) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (effort < 0) {
    effort = 0;
  }
  if (effort > 9) {
    effort = 9;
  }
  base_q = k_effort_q[effort];

  mb_w = (width + 15u) >> 4;
  mb_h = (height + 15u) >> 4;
  y_stride = mb_w * 16u;
  uv_w = (width + 1u) >> 1;
  uv_h = (height + 1u) >> 1;
  uv_stride = mb_w * 8u;

  y_plane = (uint8_t *)gimg_malloc(alloc, (size_t)y_stride * mb_h * 16u);
  u_plane = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
  v_plane = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
  if (!y_plane || !u_plane || !v_plane) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  memset(y_plane, 0, (size_t)y_stride * mb_h * 16u);
  memset(u_plane, 128, (size_t)uv_stride * mb_h * 8u);
  memset(v_plane, 128, (size_t)uv_stride * mb_h * 8u);
  rec_y = (uint8_t *)gimg_malloc(alloc, (size_t)y_stride * mb_h * 16u);
  rec_u = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
  rec_v = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
  y_mode = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * mb_h);
  uv_mode = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * mb_h);
  b_mode = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * mb_h * 16u);
  seg = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * mb_h);
  mb_skip = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * mb_h);
  mb_nz = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * mb_h);
  if (!rec_y || !rec_u || !rec_v || !y_mode || !uv_mode || !b_mode || !seg ||
      !mb_skip || !mb_nz) {
    r = GIMG_ERR_OOM;
    goto Done;
  }

  /* RGB → YUV 4:2:0, BT.601 studio swing. Chroma is the mean of the
   * 2×2, including a short block on the right or bottom edge. */
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t * row = rgba + (size_t)y * stride;
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t * px = row + (size_t)x * 4u;
      y_plane[y * y_stride + x] = (uint8_t)rgb_to_y(px[0], px[1], px[2]);
    }
  }
  for (uint32_t y = 0; y < height; y += 2u) {
    for (uint32_t x = 0; x < width; x += 2u) {
      int rsum = 0, gsum = 0, bsum = 0, n = 0;
      uint32_t yy, xx;
      for (yy = y; yy < y + 2u && yy < height; ++yy) {
        for (xx = x; xx < x + 2u && xx < width; ++xx) {
          const uint8_t * px = rgba + (size_t)yy * stride + (size_t)xx * 4u;
          rsum += px[0];
          gsum += px[1];
          bsum += px[2];
          n++;
        }
      }
      if (n > 0) {
        u_plane[(y / 2u) * uv_stride + (x / 2u)] = (uint8_t)rgb_to_uv(
            VP8_U_R, VP8_U_G, VP8_U_B, rsum, gsum, bsum, n);
        v_plane[(y / 2u) * uv_stride + (x / 2u)] = (uint8_t)rgb_to_uv(
            VP8_V_R, VP8_V_G, VP8_V_B, rsum, gsum, bsum, n);
      }
    }
  }

  memset(seg, 0, (size_t)mb_w * mb_h);
  {
    int nfine = 0;
    const int nmb = (int)mb_w * (int)mb_h;
    for (mb_y = 0; mb_y < mb_h; ++mb_y) {
      for (mb_x = 0; mb_x < mb_w; ++mb_x) {
        const uint32_t mi = mb_y * mb_w + mb_x;
        if (pred_energy(y_plane, (int)y_stride, (int)mb_x * 16,
                (int)mb_y * 16, (int)width, (int)height) > VP8_FINE_SSE) {
          seg[mi] = 1;
          nfine++;
        }
      }
    }
    if (nfine > 0 && nfine < nmb) {
      int p1;
      seg_on = 1;
      q_delta = k_fine_q[effort] - base_q;
      p1 = ((nmb - nfine) * 255 + nmb / 2) / nmb;
      if (p1 < 1) {
        p1 = 1;
      }
      if (p1 > 255) {
        p1 = 255;
      }
      seg_prob[1] = p1;
    }
    else if (nfine == nmb) {
      base_q = k_fine_q[effort];
    }
  }

  /* Score is reconstructed SSE plus the bool-coder cost of the mode
   * and its tokens. One bit (256 cost units) is priced at the luma AC
   * quantizer step. DC is first, so an equal score stays with it. */
  y4_stride = (int)mb_w * 4;
  uv4_stride = (int)mb_w * 2;
  y_nz = (uint8_t *)gimg_malloc(alloc, (size_t)y4_stride * mb_h * 4u);
  u_nz = (uint8_t *)gimg_malloc(alloc, (size_t)uv4_stride * mb_h * 2u);
  v_nz = (uint8_t *)gimg_malloc(alloc, (size_t)uv4_stride * mb_h * 2u);
  y2_above = (uint8_t *)gimg_malloc(alloc, mb_w);
  above_b = (uint8_t *)gimg_malloc(alloc, (size_t)mb_w * 4u);
  if (!y_nz || !u_nz || !v_nz || !y2_above || !above_b) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  memcpy(coeff_prob, gimg_vp8_coeffs_proba0, sizeof coeff_prob);
  memcpy(write_prob, coeff_prob, sizeof write_prob);
  for (prob_pass = 0; prob_pass < 2; ++prob_pass) {
    vp8_counts counts;
    memset(y_nz, 0, (size_t)y4_stride * mb_h * 4u);
    memset(u_nz, 0, (size_t)uv4_stride * mb_h * 2u);
    memset(v_nz, 0, (size_t)uv4_stride * mb_h * 2u);
    memset(y2_above, 0, mb_w);
    memset(above_b, 0, (size_t)mb_w * 4u);

    for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    int y2_left = 0;
    uint8_t left_b[4] = { 0, 0, 0, 0 };
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      const int bx = (int)mb_x * 16;
      const int by = (int)mb_y * 16;
      const int ubx = (int)mb_x * 8;
      const int uby = (int)mb_y * 8;
      const uint32_t mi = mb_y * mb_w + mb_x;
      int ac_levels[16][16];
      int y2_level[16];
      int levels_u[4][16];
      int levels_v[4][16];
      int mode;
      int best = 0;
      int64_t best_score = -1;
      int y2_left_in = y2_left;
      uint8_t y2_above_in = y2_above[mb_x];
      int sy;
      int sx;
      int y1_dc_m;
      int y1_ac_m;
      int y2_dc_m;
      int y2_ac_m;
      int uv_dc_m;
      int uv_ac_m;
      mb_quants(base_q + (seg[mi] ? q_delta : 0), &y1_dc_m, &y1_ac_m,
          &y2_dc_m, &y2_ac_m, &uv_dc_m, &uv_ac_m);
      for (mode = 0; mode < 4; ++mode) {
        const int64_t sse = build_i16(y_plane, (int)y_stride, (int)width,
            (int)height, rec_y, (int)y_stride, bx, by, mode, y1_ac_m,
            y2_dc_m, y2_ac_m, ac_levels, y2_level, y_nz, y4_stride,
            coeff_prob);
        const int bits = ymode_cost(mode) +
            i16_residual_cost(ac_levels, y2_level, y_nz, y4_stride, (int)mb_x,
                (int)mb_y, y2_left, y2_above[mb_x], coeff_prob);
        const int64_t score = sse * 256 + (int64_t)bits * y1_ac_m;
        if (best_score < 0 || score < best_score) {
          best = mode;
          best_score = score;
        }
      }
      if (best != 3) {
        build_i16(y_plane, (int)y_stride, (int)width, (int)height, rec_y,
            (int)y_stride, bx, by, best, y1_ac_m, y2_dc_m, y2_ac_m,
            ac_levels, y2_level, y_nz, y4_stride, coeff_prob);
      }
      y_mode[mi] = (uint8_t)best;
      y2_left = block_nz(y2_level, 0);
      y2_above[mb_x] = (uint8_t)y2_left;
      for (sy = 0; sy < 4; ++sy) {
        for (sx = 0; sx < 4; ++sx) {
          const int fx = (int)mb_x * 4 + sx;
          const int fy = (int)mb_y * 4 + sy;
          y_nz[fy * y4_stride + fx] =
              (uint8_t)block_nz(ac_levels[sy * 4 + sx], 1);
        }
      }
      if (try_bpred(y_plane, (int)y_stride, (int)width, (int)height, rec_y,
              (int)y_stride, (int)mb_w, (int)mb_x, (int)mb_y, y_nz, y4_stride,
              above_b, left_b, y1_dc_m, y1_ac_m, best_score,
              b_mode + mi * 16u, coeff_prob)) {
        int j;
        y_mode[mi] = 4;
        y2_left = y2_left_in;
        y2_above[mb_x] = y2_above_in;
        for (j = 0; j < 4; ++j) {
          above_b[mb_x * 4u + (uint32_t)j] = b_mode[mi * 16u + 12u + (uint32_t)j];
          left_b[j] = b_mode[mi * 16u + (uint32_t)j * 4u + 3u];
        }
      }
      else {
        int j;
        const uint8_t mapped = k_ymode_as_bmode[best];
        for (j = 0; j < 4; ++j) {
          above_b[mb_x * 4u + (uint32_t)j] = mapped;
          left_b[j] = mapped;
        }
      }

      best = 0;
      best_score = -1;
      for (mode = 0; mode < 4; ++mode) {
        const int64_t sse = build_uv(u_plane, v_plane, rec_u, rec_v,
            (int)uv_stride, ubx, uby, (int)uv_w, (int)uv_h, mode, uv_dc_m,
            uv_ac_m, levels_u, levels_v, u_nz, v_nz, uv4_stride, coeff_prob);
        const int bits = uvmode_cost(mode) +
            uv_residual_cost(levels_u, levels_v, u_nz, v_nz, uv4_stride,
                (int)mb_x, (int)mb_y, coeff_prob);
        const int64_t score = sse * 256 + (int64_t)bits * y1_ac_m;
        if (best_score < 0 || score < best_score) {
          best = mode;
          best_score = score;
        }
      }
      if (best != 3) {
        build_uv(u_plane, v_plane, rec_u, rec_v, (int)uv_stride, ubx, uby,
            (int)uv_w, (int)uv_h, best, uv_dc_m, uv_ac_m, levels_u, levels_v,
            u_nz, v_nz, uv4_stride, coeff_prob);
      }
      uv_mode[mi] = (uint8_t)best;
      for (sy = 0; sy < 2; ++sy) {
        for (sx = 0; sx < 2; ++sx) {
          const int fx = (int)mb_x * 2 + sx;
          const int fy = (int)mb_y * 2 + sy;
          u_nz[fy * uv4_stride + fx] = (uint8_t)block_nz(levels_u[sy * 2 + sx], 0);
          v_nz[fy * uv4_stride + fx] = (uint8_t)block_nz(levels_v[sy * 2 + sx], 0);
        }
      }
    }
    }
    memset(&counts, 0, sizeof counts);
    tally_frame(y_plane, (int)y_stride, (int)width, (int)height, rec_y, u_plane,
        v_plane, rec_u, rec_v, (int)uv_stride, (int)uv_w, (int)uv_h, (int)mb_w,
        (int)mb_h, base_q, q_delta, y_mode, uv_mode, b_mode, seg, y_nz, u_nz,
        v_nz,
        y2_above, y4_stride, uv4_stride, 0, NULL, coeff_prob, NULL, &counts);
    fit_probs(&counts, write_prob);
    if (prob_pass == 1 ||
        memcmp(write_prob, coeff_prob, sizeof coeff_prob) == 0) {
      break;
    }
    memcpy(coeff_prob, write_prob, sizeof coeff_prob);
  }

  choose_skips(y_plane, (int)y_stride, (int)width, (int)height, rec_y, u_plane,
      v_plane, rec_u, rec_v, (int)uv_stride, (int)uv_w, (int)uv_h, (int)mb_w,
      (int)mb_h, base_q, q_delta, y_mode, uv_mode, b_mode, seg, y_nz, u_nz,
      v_nz,
      y2_above, y4_stride, uv4_stride, mb_skip, &skip_on, &skip_prob,
      coeff_prob);
  {
    vp8_counts counts;
    memset(&counts, 0, sizeof counts);
    tally_frame(y_plane, (int)y_stride, (int)width, (int)height, rec_y, u_plane,
        v_plane, rec_u, rec_v, (int)uv_stride, (int)uv_w, (int)uv_h, (int)mb_w,
        (int)mb_h, base_q, q_delta, y_mode, uv_mode, b_mode, seg, y_nz, u_nz,
        v_nz,
        y2_above, y4_stride, uv4_stride, skip_on, mb_skip, coeff_prob, mb_nz,
        &counts);
    fit_probs(&counts, write_prob);
  }

  bw_init(&part0, alloc);
  bw_init(&tokens, alloc);
  filter_level = choose_filter_level(y_plane, (int)y_stride, (int)width,
      (int)height, rec_y, u_plane, v_plane, rec_u, rec_v, (int)uv_stride,
      (int)uv_w, (int)uv_h, (int)mb_w, (int)mb_h, y_mode, mb_nz, alloc, &r);
  if (r != GIMG_OK) {
    goto Done;
  }
  write_frame_header(&part0, base_q, seg_on, q_delta, seg_prob, filter_level,
      skip_on, skip_prob, write_prob);

  memset(above_b, 0, (size_t)mb_w * 4u);
  for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    uint8_t left_b[4] = { 0, 0, 0, 0 };
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      const uint32_t mi = mb_y * mb_w + mb_x;
      int j;
      if (seg_on) {
        write_segment_id(&part0, seg[mi], seg_prob);
      }
      if (skip_on) {
        bw_put_bit(&part0, mb_skip[mi] ? 1 : 0, skip_prob);
      }
      if (y_mode[mi] == 4) {
        bw_put_bit(&part0, 0, 145);
        for (j = 0; j < 16; ++j) {
          const int sy = j >> 2;
          const int sx = j & 3;
          const int above_mode = (sy == 0) ? above_b[mb_x * 4u + (uint32_t)sx]
                                            : b_mode[mi * 16u + (uint32_t)j - 4u];
          const int left_mode = (sx == 0) ? left_b[sy]
                                           : b_mode[mi * 16u + (uint32_t)j - 1u];
          write_bmode(&part0, b_mode[mi * 16u + (uint32_t)j],
              gimg_vp8_kf_bmode_prob[above_mode][left_mode]);
        }
        for (j = 0; j < 4; ++j) {
          above_b[mb_x * 4u + (uint32_t)j] =
              b_mode[mi * 16u + 12u + (uint32_t)j];
          left_b[j] = b_mode[mi * 16u + (uint32_t)j * 4u + 3u];
        }
      }
      else {
        const uint8_t mapped = k_ymode_as_bmode[y_mode[mi]];
        write_ymode(&part0, y_mode[mi]);
        for (j = 0; j < 4; ++j) {
          above_b[mb_x * 4u + (uint32_t)j] = mapped;
          left_b[j] = mapped;
        }
      }
      write_uvmode(&part0, uv_mode[mi]);
    }
  }
  if (!bw_finish(&part0)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }

  /* Tokens follow the decoder: Y2, sixteen luma AC blocks, then U and
   * V. The next macroblock predicts from this one's reconstruction. */
  {
    int y2_left;
    memset(y_nz, 0, (size_t)y4_stride * mb_h * 4u);
    memset(u_nz, 0, (size_t)uv4_stride * mb_h * 2u);
    memset(v_nz, 0, (size_t)uv4_stride * mb_h * 2u);
    memset(y2_above, 0, mb_w);

    for (mb_y = 0; mb_y < mb_h; ++mb_y) {
      y2_left = 0;
      for (mb_x = 0; mb_x < mb_w; ++mb_x) {
        const int bx = (int)mb_x * 16;
        const int by = (int)mb_y * 16;
        const int ubx = (int)mb_x * 8;
        const int uby = (int)mb_y * 8;
        const uint32_t mi = mb_y * mb_w + mb_x;
        int ac_levels[16][16];
        int y2_level[16];
        int levels_u[4][16];
        int levels_v[4][16];
        int sy;
        int sx;
        int ctx;
        int nz;
        int y1_dc_m;
        int y1_ac_m;
        int y2_dc_m;
        int y2_ac_m;
        int uv_dc_m;
        int uv_ac_m;
        mb_quants(base_q + (seg[mi] ? q_delta : 0), &y1_dc_m, &y1_ac_m,
            &y2_dc_m, &y2_ac_m, &uv_dc_m, &uv_ac_m);

        if (skip_on && mb_skip[mi]) {
          int zi;
          if (y_mode[mi] != 4) {
            y2_left = 0;
            y2_above[mb_x] = 0;
          }
          for (zi = 0; zi < 16; ++zi) {
            const int fx = (int)mb_x * 4 + (zi & 3);
            const int fy = (int)mb_y * 4 + (zi >> 2);
            y_nz[fy * y4_stride + fx] = 0;
          }
          for (zi = 0; zi < 4; ++zi) {
            const int fx = (int)mb_x * 2 + (zi & 1);
            const int fy = (int)mb_y * 2 + (zi >> 1);
            u_nz[fy * uv4_stride + fx] = 0;
            v_nz[fy * uv4_stride + fx] = 0;
          }
          continue;
        }

        if (y_mode[mi] == 4) {
          for (sy = 0; sy < 4; ++sy) {
            for (sx = 0; sx < 4; ++sx) {
              const int fx = (int)mb_x * 4 + sx;
              const int fy = (int)mb_y * 4 + sy;
              int levels[16];
              ctx = 0;
              if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
                ctx++;
              }
              if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
                ctx++;
              }
              build_b4(y_plane, (int)y_stride, (int)width, (int)height, rec_y,
                  (int)y_stride, (int)mb_w, (int)mb_x, (int)mb_y, sx, sy,
                  b_mode[mi * 16u + (uint32_t)sy * 4u + (uint32_t)sx], y1_dc_m,
                  y1_ac_m, levels, ctx, coeff_prob);
              nz = put_block(&tokens, write_prob[3], ctx, 0, levels);
              y_nz[fy * y4_stride + fx] = (uint8_t)nz;
            }
          }
        }
        else {
          build_i16(y_plane, (int)y_stride, (int)width, (int)height, rec_y,
              (int)y_stride, bx, by, y_mode[mi], y1_ac_m, y2_dc_m, y2_ac_m,
              ac_levels, y2_level, y_nz, y4_stride, coeff_prob);
          ctx = (y2_left ? 1 : 0) + (y2_above[mb_x] ? 1 : 0);
          nz = put_block(&tokens, write_prob[1], ctx, 0, y2_level);
          y2_left = nz;
          y2_above[mb_x] = (uint8_t)nz;
          for (sy = 0; sy < 4; ++sy) {
            for (sx = 0; sx < 4; ++sx) {
              const int fx = (int)mb_x * 4 + sx;
              const int fy = (int)mb_y * 4 + sy;
              ctx = 0;
              if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
                ctx++;
              }
              if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
                ctx++;
              }
              nz = put_block(&tokens, write_prob[0], ctx, 1,
                  ac_levels[sy * 4 + sx]);
              y_nz[fy * y4_stride + fx] = (uint8_t)nz;
            }
          }
        }

        build_uv(u_plane, v_plane, rec_u, rec_v, (int)uv_stride, ubx, uby,
            (int)uv_w, (int)uv_h, uv_mode[mi], uv_dc_m, uv_ac_m, levels_u,
            levels_v, u_nz, v_nz, uv4_stride, coeff_prob);
        for (int ch = 0; ch < 2; ++ch) {
          uint8_t * nz_plane = (ch == 0) ? u_nz : v_nz;
          int (*levels)[16] = (ch == 0) ? levels_u : levels_v;
          for (sy = 0; sy < 2; ++sy) {
            for (sx = 0; sx < 2; ++sx) {
              const int fx = (int)mb_x * 2 + sx;
              const int fy = (int)mb_y * 2 + sy;
              ctx = 0;
              if (fy > 0 && nz_plane[(fy - 1) * uv4_stride + fx]) {
                ctx++;
              }
              if (fx > 0 && nz_plane[fy * uv4_stride + (fx - 1)]) {
                ctx++;
              }
              nz = put_block(&tokens, write_prob[2], ctx, 0,
                  levels[sy * 2 + sx]);
              nz_plane[fy * uv4_stride + fx] = (uint8_t)nz;
            }
          }
        }
      }
    }
  }

  if (!bw_finish(&tokens)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }

  /* Frame tag (3) + keyframe header (7) + part0 + tokens. */
  out_cap = 10u + part0.pos + tokens.pos + 16u;
  out = (unsigned char *)gimg_malloc(alloc, out_cap);
  if (!out) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  {
    uint32_t part0_size = (uint32_t)part0.pos;
    uint32_t tag = (part0_size << 5) | (1u << 4); /* keyframe, profile 0, show */
    out[0] = (unsigned char)(tag & 0xffu);
    out[1] = (unsigned char)((tag >> 8) & 0xffu);
    out[2] = (unsigned char)((tag >> 16) & 0xffu);
    out[3] = 0x9d;
    out[4] = 0x01;
    out[5] = 0x2a;
    out[6] = (unsigned char)(width & 0xffu);
    out[7] = (unsigned char)((width >> 8) & 0x3fu);
    out[8] = (unsigned char)(height & 0xffu);
    out[9] = (unsigned char)((height >> 8) & 0x3fu);
    memcpy(out + 10, part0.buf, part0.pos);
    memcpy(out + 10 + part0.pos, tokens.buf, tokens.pos);
    out_len = 10u + part0.pos + tokens.pos;
  }

  *out_bytes = out;
  *out_size = out_len;
  out = NULL;

Done:
  gimg_free(alloc, out);
  gimg_free(alloc, part0.buf);
  gimg_free(alloc, tokens.buf);
  gimg_free(alloc, y_plane);
  gimg_free(alloc, u_plane);
  gimg_free(alloc, v_plane);
  gimg_free(alloc, rec_y);
  gimg_free(alloc, rec_u);
  gimg_free(alloc, rec_v);
  gimg_free(alloc, y_mode);
  gimg_free(alloc, uv_mode);
  gimg_free(alloc, b_mode);
  gimg_free(alloc, seg);
  gimg_free(alloc, mb_skip);
  gimg_free(alloc, mb_nz);
  gimg_free(alloc, above_b);
  gimg_free(alloc, y_nz);
  gimg_free(alloc, u_nz);
  gimg_free(alloc, v_nz);
  gimg_free(alloc, y2_above);
  return r;
}
