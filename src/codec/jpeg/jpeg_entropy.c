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
#include <ghoti.io/image/stream.h>
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
  memset(block, 0, 64 * sizeof(int16_t));
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

/** Progressive: decode DC only (Ss=0, Se=0). */
static GIMG_Result jpeg_decode_block_progressive_dc(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * dc_tbl,
    int16_t * block, int16_t * dc_predictor) {
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
  return GIMG_OK;
}

/** Progressive AC initial (Ah=0): decode band [ss, se], store (value << al). */
static GIMG_Result jpeg_decode_block_progressive_ac_initial(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se, int al) {
  int k = ss;
  while (k <= se) {
    int sym = jpeg_huff_decode(bs, ac_tbl);
    if (sym < 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (sym == 0) {
      // EOB: rest of band zero
      for (; k <= se; k++) {
        block[k] = 0;
      }
      break;
    }
    int run = sym >> 4;
    int size = sym & 0x0F;
    k += run;
    if (k > se) {
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
    block[k] = (int16_t)(ac << al);
    k++;
  }
  return GIMG_OK;
}

/** Progressive AC refinement (Ah!=0): skip run zeros, one bit per non-zero. */
static GIMG_Result jpeg_decode_block_progressive_ac_refine(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se) {
  int k = ss;
  while (k <= se) {
    int sym = jpeg_huff_decode(bs, ac_tbl);
    if (sym < 0) {
      return GIMG_ERR_CORRUPT;
    }
    if (sym == 0) {
      break; // EOB
    }
    int run = sym >> 4;
    // Skip run zero coefficients (from previous passes).
    while (run > 0 && k <= se) {
      if (block[k] == 0) {
        run--;
      }
      k++;
    }
    if (k > se) {
      return GIMG_ERR_CORRUPT;
    }
    int b = jpeg_bitstream_read_bit(bs);
    if (b < 0) {
      return GIMG_ERR_CORRUPT;
    }
    block[k] = (int16_t)((block[k] << 1) | b);
    k++;
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

  if (state->num_scans == 0) {
    return GIMG_ERR_CORRUPT;
  }
  const gimg_jpeg_scan_t * scan0 = &state->scans[0];
  if (!scan0->data || scan0->data_size == 0) {
    return GIMG_ERR_CORRUPT;
  }

  // Build Huffman tables for scan components.
  gimg_jpeg_huff_table_t dc_tables[4];
  gimg_jpeg_huff_table_t ac_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  memset(ac_tables, 0, sizeof(ac_tables));
  for (uint8_t c = 0; c < scan0->comp_count; c++) {
    uint8_t dc_id = scan0->dc_tbl[c];
    uint8_t ac_id = scan0->ac_tbl[c];
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
  for (uint8_t i = 0; i < scan0->comp_count; i++) {
    uint8_t comp_idx = 0;
    for (; comp_idx < num_comp; comp_idx++) {
      if (sof->comp_id[comp_idx] == scan0->comp_id[i]) {
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
  jpeg_bitstream_init(&bs, scan0->data, scan0->data_size);

  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  int16_t block_zig[64];
  int16_t block_rz[64];
  int16_t block_q[64];
  memset(block_zig, 0, sizeof(block_zig));
  memset(block_rz, 0, sizeof(block_rz));
  memset(block_q, 0, sizeof(block_q));

  // Decode MCU by MCU.
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      size_t block_idx = 0;
      for (uint8_t s = 0; s < scan0->comp_count; s++) {
        uint8_t comp_idx = 0;
        for (; comp_idx < num_comp; comp_idx++) {
          if (sof->comp_id[comp_idx] == scan0->comp_id[s]) {
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
            &dc_tables[scan0->dc_tbl[s]];
        const gimg_jpeg_huff_table_t * ac_tbl =
            &ac_tables[scan0->ac_tbl[s]];

        for (uint8_t by = 0; by < v_samp; by++) {
          for (uint8_t bx = 0; bx < h_samp; bx++) {
            (void)block_idx;
            GIMG_Result r = jpeg_decode_block(&bs, dc_tbl, ac_tbl, block_zig,
                &dc_pred[comp_idx]);
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
  else if (num_comp == 4) {
    // CMYK: decode to CMYK raster; tag color info (ICC if present).
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_decode;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    uint32_t cw[4] = {comp_w[0], comp_w[1], comp_w[2], comp_w[3]};
    uint32_t ch[4] = {comp_h[0], comp_h[1], comp_h[2], comp_h[3]};
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy[4];
      for (int i = 0; i < 4; i++) {
        cy[i] = (ch[i] > 1 && height > 1)
            ? (y * (ch[i] - 1) / (height - 1))
            : 0;
      }
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx[4];
        for (int i = 0; i < 4; i++) {
          cx[i] = (cw[i] > 1 && width > 1)
              ? (x * (cw[i] - 1) / (width - 1))
              : 0;
        }
        pixels[y * stride + x * 4 + 0] =
            comp_buf[0][cy[0] * comp_stride[0] + cx[0]];
        pixels[y * stride + x * 4 + 1] =
            comp_buf[1][cy[1] * comp_stride[1] + cx[1]];
        pixels[y * stride + x * 4 + 2] =
            comp_buf[2][cy[2] * comp_stride[2] + cx[2]];
        pixels[y * stride + x * 4 + 3] =
            comp_buf[3][cy[3] * comp_stride[3] + cx[3]];
      }
    }
  }
  else {
    r = GIMG_ERR_UNSUPPORTED;
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

GIMG_Result gimg_jpeg_decode_progressive(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!state || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (!state->is_progressive || state->num_scans == 0) {
    return GIMG_ERR_CORRUPT;
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

  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t denom_w = (uint32_t)(8 * h_max);
  uint32_t denom_h = (uint32_t)(8 * v_max);
  for (uint8_t i = 0; i < num_comp; i++) {
    uint32_t w = (uint32_t)width * (uint32_t)sof->h_samp[i];
    uint32_t h = (uint32_t)height * (uint32_t)sof->v_samp[i];
    comp_w[i] = 8 * ((w + denom_w - 1) / denom_w);
    comp_h[i] = 8 * ((h + denom_h - 1) / denom_h);
    if (comp_w[i] == 0) {
      comp_w[i] = 8;
    }
    if (comp_h[i] == 0) {
      comp_h[i] = 8;
    }
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // Coefficient buffers: one 64-int16 per block per component (zigzag order).
  size_t blocks_per_comp[GIMG_JPEG_MAX_COMPONENTS];
  int16_t * coef_blocks[GIMG_JPEG_MAX_COMPONENTS];
  memset(coef_blocks, 0, sizeof(coef_blocks));
  for (uint8_t i = 0; i < num_comp; i++) {
    size_t bw = (size_t)(comp_w[i] / 8);
    size_t bh = (size_t)(comp_h[i] / 8);
    if (!gimg_safe_mul_size(bw, bh, &blocks_per_comp[i])) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, coef_blocks[j]);
      }
      return GIMG_ERR_LIMIT;
    }
    size_t coef_size = 0;
    if (!gimg_safe_mul_size(blocks_per_comp[i], 64 * sizeof(int16_t),
            &coef_size)) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, coef_blocks[j]);
      }
      return GIMG_ERR_LIMIT;
    }
    coef_blocks[i] = (int16_t *)gimg_malloc(alloc, coef_size);
    if (!coef_blocks[i]) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, coef_blocks[j]);
      }
      return GIMG_ERR_OOM;
    }
    memset(coef_blocks[i], 0, coef_size);
  }

  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  // Process each scan.
  for (unsigned scan_idx = 0; scan_idx < state->num_scans; scan_idx++) {
    const gimg_jpeg_scan_t * scan = &state->scans[scan_idx];
    if (!scan->data || scan->data_size == 0) {
      goto fail_prog;
    }

    gimg_jpeg_huff_table_t dc_tables[4];
    gimg_jpeg_huff_table_t ac_tables[4];
    memset(dc_tables, 0, sizeof(dc_tables));
    memset(ac_tables, 0, sizeof(ac_tables));
    for (uint8_t c = 0; c < scan->comp_count; c++) {
      uint8_t dc_id = scan->dc_tbl[c];
      uint8_t ac_id = scan->ac_tbl[c];
      if (dc_id >= 4 || !state->huff_dc[dc_id] ||
          jpeg_build_huff_table(state->huff_dc[dc_id], state->huff_dc_len[dc_id],
              &dc_tables[dc_id]) != 0) {
        goto fail_prog;
      }
      if (ac_id >= 4 || !state->huff_ac[ac_id] ||
          jpeg_build_huff_table(state->huff_ac[ac_id], state->huff_ac_len[ac_id],
              &ac_tables[ac_id]) != 0) {
        goto fail_prog;
      }
    }

    gimg_jpeg_bitstream_t bs;
    jpeg_bitstream_init(&bs, scan->data, scan->data_size);

    int is_dc = (scan->ss == 0 && scan->se == 0);
    int ss = (int)scan->ss;
    int se = (int)scan->se;
    int ah = (int)scan->ah;
    int al = (int)scan->al;

    for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
      for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
        for (uint8_t s = 0; s < scan->comp_count; s++) {
          uint8_t comp_idx = 0;
          for (; comp_idx < num_comp; comp_idx++) {
            if (sof->comp_id[comp_idx] == scan->comp_id[s]) {
              break;
            }
          }
          if (comp_idx >= num_comp) {
            goto fail_prog;
          }
          uint8_t h_samp = sof->h_samp[comp_idx];
          uint8_t v_samp = sof->v_samp[comp_idx];
          size_t blocks_per_mcu_comp =
              (size_t)h_samp * (size_t)v_samp;
          size_t mcu_block_start = (size_t)(mcu_y * mcu_per_row + mcu_x) *
              blocks_per_mcu_comp;

          for (uint8_t by = 0; by < v_samp; by++) {
            for (uint8_t bx = 0; bx < h_samp; bx++) {
              size_t block_idx =
                  mcu_block_start + (size_t)by * (size_t)h_samp + (size_t)bx;
              int16_t * block = coef_blocks[comp_idx] + block_idx * 64;

              if (is_dc) {
                GIMG_Result r = jpeg_decode_block_progressive_dc(&bs,
                    &dc_tables[scan->dc_tbl[s]], block,
                    &dc_pred[comp_idx]);
                if (r != GIMG_OK) {
                  goto fail_prog;
                }
              }
              else {
                if (ah == 0) {
                  GIMG_Result r = jpeg_decode_block_progressive_ac_initial(&bs,
                      &ac_tables[scan->ac_tbl[s]], block, ss, se, al);
                  if (r != GIMG_OK) {
                    goto fail_prog;
                  }
                }
                else {
                  GIMG_Result r = jpeg_decode_block_progressive_ac_refine(&bs,
                      &ac_tables[scan->ac_tbl[s]], block, ss, se);
                  if (r != GIMG_OK) {
                    goto fail_prog;
                  }
                }
              }
            }
          }
        }
      }
    }
  }

  // Dequantise, IDCT, write to component buffers (same layout as baseline).
  size_t comp_stride[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  unsigned char * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride[i] = (size_t)comp_w[i];
    if (!gimg_safe_mul_size(comp_stride[i], (size_t)comp_h[i], &comp_size[i])) {
      goto fail_prog;
    }
    comp_buf[i] = (unsigned char *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++) {
        gimg_free(alloc, comp_buf[j]);
      }
      goto fail_prog;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  int16_t block_rz[64];
  int16_t block_q[64];
  for (uint8_t comp_idx = 0; comp_idx < num_comp; comp_idx++) {
    uint8_t qid = sof->quant_tbl_id[comp_idx];
    if (qid >= GIMG_JPEG_MAX_QUANT_TABLES ||
        !state->quant_tbl_present[qid]) {
      goto fail_prog_buf;
    }
    const uint16_t * quant = state->quant_tbl[qid];
    uint32_t blocks_w = comp_w[comp_idx] / 8;
    uint32_t blocks_h = comp_h[comp_idx] / 8;

    for (uint32_t by = 0; by < blocks_h; by++) {
      for (uint32_t bx = 0; bx < blocks_w; bx++) {
        size_t block_idx = (size_t)by * (size_t)blocks_w + (size_t)bx;
        const int16_t * block_zig = coef_blocks[comp_idx] + block_idx * 64;
        jpeg_dezigzag(block_zig, block_rz);
        jpeg_dequantise(block_rz, quant, block_q);
        jpeg_idct_8x8(block_q, block_rz);
        uint32_t dst_x = bx * 8;
        uint32_t dst_y = by * 8;
        for (int dy = 0; dy < 8; dy++) {
          uint32_t y = dst_y + (uint32_t)dy;
          if (y >= comp_h[comp_idx]) {
            break;
          }
          for (int dx = 0; dx < 8; dx++) {
            uint32_t x = dst_x + (uint32_t)dx;
            if (x >= comp_w[comp_idx]) {
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

  // Create output raster (same as baseline).
  GIMG_Result r;
  if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_prog_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    for (uint32_t y = 0; y < height; y++) {
      memcpy(
          pixels + y * stride, comp_buf[0] + y * comp_stride[0], (size_t)width);
    }
  }
  else if (num_comp == 3) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_prog_buf;
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
        if (r_val < 0) r_val = 0;
        if (r_val > 255) r_val = 255;
        if (g_val < 0) g_val = 0;
        if (g_val > 255) g_val = 255;
        if (b_val < 0) b_val = 0;
        if (b_val > 255) b_val = 255;
        pixels[y * stride + x * 4 + 0] = (unsigned char)r_val;
        pixels[y * stride + x * 4 + 1] = (unsigned char)g_val;
        pixels[y * stride + x * 4 + 2] = (unsigned char)b_val;
        pixels[y * stride + x * 4 + 3] = 255;
      }
    }
  }
  else if (num_comp == 4) {
    // CMYK: same as baseline.
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto fail_prog_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    uint32_t cw[4] = {comp_w[0], comp_w[1], comp_w[2], comp_w[3]};
    uint32_t ch[4] = {comp_h[0], comp_h[1], comp_h[2], comp_h[3]};
    for (uint32_t y = 0; y < height; y++) {
      uint32_t cy[4];
      for (int i = 0; i < 4; i++) {
        cy[i] = (ch[i] > 1 && height > 1)
            ? (y * (ch[i] - 1) / (height - 1))
            : 0;
      }
      for (uint32_t x = 0; x < width; x++) {
        uint32_t cx[4];
        for (int i = 0; i < 4; i++) {
          cx[i] = (cw[i] > 1 && width > 1)
              ? (x * (cw[i] - 1) / (width - 1))
              : 0;
        }
        pixels[y * stride + x * 4 + 0] =
            comp_buf[0][cy[0] * comp_stride[0] + cx[0]];
        pixels[y * stride + x * 4 + 1] =
            comp_buf[1][cy[1] * comp_stride[1] + cx[1]];
        pixels[y * stride + x * 4 + 2] =
            comp_buf[2][cy[2] * comp_stride[2] + cx[2]];
        pixels[y * stride + x * 4 + 3] =
            comp_buf[3][cy[3] * comp_stride[3] + cx[3]];
      }
    }
  }
  else {
    r = GIMG_ERR_UNSUPPORTED;
    goto fail_prog_buf;
  }

  if (state->app2_icc && state->app2_icc_len > 14u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    color_info.icc_bytes = state->app2_icc + 14;
    color_info.icc_size = state->app2_icc_len - 14u;
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

fail_prog_buf:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
fail_prog:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
  }
  return GIMG_ERR_CORRUPT;
}

//
// Encode: default quant tables, forward DCT, bitstream write, Huffman encode.
//

/** Default luminance quantisation table (ITU-T T.81 Annex K.1), row-major. */
static const uint8_t gimg_jpeg_default_quant_luma[64] = {
    16,
    11,
    10,
    16,
    24,
    40,
    51,
    61,
    12,
    12,
    14,
    19,
    26,
    58,
    60,
    55,
    14,
    13,
    16,
    24,
    40,
    57,
    69,
    56,
    14,
    17,
    22,
    29,
    51,
    87,
    80,
    62,
    18,
    22,
    37,
    56,
    68,
    109,
    103,
    77,
    24,
    35,
    55,
    64,
    81,
    104,
    113,
    92,
    49,
    64,
    78,
    87,
    103,
    121,
    120,
    101,
    72,
    92,
    95,
    98,
    112,
    100,
    103,
    99,
};

/** Default chrominance quantisation table (ITU-T T.81 Annex K.2), row-major. */
static const uint8_t gimg_jpeg_default_quant_chroma[64] = {
    17,
    18,
    24,
    47,
    99,
    99,
    99,
    99,
    18,
    21,
    26,
    66,
    99,
    99,
    99,
    99,
    24,
    26,
    56,
    99,
    99,
    99,
    99,
    99,
    47,
    66,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
    99,
};

/** Scale default quant by quality (1..100; 100 = finest). Clamp to 1..255. */
static void jpeg_scale_quant(
    unsigned quality, const uint8_t * in, uint16_t * out) {
  if (quality < 1) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }
  // scale 1 at quality 100, ~50 at quality 50, 100 at quality 1
  unsigned scale = 101 - quality;
  for (int i = 0; i < 64; i++) {
    unsigned v = (in[i] * scale + 50) / 100;
    out[i] = (uint16_t)(v < 1 ? 1 : (v > 255 ? 255 : v));
  }
}

/** Forward 1D DCT for 8 points (samples in, coefficients out). */
static void jpeg_fdct_1d(const int16_t * in, int16_t * out) {
  for (int u = 0; u < 8; u++) {
    int32_t sum = 0;
    int32_t c = (u == 0) ? 181 : 256; // 256/sqrt(2) for u=0
    for (int x = 0; x < 8; x++) {
      double angle = (2 * x + 1) * u * 3.14159265358979323846 / 16.0;
      sum += (int32_t)((double)in[x] * (double)c * cos(angle) + 0.5);
    }
    out[u] = (int16_t)(sum / 256);
  }
}

/** Forward 2D 8x8 DCT (level-shifted samples -128..127 in, coeffs out). */
static void jpeg_fdct_8x8(int16_t * block) {
  int16_t row[8];
  int16_t tmp[64];
  for (int y = 0; y < 8; y++) {
    jpeg_fdct_1d(block + y * 8, row);
    for (int x = 0; x < 8; x++) {
      tmp[x * 8 + y] = row[x];
    }
  }
  for (int x = 0; x < 8; x++) {
    jpeg_fdct_1d(tmp + x * 8, row);
    for (int y = 0; y < 8; y++) {
      block[y * 8 + x] = row[y];
    }
  }
}

/** Quantise block in place: block[i] = round(block[i] / quant[i]). */
static void jpeg_quantise(int16_t * block, const uint16_t * quant) {
  for (int i = 0; i < 64; i++) {
    int32_t v = (int32_t)block[i] * 256 / (int32_t)quant[i];
    block[i] = (int16_t)(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
  }
}

/** Reorder block from row-major to zigzag stream order. */
static void jpeg_zigzag_encode(const int16_t * block_rm, int16_t * block_zz) {
  for (int i = 0; i < 64; i++) {
    block_zz[i] = block_rm[gimg_jpeg_zigzag[i]];
  }
}

/** Bitstream writer: append bits (MSB first), flush with 0xFF stuffing. */
typedef struct {
  unsigned char * data;
  size_t alloc_size;
  size_t size;
  int bit_off;   ///< 0..7; next bit goes at data[size] >> (7 - bit_off)
  uint8_t cache; ///< current byte being filled
} gimg_jpeg_bitstream_write_t;

static void jpeg_bitstream_write_init(
    gimg_jpeg_bitstream_write_t * w, const GIMG_Allocator * alloc) {
  w->alloc_size = 4096;
  w->data =
      (unsigned char *)gimg_malloc(gimg_alloc_or_default(alloc), w->alloc_size);
  w->size = 0;
  w->bit_off = 0;
  w->cache = 0;
}

static void jpeg_bitstream_write_byte(
    gimg_jpeg_bitstream_write_t * w, uint8_t b, const GIMG_Allocator * alloc) {
  if (w->size >= w->alloc_size) {
    size_t new_size = w->alloc_size * 2;
    unsigned char * p = (unsigned char *)gimg_realloc(
        gimg_alloc_or_default(alloc), w->data, new_size);
    if (!p) {
      return;
    }
    w->data = p;
    w->alloc_size = new_size;
  }
  w->data[w->size++] = b;
  if (b == 0xFF) {
    // Byte stuffing
    if (w->size >= w->alloc_size) {
      size_t new_size = w->alloc_size * 2;
      unsigned char * p = (unsigned char *)gimg_realloc(
          gimg_alloc_or_default(alloc), w->data, new_size);
      if (!p) {
        return;
      }
      w->data = p;
      w->alloc_size = new_size;
    }
    w->data[w->size++] = 0x00;
  }
}

static void jpeg_bitstream_write_bits(gimg_jpeg_bitstream_write_t * w,
    unsigned code, int num_bits, const GIMG_Allocator * alloc) {
  while (num_bits > 0) {
    int shift = num_bits - 1;
    int bit = (code >> shift) & 1;
    w->cache = (uint8_t)((w->cache << 1) | bit);
    w->bit_off++;
    if (w->bit_off == 8) {
      jpeg_bitstream_write_byte(w, w->cache, alloc);
      w->bit_off = 0;
      w->cache = 0;
    }
    num_bits--;
  }
}

static void jpeg_bitstream_write_flush(
    gimg_jpeg_bitstream_write_t * w, const GIMG_Allocator * alloc) {
  if (w->bit_off > 0) {
    while (w->bit_off < 8) {
      w->cache = (uint8_t)(w->cache << 1);
      w->bit_off++;
    }
    jpeg_bitstream_write_byte(w, w->cache, alloc);
  }
}

/** Encode table: symbol -> (code, length). */
typedef struct {
  uint16_t code[256];
  uint8_t len[256];
} gimg_jpeg_huff_enc_t;

/** Build encode table from DHT-style bits (16 counts) and values. */
static void jpeg_build_huff_enc(const unsigned char * bits,
    const unsigned char * values, int num_values, gimg_jpeg_huff_enc_t * enc) {
  uint16_t code = 0;
  int idx = 0;
  for (int len = 1; len <= 16; len++) {
    uint8_t count = bits[len - 1];
    for (int k = 0; k < count && idx < num_values; k++) {
      uint8_t sym = values[idx++];
      enc->code[sym] = (uint16_t)code;
      enc->len[sym] = (uint8_t)len;
      code++;
    }
    code = (uint16_t)(code << 1);
  }
}

/** Number of bits for signed value (Table K.2 extend). */
static int jpeg_nbits(int val) {
  if (val < 0) {
    val = -val;
  }
  int n = 0;
  while (val > 0) {
    n++;
    val >>= 1;
  }
  return n;
}

/** Encode one 8x8 block (DC + AC). DC predictor updated. */
static GIMG_Result jpeg_encode_block(gimg_jpeg_bitstream_write_t * w,
    const int16_t * block_zz, const gimg_jpeg_huff_enc_t * dc_enc,
    const gimg_jpeg_huff_enc_t * ac_enc, int32_t * dc_pred,
    const GIMG_Allocator * alloc) {
  int32_t dc_val = block_zz[0];
  int32_t diff = dc_val - *dc_pred;
  *dc_pred = dc_val;
  int cat = jpeg_nbits(diff);
  if (cat > 11) {
    cat = 11;
  }
  jpeg_bitstream_write_bits(w, dc_enc->code[cat], dc_enc->len[cat], alloc);
  if (cat > 0) {
    unsigned extra =
        (diff < 0) ? (unsigned)(diff + (int)(1u << cat) - 1) : (unsigned)diff;
    jpeg_bitstream_write_bits(w, extra, cat, alloc);
  }
  int k = 1;
  while (k < 64) {
    int run = 0;
    while (k < 64 && block_zz[k] == 0) {
      run++;
      k++;
    }
    if (k >= 64) {
      break;
    }
    int ac = block_zz[k];
    int size = jpeg_nbits(ac);
    if (size > 10) {
      size = 10;
    }
    uint8_t sym = (uint8_t)((run << 4) | size);
    if (ac_enc->len[sym] > 0) {
      jpeg_bitstream_write_bits(w, ac_enc->code[sym], ac_enc->len[sym], alloc);
      if (size > 0) {
        unsigned extra =
            (ac < 0) ? (unsigned)(ac + (int)(1u << size) - 1) : (unsigned)ac;
        jpeg_bitstream_write_bits(w, extra, size, alloc);
      }
    }
    k++;
  }
  // EOB: (0,0)
  jpeg_bitstream_write_bits(w, ac_enc->code[0], ac_enc->len[0], alloc);
  return GIMG_OK;
}

// Standard Huffman: DC luminance (Table K.3), 12 symbols, lengths
// 2,3,3,3,3,3,4,5,6,7,8,9.
static const unsigned char jpeg_std_bits_dc_lum[16] = {
    0,
    0,
    1,
    5,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
    0,
    0,
};
static const unsigned char jpeg_std_vals_dc_lum[12] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
};

// AC luminance (Table K.4): 162 symbols.
static const unsigned char jpeg_std_bits_ac_lum[16] = {
    0,
    0,
    2,
    1,
    3,
    3,
    2,
    4,
    3,
    5,
    5,
    4,
    4,
    0,
    0,
    1,
};
static const unsigned char jpeg_std_vals_ac_lum[162] = {
    0x01,
    0x02,
    0x03,
    0x00,
    0x04,
    0x11,
    0x05,
    0x12,
    0x21,
    0x31,
    0x41,
    0x06,
    0x13,
    0x51,
    0x61,
    0x07,
    0x22,
    0x71,
    0x14,
    0x32,
    0x81,
    0x91,
    0xa1,
    0x08,
    0x23,
    0x42,
    0xb1,
    0xc1,
    0x15,
    0x52,
    0xd1,
    0xf0,
    0x24,
    0x33,
    0x62,
    0x72,
    0x82,
    0x09,
    0x0a,
    0x16,
    0x17,
    0x18,
    0x19,
    0x1a,
    0x25,
    0x26,
    0x27,
    0x28,
    0x29,
    0x2a,
    0x34,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x3a,
    0x43,
    0x44,
    0x45,
    0x46,
    0x47,
    0x48,
    0x49,
    0x4a,
    0x53,
    0x54,
    0x55,
    0x56,
    0x57,
    0x58,
    0x59,
    0x5a,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67,
    0x68,
    0x69,
    0x6a,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77,
    0x78,
    0x79,
    0x7a,
    0x83,
    0x84,
    0x85,
    0x86,
    0x87,
    0x88,
    0x89,
    0x8a,
    0x92,
    0x93,
    0x94,
    0x95,
    0x96,
    0x97,
    0x98,
    0x99,
    0x9a,
    0xa2,
    0xa3,
    0xa4,
    0xa5,
    0xa6,
    0xa7,
    0xa8,
    0xa9,
    0xaa,
    0xb2,
    0xb3,
    0xb4,
    0xb5,
    0xb6,
    0xb7,
    0xb8,
    0xb9,
    0xba,
    0xc2,
    0xc3,
    0xc4,
    0xc5,
    0xc6,
    0xc7,
    0xc8,
    0xc9,
    0xca,
    0xd2,
    0xd3,
    0xd4,
    0xd5,
    0xd6,
    0xd7,
    0xd8,
    0xd9,
    0xda,
    0xe1,
    0xe2,
    0xe3,
    0xe4,
    0xe5,
    0xe6,
    0xe7,
    0xe8,
    0xe9,
    0xea,
    0xf1,
    0xf2,
    0xf3,
    0xf4,
    0xf5,
    0xf6,
    0xf7,
    0xf8,
    0xf9,
    0xfa,
};

// DC chrominance (Table K.5) - same structure as lum for baseline.
static const unsigned char jpeg_std_bits_dc_chrom[16] = {
    0,
    0,
    3,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    1,
    0,
    0,
    0,
    0,
};
static const unsigned char jpeg_std_vals_dc_chrom[12] = {
    0,
    1,
    2,
    3,
    4,
    5,
    6,
    7,
    8,
    9,
    10,
    11,
};

// AC chrominance (Table K.6).
static const unsigned char jpeg_std_bits_ac_chrom[16] = {
    0,
    0,
    2,
    1,
    2,
    4,
    4,
    3,
    4,
    7,
    5,
    4,
    4,
    0,
    1,
    2,
};
static const unsigned char jpeg_std_vals_ac_chrom[162] = {
    0x00,
    0x01,
    0x02,
    0x03,
    0x11,
    0x04,
    0x05,
    0x21,
    0x31,
    0x06,
    0x12,
    0x41,
    0x51,
    0x07,
    0x61,
    0x71,
    0x13,
    0x22,
    0x32,
    0x81,
    0x08,
    0x14,
    0x42,
    0x91,
    0xa1,
    0xb1,
    0xc1,
    0x09,
    0x23,
    0x33,
    0x52,
    0xf0,
    0x15,
    0x62,
    0x72,
    0xd1,
    0x0a,
    0x16,
    0x24,
    0x34,
    0xe1,
    0x25,
    0xf1,
    0x17,
    0x18,
    0x19,
    0x1a,
    0x26,
    0x27,
    0x28,
    0x29,
    0x2a,
    0x35,
    0x36,
    0x37,
    0x38,
    0x39,
    0x3a,
    0x43,
    0x44,
    0x45,
    0x46,
    0x47,
    0x48,
    0x49,
    0x4a,
    0x53,
    0x54,
    0x55,
    0x56,
    0x57,
    0x58,
    0x59,
    0x5a,
    0x63,
    0x64,
    0x65,
    0x66,
    0x67,
    0x68,
    0x69,
    0x6a,
    0x73,
    0x74,
    0x75,
    0x76,
    0x77,
    0x78,
    0x79,
    0x7a,
    0x82,
    0x83,
    0x84,
    0x85,
    0x86,
    0x87,
    0x88,
    0x89,
    0x8a,
    0x92,
    0x93,
    0x94,
    0x95,
    0x96,
    0x97,
    0x98,
    0x99,
    0x9a,
    0xa2,
    0xa3,
    0xa4,
    0xa5,
    0xa6,
    0xa7,
    0xa8,
    0xa9,
    0xaa,
    0xb2,
    0xb3,
    0xb4,
    0xb5,
    0xb6,
    0xb7,
    0xb8,
    0xb9,
    0xba,
    0xc2,
    0xc3,
    0xc4,
    0xc5,
    0xc6,
    0xc7,
    0xc8,
    0xc9,
    0xca,
    0xd2,
    0xd3,
    0xd4,
    0xd5,
    0xd6,
    0xd7,
    0xd8,
    0xd9,
    0xda,
    0xe2,
    0xe3,
    0xe4,
    0xe5,
    0xe6,
    0xe7,
    0xe8,
    0xe9,
    0xea,
    0xf2,
    0xf3,
    0xf4,
    0xf5,
    0xf6,
    0xf7,
    0xf8,
    0xf9,
    0xfa,
};

GIMG_Result gimg_jpeg_encode_baseline_scan(uint32_t width, uint32_t height,
    int num_components, const unsigned char * comp0,
    const unsigned char * comp1, const unsigned char * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, const GIMG_Allocator * alloc,
    unsigned char ** out_scan_data, size_t * out_scan_size) {
  if (!out_scan_data || !out_scan_size || width == 0 || height == 0) {
    return GIMG_ERR_INTERNAL;
  }
  if (num_components != 1 && num_components != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  const GIMG_Allocator * a = gimg_alloc_or_default(alloc);

  gimg_jpeg_huff_enc_t dc_lum, dc_chrom, ac_lum, ac_chrom;
  memset(&dc_lum, 0, sizeof(dc_lum));
  memset(&dc_chrom, 0, sizeof(dc_chrom));
  memset(&ac_lum, 0, sizeof(ac_lum));
  memset(&ac_chrom, 0, sizeof(ac_chrom));
  jpeg_build_huff_enc(jpeg_std_bits_dc_lum, jpeg_std_vals_dc_lum, 12, &dc_lum);
  jpeg_build_huff_enc(jpeg_std_bits_ac_lum, jpeg_std_vals_ac_lum, 162, &ac_lum);
  jpeg_build_huff_enc(
      jpeg_std_bits_dc_chrom, jpeg_std_vals_dc_chrom, 12, &dc_chrom);
  jpeg_build_huff_enc(
      jpeg_std_bits_ac_chrom, jpeg_std_vals_ac_chrom, 162, &ac_chrom);

  gimg_jpeg_bitstream_write_t w;
  jpeg_bitstream_write_init(&w, a);
  if (!w.data) {
    return GIMG_ERR_OOM;
  }

  uint32_t mcu_per_row = (width + 7) / 8;
  uint32_t mcu_per_col = (height + 7) / 8;
  int32_t dc_pred[3] = {0, 0, 0};
  int16_t block_rm[64];
  int16_t block_zz[64];
  const unsigned char * comps[3] = {comp0, comp1, comp2};
  size_t strides[3] = {stride0, stride1, stride2};
  const uint16_t * quants[3] = {quant_luma, quant_chroma, quant_chroma};
  const gimg_jpeg_huff_enc_t * dc_tbls[3] = {&dc_lum, &dc_chrom, &dc_chrom};
  const gimg_jpeg_huff_enc_t * ac_tbls[3] = {&ac_lum, &ac_chrom, &ac_chrom};

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      for (int c = 0; c < num_components; c++) {
        const unsigned char * row_ptr = comps[c] + mcu_y * 8 * strides[c];
        const uint16_t * q = quants[c];
        for (int by = 0; by < 8; by++) {
          uint32_t y = mcu_y * 8 + (uint32_t)by;
          if (y >= height) {
            for (int dx = 0; dx < 8; dx++) {
              block_rm[by * 8 + dx] = 0;
            }
            continue;
          }
          const unsigned char * p =
              row_ptr + (size_t)by * strides[c] + (size_t)(mcu_x * 8);
          for (int bx = 0; bx < 8; bx++) {
            uint32_t x = mcu_x * 8 + (uint32_t)bx;
            int16_t s = (x < width) ? (int16_t)((int)p[bx] - 128) : 0;
            block_rm[by * 8 + bx] = s;
          }
        }
        jpeg_fdct_8x8(block_rm);
        jpeg_quantise(block_rm, q);
        jpeg_zigzag_encode(block_rm, block_zz);
        jpeg_encode_block(&w, block_zz, dc_tbls[c], ac_tbls[c], &dc_pred[c], a);
      }
    }
  }

  jpeg_bitstream_write_flush(&w, a);
  *out_scan_data = w.data;
  *out_scan_size = w.size;
  return GIMG_OK;
}

void gimg_jpeg_default_quant_scaled(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma) {
  jpeg_scale_quant(quality, gimg_jpeg_default_quant_luma, quant_luma);
  jpeg_scale_quant(quality, gimg_jpeg_default_quant_chroma, quant_chroma);
}

/** Write the four standard DHT segments (DC0, AC0, DC1, AC1) to stream. */
GIMG_Result gimg_jpeg_write_standard_dht(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  size_t n = 0;
  GIMG_Result r;
  unsigned char seg[2];
  // DHT DC0: payload = TcTh(1) + 16 bits + 12 vals = 29; Lh = 31
  unsigned char dht_dc0[29];
  dht_dc0[0] = 0x00; // Tc=0, Th=0
  memcpy(dht_dc0 + 1, jpeg_std_bits_dc_lum, 16);
  memcpy(dht_dc0 + 17, jpeg_std_vals_dc_lum, 12);
  uint16_t len_dc0 = 31; // 2 + 29
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_dc0 >> 8);
  seg[1] = (unsigned char)(len_dc0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_dc0, sizeof(dht_dc0), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  // DHT AC0: payload = 1 + 16 + 162 = 179; Lh = 181
  unsigned char dht_ac0[179];
  dht_ac0[0] = 0x10; // Tc=1, Th=0
  memcpy(dht_ac0 + 1, jpeg_std_bits_ac_lum, 16);
  memcpy(dht_ac0 + 17, jpeg_std_vals_ac_lum, 162);
  uint16_t len_ac0 = 181;
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_ac0 >> 8);
  seg[1] = (unsigned char)(len_ac0 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_ac0, sizeof(dht_ac0), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  // DHT DC1 chrominance
  unsigned char dht_dc1[29];
  dht_dc1[0] = 0x01; // Tc=0, Th=1
  memcpy(dht_dc1 + 1, jpeg_std_bits_dc_chrom, 16);
  memcpy(dht_dc1 + 17, jpeg_std_vals_dc_chrom, 12);
  uint16_t len_dc1 = 31;
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_dc1 >> 8);
  seg[1] = (unsigned char)(len_dc1 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_dc1, sizeof(dht_dc1), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  // DHT AC1 chrominance
  unsigned char dht_ac1[179];
  dht_ac1[0] = 0x11; // Tc=1, Th=1
  memcpy(dht_ac1 + 1, jpeg_std_bits_ac_chrom, 16);
  memcpy(dht_ac1 + 17, jpeg_std_vals_ac_chrom, 162);
  uint16_t len_ac1 = 181;
  r = gimg_stream_write(stream, (const unsigned char *)"\xFF\xC4", 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  seg[0] = (unsigned char)(len_ac1 >> 8);
  seg[1] = (unsigned char)(len_ac1 & 0xFF);
  r = gimg_stream_write(stream, seg, 2, &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  r = gimg_stream_write(stream, dht_ac1, sizeof(dht_ac1), &n);
  if (r != GIMG_OK) {
    return r;
  }
  total += n;
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}
