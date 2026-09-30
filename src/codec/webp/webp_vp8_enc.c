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
 * VP8 keyframe encoder. Every macroblock is Intra16 DC with one quantizer.
 * The residual is the section 14 Walsh-Hadamard of the sixteen DC
 * coefficients and the 4×4 DCT of everything else. Mode search is still
 * one mode. dwebp accepts the output. make webp-rd is the comparison
 * against cwebp.
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

/* effort 0..9 → base_q (higher = coarser). Default effort 4 → mid ladder. */
static const int k_effort_q[10] = {
  100, 90, 80, 70, 60, 50, 40, 30, 20, 10
};

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
    const uint8_t bands[GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
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


static void write_frame_header(bool_writer_t * part0, int base_q) {
  int t, b, c, p;
  /* keyframe: colorspace=0 (YUV), clamp=0 */
  bw_put_bit_uniform(part0, 0);
  bw_put_bit_uniform(part0, 0);
  /* segment: unused */
  bw_put_bit_uniform(part0, 0);
  /* filter: off */
  bw_put_bit_uniform(part0, 0); /* simple */
  bw_put_bits(part0, 0, 6);     /* level */
  bw_put_bits(part0, 0, 3);     /* sharpness */
  bw_put_bit_uniform(part0, 0); /* lf delta */
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
          bw_put_bit(part0, 0, gimg_vp8_coeffs_update_proba[t][b][c][p]);
        }
      }
    }
  }
  bw_put_bit_uniform(part0, 0); /* no skip proba */
}

