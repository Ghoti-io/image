/**
 * @file
 *
 * JPEG save: baseline encode (DCT, quantisation, zigzag, Huffman); emit SOI,
 * DQT, DHT, SOF0, SOS, EOI. Supports grayscale and YCbCr 4:4:4. Synthetic
 * document support: when doc was not loaded by a codec, pixel data from
 * gimg_item_raster(item) only.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../meta/exif_internal.h"
#include "../../raster/raster_internal.h"
#include "../codec_internal.h"
#include "jpeg_internal.h"

/** Default quality when not specified (1..100). */
#define GIMG_JPEG_DEFAULT_QUALITY 85u

/** T.81 Annex K.1 sample quantization tables (natural/row-major order). */
static const unsigned int gimg_jpeg_std_luminance_quant[64] = {16, 11, 10, 16,
    24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55, 14, 13, 16, 24, 40, 57, 69,
    56, 14, 17, 22, 29, 51, 87, 80, 62, 18, 22, 37, 56, 68, 109, 103, 77, 24,
    35, 55, 64, 81, 104, 113, 92, 49, 64, 78, 87, 103, 121, 120, 101, 72, 92,
    95, 98, 112, 100, 103, 99};
static const unsigned int gimg_jpeg_std_chrominance_quant[64] = {17, 18, 24, 47,
    99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99, 24, 26, 56, 99, 99, 99, 99,
    99, 47, 66, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99};

/** Quality 0..100 -> scale percentage (IJG curve). Q 50 -> 100; Q<50 -> 5000/Q;
 * Q>=50 -> 200-2*Q. */
static unsigned int gimg_jpeg_quality_to_scale(unsigned quality) {
  if (quality <= 0) {
    quality = 1;
  }
  if (quality > 100) {
    quality = 100;
  }
  if (quality < 50) {
    return (unsigned int)(5000 / quality);
  }
  return (unsigned int)(200 - quality * 2);
}

void gimg_jpeg_default_quant_scaled(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma) {
  unsigned int scale = gimg_jpeg_quality_to_scale(quality);
  for (int i = 0; i < 64; i++) {
    unsigned long t =
        (unsigned long)gimg_jpeg_std_luminance_quant[i] * scale + 50UL;
    t /= 100UL;
    if (t <= 0UL)
      t = 1UL;
    if (t > 255UL)
      t = 255UL;
    quant_luma[i] = (uint16_t)t;
  }
  for (int i = 0; i < 64; i++) {
    unsigned long t =
        (unsigned long)gimg_jpeg_std_chrominance_quant[i] * scale + 50UL;
    t /= 100UL;
    if (t <= 0UL)
      t = 1UL;
    if (t > 255UL)
      t = 255UL;
    quant_chroma[i] = (uint16_t)t;
  }
}

void gimg_jpeg_default_quant_scaled_16bit(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma) {
  unsigned int scale = gimg_jpeg_quality_to_scale(quality);
  for (int i = 0; i < 64; i++) {
    unsigned long t =
        (unsigned long)gimg_jpeg_std_luminance_quant[i] * scale + 50UL;
    t /= 100UL;
    if (t <= 0UL)
      t = 1UL;
    if (t > 32767UL)
      t = 32767UL;
    quant_luma[i] = (uint16_t)t;
  }
  for (int i = 0; i < 64; i++) {
    unsigned long t =
        (unsigned long)gimg_jpeg_std_chrominance_quant[i] * scale + 50UL;
    t /= 100UL;
    if (t <= 0UL)
      t = 1UL;
    if (t > 32767UL)
      t = 32767UL;
    quant_chroma[i] = (uint16_t)t;
  }
}

/** 12-bit quant scaling (T.81 Annex B/K): Pq=1, same range as 16-bit. */
void gimg_jpeg_default_quant_scaled_12bit(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma) {
  gimg_jpeg_default_quant_scaled_16bit(
      quality, quant_luma, quant_chroma);
}

/** Write a 2-byte segment length (big-endian). */
static GIMG_Result jpeg_write_u16(
    GIMG_Stream * stream, uint16_t val, size_t * out_n) {
  unsigned char buf[2] = {
      (unsigned char)(val >> 8), (unsigned char)(val & 0xFF)};
  size_t n = 0;
  GIMG_Result r = gimg_stream_write(stream, buf, 2, &n);
  if (out_n) {
    *out_n += n;
  }
  return r;
}

/** Write marker (0xFF + byte). */
static GIMG_Result jpeg_write_marker(
    GIMG_Stream * stream, uint8_t marker, size_t * out_n) {
  unsigned char buf[2] = {0xFF, marker};
  size_t n = 0;
  GIMG_Result r = gimg_stream_write(stream, buf, 2, &n);
  if (out_n) {
    *out_n += n;
  }
  return r;
}

/** Write entropy-coded segment with 0xFF stuffing (T.81 B.2.2: 0xFF in scan
 * data must be followed by a stuffed 0x00 so the next 0xFF is not misread).
 * Do not insert stuffing when the encoder already stuffed (next byte is 0x00)
 * or when the next byte is RST (0xD0..0xD7): 0xFF 0xDn is the RST marker. */
static GIMG_Result jpeg_write_scan_data_with_stuffing(GIMG_Stream * stream,
    const unsigned char * scan_data, size_t scan_size, size_t * out_n) {
  size_t total = 0;
  for (size_t i = 0; i < scan_size; i++) {
    unsigned char b = scan_data[i];
    size_t n = 0;
    GIMG_Result r = gimg_stream_write(stream, &b, 1, &n);
    if (r != GIMG_OK) {
      return r;
    }
    total += n;
    if (b == 0xFF && (i + 1) < scan_size) {
      unsigned char next = scan_data[i + 1];
      if (next == 0x00 || (next >= 0xD0 && next <= 0xD7)) {
        /* Already stuffed by encoder, or RST marker: do not add stuffing. */
        continue;
      }
    }
    if (b == 0xFF) {
      const unsigned char stuff = 0x00;
      r = gimg_stream_write(stream, &stuff, 1, &n);
      if (r != GIMG_OK) {
        return r;
      }
      total += n;
    }
  }
  if (out_n) {
    *out_n += total;
  }
  return GIMG_OK;
}

/** Write APP segment: marker + length (2 + payload_len) + payload. */
static GIMG_Result jpeg_write_app_segment(GIMG_Stream * stream, uint8_t marker,
    const void * payload, size_t payload_len, size_t * out_n) {
  if (payload_len > 65533u) {
    return GIMG_ERR_LIMIT;
  }
  GIMG_Result r = jpeg_write_marker(stream, marker, out_n);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_u16(stream, (uint16_t)(2 + payload_len), out_n);
  if (r != GIMG_OK) {
    return r;
  }
  if (payload_len > 0 && payload) {
    size_t n = 0;
    r = gimg_stream_write(stream, payload, payload_len, &n);
    if (out_n) {
      *out_n += n;
    }
    if (r != GIMG_OK) {
      return r;
    }
  }
  return GIMG_OK;
}

/** Build minimal APP0 JFIF (14 bytes), per JFIF 1.01. Lh=16 so segment is
 * 2+16 bytes. If x_dpi and y_dpi are both 0, use units=0 and density 1,1;
 * else units=1 (dots per inch). No thumbnail (Xthumbnail=0, Ythumbnail=0). */
static void jpeg_build_minimal_app0(
    unsigned char * buf, uint32_t x_dpi, uint32_t y_dpi) {
  memcpy(buf, "JFIF\0", 5);
  buf[5] = 0x01;
  buf[6] = 0x01;
  if (x_dpi == 0 && y_dpi == 0) {
    buf[7] = 0; // no units
    buf[8] = 0;
    buf[9] = 1;
    buf[10] = 0;
    buf[11] = 1;
  }
  else {
    if (x_dpi == 0) {
      x_dpi = 1;
    }
    if (y_dpi == 0) {
      y_dpi = 1;
    }
    buf[7] = 1; // dots per inch
    buf[8] = (unsigned char)(x_dpi >> 8);
    buf[9] = (unsigned char)(x_dpi & 0xFFu);
    buf[10] = (unsigned char)(y_dpi >> 8);
    buf[11] = (unsigned char)(y_dpi & 0xFFu);
  }
  buf[12] = 0; // Xthumbnail
  buf[13] = 0; // Ythumbnail
}

/** RGB to YCbCr per ITU-R BT.601. R,G,B 0..255 -> Y,Cb,Cr 0..255.
 * FIX(x) = (x * 65536 + 0.5) for rounding. */
static void jpeg_rgb_to_ycbcr(
    uint8_t r, uint8_t g, uint8_t b, uint8_t * y, uint8_t * cb, uint8_t * cr) {
  /* Y  = 0.299*R + 0.587*G + 0.114*B; FIX(0.299)=19595, FIX(0.587)=38470,
   * FIX(0.114)=7471; +ONE_HALF for B */
  int32_t yv =
      (19595 * (int32_t)r + 38470 * (int32_t)g + 7471 * (int32_t)b + 32768) >>
      16;
  /* Cb = -0.16874*R - 0.33126*G + 0.5*B + 128 (CBCR_OFFSET + ONE_HALF - 1). */
  int32_t cbv = (-11059 * (int32_t)r - 21709 * (int32_t)g + 32768 * (int32_t)b +
                    8421375) >>
      16;
  /* Cr = 0.5*R - 0.41869*G - 0.08131*B + 128 */
  int32_t crv =
      (32768 * (int32_t)r - 27439 * (int32_t)g - 5331 * (int32_t)b + 8421375) >>
      16;
  *y = (uint8_t)(yv < 0 ? 0 : (yv > 255 ? 255 : (uint8_t)yv));
  *cb = (uint8_t)(cbv < 0 ? 0 : (cbv > 255 ? 255 : (uint8_t)cbv));
  *cr = (uint8_t)(crv < 0 ? 0 : (crv > 255 ? 255 : (uint8_t)crv));
}

/** Chroma subsampling: 0 = 4:2:0, 1 = 4:2:2, 2 = 4:4:4. */
#define CHROMA_420 0
#define CHROMA_422 1
#define CHROMA_444 2

/** RGB 16-bit to YCbCr 16-bit (BT.601). R,G,B 0..65535 -> Y,Cb,Cr 0..65535. */
static void jpeg_rgb16_to_ycbcr16(uint16_t r, uint16_t g, uint16_t b,
    uint16_t * y, uint16_t * cb, uint16_t * cr) {
  uint32_t ri = (uint32_t)(r >> 8);
  uint32_t gi = (uint32_t)(g >> 8);
  uint32_t bi = (uint32_t)(b >> 8);
  int yv = (int)((77 * ri + 150 * gi + 29 * bi + 128) / 256);
  int cbv =
      (int)((-43 * (int)ri - 84 * (int)gi + 127 * (int)bi + 128 * 256) / 256) +
      128;
  int crv =
      (int)((127 * (int)ri - 106 * (int)gi - 21 * (int)bi + 128 * 256) / 256) +
      128;
  *y = (uint16_t)(yv < 0 ? 0 : (yv > 255 ? 65535u : (uint32_t)yv << 8));
  *cb = (uint16_t)(cbv < 0 ? 0 : (cbv > 255 ? 65535u : (uint32_t)cbv << 8));
  *cr = (uint16_t)(crv < 0 ? 0 : (crv > 255 ? 65535u : (uint32_t)crv << 8));
}

/** RGB 12-bit to YCbCr 12-bit (BT.601). R,G,B 0..4095 -> Y,Cb,Cr 0..4095. */
static void jpeg_rgb12_to_ycbcr12(uint16_t r, uint16_t g, uint16_t b,
    uint16_t * y, uint16_t * cb, uint16_t * cr) {
  if (r > 4095u) r = 4095u;
  if (g > 4095u) g = 4095u;
  if (b > 4095u) b = 4095u;
  int32_t ri = (int32_t)r;
  int32_t gi = (int32_t)g;
  int32_t bi = (int32_t)b;
  int32_t yv = (77 * ri + 150 * gi + 29 * bi + 2048) >> 12;
  int32_t cbv = (-44 * ri - 87 * gi + 131 * bi + 2048 * 256) >> 8;
  int32_t crv = (131 * ri - 110 * gi - 21 * bi + 2048 * 256) >> 8;
  *y = (uint16_t)(yv < 0 ? 0 : (yv > 4095 ? 4095u : (uint32_t)yv));
  *cb = (uint16_t)(cbv < 0 ? 0 : (cbv > 4095 ? 4095u : (uint32_t)cbv));
  *cr = (uint16_t)(crv < 0 ? 0 : (crv > 4095 ? 4095u : (uint32_t)crv));
}

/** 16-bit path: fill uint16_t comps, quant 16-bit, encode with extended
 * precision. */
