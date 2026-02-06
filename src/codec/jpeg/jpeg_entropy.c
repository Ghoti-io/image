/**
 * @file
 *
 * JPEG entropy decoding (Huffman), dezigzag, dequantise, inverse DCT.
 * Used for baseline decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/raster.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../raster/raster_internal.h"
#include "jpeg_internal.h"

// Disable clang-format for this block of code.
// clang-format off

// Zigzag order (stream index -> 8x8 row-major position). ITU-T T.81.
static const uint8_t gimg_jpeg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// clang-format on

/** Huffman decode table: for each code length L, min/max code and symbol base.
 */
typedef struct {
  uint16_t min_code[17]; ///< Min code value for length 1..16 (L bits).
  uint16_t max_code[17]; ///< Max code value for length 1..16.
  uint16_t
      base_index[17];  ///< Index into values for first symbol at this length.
  uint8_t values[256]; ///< Symbol values in code length order.
  int num_values;
} gimg_jpeg_huff_table_t;

/** Bitstream over scan data (MSB first; 0xFF 0x00 is data). */
typedef struct {
  const unsigned char * data;
  size_t size;
  size_t byte_off;
  int bit_off; ///< 0..7; next bit is at (data[byte_off] >> (7 - bit_off)) & 1.
} gimg_jpeg_bitstream_t;

static void jpeg_bitstream_init(
    gimg_jpeg_bitstream_t * bs, const unsigned char * data, size_t size) {
  bs->data = data;
  bs->size = size;
  bs->byte_off = 0;
  bs->bit_off = 0;
}

/** Read one bit; return 0 or 1, or -1 on underflow. */
static int jpeg_bitstream_read_bit(gimg_jpeg_bitstream_t * bs) {
  if (bs->byte_off >= bs->size) {
    return -1;
  }
  unsigned char b = bs->data[bs->byte_off];
  int bit = (b >> (7 - bs->bit_off)) & 1;
  bs->bit_off++;
  if (bs->bit_off == 8) {
    bs->bit_off = 0;
    bs->byte_off++;
    // Byte stuffing: 0xFF 0x00 is data 0xFF.
    if (bs->byte_off < bs->size && bs->data[bs->byte_off - 1] == 0xFF &&
        bs->data[bs->byte_off] == 0x00) {
      bs->byte_off++;
    }
  }
  return bit;
}

/** Read n bits (1..16), MSB first. Return value or -1 on underflow. */
static int jpeg_bitstream_read_bits(gimg_jpeg_bitstream_t * bs, int n) {
  int v = 0;
  for (int i = 0; i < n; i++) {
    int b = jpeg_bitstream_read_bit(bs);
    if (b < 0) {
      return -1;
    }
    v = (v << 1) | b;
  }
  return v;
}

/** Build Huffman decode table from DHT payload (TcTh byte + 16 counts +
 * symbols). */
static int jpeg_build_huff_table(
    const unsigned char * dht, size_t dht_len, gimg_jpeg_huff_table_t * tbl) {
  if (dht_len < 17) {
    return -1;
  }
  const unsigned char * bits = dht + 1;
  size_t num_syms = 0;
  for (int i = 0; i < 16; i++) {
    num_syms += bits[i];
  }
  if (dht_len < 17 + num_syms) {
    return -1;
  }
  const unsigned char * vals = dht + 17;
  tbl->num_values = (int)num_syms;

  // Canonical code assignment.
  uint32_t code = 0;
  uint16_t base = 0;
  for (int len = 1; len <= 16; len++) {
    uint8_t count = bits[len - 1];
    tbl->min_code[len] = (uint16_t)code;
    if (count > 0) {
      tbl->max_code[len] = (uint16_t)(code + count - 1);
      tbl->base_index[len] = base;
      for (int k = 0; k < count; k++) {
        tbl->values[base + k] = vals[base + k];
      }
      base += count;
    }
    else {
      tbl->max_code[len] = 0;
      tbl->min_code[len] = 1;
      tbl->base_index[len] = 0;
    }
    code = (code + count) << 1;
  }
  return 0;
}

/** Decode one symbol using the given Huffman table. Return symbol or -1. */
static int jpeg_huff_decode(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * tbl) {
  uint16_t code = 0;
  for (int len = 1; len <= 16; len++) {
    int b = jpeg_bitstream_read_bit(bs);
    if (b < 0) {
      return -1;
    }
    code = (code << 1) | (uint16_t)b;
    if (code >= tbl->min_code[len] && code <= tbl->max_code[len]) {
      uint16_t idx = tbl->base_index[len] + (code - tbl->min_code[len]);
      return (int)tbl->values[idx];
    }
  }
  return -1;
}