static void write_mb_modes(bool_writer_t * part0) {
  /* Intra16 (bit 1 @145), DC_PRED (0,0 @156/163), UV DC (0 @142) */
  bw_put_bit(part0, 1, 145);
  bw_put_bit(part0, 0, 156);
  bw_put_bit(part0, 0, 163);
  bw_put_bit(part0, 0, 142);
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
  int y1_ac_q;
  int y2_dc_q;
  int y2_ac_q;
  int uv_dc_q;
  int uv_ac_q;
  uint8_t * rec_y = NULL;
  uint8_t * rec_u = NULL;
  uint8_t * rec_v = NULL;
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
  y1_ac_q = (int)gimg_vp8_ac_qlookup[clip127(base_q)];
  y2_dc_q = gimg_vp8_dc_qlookup[clip127(base_q)] * 2;
  y2_ac_q = (int)gimg_vp8_ac_qlookup[clip127(base_q)] * 155 / 100;
  uv_dc_q = gimg_vp8_dc_qlookup[clip127(base_q)];
  uv_ac_q = (int)gimg_vp8_ac_qlookup[clip127(base_q)];

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
  (void)uv_w;
  (void)uv_h;

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

  bw_init(&part0, alloc);
  bw_init(&tokens, alloc);
  write_frame_header(&part0, base_q);

  /* Modes for every MB (partition 0). */
  for (mb_y = 0; mb_y < mb_h; ++mb_y) {
    for (mb_x = 0; mb_x < mb_w; ++mb_x) {
      write_mb_modes(&part0);
    }
  }
  if (!bw_finish(&part0)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }

  /* Tokens follow the decoder: Y2, sixteen luma AC blocks, then U and
   * V. The next macroblock predicts from this one's reconstruction. */
  {
    const int y4_stride = (int)mb_w * 4;
    const int uv4_stride = (int)mb_w * 2;
    uint8_t * y_nz =
        (uint8_t *)gimg_malloc(alloc, (size_t)y4_stride * mb_h * 4u);
    uint8_t * u_nz =
        (uint8_t *)gimg_malloc(alloc, (size_t)uv4_stride * mb_h * 2u);
    uint8_t * v_nz =
        (uint8_t *)gimg_malloc(alloc, (size_t)uv4_stride * mb_h * 2u);
    uint8_t * y2_above = (uint8_t *)gimg_malloc(alloc, mb_w);
    int y2_left;
    rec_y = (uint8_t *)gimg_malloc(alloc, (size_t)y_stride * mb_h * 16u);
    rec_u = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
    rec_v = (uint8_t *)gimg_malloc(alloc, (size_t)uv_stride * mb_h * 8u);
    if (!y_nz || !u_nz || !v_nz || !y2_above || !rec_y || !rec_u || !rec_v) {
      gimg_free(alloc, y_nz);
      gimg_free(alloc, u_nz);
      gimg_free(alloc, v_nz);
      gimg_free(alloc, y2_above);
      r = GIMG_ERR_OOM;
      goto Done;
    }
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
        int pred;
        int dc_coeff[16];
        int ac_levels[16][16];
        int y2_level[16];
        int wht[16];
        int y2_hat[16];
        int spatial[16];
        int sy;
        int sx;
        int i;
        int ctx;
        int nz;

        pred = dc_pred(rec_y, (int)y_stride, bx, by, 16);
        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            int residual[16];
            int coeff[16];
            int dy;
            int dx;
            const int bi = sy * 4 + sx;
            for (dy = 0; dy < 4; ++dy) {
              for (dx = 0; dx < 4; ++dx) {
                residual[dy * 4 + dx] =
                    src_at(y_plane, (int)y_stride, bx + sx * 4 + dx,
                        by + sy * 4 + dy, (int)width, (int)height) -
                    pred;
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
          y2_hat[i] = y2_level[i] * (i == 0 ? y2_dc_q : y2_ac_q);
        }
        ctx = (y2_left ? 1 : 0) + (y2_above[mb_x] ? 1 : 0);
        nz = put_block(&tokens, gimg_vp8_coeffs_proba0[1], ctx, 0, y2_level);
        y2_left = nz;
        y2_above[mb_x] = (uint8_t)nz;
        iwht4(y2_hat, spatial);

        for (sy = 0; sy < 4; ++sy) {
          for (sx = 0; sx < 4; ++sx) {
            const int fx = (int)mb_x * 4 + sx;
            const int fy = (int)mb_y * 4 + sy;
            const int bi = sy * 4 + sx;
            int block[16];
            int residue[16];
            int dy;
            int dx;
            ctx = 0;
            if (fy > 0 && y_nz[(fy - 1) * y4_stride + fx]) {
              ctx++;
            }
            if (fx > 0 && y_nz[fy * y4_stride + (fx - 1)]) {
              ctx++;
            }
            nz = put_block(
                &tokens, gimg_vp8_coeffs_proba0[0], ctx, 1, ac_levels[bi]);
            y_nz[fy * y4_stride + fx] = (uint8_t)nz;
            block[0] = spatial[bi];
            for (i = 1; i < 16; ++i) {
              block[i] = ac_levels[bi][i] * y1_ac_q;
            }
            idct4(block, residue);
            for (dy = 0; dy < 4; ++dy) {
              for (dx = 0; dx < 4; ++dx) {
                const int v =
                    pred + residue[dy * 4 + dx];
                rec_y[(by + sy * 4 + dy) * (int)y_stride + bx + sx * 4 + dx] =
                    (uint8_t)clip8(v);
              }
            }
          }
        }

        for (int ch = 0; ch < 2; ++ch) {
          const uint8_t * src = (ch == 0) ? u_plane : v_plane;
          uint8_t * rec = (ch == 0) ? rec_u : rec_v;
          uint8_t * nz_plane = (ch == 0) ? u_nz : v_nz;
          int uv_levels[4][16];
          int uv_pred;
          int bi;
          uv_pred = dc_pred(rec, (int)uv_stride, ubx, uby, 8);
          for (sy = 0; sy < 2; ++sy) {
            for (sx = 0; sx < 2; ++sx) {
              int residual[16];
              int coeff[16];
              int dy;
              int dx;
              bi = sy * 2 + sx;
              for (dy = 0; dy < 4; ++dy) {
                for (dx = 0; dx < 4; ++dx) {
                  residual[dy * 4 + dx] =
                      src_at(src, (int)uv_stride, ubx + sx * 4 + dx,
                          uby + sy * 4 + dy, (int)uv_w, (int)uv_h) -
                      uv_pred;
                }
              }
              fdct4(residual, coeff);
              for (i = 0; i < 16; ++i) {
                uv_levels[bi][i] =
                    quantize(coeff[i], i == 0 ? uv_dc_q : uv_ac_q);
              }
            }
          }
          for (sy = 0; sy < 2; ++sy) {
            for (sx = 0; sx < 2; ++sx) {
              const int fx = (int)mb_x * 2 + sx;
              const int fy = (int)mb_y * 2 + sy;
              int block[16];
              int residue[16];
              int dy;
              int dx;
              bi = sy * 2 + sx;
              ctx = 0;
              if (fy > 0 && nz_plane[(fy - 1) * uv4_stride + fx]) {
                ctx++;
              }
              if (fx > 0 && nz_plane[fy * uv4_stride + (fx - 1)]) {
                ctx++;
              }
              nz = put_block(&tokens, gimg_vp8_coeffs_proba0[2], ctx, 0,
                  uv_levels[bi]);
              nz_plane[fy * uv4_stride + fx] = (uint8_t)nz;
              for (i = 0; i < 16; ++i) {
                block[i] =
                    uv_levels[bi][i] * (i == 0 ? uv_dc_q : uv_ac_q);
              }
              idct4(block, residue);
              for (dy = 0; dy < 4; ++dy) {
                for (dx = 0; dx < 4; ++dx) {
                  const int v = uv_pred + residue[dy * 4 + dx];
                  rec[(uby + sy * 4 + dy) * (int)uv_stride + ubx + sx * 4 +
                      dx] = (uint8_t)clip8(v);
                }
              }
            }
          }
        }
      }
    }
    gimg_free(alloc, y_nz);
    gimg_free(alloc, u_nz);
    gimg_free(alloc, v_nz);
    gimg_free(alloc, y2_above);
  }

  if (!bw_finish(&tokens)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  /* Padding so the decoder's bool reader is not at eof after the last MB
   * (VP8DecodeMB fails when token_br->eof_ is set at return). Large frames
   * need more slack than a handful of bytes. */
  if (!bw_resize(&tokens, 256u)) {
    r = GIMG_ERR_OOM;
    goto Done;
  }
  {
    int pi;
    for (pi = 0; pi < 256; ++pi) {
      tokens.buf[tokens.pos++] = 0;
    }
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
  return r;
}