static GIMG_Result jpeg_raster_to_scan_data_16bit(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, unsigned quality, unsigned chroma_subsampling,
    bool progressive, unsigned char ** out_scan_data, size_t * out_scan_size,
    int16_t ** out_coef_buffer, size_t * out_total_blocks,
    uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES], uint32_t * out_width,
    uint32_t * out_height, int * out_num_components, uint8_t out_h_samp[3],
    uint8_t out_v_samp[3]) {
  (void)progressive;
  (void)out_scan_data;
  (void)out_scan_size;
  // 16-bit always uses coef buffer (SOF2 + two scans).
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  int num_components = (fmt->channel_model == GIMG_CHANNEL_GRAY) ? 1 : 3;
  size_t comp_size = 0;
  if (!gimg_safe_mul_size((size_t)width, (size_t)height, &comp_size)) {
    return GIMG_ERR_LIMIT;
  }
  uint16_t * comp_y =
      (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
  if (!comp_y) {
    return GIMG_ERR_OOM;
  }
  uint16_t * comp_cb = NULL;
  uint16_t * comp_cr = NULL;
  if (num_components == 3) {
    comp_cb = (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
    comp_cr = (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
    if (!comp_cb || !comp_cr) {
      gimg_free(alloc, comp_y);
      if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      return GIMG_ERR_OOM;
    }
  }
  size_t stride_bytes = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  if (!pixels) {
    gimg_free(alloc, comp_y);
    gimg_free(alloc, comp_cb);
    gimg_free(alloc, comp_cr);
    return GIMG_ERR_UNSUPPORTED;
  }
  if (num_components == 1) {
    for (uint32_t y = 0; y < height; y++) {
      const uint16_t * row = (const uint16_t *)(pixels + y * stride_bytes);
      for (uint32_t x = 0; x < width; x++) {
        comp_y[y * (size_t)width + x] = row[x];
      }
    }
  }
  else {
    int ch_count = (int)fmt->channel_count;
    if (ch_count < 3) {
      ch_count = 3;
    }
    for (uint32_t y = 0; y < height; y++) {
      const uint16_t * row = (const uint16_t *)(pixels + y * stride_bytes);
      for (uint32_t x = 0; x < width; x++) {
        uint16_t r = row[x * (size_t)ch_count + 0];
        uint16_t g = row[x * (size_t)ch_count + 1];
        uint16_t b = row[x * (size_t)ch_count + 2];
        uint16_t yv, cb, cr;
        jpeg_rgb16_to_ycbcr16(r, g, b, &yv, &cb, &cr);
        comp_y[y * (size_t)width + x] = yv;
        comp_cb[y * (size_t)width + x] = cb;
        comp_cr[y * (size_t)width + x] = cr;
      }
    }
  }
  uint8_t h_samp[3] = {1, 1, 1};
  uint8_t v_samp[3] = {1, 1, 1};
  size_t stride0 = (size_t)width;
  size_t stride1 = (size_t)width;
  size_t stride2 = (size_t)width;
  uint16_t * use_cb = comp_cb;
  uint16_t * use_cr = comp_cr;
  if (num_components == 3 && chroma_subsampling != CHROMA_444) {
    if (chroma_subsampling == CHROMA_420) {
      /* T.81 Annex A: expand chroma to fill integral DCT blocks (output_cols =
       * width_in_blocks*8). */
      uint32_t mcu_per_row = (width + 15u) / 16u;
      uint32_t cw = 8u * mcu_per_row;
      uint32_t ch = (height + 1u) / 2u;
      if (ch == 0u) {
        ch = 1u;
      }
      h_samp[0] = 2;
      h_samp[1] = 1;
      h_samp[2] = 1;
      v_samp[0] = 2;
      v_samp[1] = 1;
      v_samp[2] = 1;
      size_t chroma_size = 0;
      if (!gimg_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      chroma_size *= sizeof(uint16_t);
      use_cb = (uint16_t *)gimg_malloc(alloc, chroma_size);
      use_cr = (uint16_t *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        if (use_cb) {
          gimg_free(alloc, use_cb);
        }
        return GIMG_ERR_OOM;
      }
      /* T.81 Annex A: chroma has (width+1)/2 samples per line for 2h; fill to
       * width_in_blocks*8 by replicating the last sample (Annex A data unit
       * alignment; downsampler output width = compptr->width_in_blocks*8). */
      uint32_t real_cw = (width + 1u) / 2u;
      if (real_cw == 0u) {
        real_cw = 1u;
      }
      for (uint32_t cb_y = 0; cb_y < ch; cb_y++) {
        uint32_t y_lo = cb_y * 2u;
        uint32_t y1 = y_lo + 1u < height ? y_lo + 1u : y_lo;
        if (y_lo >= height) {
          y_lo = height - 1u;
        }
        if (y1 >= height) {
          y1 = height - 1u;
        }
        for (uint32_t cb_x = 0; cb_x < real_cw; cb_x++) {
          uint32_t x_lo = cb_x * 2u;
          uint32_t x1 = x_lo + 1u < width ? x_lo + 1u : x_lo;
          if (x_lo >= width) {
            x_lo = width - 1u;
          }
          if (x1 >= width) {
            x1 = width - 1u;
          }
          uint32_t sum_cb = (uint32_t)comp_cb[y_lo * (size_t)width + x_lo] +
              (uint32_t)comp_cb[y_lo * (size_t)width + x1] +
              (uint32_t)comp_cb[y1 * (size_t)width + x_lo] +
              (uint32_t)comp_cb[y1 * (size_t)width + x1];
          uint32_t sum_cr = (uint32_t)comp_cr[y_lo * (size_t)width + x_lo] +
              (uint32_t)comp_cr[y_lo * (size_t)width + x1] +
              (uint32_t)comp_cr[y1 * (size_t)width + x_lo] +
              (uint32_t)comp_cr[y1 * (size_t)width + x1];
          /* Ordered-dither rounding for 2×2 box: bias 1,2,1,2 per column (T.81
           * does not specify filter). */
          unsigned bias_16 = 1u + (cb_x % 2u);
          use_cb[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cb + bias_16) / 4);
          use_cr[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cr + bias_16) / 4);
        }
        /* Replicate rightmost chroma column to fill to cw (T.81 Annex A). */
        for (uint32_t cb_x = real_cw; cb_x < cw; cb_x++) {
          use_cb[cb_y * (size_t)cw + cb_x] =
              use_cb[cb_y * (size_t)cw + (real_cw - 1u)];
          use_cr[cb_y * (size_t)cw + cb_x] =
              use_cr[cb_y * (size_t)cw + (real_cw - 1u)];
        }
      }
      stride1 = (size_t)cw;
      stride2 = (size_t)cw;
      gimg_free(alloc, comp_cb);
      gimg_free(alloc, comp_cr);
      comp_cb = NULL;
      comp_cr = NULL;
    }
    else {
      uint32_t mcu_per_row = (width + 15u) / 16u;
      uint32_t mcu_per_col = (height + 7u) / 8u;
      uint32_t cw = 8u * mcu_per_row;
      uint32_t ch = 8u * mcu_per_col;
      h_samp[0] = 2;
      h_samp[1] = 1;
      h_samp[2] = 1;
      v_samp[0] = 1;
      v_samp[1] = 1;
      v_samp[2] = 1;
      size_t chroma_size = 0;
      if (!gimg_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      chroma_size *= sizeof(uint16_t);
      use_cb = (uint16_t *)gimg_malloc(alloc, chroma_size);
      use_cr = (uint16_t *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        if (use_cb) {
          gimg_free(alloc, use_cb);
        }
        return GIMG_ERR_OOM;
      }
      for (uint32_t cb_y = 0; cb_y < ch; cb_y++) {
        uint32_t y_src = (cb_y * height) / ch;
        if (y_src >= height) {
          y_src = height - 1u;
        }
        size_t row_off = (size_t)y_src * (size_t)width;
        for (uint32_t cb_x = 0; cb_x < cw; cb_x++) {
          uint32_t x_lo = cb_x * 2u;
          uint32_t x1 = x_lo + 1u < width ? x_lo + 1u : x_lo;
          if (x_lo >= width) {
            x_lo = width - 1u;
          }
          if (x1 >= width) {
            x1 = width - 1u;
          }
          uint32_t sum_cb = (uint32_t)comp_cb[row_off + x_lo] +
              (uint32_t)comp_cb[row_off + x1];
          uint32_t sum_cr = (uint32_t)comp_cr[row_off + x_lo] +
              (uint32_t)comp_cr[row_off + x1];
          use_cb[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cb + 1) / 2);
          use_cr[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cr + 1) / 2);
        }
      }
      stride1 = (size_t)cw;
      stride2 = (size_t)cw;
      gimg_free(alloc, comp_cb);
      gimg_free(alloc, comp_cr);
      comp_cb = NULL;
      comp_cr = NULL;
    }
  }
  if (out_h_samp && out_v_samp && num_components >= 3) {
    out_h_samp[0] = h_samp[0];
    out_h_samp[1] = h_samp[1];
    out_h_samp[2] = h_samp[2];
    out_v_samp[0] = v_samp[0];
    out_v_samp[1] = v_samp[1];
    out_v_samp[2] = v_samp[2];
  }
  if (quality > 100) {
    quality = 100;
  }
  gimg_jpeg_default_quant_scaled_16bit(quality, quant_luma, quant_chroma);
  const uint8_t * h_ptr =
      (num_components == 3 && (h_samp[0] != 1 || h_samp[1] != 1)) ? h_samp
                                                                  : NULL;
  const uint8_t * v_ptr =
      (num_components == 3 && (v_samp[0] != 1 || v_samp[1] != 1)) ? v_samp
                                                                  : NULL;
  GIMG_Result r;
  int precision = 16;
  // 16-bit must use SOF2 (progressive) with two scans (DC then AC) so the
  // decoder can decode; always fill coef buffer and let the writer emit
  // SOF2 + DC scan + AC scan.
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max) {
      h_max = h_samp[1];
    }
    if (h_samp[2] > h_max) {
      h_max = h_samp[2];
    }
    if (v_samp[1] > v_max) {
      v_max = v_samp[1];
    }
    if (v_samp[2] > v_max) {
      v_max = v_samp[2];
    }
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  size_t total_blocks = 0;
  if (!gimg_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gimg_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
    gimg_free(alloc, comp_y);
    if (use_cb != comp_cb) {
      gimg_free(alloc, use_cb);
    }
    if (use_cr != comp_cr) {
      gimg_free(alloc, use_cr);
    }
    return GIMG_ERR_LIMIT;
  }
  int16_t * coef_buf =
      (int16_t *)gimg_malloc(alloc, total_blocks * 64 * sizeof(int16_t));
  if (!coef_buf) {
    gimg_free(alloc, comp_y);
    if (use_cb != comp_cb) {
      gimg_free(alloc, use_cb);
    }
    if (use_cr != comp_cr) {
      gimg_free(alloc, use_cr);
    }
    return GIMG_ERR_OOM;
  }
  size_t out_blocks = 0;
  r = gimg_jpeg_progressive_fill_coef_buffer_16bit(width, height,
      num_components, comp_y, use_cb, use_cr, stride0, stride1, stride2, h_ptr,
      v_ptr, quant_luma, quant_chroma, precision, coef_buf, &out_blocks);
  gimg_free(alloc, comp_y);
  if (use_cb != comp_cb) {
    gimg_free(alloc, use_cb);
  }
  if (use_cr != comp_cr) {
    gimg_free(alloc, use_cr);
  }
  if (num_components == 3) {
    gimg_free(alloc, comp_cb);
    gimg_free(alloc, comp_cr);
  }
  if (r != GIMG_OK) {
    gimg_free(alloc, coef_buf);
    return r;
  }
  *out_coef_buffer = coef_buf;
  *out_total_blocks = out_blocks;
  *out_width = width;
  *out_height = height;
  *out_num_components = num_components;
  return GIMG_OK;
}

/** 12-bit path: GRAY12/RGBA12 (0..4095). Progressive: fill coef and return;
 * baseline: fill coef then encode one scan (Ss=0,Se=63) via extended tables. */