/** Extend sign for n-bit value (Table K.2). */
static int16_t jpeg_extend(int val, int n) {
  if (n == 0) {
    return 0;
  }
  int half = 1 << (n - 1);
  if (val < half) {
    return (int16_t)(val + (int)(1 - (1u << n)));
  }
  return (int16_t)val;
}

/** Decode one 8x8 block (DC + AC). Block is 64 int16_t in zigzag order. */
static GIMG_Result jpeg_decode_block(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * dc_tbl,
    const gimg_jpeg_huff_table_t * ac_tbl, int16_t * block,
    int16_t * dc_predictor) {
  int sym = jpeg_huff_decode(bs, dc_tbl);
  if (sym < 0) {
    return GIMG_ERR_CORRUPT;
  }
  int nbits = sym;
  int diff = 0;
  if (nbits > 0) {
    diff = jpeg_bitstream_read_bits(bs, nbits);
    if (diff < 0) {
      return GIMG_ERR_CORRUPT;
    }
    diff = jpeg_extend(diff, nbits);
  }
  *dc_predictor += diff;
  block[0] = *dc_predictor;

  for (int k = 1; k < 64; k++) {
    sym = jpeg_huff_decode(bs, ac_tbl);
    if (sym < 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (sym == 0) {
      // EOB: rest are zero (stream order).
      for (; k < 64; k++) {
        block[k] = 0;
      }
      break;
    }
    int run = sym >> 4;
    int size = sym & 0x0F;
    k += run;
    if (k >= 64) {
      return GIMG_ERR_CORRUPT;
    }
    int ac = 0;
    if (size > 0) {
      ac = jpeg_bitstream_read_bits(bs, size);
      if (ac < 0) {
        return GIMG_ERR_CORRUPT;
      }
      ac = jpeg_extend(ac, size);
    }
    block[k] = (int16_t)ac;
  }
  return GIMG_OK;
}

/** Dezigzag: block has 64 entries in zigzag order; write to out in row-major.
 */
static void jpeg_dezigzag(const int16_t * block, int16_t * out) {
  for (int i = 0; i < 64; i++) {
    out[gimg_jpeg_zigzag[i]] = block[i];
  }
}

/** Dequantise: out[i] = block[i] * quant[i]. */
static void jpeg_dequantise(
    const int16_t * block, const uint16_t * quant, int16_t * out) {
  for (int i = 0; i < 64; i++) {
    out[i] = (int16_t)((int)block[i] * (int)quant[i]);
  }
}

// 1D IDCT for 8 points (row or column). Coefficients in, samples out.
static void jpeg_idct_1d(const int16_t * in, int16_t * out) {
  // C(u) = 1/sqrt(2) for u=0, else 1. Scale by 256 for integer output.
  static const int scale = 256;
  for (int x = 0; x < 8; x++) {
    int32_t sum = 0;
    for (int u = 0; u < 8; u++) {
      int32_t c = (u == 0) ? 181 : 256; // 256/sqrt(2) ~ 181
      // cos((2*x+1)*u*pi/16) * c
      double angle = (2 * x + 1) * u * 3.14159265358979323846 / 16.0;
      sum += (int32_t)((double)in[u] * (double)c * cos(angle) + 0.5);
    }
    out[x] = (int16_t)(sum / scale);
  }
}

/** 2D 8x8 IDCT (row-major 64 entries). */
static void jpeg_idct_8x8(const int16_t * in, int16_t * out) {
  int16_t row[8];
  int16_t tmp[64];
  for (int y = 0; y < 8; y++) {
    jpeg_idct_1d(in + y * 8, row);
    for (int x = 0; x < 8; x++) {
      tmp[x * 8 + y] = row[x];
    }
  }
  for (int x = 0; x < 8; x++) {
    jpeg_idct_1d(tmp + x * 8, row);
    for (int y = 0; y < 8; y++) {
      out[y * 8 + x] = row[y];
    }
  }
}

GIMG_Result gimg_jpeg_decode_baseline(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!state || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  if (state->is_progressive) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const gimg_jpeg_sof_t * sof = &state->sof;
  uint16_t width = sof->width;
  uint16_t height = sof->height;
  uint8_t num_comp = sof->num_components;

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count((uint32_t)width, (uint32_t)height, &pixel_count) !=
      GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Limits * limits =
      options && options->limits ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  if (!state->scan_data || state->scan_data_size == 0) {
    return GIMG_ERR_CORRUPT;
  }

  // Build Huffman tables for scan components.
  gimg_jpeg_huff_table_t dc_tables[4];
  gimg_jpeg_huff_table_t ac_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  memset(ac_tables, 0, sizeof(ac_tables));
  for (uint8_t c = 0; c < state->scan_comp_count; c++) {
    uint8_t dc_id = state->scan_dc_tbl[c];
    uint8_t ac_id = state->scan_ac_tbl[c];
    if (dc_id >= 4 || !state->huff_dc[dc_id] ||
        jpeg_build_huff_table(state->huff_dc[dc_id], state->huff_dc_len[dc_id],
            &dc_tables[dc_id]) != 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (ac_id >= 4 || !state->huff_ac[ac_id] ||
        jpeg_build_huff_table(state->huff_ac[ac_id], state->huff_ac_len[ac_id],
            &ac_tables[ac_id]) != 0) {
      return GIMG_ERR_CORRUPT;
    }
  }

  // MCU dimensions: Hmax, Vmax; blocks per MCU.
  uint8_t h_max = 0;
  uint8_t v_max = 0;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (sof->h_samp[i] > h_max) {
      h_max = sof->h_samp[i];
    }
    if (sof->v_samp[i] > v_max) {
      v_max = sof->v_samp[i];
    }
  }
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t mcu_total = 0;
  if (!gimg_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &mcu_total)) {
    return GIMG_ERR_LIMIT;
  }

  // Blocks per MCU per component.
  size_t blocks_per_mcu = 0;
  for (uint8_t i = 0; i < state->scan_comp_count; i++) {
    uint8_t comp_idx = 0;
    for (; comp_idx < num_comp; comp_idx++) {
      if (sof->comp_id[comp_idx] == state->scan_comp_id[i]) {
        break;
      }
    }
    if (comp_idx >= num_comp) {
      return GIMG_ERR_FORMAT;
    }
    blocks_per_mcu +=
        (size_t)sof->h_samp[comp_idx] * (size_t)sof->v_samp[comp_idx];
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // Per-component dimensions: 8 * ceil((width*H_c)/(8*H_max)) (ITU-T T.81).
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t denom_w = (uint32_t)(8 * h_max);
  uint32_t denom_h = (uint32_t)(8 * v_max);
  for (uint8_t i = 0; i < num_comp; i++) {
    uint32_t w = (uint32_t)width * (uint32_t)sof->h_samp[i];
    uint32_t h = (uint32_t)height * (uint32_t)sof->v_samp[i];
    comp_w[i] = 8 * ((w + denom_w - 1) / denom_w);
    comp_h[i] = 8 * ((h + denom_h - 1) / denom_h);
    if (comp_w[i] == 0)
      comp_w[i] = 8;
    if (comp_h[i] == 0)
      comp_h[i] = 8;
  }

  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_stride[GIMG_JPEG_MAX_COMPONENTS];
  unsigned char * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride[i] = (size_t)comp_w[i];
    if (!gimg_safe_mul_size(comp_stride[i], (size_t)comp_h[i], &comp_size[i])) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, comp_buf[j]);
      }
      return GIMG_ERR_LIMIT;
    }
    comp_buf[i] = (unsigned char *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, comp_buf[j]);
      }
      return GIMG_ERR_OOM;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  gimg_jpeg_bitstream_t bs;
  jpeg_bitstream_init(&bs, state->scan_data, state->scan_data_size);

  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  int16_t block_zig[64];
  int16_t block_rz[64];
  int16_t block_q[64];

  // Decode MCU by MCU.
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      size_t block_idx = 0;
      for (uint8_t s = 0; s < state->scan_comp_count; s++) {
        uint8_t comp_idx = 0;
        for (; comp_idx < num_comp; comp_idx++) {
          if (sof->comp_id[comp_idx] == state->scan_comp_id[s]) {
            break;
          }
        }
        if (comp_idx >= num_comp) {
          goto fail_comp;
        }
        uint8_t h_samp = sof->h_samp[comp_idx];
        uint8_t v_samp = sof->v_samp[comp_idx];
        uint8_t qid = sof->quant_tbl_id[comp_idx];
        if (qid >= GIMG_JPEG_MAX_QUANT_TABLES ||
            !state->quant_tbl_present[qid]) {
          goto fail_comp;
        }
        const uint16_t * quant = state->quant_tbl[qid];
        const gimg_jpeg_huff_table_t * dc_tbl =
            &dc_tables[state->scan_dc_tbl[s]];
        const gimg_jpeg_huff_table_t * ac_tbl =
            &ac_tables[state->scan_ac_tbl[s]];

        for (uint8_t by = 0; by < v_samp; by++) {
          for (uint8_t bx = 0; bx < h_samp; bx++) {
            (void)block_idx;
            GIMG_Result r =
                jpeg_decode_block(&bs, dc_tbl, ac_tbl, block_zig, &dc_pred[s]);
            if (r != GIMG_OK) {
              goto fail_decode;
            }
            jpeg_dezigzag(block_zig, block_rz);
            jpeg_dequantise(block_rz, quant, block_q);
            jpeg_idct_8x8(block_q, block_rz);

            uint32_t dst_x = mcu_x * (uint32_t)(8 * h_samp) + (uint32_t)bx * 8;
            uint32_t dst_y = mcu_y * (uint32_t)(8 * v_samp) + (uint32_t)by * 8;
            uint32_t comp_width = comp_w[comp_idx];
            uint32_t comp_height = comp_h[comp_idx];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = dst_y + (uint32_t)dy;
              if (y >= comp_height) {
                break;
              }
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = dst_x + (uint32_t)dx;
                if (x >= comp_width) {
                  break;
                }
                int v = block_rz[dy * 8 + dx] + 128;
                if (v < 0) {
                  v = 0;
                }
                if (v > 255) {
                  v = 255;
                }
                comp_buf[comp_idx][y * comp_stride[comp_idx] + x] =
                    (unsigned char)v;
              }
            }
          }
        }
      }
    }
  }

  // Create output raster: grayscale or RGB.
  GIMG_Result r;
  if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_decode;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    for (uint32_t y = 0; y < height; y++) {
      memcpy(
          pixels + y * stride, comp_buf[0] + y * comp_stride[0], (size_t)width);
    }
  }
  else if (num_comp == 3) {
    // YCbCr -> RGB (BT.601); sample Cb/Cr with replication for subsampling.
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_decode;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy1 =
          (ch1 > 1 && height > 1) ? (y * (ch1 - 1) / (height - 1)) : 0;
      uint32_t cy2 =
          (ch2 > 1 && height > 1) ? (y * (ch2 - 1) / (height - 1)) : 0;
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx1 =
            (cw1 > 1 && width > 1) ? (x * (cw1 - 1) / (width - 1)) : 0;
        uint32_t cx2 =
            (cw2 > 1 && width > 1) ? (x * (cw2 - 1) / (width - 1)) : 0;
        int yy = comp_buf[0][y * comp_stride[0] + x];
        int cb = comp_buf[1][cy1 * comp_stride[1] + cx1];
        int cr = comp_buf[2][cy2 * comp_stride[2] + cx2];
        int r_val = yy + (int)(1.402 * (cr - 128) + 0.5);
        int g_val = yy - (int)(0.344 * (cb - 128) + 0.714 * (cr - 128) + 0.5);
        int b_val = yy + (int)(1.772 * (cb - 128) + 0.5);
        if (r_val < 0)
          r_val = 0;
        if (r_val > 255)
          r_val = 255;
        if (g_val < 0)
          g_val = 0;
        if (g_val > 255)
          g_val = 255;
        if (b_val < 0)
          b_val = 0;
        if (b_val > 255)
          b_val = 255;
        pixels[y * stride + x * 4 + 0] = (unsigned char)r_val;
        pixels[y * stride + x * 4 + 1] = (unsigned char)g_val;
        pixels[y * stride + x * 4 + 2] = (unsigned char)b_val;
        pixels[y * stride + x * 4 + 3] = 255;
      }
    }
  }
  else {
    r = GIMG_ERR_UNSUPPORTED; // CMYK later.
    goto fail_decode;
  }

  // Attach ICC profile from APP2 to raster color info (single-segment only).
  if (state->app2_icc && state->app2_icc_len > 14u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    color_info.icc_bytes = state->app2_icc + 14;
    color_info.icc_size = state->app2_icc_len - 14u;
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

fail_decode:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return r;
fail_comp:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_ERR_FORMAT;
}
