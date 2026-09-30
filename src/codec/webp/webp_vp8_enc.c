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
 * The boolean coder follows RFC 6386 section 7. The coefficient
 * probabilities, category probabilities, coefficient bands, and the DC
 * dequantization table are generated from that RFC; see
 * webp_vp8_proba.inc.
 */

/**
 * @file
 *
 * Deliberately minimal VP8 keyframe encoder. Every macroblock is Intra16
 * DC with a single quantizer; residuals are Y2/UV DC only. dwebp accepts
 * the output. It is not a quality claim.
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

static const uint8_t k_norm[128] = {
  7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
  3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
  2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0
};

static const uint8_t k_new_range[128] = {
  127, 127, 191, 127, 159, 191, 223, 127, 143, 159, 175, 191, 207, 223, 239,
  127, 135, 143, 151, 159, 167, 175, 183, 191, 199, 207, 215, 223, 231, 239,
  247, 127, 131, 135, 139, 143, 147, 151, 155, 159, 163, 167, 171, 175, 179,
  183, 187, 191, 195, 199, 203, 207, 211, 215, 219, 223, 227, 231, 235, 239,
  243, 247, 251, 127, 129, 131, 133, 135, 137, 139, 141, 143, 145, 147, 149,
  151, 153, 155, 157, 159, 161, 163, 165, 167, 169, 171, 173, 175, 177, 179,
  181, 183, 185, 187, 189, 191, 193, 195, 197, 199, 201, 203, 205, 207, 209,
  211, 213, 215, 217, 219, 221, 223, 225, 227, 229, 231, 233, 235, 237, 239,
  241, 243, 245, 247, 249, 251, 253, 127
};

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
    const int shift = k_norm[bw->range];
    bw->range = k_new_range[bw->range];
    bw->value <<= shift;
    bw->nb_bits += shift;
    if (bw->nb_bits > 0) {
      bw_flush(bw);
    }
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
    bw->range = k_new_range[bw->range];
    bw->value <<= 1;
    bw->nb_bits += 1;
    if (bw->nb_bits > 0) {
      bw_flush(bw);
    }
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

static int clip117(int v) {
  return v < 0 ? 0 : v > 117 ? 117 : v;
}

static int rgb_to_y(int r, int g, int b) {
  const int luma = 16839 * r + 33059 * g + 6420 * b;
  return (luma + YUV_HALF + (16 << YUV_FIX)) >> YUV_FIX;
}

static int clip_uv(int uv, int rounding) {
  uv = (uv + rounding + (128 << (YUV_FIX + 2))) >> (YUV_FIX + 2);
  return ((uv & ~0xff) == 0) ? uv : (uv < 0) ? 0 : 255;
}

static int rgb_to_u(int r, int g, int b) {
  return clip_uv(-9719 * r - 19081 * g + 28800 * b, YUV_HALF << 2);
}

static int rgb_to_v(int r, int g, int b) {
  return clip_uv(28800 * r - 24116 * g - 4684 * b, YUV_HALF << 2);
}

static uint8_t sample_y(const uint8_t * y, int stride, int x, int y0, int w,
    int h) {
  if (x < 0) {
    x = 0;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (x >= w) {
    x = w - 1;
  }
  if (y0 >= h) {
    y0 = h - 1;
  }
  return y[y0 * stride + x];
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

/** Emit one coeff band starting at position @a first; only coeff[0] may be set.
 *  Returns the GetCoeffs-style return value (position after last nonzero, or
 *  first on immediate EOB). */