static GIMG_Result jpeg_raster_to_scan_data_12bit(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, unsigned quality, unsigned chroma_subsampling,
    bool progressive, uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size, int16_t ** out_coef_buffer, size_t * out_total_blocks,
    uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES], uint32_t * out_width,
    uint32_t * out_height, int * out_num_components, uint8_t out_h_samp[3],
    uint8_t out_v_samp[3]) {
  /* Quality 100 triggers a known round-trip decode failure (ac_run_overflow);
   * reject to avoid producing JPEG that our decoder cannot read. */
  if (quality >= 100) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  int num_components = (fmt->channel_model == GIMG_CHANNEL_GRAY) ? 1 : 3;
  size_t comp_size = 0;
  if (!gimg_safe_mul_size((size_t)width, (size_t)height, &comp_size)) {
    return GIMG_ERR_LIMIT;
  }
  uint16_t * comp_y =
      (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
  if (!comp_y) {
    return GIMG_ERR_OOM;
  }
  uint16_t * comp_cb = NULL;
  uint16_t * comp_cr = NULL;
  if (num_components == 3) {
    comp_cb = (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
    comp_cr = (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
    if (!comp_cb || !comp_cr) {
      gimg_free(alloc, comp_y);
      if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      return GIMG_ERR_OOM;
    }
  }
  size_t stride_bytes = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  if (!pixels) {
    gimg_free(alloc, comp_y);
    gimg_free(alloc, comp_cb);
    gimg_free(alloc, comp_cr);
    return GIMG_ERR_UNSUPPORTED;
  }
  if (num_components == 1) {
    for (uint32_t y = 0; y < height; y++) {
      const uint16_t * row = (const uint16_t *)(pixels + y * stride_bytes);
      for (uint32_t x = 0; x < width; x++) {
        uint16_t s = row[x];
        comp_y[y * (size_t)width + x] = s > 4095u ? 4095u : s;
      }
    }
  }
  else {
    int ch_count = (int)fmt->channel_count;
    if (ch_count < 3) {
      ch_count = 3;
    }
    for (uint32_t y = 0; y < height; y++) {
      const uint16_t * row = (const uint16_t *)(pixels + y * stride_bytes);
      for (uint32_t x = 0; x < width; x++) {
        uint16_t r = row[x * (size_t)ch_count + 0];
        uint16_t g = row[x * (size_t)ch_count + 1];
        uint16_t b = row[x * (size_t)ch_count + 2];
        if (r > 4095u) r = 4095u;
        if (g > 4095u) g = 4095u;
        if (b > 4095u) b = 4095u;
        uint16_t yv, cb, cr;
        jpeg_rgb12_to_ycbcr12(r, g, b, &yv, &cb, &cr);
        comp_y[y * (size_t)width + x] = yv;
        comp_cb[y * (size_t)width + x] = cb;
        comp_cr[y * (size_t)width + x] = cr;
      }
    }
  }
  uint8_t h_samp[3] = {1, 1, 1};
  uint8_t v_samp[3] = {1, 1, 1};
  size_t stride0 = (size_t)width;
  size_t stride1 = (size_t)width;
  size_t stride2 = (size_t)width;
  uint16_t * use_cb = comp_cb;
  uint16_t * use_cr = comp_cr;
  if (num_components == 3 && chroma_subsampling != CHROMA_444) {
    if (chroma_subsampling == CHROMA_420) {
      uint32_t mcu_per_row = (width + 15u) / 16u;
      uint32_t cw = 8u * mcu_per_row;
      uint32_t ch = (height + 1u) / 2u;
      if (ch == 0u) {
        ch = 1u;
      }
      h_samp[0] = 2;
      h_samp[1] = 1;
      h_samp[2] = 1;
      v_samp[0] = 2;
      v_samp[1] = 1;
      v_samp[2] = 1;
      size_t chroma_size = 0;
      if (!gimg_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      chroma_size *= sizeof(uint16_t);
      use_cb = (uint16_t *)gimg_malloc(alloc, chroma_size);
      use_cr = (uint16_t *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        if (use_cb) {
          gimg_free(alloc, use_cb);
        }
        return GIMG_ERR_OOM;
      }
      uint32_t real_cw = (width + 1u) / 2u;
      if (real_cw == 0u) {
        real_cw = 1u;
      }
      for (uint32_t cb_y = 0; cb_y < ch; cb_y++) {
        uint32_t y_lo = cb_y * 2u;
        uint32_t y1 = y_lo + 1u < height ? y_lo + 1u : y_lo;
        if (y_lo >= height) {
          y_lo = height - 1u;
        }
        if (y1 >= height) {
          y1 = height - 1u;
        }
        for (uint32_t cb_x = 0; cb_x < real_cw; cb_x++) {
          uint32_t x_lo = cb_x * 2u;
          uint32_t x1 = x_lo + 1u < width ? x_lo + 1u : x_lo;
          if (x_lo >= width) {
            x_lo = width - 1u;
          }
          if (x1 >= width) {
            x1 = width - 1u;
          }
          uint32_t sum_cb = (uint32_t)comp_cb[y_lo * (size_t)width + x_lo] +
              (uint32_t)comp_cb[y_lo * (size_t)width + x1] +
              (uint32_t)comp_cb[y1 * (size_t)width + x_lo] +
              (uint32_t)comp_cb[y1 * (size_t)width + x1];
          uint32_t sum_cr = (uint32_t)comp_cr[y_lo * (size_t)width + x_lo] +
              (uint32_t)comp_cr[y_lo * (size_t)width + x1] +
              (uint32_t)comp_cr[y1 * (size_t)width + x_lo] +
              (uint32_t)comp_cr[y1 * (size_t)width + x1];
          unsigned bias_16 = 1u + (cb_x % 2u);
          use_cb[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cb + bias_16) / 4);
          use_cr[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cr + bias_16) / 4);
        }
        for (uint32_t cb_x = real_cw; cb_x < cw; cb_x++) {
          use_cb[cb_y * (size_t)cw + cb_x] =
              use_cb[cb_y * (size_t)cw + (real_cw - 1u)];
          use_cr[cb_y * (size_t)cw + cb_x] =
              use_cr[cb_y * (size_t)cw + (real_cw - 1u)];
        }
      }
      stride1 = (size_t)cw;
      stride2 = (size_t)cw;
      gimg_free(alloc, comp_cb);
      gimg_free(alloc, comp_cr);
      comp_cb = NULL;
      comp_cr = NULL;
    }
    else {
      uint32_t mcu_per_row = (width + 15u) / 16u;
      uint32_t mcu_per_col = (height + 7u) / 8u;
      uint32_t cw = 8u * mcu_per_row;
      uint32_t ch = 8u * mcu_per_col;
      h_samp[0] = 2;
      h_samp[1] = 1;
      h_samp[2] = 1;
      v_samp[0] = 1;
      v_samp[1] = 1;
      v_samp[2] = 1;
      size_t chroma_size = 0;
      if (!gimg_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      chroma_size *= sizeof(uint16_t);
      use_cb = (uint16_t *)gimg_malloc(alloc, chroma_size);
      use_cr = (uint16_t *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        if (use_cb) {
          gimg_free(alloc, use_cb);
        }
        return GIMG_ERR_OOM;
      }
      for (uint32_t cb_y = 0; cb_y < ch; cb_y++) {
        uint32_t y_src = (cb_y * height) / ch;
        if (y_src >= height) {
          y_src = height - 1u;
        }
        size_t row_off = (size_t)y_src * (size_t)width;
        for (uint32_t cb_x = 0; cb_x < cw; cb_x++) {
          uint32_t x_lo = cb_x * 2u;
          uint32_t x1 = x_lo + 1u < width ? x_lo + 1u : x_lo;
          if (x_lo >= width) {
            x_lo = width - 1u;
          }
          if (x1 >= width) {
            x1 = width - 1u;
          }
          uint32_t sum_cb = (uint32_t)comp_cb[row_off + x_lo] +
              (uint32_t)comp_cb[row_off + x1];
          uint32_t sum_cr = (uint32_t)comp_cr[row_off + x_lo] +
              (uint32_t)comp_cr[row_off + x1];
          use_cb[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cb + 1) / 2);
          use_cr[cb_y * (size_t)cw + cb_x] = (uint16_t)((sum_cr + 1) / 2);
        }
      }
      stride1 = (size_t)cw;
      stride2 = (size_t)cw;
      gimg_free(alloc, comp_cb);
      gimg_free(alloc, comp_cr);
      comp_cb = NULL;
      comp_cr = NULL;
    }
  }
  if (out_h_samp && out_v_samp && num_components >= 3) {
    out_h_samp[0] = h_samp[0];
    out_h_samp[1] = h_samp[1];
    out_h_samp[2] = h_samp[2];
    out_v_samp[0] = v_samp[0];
    out_v_samp[1] = v_samp[1];
    out_v_samp[2] = v_samp[2];
  }
  if (quality > 100) {
    quality = 100;
  }
  gimg_jpeg_default_quant_scaled_12bit(quality, quant_luma, quant_chroma);
  const uint8_t * h_ptr =
      (num_components == 3 && (h_samp[0] != 1 || h_samp[1] != 1)) ? h_samp
                                                                  : NULL;
  const uint8_t * v_ptr =
      (num_components == 3 && (v_samp[0] != 1 || v_samp[1] != 1)) ? v_samp
                                                                  : NULL;
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  if (num_components >= 3) {
    if (h_samp[1] > h_max) {
      h_max = h_samp[1];
    }
    if (h_samp[2] > h_max) {
      h_max = h_samp[2];
    }
    if (v_samp[1] > v_max) {
      v_max = v_samp[1];
    }
    if (v_samp[2] > v_max) {
      v_max = v_samp[2];
    }
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  size_t total_blocks = 0;
  if (!gimg_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gimg_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
    gimg_free(alloc, comp_y);
    if (use_cb != comp_cb) {
      gimg_free(alloc, use_cb);
    }
    if (use_cr != comp_cr) {
      gimg_free(alloc, use_cr);
    }
    if (comp_cb) {
      gimg_free(alloc, comp_cb);
    }
    if (comp_cr) {
      gimg_free(alloc, comp_cr);
    }
    return GIMG_ERR_LIMIT;
  }
  int16_t * coef_buf =
      (int16_t *)gimg_malloc(alloc, total_blocks * 64 * sizeof(int16_t));
  if (!coef_buf) {
    gimg_free(alloc, comp_y);
    if (use_cb != comp_cb) {
      gimg_free(alloc, use_cb);
    }
    if (use_cr != comp_cr) {
      gimg_free(alloc, use_cr);
    }
    if (comp_cb) {
      gimg_free(alloc, comp_cb);
    }
    if (comp_cr) {
      gimg_free(alloc, comp_cr);
    }
    return GIMG_ERR_OOM;
  }
  size_t out_blocks = 0;
  GIMG_Result r = gimg_jpeg_progressive_fill_coef_buffer_12bit(width, height,
      num_components, comp_y, use_cb, use_cr, stride0, stride1, stride2, h_ptr,
      v_ptr, quant_luma, quant_chroma, coef_buf, &out_blocks);
  gimg_free(alloc, comp_y);
  if (use_cb != comp_cb) {
    gimg_free(alloc, use_cb);
  }
  if (use_cr != comp_cr) {
    gimg_free(alloc, use_cr);
  }
  if (num_components == 3 && comp_cb) {
    gimg_free(alloc, comp_cb);
  }
  if (num_components == 3 && comp_cr) {
    gimg_free(alloc, comp_cr);
  }
  if (r != GIMG_OK) {
    gimg_free(alloc, coef_buf);
    return r;
  }
  if (progressive) {
    *out_coef_buffer = coef_buf;
    *out_total_blocks = out_blocks;
    *out_width = width;
    *out_height = height;
    *out_num_components = num_components;
    return GIMG_OK;
  }
  unsigned char * scan_data = NULL;
  size_t scan_size = 0;
  /* T.81 Annex F: baseline sequential uses DC table for DC then AC table for
   * AC 1..63 per block; not (0,63) band with AC table only. */
  r = gimg_jpeg_encode_baseline_scan_from_coef_buffer_extended(width, height,
      num_components, coef_buf, out_blocks, h_samp, v_samp, alloc,
      restart_interval, &scan_data, &scan_size);
  gimg_free(alloc, coef_buf);
  if (r != GIMG_OK || !scan_data) {
    return (r != GIMG_OK) ? r : GIMG_ERR_OOM;
  }
  *out_scan_data = scan_data;
  *out_scan_size = scan_size;
  *out_width = width;
  *out_height = height;
  *out_num_components = num_components;
  return GIMG_OK;
}

/** Encode raster to scan data (baseline) or coefficient buffer (progressive).
 * When !progressive: allocates *out_scan_data; caller must free. When
 * progressive: allocates *out_coef_buffer (out_total_blocks * 64 int16_t);
 * caller must free. Supports GRAY8, RGB 8-bit, GRAY16, RGB 16-bit.
 * *out_precision is set to 8 or 16. */
static GIMG_Result jpeg_raster_to_scan_data(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, unsigned quality, unsigned chroma_subsampling,
    bool progressive, uint16_t restart_interval, unsigned fdct_method,
    unsigned quant_method, unsigned char ** out_scan_data,
    size_t * out_scan_size, int16_t ** out_coef_buffer,
    size_t * out_total_blocks, uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES], uint32_t * out_width,
    uint32_t * out_height, int * out_num_components, uint8_t out_h_samp[3],
    uint8_t out_v_samp[3], int * out_precision) {
  if (!alloc || !raster || !out_width || !out_height || !out_num_components ||
      !out_precision) {
    return GIMG_ERR_INTERNAL;
  }
  if (!progressive && (!out_scan_data || !out_scan_size)) {
    return GIMG_ERR_INTERNAL;
  }
  if (progressive && (!out_coef_buffer || !out_total_blocks)) {
    return GIMG_ERR_INTERNAL;
  }
  if (out_scan_data) {
    *out_scan_data = NULL;
  }
  if (out_scan_size) {
    *out_scan_size = 0;
  }
  if (out_coef_buffer) {
    *out_coef_buffer = NULL;
  }
  if (out_total_blocks) {
    *out_total_blocks = 0;
  }
  *out_precision = 8;
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || width == 0 || height == 0 || width > GIMG_JPEG_MAX_DIMENSION ||
      height > GIMG_JPEG_MAX_DIMENSION) {
    return GIMG_ERR_UNSUPPORTED;
  }
  int num_components = 0;
  int precision = 8;
  if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count == 1 &&
      fmt->bits_per_channel[0] == 8) {
    num_components = 1;
  }
  else if ((fmt->channel_model == GIMG_CHANNEL_RGB ||
               fmt->channel_model == GIMG_CHANNEL_RGBA) &&
      fmt->channel_count >= 3 && fmt->bits_per_channel[0] == 8 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    num_components = 3;
  }
  else if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count == 1 &&
      fmt->bits_per_channel[0] == 16) {
    num_components = 1;
    precision = 16;
  }
  else if ((fmt->channel_model == GIMG_CHANNEL_RGB ||
               fmt->channel_model == GIMG_CHANNEL_RGBA) &&
      fmt->channel_count >= 3 && fmt->bits_per_channel[0] == 16 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    num_components = 3;
    precision = 16;
  }
  else if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count == 1 &&
      fmt->bits_per_channel[0] == 12) {
    num_components = 1;
    precision = 12;
  }
  else if ((fmt->channel_model == GIMG_CHANNEL_RGB ||
               fmt->channel_model == GIMG_CHANNEL_RGBA) &&
      fmt->channel_count >= 3 && fmt->bits_per_channel[0] == 12 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    num_components = 3;
    precision = 12;
  }
  else {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (precision == 12) {
    *out_precision = 12;
    return jpeg_raster_to_scan_data_12bit(alloc, raster, quality,
        chroma_subsampling, progressive, restart_interval, out_scan_data,
        out_scan_size, out_coef_buffer, out_total_blocks, quant_luma,
        quant_chroma, out_width, out_height, out_num_components, out_h_samp,
        out_v_samp);
  }
  if (precision == 16) {
    *out_precision = 16;
    return jpeg_raster_to_scan_data_16bit(alloc, raster, quality,
        chroma_subsampling, progressive, out_scan_data, out_scan_size,
        out_coef_buffer, out_total_blocks, quant_luma, quant_chroma, out_width,
        out_height, out_num_components, out_h_samp, out_v_samp);
  }
  size_t comp_size = 0;
  if (!gimg_safe_mul_size((size_t)width, (size_t)height, &comp_size)) {
    return GIMG_ERR_LIMIT;
  }
  unsigned char * comp_y = (unsigned char *)gimg_malloc(alloc, comp_size);
  if (!comp_y) {
    return GIMG_ERR_OOM;
  }
  unsigned char * comp_cb = NULL;
  unsigned char * comp_cr = NULL;
  if (num_components == 3) {
    comp_cb = (unsigned char *)gimg_malloc(alloc, comp_size);
    comp_cr = (unsigned char *)gimg_malloc(alloc, comp_size);
    if (!comp_cb || !comp_cr) {
      gimg_free(alloc, comp_y);
      if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      return GIMG_ERR_OOM;
    }
  }
  size_t stride_bytes = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  if (!pixels) {
    gimg_free(alloc, comp_y);
    gimg_free(alloc, comp_cb);
    gimg_free(alloc, comp_cr);
    return GIMG_ERR_UNSUPPORTED;
  }
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  if (num_components == 1) {
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        comp_y[y * (size_t)width + x] = row[x * bpp];
      }
    }
  }
  else {
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        uint8_t r = row[x * bpp + 0];
        uint8_t g = row[x * bpp + 1];
        uint8_t b = row[x * bpp + 2];
        uint8_t yv, cb, cr;
        jpeg_rgb_to_ycbcr(r, g, b, &yv, &cb, &cr);
        comp_y[y * (size_t)width + x] = yv;
        comp_cb[y * (size_t)width + x] = cb;
        comp_cr[y * (size_t)width + x] = cr;
      }
    }
  }

  uint8_t h_samp[3] = {1, 1, 1};
  uint8_t v_samp[3] = {1, 1, 1};
  size_t stride0 = (size_t)width;
  size_t stride1 = (size_t)width;
  size_t stride2 = (size_t)width;
  unsigned char * use_cb = comp_cb;
  unsigned char * use_cr = comp_cr;

  if (num_components == 3 && chroma_subsampling != CHROMA_444) {
    if (chroma_subsampling == CHROMA_420) {
      /* T.81 Annex A: expand chroma to fill integral DCT blocks (output_cols =
       * width_in_blocks*8). */
      uint32_t mcu_per_row = (width + 15u) / 16u;
      uint32_t cw = 8u * mcu_per_row;
      uint32_t ch = (height + 1u) / 2u;
      if (ch == 0u) {
        ch = 1u;
      }
      h_samp[0] = 2;
      h_samp[1] = 1;
      h_samp[2] = 1;
      v_samp[0] = 2;
      v_samp[1] = 1;
      v_samp[2] = 1;
      size_t chroma_size = 0;
      if (!gimg_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      use_cb = (unsigned char *)gimg_malloc(alloc, chroma_size);
      use_cr = (unsigned char *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        if (use_cb) {
          gimg_free(alloc, use_cb);
        }
        return GIMG_ERR_OOM;
      }
      for (uint32_t cb_y = 0; cb_y < ch; cb_y++) {
        uint32_t y_lo = cb_y * 2u;
        uint32_t y1 = y_lo + 1u < height ? y_lo + 1u : y_lo;
        if (y_lo >= height) {
          y_lo = height - 1u;
        }
        if (y1 >= height) {
          y1 = height - 1u;
        }
        for (uint32_t cb_x = 0; cb_x < cw; cb_x++) {
          uint32_t x_lo = cb_x * 2u;
          uint32_t x1 = x_lo + 1u < width ? x_lo + 1u : x_lo;
          if (x_lo >= width) {
            x_lo = width - 1u;
          }
          if (x1 >= width) {
            x1 = width - 1u;
          }
          unsigned sum_cb = (unsigned)comp_cb[y_lo * (size_t)width + x_lo] +
              (unsigned)comp_cb[y_lo * (size_t)width + x1] +
              (unsigned)comp_cb[y1 * (size_t)width + x_lo] +
              (unsigned)comp_cb[y1 * (size_t)width + x1];
          unsigned sum_cr = (unsigned)comp_cr[y_lo * (size_t)width + x_lo] +
              (unsigned)comp_cr[y_lo * (size_t)width + x1] +
              (unsigned)comp_cr[y1 * (size_t)width + x_lo] +
              (unsigned)comp_cr[y1 * (size_t)width + x1];
          /* Ordered-dither rounding for 2×2 box: bias 1,2,1,2 per column (T.81
           * does not specify filter). */
          unsigned bias = 1u + (cb_x % 2u);
          use_cb[cb_y * (size_t)cw + cb_x] =
              (unsigned char)((sum_cb + bias) / 4);
          use_cr[cb_y * (size_t)cw + cb_x] =
              (unsigned char)((sum_cr + bias) / 4);
        }
      }
      stride1 = (size_t)cw;
      stride2 = (size_t)cw;
      gimg_free(alloc, comp_cb);
      gimg_free(alloc, comp_cr);
      comp_cb = NULL;
      comp_cr = NULL;
    }
    else {
      // 4:2:2: 2x1 horizontal, 8x1 vertical (one Cb 8x8 block per 16x8 MCU).
      uint32_t mcu_per_row = (width + 15u) / 16u;
      uint32_t mcu_per_col = (height + 7u) / 8u;
      uint32_t cw = 8u * mcu_per_row;
      uint32_t ch = 8u * mcu_per_col;
      h_samp[0] = 2;
      h_samp[1] = 1;
      h_samp[2] = 1;
      v_samp[0] = 1;
      v_samp[1] = 1;
      v_samp[2] = 1;
      size_t chroma_size = 0;
      if (!gimg_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      use_cb = (unsigned char *)gimg_malloc(alloc, chroma_size);
      use_cr = (unsigned char *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        if (use_cb) {
          gimg_free(alloc, use_cb);
        }
        return GIMG_ERR_OOM;
      }
      for (uint32_t cb_y = 0; cb_y < ch; cb_y++) {
        uint32_t y_src = (cb_y * height) / ch;
        if (y_src >= height) {
          y_src = height - 1u;
        }
        size_t row_off = (size_t)y_src * (size_t)width;
        for (uint32_t cb_x = 0; cb_x < cw; cb_x++) {
          uint32_t x_lo = cb_x * 2u;
          uint32_t x1 = x_lo + 1u < width ? x_lo + 1u : x_lo;
          if (x_lo >= width) {
            x_lo = width - 1u;
          }
          if (x1 >= width) {
            x1 = width - 1u;
          }
          unsigned sum_cb = (unsigned)comp_cb[row_off + x_lo] +
              (unsigned)comp_cb[row_off + x1];
          unsigned sum_cr = (unsigned)comp_cr[row_off + x_lo] +
              (unsigned)comp_cr[row_off + x1];
          use_cb[cb_y * (size_t)cw + cb_x] = (unsigned char)((sum_cb + 1) / 2);
          use_cr[cb_y * (size_t)cw + cb_x] = (unsigned char)((sum_cr + 1) / 2);
        }
      }
      stride1 = (size_t)cw;
      stride2 = (size_t)cw;
      gimg_free(alloc, comp_cb);
      gimg_free(alloc, comp_cr);
      comp_cb = NULL;
      comp_cr = NULL;
    }
  }

  if (out_h_samp && out_v_samp && num_components >= 3) {
    out_h_samp[0] = h_samp[0];
    out_h_samp[1] = h_samp[1];
    out_h_samp[2] = h_samp[2];
    out_v_samp[0] = v_samp[0];
    out_v_samp[1] = v_samp[1];
    out_v_samp[2] = v_samp[2];
  }

  if (quality > 100) {
    quality = 100;
  }
  gimg_jpeg_default_quant_scaled(quality, quant_luma, quant_chroma);
  const uint8_t * h_ptr =
      (num_components == 3 && (h_samp[0] != 1 || h_samp[1] != 1)) ? h_samp
                                                                  : NULL;
  const uint8_t * v_ptr =
      (num_components == 3 && (v_samp[0] != 1 || v_samp[1] != 1)) ? v_samp
                                                                  : NULL;
  GIMG_Result r;
  if (progressive) {
    uint8_t h_max = h_samp[0];
    uint8_t v_max = v_samp[0];
    if (num_components >= 3) {
      if (h_samp[1] > h_max) {
        h_max = h_samp[1];
      }
      if (h_samp[2] > h_max) {
        h_max = h_samp[2];
      }
      if (v_samp[1] > v_max) {
        v_max = v_samp[1];
      }
      if (v_samp[2] > v_max) {
        v_max = v_samp[2];
      }
    }
    uint32_t mcu_w = (uint32_t)(8 * h_max);
    uint32_t mcu_h = (uint32_t)(8 * v_max);
    uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
    uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
    size_t blocks_per_mcu = 0;
    for (int c = 0; c < num_components; c++) {
      blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
    }
    size_t total_blocks = 0;
    if (!gimg_safe_mul_size(
            (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
        !gimg_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
      gimg_free(alloc, comp_y);
      if (use_cb != comp_cb) {
        gimg_free(alloc, use_cb);
      }
      else if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      if (use_cr != comp_cr) {
        gimg_free(alloc, use_cr);
      }
      else if (comp_cr) {
        gimg_free(alloc, comp_cr);
      }
      return GIMG_ERR_LIMIT;
    }
    int16_t * coef_buf =
        (int16_t *)gimg_malloc(alloc, total_blocks * 64 * sizeof(int16_t));
    if (!coef_buf) {
      gimg_free(alloc, comp_y);
      if (use_cb != comp_cb) {
        gimg_free(alloc, use_cb);
      }
      else if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      if (use_cr != comp_cr) {
        gimg_free(alloc, use_cr);
      }
      else if (comp_cr) {
        gimg_free(alloc, comp_cr);
      }
      return GIMG_ERR_OOM;
    }
    r = gimg_jpeg_progressive_fill_coef_buffer(width, height, num_components,
        comp_y, use_cb, use_cr, stride0, stride1, stride2, h_ptr, v_ptr,
        quant_luma, quant_chroma, fdct_method, quant_method, coef_buf,
        out_total_blocks);
    gimg_free(alloc, comp_y);
    if (use_cb != comp_cb) {
      gimg_free(alloc, use_cb);
    }
    else if (comp_cb) {
      gimg_free(alloc, comp_cb);
    }
    if (use_cr != comp_cr) {
      gimg_free(alloc, use_cr);
    }
    else if (comp_cr) {
      gimg_free(alloc, comp_cr);
    }
    if (r != GIMG_OK) {
      gimg_free(alloc, coef_buf);
      return r;
    }
    *out_coef_buffer = coef_buf;
    *out_total_blocks = total_blocks;
  }
  else {
    // Use same coefficient buffer path as progressive so baseline and
    // progressive decode to identical pixels (only scan order differs).
    uint8_t h_max = h_samp[0];
    uint8_t v_max = v_samp[0];
    if (num_components >= 3) {
      if (h_samp[1] > h_max) {
        h_max = h_samp[1];
      }
      if (h_samp[2] > h_max) {
        h_max = h_samp[2];
      }
      if (v_samp[1] > v_max) {
        v_max = v_samp[1];
      }
      if (v_samp[2] > v_max) {
        v_max = v_samp[2];
      }
    }
    uint32_t mcu_w = (uint32_t)(8 * h_max);
    uint32_t mcu_h = (uint32_t)(8 * v_max);
    uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
    uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
    size_t blocks_per_mcu = 0;
    for (int c = 0; c < num_components; c++) {
      blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
    }
    size_t total_blocks = 0;
    if (!gimg_safe_mul_size(
            (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
        !gimg_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
      gimg_free(alloc, comp_y);
      if (use_cb != comp_cb) {
        gimg_free(alloc, use_cb);
      }
      else if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      if (use_cr != comp_cr) {
        gimg_free(alloc, use_cr);
      }
      else if (comp_cr) {
        gimg_free(alloc, comp_cr);
      }
      return GIMG_ERR_LIMIT;
    }
    int16_t * coef_buf =
        (int16_t *)gimg_malloc(alloc, total_blocks * 64 * sizeof(int16_t));
    if (!coef_buf) {
      gimg_free(alloc, comp_y);
      if (use_cb != comp_cb) {
        gimg_free(alloc, use_cb);
      }
      else if (comp_cb) {
        gimg_free(alloc, comp_cb);
      }
      if (use_cr != comp_cr) {
        gimg_free(alloc, use_cr);
      }
      else if (comp_cr) {
        gimg_free(alloc, comp_cr);
      }
      return GIMG_ERR_OOM;
    }
    r = gimg_jpeg_progressive_fill_coef_buffer(width, height, num_components,
        comp_y, use_cb, use_cr, stride0, stride1, stride2, h_ptr, v_ptr,
        quant_luma, quant_chroma, fdct_method, quant_method, coef_buf,
        &total_blocks);
    gimg_free(alloc, comp_y);
    if (use_cb != comp_cb) {
      gimg_free(alloc, use_cb);
    }
    else if (comp_cb) {
      gimg_free(alloc, comp_cb);
    }
    if (use_cr != comp_cr) {
      gimg_free(alloc, use_cr);
    }
    else if (comp_cr) {
      gimg_free(alloc, comp_cr);
    }
    if (r != GIMG_OK) {
      gimg_free(alloc, coef_buf);
      return r;
    }
    {
      const char * mcu_coef_dir = getenv("GIMG_JPEG_DUMP_FIRST_MCU_COEF");
      if (mcu_coef_dir && mcu_coef_dir[0] != '\0' &&
          total_blocks >= blocks_per_mcu) {
        for (size_t blk = 0; blk < blocks_per_mcu && blk < 16u; blk++) {
          char path[1024];
          int n = snprintf(
              path, sizeof(path), "%s/block_%zu.bin", mcu_coef_dir, blk);
          if (n > 0 && (size_t)n < sizeof(path)) {
            FILE * f = fopen(path, "wb");
            if (f) {
              (void)fwrite(coef_buf + blk * 64, sizeof(int16_t), 64, f);
              (void)fclose(f);
            }
          }
        }
      }
    }
    {
      const char * first_block_path = getenv("GIMG_JPEG_DUMP_FIRST_BLOCK_COEF");
      if (first_block_path && first_block_path[0] != '\0' && total_blocks > 0) {
        FILE * f = fopen(first_block_path, "wb");
        if (f) {
          (void)fwrite(coef_buf, sizeof(int16_t), 64, f);
          (void)fclose(f);
        }
      }
    }
    /* Dump first N coefficient blocks (block_000.bin .. block_(N-1).bin) for
     * comparison with reference encoder. T.81 Annex A block order; 64 int16_t
     * per file, zigzag order. */
    {
      const char * n_blocks_dir = getenv("GIMG_JPEG_DUMP_FIRST_N_BLOCKS_COEF");
      if (n_blocks_dir && n_blocks_dir[0] != '\0' && total_blocks > 0) {
        size_t n_dump = 64;
        const char * n_env = getenv("GIMG_JPEG_DUMP_FIRST_N_BLOCKS_N");
        if (n_env && n_env[0] != '\0') {
          int n_parsed = atoi(n_env);
          if (n_parsed > 0 && (size_t)n_parsed < total_blocks)
            n_dump = (size_t)n_parsed;
        }
        if (n_dump > total_blocks)
          n_dump = total_blocks;
        for (size_t blk = 0; blk < n_dump; blk++) {
          char path[1024];
          int n = snprintf(
              path, sizeof(path), "%s/block_%03zu.bin", n_blocks_dir, blk);
          if (n > 0 && (size_t)n < sizeof(path)) {
            FILE * f = fopen(path, "wb");
            if (f) {
              (void)fwrite(coef_buf + blk * 64, sizeof(int16_t), 64, f);
              (void)fclose(f);
            }
          }
        }
      }
    }
    unsigned char * scan_data = NULL;
    size_t scan_size = 0;
    r = gimg_jpeg_encode_baseline_scan_from_coef_buffer(width, height,
        num_components, coef_buf, total_blocks, h_ptr, v_ptr, alloc,
        restart_interval, &scan_data, &scan_size);
    gimg_free(alloc, coef_buf);
    if (r != GIMG_OK || !scan_data) {
      return (r != GIMG_OK) ? r : GIMG_ERR_OOM;
    }
    {
      const char * dump_path = getenv("GIMG_JPEG_DUMP_SCAN_BASELINE");
      if (dump_path && dump_path[0] != '\0') {
        FILE * f = fopen(dump_path, "wb");
        if (f) {
          (void)fwrite(scan_data, 1, scan_size, f);
          (void)fclose(f);
        }
      }
    }
    *out_scan_data = scan_data;
    *out_scan_size = scan_size;
  }
  *out_width = width;
  *out_height = height;
  *out_num_components = num_components;
  return GIMG_OK;
}

/** Copy raster to a contiguous strip for EXIF uncompressed thumbnail (format
 * 1). Supports GRAY8 and RGB/RGBA 8-bit; output is 1 or 3 bytes per pixel.
 * Caller frees *out_strip. */
static GIMG_Result jpeg_raster_to_uncompressed_strip(
    const GIMG_Allocator * alloc, const GIMG_Raster * raster,
    unsigned char ** out_strip, size_t * out_size, uint32_t * out_width,
    uint32_t * out_height, uint16_t * out_samples_per_pixel) {
  if (!alloc || !raster || !out_strip || !out_size || !out_width ||
      !out_height || !out_samples_per_pixel) {
    return GIMG_ERR_INTERNAL;
  }
  *out_strip = NULL;
  *out_size = 0;
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || width == 0 || height == 0) {
    return GIMG_ERR_UNSUPPORTED;
  }
  int samples = 0;
  if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count == 1 &&
      fmt->bits_per_channel[0] == 8) {
    samples = 1;
  }
  else if ((fmt->channel_model == GIMG_CHANNEL_RGB ||
               fmt->channel_model == GIMG_CHANNEL_RGBA) &&
      fmt->channel_count >= 3 && fmt->bits_per_channel[0] == 8 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    samples = 3;
  }
  else {
    return GIMG_ERR_UNSUPPORTED;
  }
  size_t strip_size;
  if (!gimg_safe_mul_size((size_t)width, (size_t)height, &strip_size) ||
      !gimg_safe_mul_size(strip_size, (size_t)samples, &strip_size)) {
    return GIMG_ERR_LIMIT;
  }
  unsigned char * strip = (unsigned char *)gimg_malloc(alloc, strip_size);
  if (!strip) {
    return GIMG_ERR_OOM;
  }
  size_t stride_bytes = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  if (!pixels) {
    gimg_free(alloc, strip);
    return GIMG_ERR_UNSUPPORTED;
  }
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  size_t out_bpp = (size_t)samples;
  for (uint32_t y = 0; y < height; y++) {
    const unsigned char * row = pixels + y * stride_bytes;
    for (uint32_t x = 0; x < width; x++) {
      for (int c = 0; c < samples; c++) {
        strip[(y * (size_t)width + x) * out_bpp + (size_t)c] =
            row[x * bpp + (size_t)c];
      }
    }
  }
  *out_strip = strip;
  *out_size = strip_size;
  *out_width = width;
  *out_height = height;
  *out_samples_per_pixel = (uint16_t)samples;
  return GIMG_OK;
}

/** Write DRI segment (restart interval in MCUs). Ri=0 means no restarts. */
static GIMG_Result jpeg_write_dri(
    GIMG_Stream * stream, uint16_t restart_interval, size_t * out_n) {
  if (restart_interval == 0) {
    return GIMG_OK;
  }
  size_t n = (out_n ? *out_n : 0);
  GIMG_Result r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DRI, &n);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_u16(stream, 4, &n); // length: 2 + 2 payload
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_u16(stream, restart_interval, &n);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_n) {
    *out_n = n;
  }
  return GIMG_OK;
}

/** Standard Huffman tables for write_standard_dht (T.81 Annex K.3–K.6).
 * Symbol count for each table = sum of its 16 bit-count bytes. See
 * tasks/JPEG-T.81-Annex-K-Tables.md. */
/* T.81 Table K.3: DC luminance (Th=0). Bits sum 12 → 12 values. */
static const unsigned char jpeg_std_dc_lum_bits[16] = {
    0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
static const unsigned char jpeg_std_dc_lum_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
/* T.81 Annex K.5: DC chrominance (Th=1). Bits sum 12 → 12 values. */
static const unsigned char jpeg_std_dc_chr_bits[16] = {
    0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
static const unsigned char jpeg_std_dc_chr_vals[12] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
/* T.81 Table K.4: AC luminance (Th=0). Bits sum 162 → 162 values. */
#define GIMG_JPEG_STD_AC_LUM_VALS 162
static const unsigned char jpeg_std_ac_lum_bits[16] = {
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125};
static const unsigned char jpeg_std_ac_lum_vals[GIMG_JPEG_STD_AC_LUM_VALS] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06,
    0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
    0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72,
    0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3,
    0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
    0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9,
    0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};
/* T.81 Annex K.6 (Table K.6): AC chrominance (Th=1). Canonical table from the
 * spec: 16 code-length bytes ending in 0x77 (119) at length 16, so sum(bits) =
 * 162. B.2.4 requires the number of DHT value bytes to equal that sum; we use
 * 162 values in the order given in the spec. (Some printings cite 157 symbols;
 * the table as specified in T.81 has 162.) */
#define GIMG_JPEG_STD_AC_CHR_VALS 162
static const unsigned char jpeg_std_ac_chr_bits[16] = {
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119};
static const unsigned char jpeg_std_ac_chr_vals[GIMG_JPEG_STD_AC_CHR_VALS] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
    0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1,
    0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44,
    0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74,
    0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
    0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4,
    0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa};

/** Write one DHT segment containing a single table (T.81 B.2.4). */
static GIMG_Result jpeg_write_one_dht(GIMG_Stream * stream, unsigned char tc_th,
    const unsigned char bits[16], const unsigned char * vals, int nvals,
    size_t * out_n) {
  size_t payload_len = (size_t)(1 + 16 + nvals);
  uint16_t segment_len = (uint16_t)(2 + payload_len);
  GIMG_Result r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DHT, out_n);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_u16(stream, segment_len, out_n);
  if (r != GIMG_OK) {
    return r;
  }
  size_t w = 0;
  unsigned char tc_th_byte = (unsigned char)tc_th;
  r = gimg_stream_write(stream, &tc_th_byte, 1, &w);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_n) {
    *out_n += w;
  }
  w = 0;
  r = gimg_stream_write(stream, bits, 16, &w);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_n) {
    *out_n += w;
  }
  w = 0;
  r = gimg_stream_write(stream, vals, (size_t)nvals, &w);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_n) {
    *out_n += w;
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_write_standard_dht(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  GIMG_Result r = jpeg_write_one_dht(
      stream, 0x00, jpeg_std_dc_lum_bits, jpeg_std_dc_lum_vals, 12, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x10, jpeg_std_ac_lum_bits,
      jpeg_std_ac_lum_vals, GIMG_JPEG_STD_AC_LUM_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(
      stream, 0x01, jpeg_std_dc_chr_bits, jpeg_std_dc_chr_vals, 12, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x11, jpeg_std_ac_chr_bits,
      jpeg_std_ac_chr_vals, GIMG_JPEG_STD_AC_CHR_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

/** Extended DHT for 12/16-bit (T.81): DC size 0..16 (17 symbols), AC 242
 * symbols (162 standard + 80 for run/size with size 11..15). */
#define GIMG_JPEG_EXT_DC_VALS 17
#define GIMG_JPEG_EXT_AC_VALS 242
/* Extended DC luminance: 12 standard + 5 for size 12..16 at length 16. */
static const unsigned char jpeg_ext_dc_lum_bits[16] = {
    0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 5};
static const unsigned char jpeg_ext_dc_lum_vals[GIMG_JPEG_EXT_DC_VALS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
/* Extended DC chrominance. */
static const unsigned char jpeg_ext_dc_chr_bits[16] = {
    0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 5};
static const unsigned char jpeg_ext_dc_chr_vals[GIMG_JPEG_EXT_DC_VALS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
/* Extended AC luminance: 162 standard + 80 at length 16 (size 11..15). */
static const unsigned char jpeg_ext_ac_lum_bits[16] = {
    0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 205};
static const unsigned char jpeg_ext_ac_lum_vals[GIMG_JPEG_EXT_AC_VALS] = {0x01,
    0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13,
    0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
    0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82,
    0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29,
    0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a,
    0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76,
    0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
    0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4,
    0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7,
    0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3,
    0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5,
    0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
    /* size 11..15, run 0..15 (80 symbols). */
    0x0B, 0x1B, 0x2B, 0x3B, 0x4B, 0x5B, 0x6B, 0x7B, 0x8B, 0x9B, 0xAB, 0xBB,
    0xCB, 0xDB, 0xEB, 0xFB, 0x0C, 0x1C, 0x2C, 0x3C, 0x4C, 0x5C, 0x6C, 0x7C,
    0x8C, 0x9C, 0xAC, 0xBC, 0xCC, 0xDC, 0xEC, 0xFC, 0x0D, 0x1D, 0x2D, 0x3D,
    0x4D, 0x5D, 0x6D, 0x7D, 0x8D, 0x9D, 0xAD, 0xBD, 0xCD, 0xDD, 0xED, 0xFD,
    0x0E, 0x1E, 0x2E, 0x3E, 0x4E, 0x5E, 0x6E, 0x7E, 0x8E, 0x9E, 0xAE, 0xBE,
    0xCE, 0xDE, 0xEE, 0xFE, 0x0F, 0x1F, 0x2F, 0x3F, 0x4F, 0x5F, 0x6F, 0x7F,
    0x8F, 0x9F, 0xAF, 0xBF, 0xCF, 0xDF, 0xEF, 0xFF};
/* Extended AC chrominance: 242 symbols total (T.81 B.2.4: value bytes = sum of
 * bit counts). First 15 lengths sum to 43, length 16 has 199; 43+199=242. */
static const unsigned char jpeg_ext_ac_chr_bits[16] = {
    0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 199};
static const unsigned char jpeg_ext_ac_chr_vals[GIMG_JPEG_EXT_AC_VALS] = {0x00,
    0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51,
    0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1,
    0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a,
    0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45,
    0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
    0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75,
    0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88,
    0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2,
    0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5,
    0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8,
    0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5,
    0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0x0B, 0x1B, 0x2B, 0x3B, 0x4B, 0x5B, 0x6B,
    0x7B, 0x8B, 0x9B, 0xAB, 0xBB, 0xCB, 0xDB, 0xEB, 0xFB, 0x0C, 0x1C, 0x2C,
    0x3C, 0x4C, 0x5C, 0x6C, 0x7C, 0x8C, 0x9C, 0xAC, 0xBC, 0xCC, 0xDC, 0xEC,
    0xFC, 0x0D, 0x1D, 0x2D, 0x3D, 0x4D, 0x5D, 0x6D, 0x7D, 0x8D, 0x9D, 0xAD,
    0xBD, 0xCD, 0xDD, 0xED, 0xFD, 0x0E, 0x1E, 0x2E, 0x3E, 0x4E, 0x5E, 0x6E,
    0x7E, 0x8E, 0x9E, 0xAE, 0xBE, 0xCE, 0xDE, 0xEE, 0xFE, 0x0F, 0x1F, 0x2F,
    0x3F, 0x4F, 0x5F, 0x6F, 0x7F, 0x8F, 0x9F, 0xAF, 0xBF, 0xCF, 0xDF, 0xEF,
    0xFF};

GIMG_Result gimg_jpeg_write_standard_dht_extended(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  GIMG_Result r = jpeg_write_one_dht(stream, 0x00, jpeg_ext_dc_lum_bits,
      jpeg_ext_dc_lum_vals, GIMG_JPEG_EXT_DC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x10, jpeg_ext_ac_lum_bits,
      jpeg_ext_ac_lum_vals, GIMG_JPEG_EXT_AC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x01, jpeg_ext_dc_chr_bits,
      jpeg_ext_dc_chr_vals, GIMG_JPEG_EXT_DC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x11, jpeg_ext_ac_chr_bits,
      jpeg_ext_ac_chr_vals, GIMG_JPEG_EXT_AC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

/* T.81 Table K.6 (AC refinement): 17 symbols per spec; we use 18 to include
 * (15,0)=ZRL and (15,1). (0,0)=EOB, (0,1)..(15,1)=run then newly nz, (15,0)=ZRL.
 * Loader accepts 17 or 18 as refinement (see jpeg_apply_dht_payload). */
static const unsigned char jpeg_std_ac_refine_bits[16] = {
    0, 0, 1, 0, 2, 15, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const unsigned char jpeg_std_ac_refine_vals[18] = {
    0x00, 0x01, 0x11, 0x21, 0x31, 0x41, 0x51, 0x61, 0x71, 0x81, 0x91, 0xa1,
    0xb1, 0xc1, 0xd1, 0xe1, 0xf1, 0xf0};

GIMG_Result gimg_jpeg_write_ac_refine_dht(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  /* Th=2 (table 2) so SOS Ta=2 selects this refinement table (T.81 B.2.4). */
  GIMG_Result r = jpeg_write_one_dht(stream, 0x12, jpeg_std_ac_refine_bits,
      jpeg_std_ac_refine_vals, 18, &total);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

/** 8-bit DQT payload: 1 byte (Pq<<4|Tq) + 64 bytes = 65 (T.81 B.2.4.1). */
#define GIMG_JPEG_DQT_8BIT_PAYLOAD 65
/** Write 16-bit DQT (Pq=1): payload 1 + 64*2 = 129 bytes; segment length 131.
 */
#define GIMG_JPEG_DQT_16BIT_PAYLOAD 129

/** Write one 8-bit DQT segment: marker, length (2+65), then exactly 65 payload
 * bytes (T.81 B.2.4). */
static GIMG_Result jpeg_write_dqt_8bit_segment(GIMG_Stream * stream,
    const unsigned char payload[GIMG_JPEG_DQT_8BIT_PAYLOAD], size_t * out_n) {
  GIMG_Result r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DQT, out_n);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_u16(stream, (uint16_t)(2 + GIMG_JPEG_DQT_8BIT_PAYLOAD), out_n);
  if (r != GIMG_OK) {
    return r;
  }
  size_t written = 0;
  r = gimg_stream_write(stream, payload, GIMG_JPEG_DQT_8BIT_PAYLOAD, &written);
  if (out_n) {
    *out_n += written;
  }
  return r;
}

static GIMG_Result jpeg_write_dqt_16bit(GIMG_Stream * stream,
    int num_components, const uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    const uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES], size_t * out_n) {
  size_t n = (out_n ? *out_n : 0);
  GIMG_Result r;
  size_t written = 0;
  unsigned char dqt0[GIMG_JPEG_DQT_16BIT_PAYLOAD];
  dqt0[0] = 0x10; // Pq=1, Tq=0
  for (int z = 0; z < 64; z++) {
    uint16_t v = quant_luma[gimg_jpeg_zigzag[z]];
    dqt0[1 + z * 2] = (unsigned char)(v >> 8);
    dqt0[2 + z * 2] = (unsigned char)(v & 0xFF);
  }
  r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DQT, &n);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_u16(stream, 2 + GIMG_JPEG_DQT_16BIT_PAYLOAD, &n);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_stream_write(stream, dqt0, sizeof(dqt0), &written);
  if (r != GIMG_OK) {
    return r;
  }
  n += written;
  if (num_components == 3) {
    unsigned char dqt1[GIMG_JPEG_DQT_16BIT_PAYLOAD];
    dqt1[0] = 0x11; // Pq=1, Tq=1
    for (int z = 0; z < 64; z++) {
      uint16_t v = quant_chroma[gimg_jpeg_zigzag[z]];
      dqt1[1 + z * 2] = (unsigned char)(v >> 8);
      dqt1[2 + z * 2] = (unsigned char)(v & 0xFF);
    }
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DQT, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, 2 + GIMG_JPEG_DQT_16BIT_PAYLOAD, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = gimg_stream_write(stream, dqt1, sizeof(dqt1), &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  if (out_n) {
    *out_n = n;
  }
  return GIMG_OK;
}

/** Write DQT, [DRI if restart_interval>0], SOF0/SOF1/SOF2, DHT, SOS, scan
 * data, EOI to stream (SOF before DHT to match common decoders). Does not free
 * scan_data. precision 8 = SOF0; 12 = SOF1; 16 = SOF2. */
static GIMG_Result jpeg_write_image_body(GIMG_Stream * stream, uint32_t width,
    uint32_t height, int num_components, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    const uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES],
    const unsigned char * scan_data, size_t scan_size, int precision,
    uint16_t restart_interval, size_t * out_n) {
  size_t n = (out_n ? *out_n : 0);
  GIMG_Result r;
  size_t written = 0;
  if (precision > 8) {
    r = jpeg_write_dqt_16bit(
        stream, num_components, quant_luma, quant_chroma, &n);
    if (r != GIMG_OK) {
      return r;
    }
    {
      size_t dht_written = 0;
      r = gimg_jpeg_write_standard_dht_extended(stream, &dht_written);
      if (r != GIMG_OK) {
        return r;
      }
      n += dht_written;
    }
  }
  else {
    unsigned char dqt0[GIMG_JPEG_DQT_8BIT_PAYLOAD];
    memset(dqt0, 0, sizeof(dqt0));
    dqt0[0] = 0x00;
    for (int z = 0; z < 64; z++) {
      uint16_t v = quant_luma[gimg_jpeg_zigzag[z]];
      dqt0[1 + z] = (unsigned char)(v > 255 ? 255 : v);
    }
    r = jpeg_write_dqt_8bit_segment(stream, dqt0, &n);
    if (r != GIMG_OK) {
      return r;
    }
    if (num_components == 3) {
      unsigned char dqt1[GIMG_JPEG_DQT_8BIT_PAYLOAD];
      memset(dqt1, 0, sizeof(dqt1));
      dqt1[0] = 0x01;
      for (int z = 0; z < 64; z++) {
        uint16_t v = quant_chroma[gimg_jpeg_zigzag[z]];
        dqt1[1 + z] = (unsigned char)(v > 255 ? 255 : v);
      }
      r = jpeg_write_dqt_8bit_segment(stream, dqt1, &n);
      if (r != GIMG_OK) {
        return r;
      }
    }
  }
  /* T.81 B.2.2: frame header (SOF) before table specifications (DHT). */
  {
    uint8_t sof_marker = GIMG_JPEG_MARKER_SOF0;
    if (precision == 12) {
      sof_marker = GIMG_JPEG_MARKER_SOF1;
    }
    else if (precision == 16) {
      sof_marker = GIMG_JPEG_MARKER_SOF2;
    }
    uint8_t prec_byte = (uint8_t)(precision < 8 ? 8 : precision);
    /* SOF Lf = 8 + 3*Nc (T.81 B.2.2); payload = Lf - 2 = 6 + 3*Nc bytes. */
    uint16_t sof_len = (uint16_t)(8 + 3 * (uint16_t)num_components);
    size_t sof_payload = 6 + 3 * (size_t)num_components;
    r = jpeg_write_marker(stream, sof_marker, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, sof_len, &n);
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char sof[8 + 3 * 4];
    memset(sof, 0, sizeof(sof));
    sof[0] = prec_byte;
    sof[1] = (unsigned char)(height >> 8);
    sof[2] = (unsigned char)(height & 0xFF);
    sof[3] = (unsigned char)(width >> 8);
    sof[4] = (unsigned char)(width & 0xFF);
    sof[5] = (unsigned char)num_components;
    for (int c = 0; c < num_components; c++) {
      uint8_t h = (h_samp && c < 3) ? h_samp[c] : 1;
      uint8_t v = (v_samp && c < 3) ? v_samp[c] : 1;
      if (h == 0) {
        h = 1;
      }
      if (v == 0) {
        v = 1;
      }
      sof[6 + c * 3] = (unsigned char)(c + 1);
      sof[7 + c * 3] = (unsigned char)((h << 4) | v);
      sof[8 + c * 3] = (unsigned char)(c == 0 ? 0 : 1);
    }
    r = gimg_stream_write(stream, sof, sof_payload, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  if (precision <= 8) {
    size_t dht_written = 0;
    r = gimg_jpeg_write_standard_dht(stream, &dht_written);
    if (r != GIMG_OK) {
      return r;
    }
    n += dht_written;
  }
  /* T.81: DRI after SOF, before SOS. */
  r = jpeg_write_dri(stream, restart_interval, &n);
  if (r != GIMG_OK) {
    return r;
  }
  {
    uint16_t sos_len = (uint16_t)(6 + 2 * (uint16_t)num_components);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_SOS, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, sos_len, &n);
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char sos[12];
    memset(sos, 0, sizeof(sos));
    sos[0] = (unsigned char)num_components;
    if (num_components == 1) {
      sos[1] = 0x01;
      sos[2] = 0x00;
    }
    else {
      sos[1] = 0x01;
      sos[2] = 0x00;
      sos[3] = 0x02;
      sos[4] = 0x11;
      sos[5] = 0x03;
      sos[6] = 0x11;
    }
    size_t tail = 1 + 2 * (size_t)num_components;
    sos[tail] = 0x00;
    sos[tail + 1] = 0x3F;
    sos[tail + 2] = 0x00;
    r = gimg_stream_write(stream, sos, tail + 3, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  r = jpeg_write_scan_data_with_stuffing(stream, scan_data, scan_size, &n);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_EOI, &n);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_n) {
    *out_n = n;
  }
  return GIMG_OK;
}

/** Default progressive scan script: one DC scan then one AC scan (Ss=1..63). */
static const GIMG_JPEG_Progressive_Scan gimg_jpeg_default_progressive_scans[] =
    {
        {0, 0, 0, 0},
        {1, 63, 0, 0},
};
static const unsigned gimg_jpeg_default_progressive_scan_count = 2;

/** Validate progressive config: Ss, Se in 0..63, Ss<=Se; Ah, Al in 0..15;
 * scan_count in limit. T.81 Annex G: spectral selection bands are coded in
 * separate scans and bands must not overlap (successive bands). We reject
 * overlapping [Ss,Se] ranges between initial AC scans (Ah=0, Ss>=1). */
static GIMG_Result jpeg_validate_progressive_config(
    const GIMG_JPEG_Progressive_Config * config) {
  if (!config || config->scan_count == 0) {
    return GIMG_OK;
  }
  if (config->scan_count > GIMG_JPEG_MAX_SCANS) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (!config->scans) {
    return GIMG_ERR_FORMAT;
  }
  for (unsigned i = 0; i < config->scan_count; i++) {
    uint8_t Ss = config->scans[i].Ss;
    uint8_t Se = config->scans[i].Se;
    uint8_t Ah = config->scans[i].Ah;
    uint8_t Al = config->scans[i].Al;
    if (Ss > 63 || Se > 63 || Ss > Se) {
      return GIMG_ERR_UNSUPPORTED;
    }
    if (Ah > 15 || Al > 15) {
      return GIMG_ERR_UNSUPPORTED;
    }
  }
  /* T.81 Annex G: initial AC bands [Ss,Se] (Ah=0, Ss>=1) must not overlap. */
  for (unsigned i = 0; i < config->scan_count; i++) {
    uint8_t Ss_i = config->scans[i].Ss;
    uint8_t Se_i = config->scans[i].Se;
    uint8_t Ah_i = config->scans[i].Ah;
    int ac_initial_i = (Ah_i == 0 && (Ss_i != 0 || Se_i != 0));
    if (!ac_initial_i) {
      continue;
    }
    for (unsigned j = i + 1; j < config->scan_count; j++) {
      uint8_t Ss_j = config->scans[j].Ss;
      uint8_t Se_j = config->scans[j].Se;
      uint8_t Ah_j = config->scans[j].Ah;
      int ac_initial_j = (Ah_j == 0 && (Ss_j != 0 || Se_j != 0));
      if (!ac_initial_j) {
        continue;
      }
      /* Bands [Ss_i, Se_i] and [Ss_j, Se_j] overlap iff Ss_i <= Se_j && Ss_j <= Se_i */
      if (Ss_i <= (unsigned)Se_j && Ss_j <= (unsigned)Se_i) {
        return GIMG_ERR_UNSUPPORTED;
      }
    }
  }
  return GIMG_OK;
}

/** Write DQT, DHT, [DRI if restart_interval>0], SOF2, then for each scan: SOS
 * (Ss,Se,Ah,Al) + scan data; then EOI. precision 8 or 16. Frees scan data
 * after each write. */
static GIMG_Result jpeg_write_image_body_progressive(GIMG_Stream * stream,
    uint32_t width, uint32_t height, int num_components, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    const uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES],
    const int16_t * coef_buffer, size_t total_blocks,
    const GIMG_JPEG_Progressive_Scan * scans, unsigned scan_count,
    int precision, const GIMG_Allocator * alloc, uint16_t restart_interval,
    size_t * out_n) {
  size_t n = (out_n ? *out_n : 0);
  GIMG_Result r;
  size_t written = 0;
  if (precision > 8) {
    r = jpeg_write_dqt_16bit(
        stream, num_components, quant_luma, quant_chroma, &n);
    if (r != GIMG_OK) {
      return r;
    }
    {
      size_t dht_written = 0;
      r = gimg_jpeg_write_standard_dht_extended(stream, &dht_written);
      if (r != GIMG_OK) {
        return r;
      }
      n += dht_written;
    }
  }
  else {
    unsigned char dqt0[GIMG_JPEG_DQT_8BIT_PAYLOAD];
    memset(dqt0, 0, sizeof(dqt0));
    dqt0[0] = 0x00;
    for (int z = 0; z < 64; z++) {
      uint16_t v = quant_luma[gimg_jpeg_zigzag[z]];
      dqt0[1 + z] = (unsigned char)(v > 255 ? 255 : v);
    }
    r = jpeg_write_dqt_8bit_segment(stream, dqt0, &n);
    if (r != GIMG_OK) {
      return r;
    }
    if (num_components == 3) {
      unsigned char dqt1[GIMG_JPEG_DQT_8BIT_PAYLOAD];
      memset(dqt1, 0, sizeof(dqt1));
      dqt1[0] = 0x01;
      for (int z = 0; z < 64; z++) {
        uint16_t v = quant_chroma[gimg_jpeg_zigzag[z]];
        dqt1[1 + z] = (unsigned char)(v > 255 ? 255 : v);
      }
      r = jpeg_write_dqt_8bit_segment(stream, dqt1, &n);
      if (r != GIMG_OK) {
        return r;
      }
    }
    {
      size_t dht_written = 0;
      r = gimg_jpeg_write_standard_dht(stream, &dht_written);
      if (r != GIMG_OK) {
        return r;
      }
      n += dht_written;
    }
  }
  {
    int need_refine_dht = 0;
    for (unsigned s = 0; s < scan_count && !need_refine_dht; s++) {
      if (scans[s].Ah != 0 && !(scans[s].Ss == 0 && scans[s].Se == 0)) {
        need_refine_dht = 1;
      }
    }
    if (need_refine_dht) {
      size_t dht_written = 0;
      r = gimg_jpeg_write_ac_refine_dht(stream, &dht_written);
      if (r != GIMG_OK) {
        return r;
      }
      n += dht_written;
    }
  }
  r = jpeg_write_dri(stream, restart_interval, &n);
  if (r != GIMG_OK) {
    return r;
  }
  {
    uint8_t prec_byte = (uint8_t)(precision > 8 ? precision : 8);
    uint16_t sof_len = (uint16_t)(8 + 3 * (uint16_t)num_components);
    size_t sof_payload = 6 + 3 * (size_t)num_components;
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_SOF2, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, sof_len, &n);
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char sof[8 + 3 * 4];
    memset(sof, 0, sizeof(sof));
    sof[0] = prec_byte;
    sof[1] = (unsigned char)(height >> 8);
    sof[2] = (unsigned char)(height & 0xFF);
    sof[3] = (unsigned char)(width >> 8);
    sof[4] = (unsigned char)(width & 0xFF);
    sof[5] = (unsigned char)num_components;
    for (int c = 0; c < num_components; c++) {
      uint8_t h = (h_samp && c < 3) ? h_samp[c] : 1;
      uint8_t v = (v_samp && c < 3) ? v_samp[c] : 1;
      if (h == 0) {
        h = 1;
      }
      if (v == 0) {
        v = 1;
      }
      sof[6 + c * 3] = (unsigned char)(c + 1);
      sof[7 + c * 3] = (unsigned char)((h << 4) | v);
      sof[8 + c * 3] = (unsigned char)(c == 0 ? 0 : 1);
    }
    r = gimg_stream_write(stream, sof, sof_payload, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  /* State after AC initial scan so refinement scan can tell newly vs already
   * nonzero. */
  int16_t * ac_initial_state = NULL;
  if (scan_count >= 2) {
    int need_state = 0;
    for (unsigned s = 1; s < scan_count && !need_state; s++) {
      int prev_ac_initial = (scans[s - 1].Ah == 0 &&
          (scans[s - 1].Ss != 0 || scans[s - 1].Se != 0));
      int this_refinement =
          (scans[s].Ah != 0 && (scans[s].Ss != 0 || scans[s].Se != 0));
      if (prev_ac_initial && this_refinement) {
        need_state = 1;
      }
    }
    if (need_state) {
      ac_initial_state =
          (int16_t *)gimg_malloc(alloc, total_blocks * 64 * sizeof(int16_t));
    }
  }
  for (unsigned s = 0; s < scan_count; s++) {
    unsigned char * scan_data = NULL;
    size_t scan_size = 0;
    int this_ac_initial =
        (scans[s].Ah == 0 && (scans[s].Ss != 0 || scans[s].Se != 0));
    int this_refinement =
        (scans[s].Ah != 0 && (scans[s].Ss != 0 || scans[s].Se != 0));
    int prev_ac_initial = (s > 0 && scans[s - 1].Ah == 0 &&
        (scans[s - 1].Ss != 0 || scans[s - 1].Se != 0));
    int16_t * state_out =
        (ac_initial_state && this_ac_initial) ? ac_initial_state : NULL;
    const int16_t * state_in =
        (ac_initial_state && this_refinement && prev_ac_initial)
        ? ac_initial_state
        : NULL;
    if (precision > 8) {
      r = gimg_jpeg_encode_progressive_scan_16bit(width, height, num_components,
          coef_buffer, total_blocks, h_samp, v_samp, scans[s].Ss, scans[s].Se,
          scans[s].Ah, scans[s].Al, alloc, restart_interval, &scan_data,
          &scan_size);
    }
    else {
      r = gimg_jpeg_encode_progressive_scan(width, height, num_components,
          coef_buffer, total_blocks, h_samp, v_samp, scans[s].Ss, scans[s].Se,
          scans[s].Ah, scans[s].Al, alloc, restart_interval, &scan_data,
          &scan_size, state_out, state_in, (int)s);
    }
    if (r != GIMG_OK) {
      return r;
    }
    uint16_t sos_len = (uint16_t)(6 + 2 * (uint16_t)num_components);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_SOS, &n);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    r = jpeg_write_u16(stream, sos_len, &n);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    unsigned char sos[12];
    memset(sos, 0, sizeof(sos));
    sos[0] = (unsigned char)num_components;
    // AC refinement scans use Ta=2 (refinement table); else Ta=0/1.
    int ac_refine =
        (scans[s].Ah != 0 && !(scans[s].Ss == 0 && scans[s].Se == 0));
    if (num_components == 1) {
      sos[1] = 0x01;
      sos[2] = (unsigned char)(ac_refine ? 0x02 : 0x00);
    }
    else {
      sos[1] = 0x01;
      sos[2] = (unsigned char)(ac_refine ? 0x02 : 0x00);
      sos[3] = 0x02;
      sos[4] = (unsigned char)(ac_refine ? 0x12 : 0x11);
      sos[5] = 0x03;
      sos[6] = (unsigned char)(ac_refine ? 0x12 : 0x11);
    }
    size_t tail = 1 + 2 * (size_t)num_components;
    sos[tail] = scans[s].Ss;
    sos[tail + 1] = scans[s].Se;
    sos[tail + 2] = (unsigned char)((scans[s].Ah << 4) | (scans[s].Al & 0x0F));
    r = gimg_stream_write(stream, sos, tail + 3, &written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    n += written;
    r = jpeg_write_scan_data_with_stuffing(stream, scan_data, scan_size, &n);
    gimg_free(alloc, scan_data);
    if (r != GIMG_OK) {
      gimg_free(alloc, ac_initial_state);
      return r;
    }
  }
  gimg_free(alloc, ac_initial_state);
  r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_EOI, &n);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_n) {
    *out_n = n;
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)format_name;
  if (!codec || !doc || !stream || !report) {
    return GIMG_ERR_INTERNAL;
  }
  report->bytes_written = 0;
  if (gimg_doc_item_count(doc) == 0) {
    return GIMG_ERR_UNSUPPORTED; // No image to save.
  }
  GIMG_Item * item = gimg_doc_item((GIMG_Doc *)doc, 0);
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }

  // Synthetic document support: use raster if present; else decode only when
  // doc was loaded by this codec.
  GIMG_Raster * raster = gimg_item_raster(item);
  int raster_owned = 0;
  if (!raster && doc->loaded_by_codec == (struct GIMG_Codec *)codec) {
    GIMG_Result r = gimg_item_decode(item, NULL, &raster);
    if (r != GIMG_OK || !raster) {
      return (r != GIMG_OK) ? r : GIMG_ERR_UNSUPPORTED;
    }
    raster_owned = 1;
  }
  if (!raster) {
    return GIMG_ERR_UNSUPPORTED; // No raster and not loaded by us.
  }

  /* When jpeg_precision is 8, 12, or 16 and raster depth differs, convert via
   * library bit-depth API (T.81 / first-class). */
  if (options && (options->jpeg_precision == 8 || options->jpeg_precision == 12
                      || options->jpeg_precision == 16)) {
    uint8_t want_bits = options->jpeg_precision;
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    uint8_t have_bits = fmt && fmt->channel_count > 0
        ? fmt->bits_per_channel[0]
        : 0;
    if (have_bits != want_bits && (have_bits == 8 || have_bits == 12 ||
            have_bits == 16)) {
      GIMG_Raster * converted = NULL;
      GIMG_Result r_conv =
          gimg_ops_convert_bit_depth(raster, want_bits, &converted);
      if (r_conv == GIMG_OK && converted) {
        if (raster_owned) {
          gimg_raster_destroy(raster);
        }
        raster = converted;
        raster_owned = 1;
      }
    }
  }

  const GIMG_Allocator * alloc = codec->allocator;
  alloc = gimg_alloc_or_default(alloc);

  unsigned quality = GIMG_JPEG_DEFAULT_QUALITY;
  if (options && options->quality != 0) {
    quality = options->quality;
    if (quality > 100) {
      quality = 100;
    }
  }

  unsigned chroma_subsampling =
      (options && options->jpeg_chroma_subsampling <= 2)
      ? options->jpeg_chroma_subsampling
      : (unsigned)CHROMA_420;
  bool progressive = (options && options->jpeg_progressive) ? true : false;
  GIMG_Result r;
  if (progressive) {
    r = jpeg_validate_progressive_config(
        options ? options->jpeg_progressive_config : NULL);
    if (r != GIMG_OK) {
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      return r;
    }
  }
  uint16_t restart_interval = (options && options->jpeg_restart_interval != 0)
      ? options->jpeg_restart_interval
      : 0;
  unsigned fdct_method =
      options && options->jpeg_fdct_method <= GIMG_JPEG_FDCT_REF
      ? options->jpeg_fdct_method
      : (unsigned)GIMG_JPEG_FDCT_LOEFFLER;
  unsigned quant_method =
      options && options->jpeg_quant_method <= GIMG_JPEG_QUANT_DIV
      ? options->jpeg_quant_method
      : (unsigned)GIMG_JPEG_QUANT_RECIP;
  uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES];
  uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES];
  unsigned char * scan_data = NULL;
  size_t scan_size = 0;
  int16_t * coef_buffer = NULL;
  size_t total_blocks = 0;
  uint32_t width = 0, height = 0;
  int num_components = 0;
  uint8_t h_samp[3] = {1, 1, 1};
  uint8_t v_samp[3] = {1, 1, 1};
  int precision = 8;
  r = jpeg_raster_to_scan_data(alloc, raster, quality, chroma_subsampling,
      progressive, restart_interval, fdct_method, quant_method, &scan_data,
      &scan_size, &coef_buffer, &total_blocks, quant_luma, quant_chroma, &width,
      &height, &num_components, h_samp, v_samp, &precision);
  if (raster_owned) {
    gimg_raster_destroy(raster);
  }
  if (r != GIMG_OK) {
    return r;
  }
  /* Use progressive image body when we have coefficient buffer (8-bit
   * progressive, or 12/16-bit which use coef path for both baseline and
   * progressive). */
  bool use_progressive_body = (coef_buffer != NULL);
  if (use_progressive_body) {
    if (!coef_buffer) {
      return GIMG_ERR_OOM;
    }
  }
  else {
    if (!scan_data) {
      return GIMG_ERR_OOM;
    }
  }
  void * to_free =
      use_progressive_body ? (void *)coef_buffer : (void *)scan_data;

  size_t written = 0;
  r = gimg_stream_write(
      stream, gimg_jpeg_signature, GIMG_JPEG_SIGNATURE_LEN, &written);
  if (r != GIMG_OK) {
    gimg_free(alloc, to_free);
    return r;
  }
  report->bytes_written += written;

  // APP segments per metadata policy (order: COM, APP0, APP1 EXIF, APP1 XMP,
  // APP2 ICC). COM and APP segments before SOF.
  {
    GIMG_Meta_Policy policy =
        options ? options->metadata_policy : GIMG_META_PRESERVE_ALL;
    uint32_t x_dpi = 0, y_dpi = 0;
    GIMG_Meta_Common * meta_common = gimg_doc_meta_common(doc);
    if (meta_common) {
      gimg_meta_common_dpi(meta_common, &x_dpi, &y_dpi);
    }
    GIMG_Meta_Raw * meta_raw = gimg_doc_meta_raw(doc);

    // COM segment(s): when policy preserves metadata, write from meta_raw if
    // present; else from meta_common description if set (programmatic case).
    if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_COMMON_ONLY) {
      size_t com_size = 0;
      bool have_com_raw = meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_COM, NULL,
              &com_size) == GIMG_OK &&
          com_size > 0;
      if (have_com_raw) {
        unsigned char * com_buf = (unsigned char *)gimg_malloc(alloc, com_size);
        if (com_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_COM, com_buf, &com_size);
          if (r == GIMG_OK) {
            size_t offset = 0;
            while (offset + 2 <= com_size) {
              uint16_t plen =
                  (uint16_t)((com_buf[offset] << 8) | com_buf[offset + 1]);
              offset += 2;
              if (offset + plen > com_size) {
                break;
              }
              r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_COM,
                  com_buf + offset, plen, &report->bytes_written);
              if (r != GIMG_OK) {
                gimg_free(alloc, com_buf);
                gimg_free(alloc, to_free);
                return r;
              }
              offset += plen;
            }
          }
          gimg_free(alloc, com_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }
      else if (meta_common) {
        const char * desc = gimg_meta_common_description(meta_common);
        if (desc) {
          size_t dlen = strlen(desc);
          if (dlen <= 65535u) {
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_COM,
                (const unsigned char *)desc, dlen, &report->bytes_written);
            if (r != GIMG_OK) {
              gimg_free(alloc, to_free);
              return r;
            }
          }
        }
      }
    }

    // APP0: from meta_raw when preserving and present, else minimal (JFIF).
    size_t app0_len = 0;
    bool have_app0 = (policy != GIMG_META_DROP_ALL &&
                         policy != GIMG_META_KEEP_COMMON_ONLY) &&
        meta_raw &&
        gimg_meta_raw_get(
            meta_raw, "jpeg", GIMG_JPEG_RAW_APP0, NULL, &app0_len) == GIMG_OK &&
        app0_len > 0;
    if (have_app0) {
      unsigned char * app0_buf = (unsigned char *)gimg_malloc(alloc, app0_len);
      if (app0_buf) {
        r = gimg_meta_raw_get(
            meta_raw, "jpeg", GIMG_JPEG_RAW_APP0, app0_buf, &app0_len);
        if (r == GIMG_OK) {
          r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP0, app0_buf,
              app0_len, &report->bytes_written);
        }
        gimg_free(alloc, app0_buf);
      }
      if (r != GIMG_OK) {
        gimg_free(alloc, to_free);
        return r;
      }
    }
    else {
      unsigned char app0[14];
      jpeg_build_minimal_app0(app0, x_dpi, y_dpi);
      r = jpeg_write_app_segment(
          stream, GIMG_JPEG_MARKER_APP0, app0, 14, &report->bytes_written);
      if (r != GIMG_OK) {
        gimg_free(alloc, to_free);
        return r;
      }
    }

    // APP0 JFXX (JFIF 1.02 extension): write when preserved in meta_raw.
    if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_COMMON_ONLY &&
        meta_raw) {
      size_t jfxx_len = 0;
      if (gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP0_JFXX, NULL,
              &jfxx_len) == GIMG_OK &&
          jfxx_len > 0) {
        unsigned char * jfxx_buf =
            (unsigned char *)gimg_malloc(alloc, jfxx_len);
        if (jfxx_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_APP0_JFXX, jfxx_buf, &jfxx_len);
          if (r == GIMG_OK) {
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP0, jfxx_buf,
                jfxx_len, &report->bytes_written);
          }
          gimg_free(alloc, jfxx_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }
    }

    // APP1 EXIF / XMP and APP2 ICC only when policy preserves metadata
    if (policy != GIMG_META_DROP_ALL && policy != GIMG_META_KEEP_COMMON_ONLY) {
      // Build EXIF with thumbnail when we are writing APP1 EXIF and doc has a
      // second item. The spec does not say when to synthesize APP1
      // EXIF for docs that have no EXIF; we use (have_exif_raw ||
      // !have_app0_only) so that:
      //   - JFIF-only docs (APP0 thumbnail, no EXIF): we do not synthesize APP1
      //     EXIF; the thumbnail stays in APP0 and round-trip is correct.
      //   - Synthetic 2-item docs (no EXIF, no APP0): we do build APP1 EXIF and
      //     encode item 1 into IFD1 so save-from-programmatic-doc works.
      // have_app0_only = we have APP0 in meta_raw and no APP1 EXIF.
      {
        size_t item_count = gimg_doc_item_count(doc);
        size_t raw_exif_size = 0;
        bool have_exif_raw = meta_raw &&
            gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF, NULL,
                &raw_exif_size) == GIMG_OK &&
            raw_exif_size > 6;
        size_t app0_len_check = 0;
        bool have_app0_only = meta_raw &&
            gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP0, NULL,
                &app0_len_check) == GIMG_OK &&
            app0_len_check > 0 && !have_exif_raw;
        unsigned int thumb_fmt = options
            ? options->exif_thumbnail_format
            : (unsigned int)GIMG_EXIF_THUMB_FORMAT_DEFAULT;
        if (thumb_fmt == 0) {
          thumb_fmt = GIMG_EXIF_THUMB_FORMAT_JPEG;
        }
        unsigned int thumb_quality =
            (options && options->exif_thumbnail_quality != 0)
            ? options->exif_thumbnail_quality
            : GIMG_JPEG_DEFAULT_QUALITY;
        if (thumb_quality > 100) {
          thumb_quality = 100;
        }

        bool wrote_exif = false;
        if ((have_exif_raw || !have_app0_only) && item_count >= 2 &&
            (thumb_fmt == GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED ||
                thumb_fmt == GIMG_EXIF_THUMB_FORMAT_JPEG ||
                thumb_fmt == GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG)) {
          GIMG_Item * thumb_item = gimg_doc_item((GIMG_Doc *)doc, 1);
          GIMG_Raster * thumb_raster =
              thumb_item ? gimg_item_raster(thumb_item) : NULL;
          bool thumb_raster_owned = false;
          if (!thumb_raster &&
              doc->loaded_by_codec == (struct GIMG_Codec *)codec &&
              thumb_item) {
            r = gimg_item_decode(thumb_item, NULL, &thumb_raster);
            if (r == GIMG_OK && thumb_raster) {
              thumb_raster_owned = true;
            }
          }
          if (thumb_raster) {
            const void * base_exif = NULL;
            size_t base_size = 0;
            unsigned char * base_exif_owned = NULL;
            if (meta_raw) {
              size_t raw_exif_size = 0;
              if (gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF,
                      NULL, &raw_exif_size) == GIMG_OK &&
                  raw_exif_size > 6) {
                unsigned char * raw_buf =
                    (unsigned char *)gimg_malloc(alloc, raw_exif_size);
                if (raw_buf &&
                    gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF,
                        raw_buf, &raw_exif_size) == GIMG_OK &&
                    raw_buf[0] == 'E' && raw_buf[1] == 'x' &&
                    raw_buf[2] == 'i' && raw_buf[3] == 'f' && raw_buf[4] == 0 &&
                    raw_buf[5] == 0) {
                  base_exif = raw_buf + 6;
                  base_size = raw_exif_size - 6;
                  base_exif_owned = raw_buf;
                }
                else if (raw_buf) {
                  gimg_free(alloc, raw_buf);
                }
              }
            }

            if (thumb_fmt == GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED) {
              unsigned char * thumb_strip = NULL;
              size_t thumb_strip_size = 0;
              uint32_t tw = 0, th = 0;
              uint16_t samples = 0;
              r = jpeg_raster_to_uncompressed_strip(alloc, thumb_raster,
                  &thumb_strip, &thumb_strip_size, &tw, &th, &samples);
              if (thumb_raster_owned) {
                gimg_raster_destroy(thumb_raster);
              }
              if (r == GIMG_OK && thumb_strip) {
                void * exif_tiff = NULL;
                size_t exif_tiff_size = 0;
                r = gimg_exif_build_with_thumbnail_uncompressed(alloc,
                    base_exif, base_size, thumb_strip, thumb_strip_size, tw, th,
                    samples, 8, &exif_tiff, &exif_tiff_size);
                if (r == GIMG_OK && exif_tiff) {
                  static const unsigned char exif_prefix[] = {
                      'E', 'x', 'i', 'f', 0, 0};
                  size_t app1_len = 6 + exif_tiff_size;
                  unsigned char * app1_payload =
                      (unsigned char *)gimg_malloc(alloc, app1_len);
                  if (app1_payload) {
                    memcpy(app1_payload, exif_prefix, 6);
                    memcpy(app1_payload + 6, exif_tiff, exif_tiff_size);
                    r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP1,
                        app1_payload, app1_len, &report->bytes_written);
                    gimg_free(alloc, app1_payload);
                    wrote_exif = (r == GIMG_OK);
                  }
                  gimg_free(alloc, exif_tiff);
                }
                gimg_free(alloc, thumb_strip);
              }
              if (base_exif_owned) {
                gimg_free(alloc, base_exif_owned);
              }
            }
            else {
              unsigned char * thumb_scan = NULL;
              size_t thumb_scan_size = 0;
              uint16_t tq_luma[GIMG_JPEG_DQT_ENTRIES];
              uint16_t tq_chroma[GIMG_JPEG_DQT_ENTRIES];
              uint32_t tw = 0, th = 0;
              int tnc = 0;
              int thumb_prec = 8;
              r = jpeg_raster_to_scan_data(alloc, thumb_raster, thumb_quality,
                  CHROMA_444, false, 0, fdct_method, quant_method, &thumb_scan,
                  &thumb_scan_size, NULL, NULL, tq_luma, tq_chroma, &tw, &th,
                  &tnc, NULL, NULL, &thumb_prec);
              if (thumb_raster_owned) {
                gimg_raster_destroy(thumb_raster);
              }
              if (r == GIMG_OK && thumb_scan) {
                GIMG_Stream * mem_stream = NULL;
                r = gimg_stream_create_memory_output_with_allocator(
                    alloc, &mem_stream);
                if (r == GIMG_OK && mem_stream) {
                  size_t mem_n = 0;
                  r = jpeg_write_marker(
                      mem_stream, GIMG_JPEG_MARKER_SOI, &mem_n);
                  if (r == GIMG_OK) {
                    r = jpeg_write_image_body(mem_stream, tw, th, tnc, NULL,
                        NULL, tq_luma, tq_chroma, thumb_scan, thumb_scan_size,
                        8, 0, &mem_n);
                  }
                  if (r == GIMG_OK) {
                    const void * jpeg_buf = NULL;
                    size_t jpeg_buf_size = 0;
                    gimg_stream_output_buffer(
                        mem_stream, &jpeg_buf, &jpeg_buf_size);
                    void * exif_tiff = NULL;
                    size_t exif_tiff_size = 0;
                    if (thumb_fmt == GIMG_EXIF_THUMB_FORMAT_JPEG) {
                      r = gimg_exif_build_with_thumbnail_jpeg(alloc, base_exif,
                          base_size, jpeg_buf, jpeg_buf_size, &exif_tiff,
                          &exif_tiff_size);
                    }
                    else {
                      r = gimg_exif_build_with_thumbnail_tiff_jpeg(alloc,
                          base_exif, base_size, jpeg_buf, jpeg_buf_size,
                          &exif_tiff, &exif_tiff_size);
                    }
                    if (r == GIMG_OK && exif_tiff) {
                      static const unsigned char exif_prefix[] = {
                          'E', 'x', 'i', 'f', 0, 0};
                      size_t app1_len = 6 + exif_tiff_size;
                      unsigned char * app1_payload =
                          (unsigned char *)gimg_malloc(alloc, app1_len);
                      if (app1_payload) {
                        memcpy(app1_payload, exif_prefix, 6);
                        memcpy(app1_payload + 6, exif_tiff, exif_tiff_size);
                        r = jpeg_write_app_segment(stream,
                            GIMG_JPEG_MARKER_APP1, app1_payload, app1_len,
                            &report->bytes_written);
                        gimg_free(alloc, app1_payload);
                        wrote_exif = (r == GIMG_OK);
                      }
                      gimg_free(alloc, exif_tiff);
                    }
                  }
                  gimg_stream_destroy(mem_stream);
                }
              }
              if (thumb_scan) {
                gimg_free(alloc, thumb_scan);
              }
              if (base_exif_owned) {
                gimg_free(alloc, base_exif_owned);
              }
            }
          }
        }

        if (!wrote_exif) {
          size_t exif_size = 0;
          if (meta_raw &&
              gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF, NULL,
                  &exif_size) == GIMG_OK &&
              exif_size > 0) {
            unsigned char * exif_buf =
                (unsigned char *)gimg_malloc(alloc, exif_size);
            if (exif_buf) {
              r = gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF,
                  exif_buf, &exif_size);
              if (r == GIMG_OK) {
                const void * to_write = exif_buf;
                size_t to_write_size = exif_size;
                void * modified = NULL;
                size_t modified_size = 0;
                if (policy == GIMG_META_STRIP_GPS) {
                  if (gimg_exif_strip_gps(alloc, exif_buf, exif_size, &modified,
                          &modified_size) == GIMG_OK) {
                    to_write = modified;
                    to_write_size = modified_size;
                  }
                }
                else if (policy == GIMG_META_NORMALIZE_EXIF) {
                  if (gimg_exif_normalize(alloc, exif_buf, exif_size, &modified,
                          &modified_size) == GIMG_OK) {
                    to_write = modified;
                    to_write_size = modified_size;
                  }
                }
                r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP1,
                    to_write, to_write_size, &report->bytes_written);
                if (modified) {
                  gimg_free(alloc, modified);
                }
              }
              gimg_free(alloc, exif_buf);
            }
          }
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }

      // APP1 XMP
      size_t xmp_size = 0;
      if (meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_XMP, NULL,
              &xmp_size) == GIMG_OK &&
          xmp_size > 0) {
        unsigned char * xmp_buf = (unsigned char *)gimg_malloc(alloc, xmp_size);
        if (xmp_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_APP1_XMP, xmp_buf, &xmp_size);
          if (r == GIMG_OK) {
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP1, xmp_buf,
                xmp_size, &report->bytes_written);
          }
          gimg_free(alloc, xmp_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }

      // APP2 ICC: multi-segment (CHUNKS) or single segment (APP2_ICC).
      size_t icc_chunks_size = 0;
      if (meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC_CHUNKS,
              NULL, &icc_chunks_size) == GIMG_OK &&
          icc_chunks_size >= 2) {
        unsigned char * chunks_buf =
            (unsigned char *)gimg_malloc(alloc, icc_chunks_size);
        if (chunks_buf) {
          r = gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC_CHUNKS,
              chunks_buf, &icc_chunks_size);
          if (r == GIMG_OK) {
            uint16_t n = (uint16_t)((chunks_buf[0] << 8) | chunks_buf[1]);
            size_t off = 2;
            for (uint16_t i = 0; i < n && off + 2 <= icc_chunks_size; i++) {
              uint16_t plen =
                  (uint16_t)((chunks_buf[off] << 8) | chunks_buf[off + 1]);
              off += 2;
              if (off + plen > icc_chunks_size) {
                r = GIMG_ERR_CORRUPT;
                break;
              }
              r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP2,
                  chunks_buf + off, plen, &report->bytes_written);
              off += plen;
              if (r != GIMG_OK) {
                break;
              }
            }
          }
          gimg_free(alloc, chunks_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }
      else if (meta_raw) {
        size_t icc_size = 0;
        if (gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC, NULL,
                &icc_size) == GIMG_OK &&
            icc_size > 0) {
          unsigned char * icc_buf =
              (unsigned char *)gimg_malloc(alloc, icc_size);
          if (icc_buf) {
            r = gimg_meta_raw_get(
                meta_raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC, icc_buf, &icc_size);
            if (r == GIMG_OK) {
              r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP2, icc_buf,
                  icc_size, &report->bytes_written);
            }
            gimg_free(alloc, icc_buf);
          }
          if (r != GIMG_OK) {
            gimg_free(alloc, to_free);
            return r;
          }
        }
      }

      // APP13 (IPTC/Photoshop) and APP14 (Adobe) in read order after APP2.
      size_t app13_size = 0;
      if (meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP13, NULL,
              &app13_size) == GIMG_OK &&
          app13_size > 0) {
        unsigned char * app13_buf =
            (unsigned char *)gimg_malloc(alloc, app13_size);
        if (app13_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_APP13, app13_buf, &app13_size);
          if (r == GIMG_OK) {
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP13,
                app13_buf, app13_size, &report->bytes_written);
          }
          gimg_free(alloc, app13_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }
      size_t app14_size = 0;
      if (meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP14, NULL,
              &app14_size) == GIMG_OK &&
          app14_size > 0) {
        unsigned char * app14_buf =
            (unsigned char *)gimg_malloc(alloc, app14_size);
        if (app14_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_APP14, app14_buf, &app14_size);
          if (r == GIMG_OK) {
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP14,
                app14_buf, app14_size, &report->bytes_written);
          }
          gimg_free(alloc, app14_buf);
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }

      // Unknown APP segments (APP3–APP15 and unhandled APP0/1/2) in read order.
      if (policy == GIMG_META_PRESERVE_ALL ||
          policy == GIMG_META_KEEP_RAW_ONLY) {
        size_t unknown_size = 0;
        if (meta_raw &&
            gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP_UNKNOWN, NULL,
                &unknown_size) == GIMG_OK &&
            unknown_size > 0) {
          unsigned char * unknown_buf =
              (unsigned char *)gimg_malloc(alloc, unknown_size);
          if (unknown_buf) {
            r = gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP_UNKNOWN,
                unknown_buf, &unknown_size);
            if (r == GIMG_OK) {
              size_t off = 0;
              while (off + 3 <= unknown_size) {
                uint8_t app_marker = unknown_buf[off];
                size_t plen = (size_t)((unknown_buf[off + 1] << 8) |
                    unknown_buf[off + 2]);
                off += 3;
                if (off + plen > unknown_size) {
                  break;
                }
                r = jpeg_write_app_segment(stream, app_marker,
                    unknown_buf + off, plen, &report->bytes_written);
                if (r != GIMG_OK) {
                  gimg_free(alloc, unknown_buf);
                  gimg_free(alloc, to_free);
                  return r;
                }
                off += plen;
              }
            }
            gimg_free(alloc, unknown_buf);
          }
        }
        if (r != GIMG_OK) {
          gimg_free(alloc, to_free);
          return r;
        }
      }
    }
  }

  if (use_progressive_body) {
    const GIMG_JPEG_Progressive_Scan * scans =
        gimg_jpeg_default_progressive_scans;
    unsigned scan_count = gimg_jpeg_default_progressive_scan_count;
    if (progressive && options && options->jpeg_progressive_config &&
        options->jpeg_progressive_config->scan_count > 0) {
      scans = options->jpeg_progressive_config->scans;
      scan_count = options->jpeg_progressive_config->scan_count;
    }
    r = jpeg_write_image_body_progressive(stream, width, height, num_components,
        num_components == 3 ? h_samp : NULL,
        num_components == 3 ? v_samp : NULL, quant_luma, quant_chroma,
        coef_buffer, total_blocks, scans, scan_count, precision, alloc,
        restart_interval, &report->bytes_written);
    gimg_free(alloc, coef_buffer);
  }
  else {
    r = jpeg_write_image_body(stream, width, height, num_components,
        num_components == 3 ? h_samp : NULL,
        num_components == 3 ? v_samp : NULL, quant_luma, quant_chroma,
        scan_data, scan_size, precision, restart_interval,
        &report->bytes_written);
    gimg_free(alloc, to_free);
  }
  if (r != GIMG_OK) {
    return r;
  }
  return GIMG_OK;
}