static int put_dc_token(bool_writer_t * bw,
    const uint8_t bands[GIMG_VP8_NUM_BANDS][GIMG_VP8_NUM_CTX]
        [GIMG_VP8_NUM_PROBAS],
    int ctx, int first, int level) {
  const uint8_t * p = bands[gimg_vp8_bands[first]][ctx];
  int abs_l;
  if (level == 0) {
    bw_put_bit(bw, 0, p[0]); /* EOB */
    return first;
  }
  bw_put_bit(bw, 1, p[0]); /* not EOB */
  bw_put_bit(bw, 1, p[1]); /* not zero */
  abs_l = level < 0 ? -level : level;
  if (abs_l == 1) {
    bw_put_bit(bw, 0, p[2]);
  }
  else {
    bw_put_bit(bw, 1, p[2]);
    put_large_value(bw, p, abs_l);
  }
  bw_put_bit_uniform(bw, level < 0); /* sign: 1 = negative */
  /* EOB for remaining coeffs in the band. */
  {
    const uint8_t * p_next =
        bands[gimg_vp8_bands[first + 1]][abs_l > 1 ? 2 : 1];
    bw_put_bit(bw, 0, p_next[0]);
  }
  return first + 1;
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
  int y2_q;
  int uv_q;
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
  y2_q = gimg_vp8_dc_qlookup[clip127(base_q)] * 2;
  uv_q = gimg_vp8_dc_qlookup[clip117(base_q)];

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

  /* RGB → YUV 4:2:0 (libwebp fixed-point spirit). */
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
        rsum /= n;
        gsum /= n;
        bsum /= n;
      }
      u_plane[(y / 2u) * uv_stride + (x / 2u)] =
          (uint8_t)rgb_to_u(rsum, gsum, bsum);
      v_plane[(y / 2u) * uv_stride + (x / 2u)] =
          (uint8_t)rgb_to_v(rsum, gsum, bsum);
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

  /* Tokens: Y2 DC + 16 Y EOBs + 4 UV DCs per MB. Track nz like the decoder. */
  {
    uint8_t * row_nz_dc = (uint8_t *)gimg_malloc(alloc, mb_w);
    uint8_t * row_nz = (uint8_t *)gimg_malloc(alloc, mb_w);
    uint8_t left_dc = 0;
    uint8_t left_nz_bits = 0;
    if (!row_nz_dc || !row_nz) {
      gimg_free(alloc, row_nz_dc);
      gimg_free(alloc, row_nz);
      r = GIMG_ERR_OOM;
      goto Done;
    }
    memset(row_nz_dc, 0, mb_w);
    memset(row_nz, 0, mb_w);

    for (mb_y = 0; mb_y < mb_h; ++mb_y) {
      left_dc = 0;
      left_nz_bits = 0;
      for (mb_x = 0; mb_x < mb_w; ++mb_x) {
        const uint32_t bx = mb_x * 16u;
        const uint32_t by = mb_y * 16u;
        int pred;
        int64_t sum;
        int level;
        int ctx;
        int i;
        int x;
        int y;
        uint8_t top_dc = row_nz_dc[mb_x];
        uint8_t top_nz_bits = row_nz[mb_x];
        uint8_t out_t_nz;
        uint8_t out_l_nz;
        uint8_t tnz;
        uint8_t lnz;
        int l;
        int nz;

        /* Intra16 DC prediction from top/left borders. */
        if (mb_x == 0u && mb_y == 0u) {
          pred = 128;
        }
        else if (mb_x == 0u) {
          int s = 0;
          for (i = 0; i < 16; ++i) {
            s += sample_y(y_plane, (int)y_stride, (int)bx + i, (int)by - 1,
                (int)width, (int)height);
          }
          pred = (s + 8) >> 4;
        }
        else if (mb_y == 0u) {
          int s = 0;
          for (i = 0; i < 16; ++i) {
            s += sample_y(y_plane, (int)y_stride, (int)bx - 1, (int)by + i,
                (int)width, (int)height);
          }
          pred = (s + 8) >> 4;
        }
        else {
          int s = 0;
          for (i = 0; i < 16; ++i) {
            s += sample_y(y_plane, (int)y_stride, (int)bx + i, (int)by - 1,
                (int)width, (int)height);
            s += sample_y(y_plane, (int)y_stride, (int)bx - 1, (int)by + i,
                (int)width, (int)height);
          }
          pred = (s + 16) >> 5;
        }

        sum = 0;
        for (uint32_t yy = 0; yy < 16u; ++yy) {
          for (uint32_t xx = 0; xx < 16u; ++xx) {
            uint32_t px = bx + xx;
            uint32_t py = by + yy;
            int sample = 128;
            if (px < width && py < height) {
              sample = y_plane[py * y_stride + px];
            }
            else if (width > 0u && height > 0u) {
              sample = sample_y(y_plane, (int)y_stride, (int)px, (int)py,
                  (int)width, (int)height);
            }
            sum += sample - pred;
          }
        }
        {
          int mean8 = (int)((sum * 8) / 256);
          level = quantize(mean8, y2_q);
        }
        ctx = top_dc + left_dc;
        nz = put_dc_token(&tokens, gimg_vp8_coeffs_proba0[1], ctx, 0, level);
        left_dc = top_dc = (nz > 0);
        row_nz_dc[mb_x] = top_dc;

        /* 16 luma 4x4: AC only (first=1), all EOB — still track nz bits. */
        tnz = top_nz_bits & 0x0fu;
        lnz = left_nz_bits & 0x0fu;
        for (y = 0; y < 4; ++y) {
          l = lnz & 1;
          for (x = 0; x < 4; ++x) {
            ctx = l + (tnz & 1);
            nz = put_dc_token(
                &tokens, gimg_vp8_coeffs_proba0[0], ctx, 1, 0);
            l = (nz > 1);
            tnz = (uint8_t)((tnz >> 1) | (l << 7));
          }
          tnz = (uint8_t)(tnz >> 4);
          lnz = (uint8_t)((lnz >> 1) | (l << 7));
        }
        out_t_nz = tnz;
        out_l_nz = (uint8_t)(lnz >> 4);

        /* 4 UV 4x4 blocks (U then V), DC only. */
        for (int ch = 0; ch < 4; ch += 2) {
          const uint8_t * uv = (ch == 0) ? u_plane : v_plane;
          const uint32_t ubx = mb_x * 8u;
          const uint32_t uby = mb_y * 8u;
          int upred = 128;
          int uy;
          int ux;
          if (mb_x > 0u || mb_y > 0u) {
            int s = 0;
            int n = 0;
            if (mb_y > 0u) {
              for (ux = 0; ux < 8; ++ux) {
                uint32_t px = ubx + (uint32_t)ux;
                uint32_t py = uby - 1u;
                if (px < ((width + 1u) >> 1) && py < ((height + 1u) >> 1)) {
                  s += uv[py * uv_stride + px];
                  n++;
                }
              }
            }
            if (mb_x > 0u) {
              for (uy = 0; uy < 8; ++uy) {
                uint32_t px = ubx - 1u;
                uint32_t py = uby + (uint32_t)uy;
                if (px < ((width + 1u) >> 1) && py < ((height + 1u) >> 1)) {
                  s += uv[py * uv_stride + px];
                  n++;
                }
              }
            }
            if (n > 0) {
              upred = (s + n / 2) / n;
            }
          }
          tnz = (uint8_t)(top_nz_bits >> (4 + ch));
          lnz = (uint8_t)(left_nz_bits >> (4 + ch));
          for (uy = 0; uy < 2; ++uy) {
            l = lnz & 1;
            for (ux = 0; ux < 2; ++ux) {
              int64_t usum = 0;
              int ulevel;
              for (int dy = 0; dy < 4; ++dy) {
                for (int dx = 0; dx < 4; ++dx) {
                  uint32_t px = ubx + (uint32_t)(ux * 4 + dx);
                  uint32_t py = uby + (uint32_t)(uy * 4 + dy);
                  int sample = 128;
                  if (px < ((width + 1u) >> 1) &&
                      py < ((height + 1u) >> 1)) {
                    sample = uv[py * uv_stride + px];
                  }
                  usum += sample - upred;
                }
              }
              ulevel = quantize((int)(usum / 2), uv_q);
              ctx = l + (tnz & 1);
              nz = put_dc_token(
                  &tokens, gimg_vp8_coeffs_proba0[2], ctx, 0, ulevel);
              l = (nz > 0);
              tnz = (uint8_t)((tnz >> 1) | (l << 3));
            }
            tnz = (uint8_t)(tnz >> 2);
            lnz = (uint8_t)((lnz >> 1) | (l << 5));
          }
          out_t_nz = (uint8_t)(out_t_nz | ((tnz << 4) << ch));
          out_l_nz = (uint8_t)(out_l_nz | ((lnz & 0xf0) << ch));
        }
        row_nz[mb_x] = out_t_nz;
        left_nz_bits = out_l_nz;
      }
    }
    gimg_free(alloc, row_nz_dc);
    gimg_free(alloc, row_nz);
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
  return r;
}
