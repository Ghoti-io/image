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
 * JPEG save: baseline encode (DCT, quantization, zigzag, Huffman); emit SOI,
 * DQT, DHT, SOF0, SOS, EOI. Supports grayscale and YCbCr 4:4:4. Synthetic
 * document support: when doc was not loaded by a codec, pixel data from
 * gimg_item_raster(item) only.
 */

#include <ghoti.io/image/macros.h>
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
#include <ghoti.io/color/icc.h>
#include "../../core/safe_math_internal.h"
#include "../../meta/exif_internal.h"
#include "../../raster/raster_internal.h"
#include "../codec_internal.h"
#include "jpeg_debug_internal.h"
#include "jpeg_huffman_tables_internal.h"
#include "jpeg_internal.h"
#include "jpeg_quant_tables_internal.h"

/** Default quality when not specified (1..100). */
#define GIMG_JPEG_DEFAULT_QUALITY 85u

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
        // Already stuffed by encoder, or RST marker: do not add stuffing.
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
  if (payload_len > GIMG_JPEG_MAX_APP_PAYLOAD) {
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

/**
 * Write the raster's ICC profile as APP2 ICC_PROFILE segment(s).
 *
 * This is the fallback for a document that did not arrive as a JPEG and so
 * has no APP2 in its raw metadata to preserve.  Without it a BMP with a V5
 * embedded profile, or a PNG with iCCP, saved as a JPEG came out untagged:
 * the profile was read only to be dropped, exactly as PNG's iCCP was before
 * the raster became its fallback source.
 *
 * A profile is split across segments because one APP2 holds only 65519 bytes
 * of it.  Each carries "ICC_PROFILE\0", its own 1-based number and the count,
 * which is how a reader knows to reassemble them (ICC.1:2010 Annex B.4).  The
 * count lives in one byte, so 255 segments is the format's ceiling; a profile
 * past it cannot be written as a JPEG at all.
 *
 * When the raster carries no profile but does state a color model, one is
 * built to say what it states.  This is the only place the library writes a
 * color statement the source did not carry, and it is here because APP2 is
 * the only place a JPEG has to put one: a raster that knew its primaries and
 * its gamma had both silently dropped on the way into a format with no gAMA
 * and no cHRM.  It is done only for a three-component frame, the one an RGB
 * matrix profile describes; a gray or CMYK frame would need a different kind
 * of profile and gets none.
 *
 * @param stream Destination.
 * @param info The raster's color info; nothing is written when it has neither
 *   a profile nor a color model complete enough to state.
 * @param num_components Components in the frame being written.
 * @param alloc Allocator for the one segment-sized staging buffer.
 * @param report Byte count is added to.
 * @return GIMG_OK, or a write error.  A profile too large for the format is
 *   skipped rather than fatal: the picture is not wrong because its color
 *   annotation could not be carried.
 */
static GIMG_Result jpeg_write_icc_from_info(GIMG_Stream * stream,
    const GCOL_Color_Info * info, int num_components,
    const GIMG_Allocator * alloc, GIMG_Save_Report * report) {
  if (!info) {
    return GIMG_OK;
  }
  const unsigned char * src = (const unsigned char *)info->icc_bytes;
  size_t icc_size = info->icc_size;
  void * synthesized = NULL;
  if (!src || icc_size == 0) {
    if (num_components != 3) {
      return GIMG_OK;
    }
    GCOL_Result sr = gcol_icc_write(alloc, info, &synthesized, &icc_size);
    if (sr != GCOL_OK) {
      return sr == GCOL_ERR_OOM ? GIMG_ERR_OOM : GIMG_ERR_INTERNAL;
    }
    if (!synthesized) {
      return GIMG_OK;
    }
    src = (const unsigned char *)synthesized;
  }
  size_t per_chunk = GIMG_JPEG_MAX_APP_PAYLOAD - GIMG_JPEG_ICC_PREFIX_LEN;
  size_t chunks = (icc_size + per_chunk - 1) / per_chunk;
  if (chunks > GIMG_JPEG_MAX_ICC_CHUNKS) {
    gimg_free(alloc, synthesized);
    return GIMG_OK;
  }

  unsigned char * seg = (unsigned char *)gimg_malloc(
      alloc, GIMG_JPEG_ICC_PREFIX_LEN + per_chunk);
  if (!seg) {
    gimg_free(alloc, synthesized);
    return GIMG_ERR_OOM;
  }
  memcpy(seg, "ICC_PROFILE\0", 12);
  seg[13] = (unsigned char)chunks;

  GIMG_Result r = GIMG_OK;
  size_t left = icc_size;
  for (size_t i = 0; i < chunks; i++) {
    size_t take = left < per_chunk ? left : per_chunk;
    seg[12] = (unsigned char)(i + 1);
    memcpy(seg + GIMG_JPEG_ICC_PREFIX_LEN, src, take);
    r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP2, seg,
        GIMG_JPEG_ICC_PREFIX_LEN + take, &report->bytes_written);
    if (r != GIMG_OK) {
      break;
    }
    src += take;
    left -= take;
  }
  gimg_free(alloc, seg);
  gimg_free(alloc, synthesized);
  return r;
}

/** Build minimal APP0 JFIF (14 bytes), per JFIF 1.01. Lh=16 so segment is
 * 2+16 bytes. If x_dpi and y_dpi are both 0, use units=0 and density 1,1;
 * else units=1 (dots per inch). No thumbnail (Xthumbnail=0, Ythumbnail=0). */
/**
 * Put the document's pixel aspect ratio into a JFIF APP0's density fields.
 *
 * JFIF 1.02 units 0 means the two density fields are the pixel's aspect ratio
 * and not a resolution, which is what PNG's pHYs unit 0 and GIF's Pixel Aspect
 * Ratio byte also say; all three reach a caller through
 * gimg_doc_pixel_aspect_ratio(), so all three are written from it.
 *
 * This runs over a *preserved* APP0 as well as a freshly built one, because
 * otherwise gimg_doc_set_pixel_aspect_ratio() on a loaded JPEG would be a
 * no-op that reports success: the accessor would give the new value and the
 * file would keep the old one.  A physical resolution wins over a ratio - a
 * size says more - so a document with DPI is left alone here.
 *
 * The density fields are sixteen bits each.  A ratio whose terms do not fit is
 * not written at all rather than rounded to something that does, for the same
 * reason GIF writes a zero for a ratio outside its range: a number the caller
 * did not ask for is worse than no number.
 */
static void jpeg_apply_aspect_to_app0(unsigned char * buf, size_t len,
    const GIMG_Doc * doc, uint32_t x_dpi, uint32_t y_dpi) {
  if (!buf || len < 14u || memcmp(buf, "JFIF\0", 5) != 0) {
    return;
  }
  if (x_dpi > 0u && y_dpi > 0u) {
    return;
  }
  uint32_t par_x = 0u, par_y = 0u;
  const bool declared =
      gimg_doc_pixel_aspect_ratio(doc, &par_x, &par_y) != 0 && par_x > 0u &&
      par_y > 0u;
  if (declared && par_x <= 0xFFFFu && par_y <= 0xFFFFu) {
    buf[7] = 0u;
    buf[8] = (unsigned char)(par_x >> 8);
    buf[9] = (unsigned char)(par_x & 0xFFu);
    buf[10] = (unsigned char)(par_y >> 8);
    buf[11] = (unsigned char)(par_y & 0xFFu);
    return;
  }
  // Nothing declared: a unit 0 segment claiming a non-square pixel is saying
  // something the document no longer says, so it goes back to the 1:1 that
  // means "square, and no size given".
  if (buf[7] == 0u) {
    buf[8] = 0u;
    buf[9] = 1u;
    buf[10] = 0u;
    buf[11] = 1u;
  }
}

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

/**
 * RGB to YCbCr (ITU-R BT.601, the transform JFIF specifies), at the frame's own
 * sample precision.
 *
 *   Y  =  0.29900*R + 0.58700*G + 0.11400*B
 *   Cb = -0.16874*R - 0.33126*G + 0.50000*B + center
 *   Cr =  0.50000*R - 0.41869*G - 0.08131*B + center
 *
 * Scaled-integer form with SCALEBITS = 16, as libjpeg's jccolor.c does it:
 * FIX(x) = round(x * 65536), ONE_HALF = 1 << 15 for rounding, and the chroma
 * terms carry an extra (center << 16) - 1.
 *
 * @param center 2^(P-1): 128 at P=8, 2048 at P=12 (T.81 Table B.2 allows both).
 * @param max_val 2^P - 1.
 *
 * The 12-bit case used to have a transform of its own, with coefficients scaled
 * for 8-bit data (77, 150, 29 - they sum to 256) but a shift of 12 rather than
 * 8, so every 12-bit luminance sample we encoded came out sixteen times too
 * small.  Nothing caught it: no external decoder could open a 12-bit file, and
 * our own decoder read the file back exactly as libjpeg would - both of them
 * faithfully reproducing an image that had been wrong before it was written.
 * The widest intermediate here is (19595 + 38470 + 7471) * 4095, which is
 * 65536 * 4095, comfortably inside int32.
 */
static void jpeg_rgb_to_ycbcr_at(int32_t r, int32_t g, int32_t b,
    int32_t center, int32_t max_val, int32_t * y, int32_t * cb, int32_t * cr) {
  if (r < 0)
    r = 0;
  if (r > max_val)
    r = max_val;
  if (g < 0)
    g = 0;
  if (g > max_val)
    g = max_val;
  if (b < 0)
    b = 0;
  if (b > max_val)
    b = max_val;
  const int32_t one_half = 1 << 15;
  const int32_t cbcr_bias = (center << 16) + one_half - 1;
  int32_t yv = (19595 * r + 38470 * g + 7471 * b + one_half) >> 16;
  int32_t cbv = (-11059 * r - 21709 * g + 32768 * b + cbcr_bias) >> 16;
  int32_t crv = (32768 * r - 27439 * g - 5331 * b + cbcr_bias) >> 16;
  if (yv < 0)
    yv = 0;
  if (yv > max_val)
    yv = max_val;
  if (cbv < 0)
    cbv = 0;
  if (cbv > max_val)
    cbv = max_val;
  if (crv < 0)
    crv = 0;
  if (crv > max_val)
    crv = max_val;
  *y = yv;
  *cb = cbv;
  *cr = crv;
}

/** RGB to YCbCr at 8 bits. See jpeg_rgb_to_ycbcr_at. */
void jpeg_rgb_to_ycbcr(
    uint8_t r, uint8_t g, uint8_t b, uint8_t * y, uint8_t * cb, uint8_t * cr) {
  int32_t yv, cbv, crv;
  jpeg_rgb_to_ycbcr_at(
      (int32_t)r, (int32_t)g, (int32_t)b, 128, 255, &yv, &cbv, &crv);
  *y = (uint8_t)yv;
  *cb = (uint8_t)cbv;
  *cr = (uint8_t)crv;
}

/** Chroma subsampling: 0 = 4:2:0, 1 = 4:2:2, 2 = 4:4:4. */
#define CHROMA_420 0
#define CHROMA_422 1
#define CHROMA_444 2

/** RGB to YCbCr at 12 bits. See jpeg_rgb_to_ycbcr_at. */
static void jpeg_rgb12_to_ycbcr12(uint16_t r, uint16_t g, uint16_t b,
    uint16_t * y, uint16_t * cb, uint16_t * cr) {
  int32_t yv, cbv, crv;
  jpeg_rgb_to_ycbcr_at(
      (int32_t)r, (int32_t)g, (int32_t)b, 2048, 4095, &yv, &cbv, &crv);
  *y = (uint16_t)yv;
  *cb = (uint16_t)cbv;
  *cr = (uint16_t)crv;
}

/**
 * Hand back the planes a twelve-bit frame took for its fourth and later
 * components.
 *
 * Every cleanup arm below has to call this, not only the one at the end of a
 * successful walk: the arms that free comp_y, comp_cb and comp_cr by name
 * were written when the path stopped at three components, and a wider frame
 * leaked one plane per component past the third whenever it failed after
 * taking them.
 */
static void jpeg_extra_planes_free(const GIMG_Allocator * alloc,
    uint16_t * extra[GIMG_JPEG_MAX_COMPONENTS], int num_components) {
  for (int c = 3; c < num_components; c++) {
    gimg_free(alloc, extra[c]);
    extra[c] = NULL;
  }
}

/** 12-bit path: GRAY12/RGBA12 (0..4095). Progressive: fill coef and return;
 * baseline: fill coef then encode one scan (Ss=0,Se=63) via extended tables. */
static GIMG_Result jpeg_raster_to_scan_data_12bit(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, unsigned quality, unsigned chroma_subsampling,
    bool progressive, bool arithmetic, const jpeg_arith_cond_t * cond,
    unsigned fdct_method, uint16_t restart_interval,
    unsigned char ** out_scan_data,
    size_t * out_scan_size, int16_t ** out_coef_buffer, size_t * out_total_blocks,
    uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES], uint32_t * out_width,
    uint32_t * out_height, int * out_num_components,
    uint8_t out_h_samp[GIMG_JPEG_MAX_COMPONENTS],
    uint8_t out_v_samp[GIMG_JPEG_MAX_COMPONENTS],
    uint8_t out_tbl_sel[GIMG_JPEG_MAX_COMPONENTS]) {
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  // T.81 B.2.2 counts components from 1 to 255 and Table B.2 allows P = 12 in
  // a DCT frame; the two are independent, so four components and more are as
  // legal here as at eight bits.  Only the three-component case is color, and
  // only it converts.
  const int is_color = (fmt->channel_model == GIMG_CHANNEL_RGB ||
      fmt->channel_model == GIMG_CHANNEL_RGBA);
  int num_components;
  if (fmt->channel_model == GIMG_CHANNEL_GRAY) {
    num_components = 1;
  }
  else if (is_color) {
    num_components = 3;
  }
  else {
    num_components = (int)fmt->channel_count;
  }
  if (num_components < 1 || num_components > (int)GIMG_JPEG_MAX_COMPONENTS) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (num_components > (int)GIMG_JPEG_MAX_SCAN_COMPONENTS && !progressive) {
    // B.2.3 caps Ns at 4, so such a frame has to be split; the twelve-bit path
    // writes one interleaved scan, so it cannot carry one.
    return GIMG_ERR_UNSUPPORTED;
  }
  if (out_tbl_sel) {
    for (int c = 0; c < num_components; c++) {
      // No component of a frame with no color convention is chrominance, so
      // none of them wants the chrominance tables (see the eight-bit path).
      out_tbl_sel[c] =
          is_color ? (uint8_t)(c == 0 ? 0 : 1) : (uint8_t)0;
    }
  }
  size_t comp_size = 0;
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &comp_size)) {
    return GIMG_ERR_LIMIT;
  }
  uint16_t * comp_y =
      (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
  if (!comp_y) {
    return GIMG_ERR_OOM;
  }
  uint16_t * comp_cb = NULL;
  uint16_t * comp_cr = NULL;
  // Beyond the third, a component has no name here and no color step; it is
  // held in extra[] and handed to the coefficient walk beside the others.
  uint16_t * extra[GIMG_JPEG_MAX_COMPONENTS];
  memset(extra, 0, sizeof(extra));
  // One plane per component, and the count is the component count.  A
  // two-component frame is legal (T.81 B.2.2 gives it no meaning, and the
  // decoder reads one) and needs comp_cb; asking for three here meant it did
  // not get one, and the fill below wrote every second sample through NULL.
  if (num_components >= 2) {
    comp_cb = (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
  }
  if (num_components >= 3) {
    comp_cr = (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
  }
  if ((num_components >= 2 && !comp_cb) || (num_components >= 3 && !comp_cr)) {
    // Both pointers, not just the one that is obviously live: either
    // allocation can be the one that failed, and the arm used to hand back
    // comp_cb only - so a failure of comp_cb with comp_cr succeeding leaked
    // a whole component plane.  gimg_free ignores NULL, which is what the
    // loop below this already relies on.
    gimg_free(alloc, comp_y);
    gimg_free(alloc, comp_cb);
    gimg_free(alloc, comp_cr);
    return GIMG_ERR_OOM;
  }
  for (int c = 3; c < num_components; c++) {
    extra[c] = (uint16_t *)gimg_malloc(alloc, comp_size * sizeof(uint16_t));
    if (!extra[c]) {
      gimg_free(alloc, comp_y);
      gimg_free(alloc, comp_cb);
      gimg_free(alloc, comp_cr);
      for (int k = 3; k < c; k++) {
        gimg_free(alloc, extra[k]);
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
    jpeg_extra_planes_free(alloc, extra, num_components);
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
  else if (is_color) {
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
  else {
    // CMYK, or channels with no color convention: the samples go out as they
    // came in, as they do at eight bits.
    for (uint32_t y = 0; y < height; y++) {
      const uint16_t * row = (const uint16_t *)(pixels + y * stride_bytes);
      for (uint32_t x = 0; x < width; x++) {
        for (int c = 0; c < num_components; c++) {
          uint16_t v = row[x * (size_t)num_components + (size_t)c];
          if (v > 4095u) {
            v = 4095u;
          }
          uint16_t * dst = (c == 0) ? comp_y
              : (c == 1)            ? comp_cb
              : (c == 2)            ? comp_cr
                                    : extra[c];
          dst[y * (size_t)width + x] = v;
        }
      }
    }
  }
  uint8_t h_samp[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t v_samp[GIMG_JPEG_MAX_COMPONENTS];
  for (int c = 0; c < (int)GIMG_JPEG_MAX_COMPONENTS; c++) {
    h_samp[c] = 1;
    v_samp[c] = 1;
  }
  size_t stride0 = (size_t)width;
  size_t stride1 = (size_t)width;
  size_t stride2 = (size_t)width;
  uint16_t * use_cb = comp_cb;
  uint16_t * use_cr = comp_cr;
  if (is_color && num_components == 3 &&
      chroma_subsampling != CHROMA_444) {
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
      if (!gcu_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      chroma_size *= sizeof(uint16_t);
      use_cb = (uint16_t *)gimg_malloc(alloc, chroma_size);
      use_cr = (uint16_t *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        // Both, for the reason given where comp_cb and comp_cr are taken: the
        // arm runs when either allocation failed, and handing back only the
        // first leaked a whole subsampled plane whenever it was the one that
        // failed.
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        gimg_free(alloc, use_cb);
        gimg_free(alloc, use_cr);
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
      if (!gcu_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        return GIMG_ERR_LIMIT;
      }
      chroma_size *= sizeof(uint16_t);
      use_cb = (uint16_t *)gimg_malloc(alloc, chroma_size);
      use_cr = (uint16_t *)gimg_malloc(alloc, chroma_size);
      if (!use_cb || !use_cr) {
        // Both, for the reason given where comp_cb and comp_cr are taken: the
        // arm runs when either allocation failed, and handing back only the
        // first leaked a whole subsampled plane whenever it was the one that
        // failed.
        gimg_free(alloc, comp_y);
        gimg_free(alloc, comp_cb);
        gimg_free(alloc, comp_cr);
        gimg_free(alloc, use_cb);
        gimg_free(alloc, use_cr);
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
  uint8_t h_max, v_max;
  gimg_jpeg_sampling_max(num_components, h_samp, v_samp, &h_max, &v_max);
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  size_t total_blocks = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gcu_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
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
    jpeg_extra_planes_free(alloc, extra, num_components);
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
    jpeg_extra_planes_free(alloc, extra, num_components);
    return GIMG_ERR_OOM;
  }
  size_t out_blocks = 0;
  const uint16_t * comps12[GIMG_JPEG_MAX_COMPONENTS];
  size_t strides12[GIMG_JPEG_MAX_COMPONENTS];
  for (int c = 0; c < (int)GIMG_JPEG_MAX_COMPONENTS; c++) {
    comps12[c] = (c == 0) ? comp_y
        : (c == 1)        ? use_cb
        : (c == 2)        ? use_cr
                          : extra[c];
    strides12[c] = (c == 0) ? stride0
        : (c == 1)          ? stride1
        : (c == 2)          ? stride2
                            : (size_t)width;
  }
  GIMG_Result r = gimg_jpeg_progressive_fill_coef_buffer_12bit(width, height,
      num_components, comps12, strides12, h_ptr, v_ptr, out_tbl_sel,
      quant_luma, quant_chroma, fdct_method, coef_buf, &out_blocks);
  jpeg_extra_planes_free(alloc, extra, num_components);
  gimg_free(alloc, comp_y);
  if (use_cb != comp_cb) {
    gimg_free(alloc, use_cb);
  }
  if (use_cr != comp_cr) {
    gimg_free(alloc, use_cr);
  }
  // These are allocated for any frame of three or more components, not only
  // for a three-component one; freeing them only at three leaked a plane pair
  // for every wider frame.
  gimg_free(alloc, comp_cb);
  gimg_free(alloc, comp_cr);
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
  if (arithmetic) {
    // T.81 Annex D.  The coder is the same at either precision: it codes binary
    // decisions about coefficient magnitudes, and a wider coefficient simply
    // makes the magnitude chain longer.
    r = gimg_jpeg_encode_arith_scan_from_coef_buffer(width, height,
        num_components, coef_buf, out_blocks, h_samp, v_samp, out_tbl_sel,
        cond, alloc, restart_interval, 0, &scan_data, &scan_size);
  }
  else {
    // T.81 Annex F: baseline sequential uses DC table for DC then AC table for
    // AC 1..63 per block.
    r = gimg_jpeg_encode_baseline_scan_from_coef_buffer_extended(width, height,
        num_components, coef_buf, out_blocks, h_samp, v_samp, out_tbl_sel,
        alloc, restart_interval, &scan_data, &scan_size);
  }
  gimg_free(alloc, coef_buf);
  if (r != GIMG_OK || (!scan_data && !(arithmetic && scan_size == 0))) {
    return (r != GIMG_OK) ? r : GIMG_ERR_OOM;
  }
  *out_scan_data = scan_data;
  *out_scan_size = scan_size;
  *out_width = width;
  *out_height = height;
  *out_num_components = num_components;
  return GIMG_OK;
}

/**
 * Free the planes a frame's components were built into.
 *
 * Every plane is owned by jpeg_raster_to_scan_data - a subsampled chrominance
 * plane replaces the full-size one rather than living beside it - so one loop
 * releases the lot, whatever Nf is.
 */
static void jpeg_planes_free(
    const GIMG_Allocator * alloc, unsigned char ** plane, int count) {
  for (int c = 0; c < count; c++) {
    gimg_free(alloc, plane[c]);
    plane[c] = NULL;
  }
}

/**
 * Encode a raster to scan data (sequential) or to a coefficient buffer.
 *
 * When !progressive: allocates *out_scan_data; caller frees.  When progressive:
 * allocates *out_coef_buffer (out_total_blocks * 64 int16_t); caller frees.
 * Accepts GRAY8, RGB/RGBA 8-bit, GRAY12, RGB 12-bit and CMYK8.
 * *out_precision is set to 8 or 12.
 *
 * The components are held in one array of planes rather than in comp_y /
 * comp_cb / comp_cr, because T.81 B.2.2 counts components from 1 to 255 and
 * nothing below this line cares which color they carry: the DCT, the
 * quantizer and the entropy coder see a list of planes and a list of sampling
 * factors.  Naming three of them after YCbCr was what kept a four-component
 * frame out.
 *
 * @param cmyk_transform  For a four-component raster, the Adobe APP14 transform
 *                        to write: 0 for CMYK (the components go out as they
 *                        came in) or 2 for YCCK (T.81 has no opinion; see
 *                        jpeg_cmyk_to_ycck).
 * @param out_adobe_transform  Out: the transform actually used, or -1 when the
 *                        frame needs no Adobe marker.
 */
static GIMG_Result jpeg_raster_to_scan_data(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, unsigned quality, unsigned chroma_subsampling,
    bool progressive, bool arithmetic, const jpeg_arith_cond_t * cond,
    uint16_t restart_interval,
    unsigned fdct_method, unsigned quant_method, unsigned cmyk_transform,
    unsigned char ** out_scan_data,
    size_t * out_scan_size, int16_t ** out_coef_buffer,
    size_t * out_total_blocks, uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES], uint32_t * out_width,
    uint32_t * out_height, int * out_num_components,
    uint8_t out_h_samp[GIMG_JPEG_MAX_COMPONENTS],
    uint8_t out_v_samp[GIMG_JPEG_MAX_COMPONENTS],
    uint8_t out_tbl_sel[GIMG_JPEG_MAX_COMPONENTS], int * out_precision,
    int * out_adobe_transform) {
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
  if (out_adobe_transform) {
    *out_adobe_transform = -1;
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
  else if (fmt->channel_model == GIMG_CHANNEL_CMYK &&
      fmt->channel_count == 4 && fmt->bits_per_channel[0] == 8 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    num_components = 4;
  }
  else if (fmt->channel_model == GIMG_CHANNEL_UNKNOWN &&
      fmt->channel_count >= 1 && fmt->bits_per_channel[0] == 8 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    // Channels with no color meaning: T.81 B.2.2 allows Nf from 1 to 255 and
    // says nothing about what the components are, so they are written as they
    // came in.  This is libjpeg's JCS_UNKNOWN.
    num_components = (int)fmt->channel_count;
  }
  else if (fmt->channel_model == GIMG_CHANNEL_GRAY && fmt->channel_count == 1 &&
      fmt->bits_per_channel[0] == 16) {
    // A 16-bit raster is converted to 12-bit before this point (T.81 has no
    // 16-bit DCT frame).  Reaching here means that conversion did not happen.
    return GIMG_ERR_UNSUPPORTED;
  }
  else if ((fmt->channel_model == GIMG_CHANNEL_RGB ||
               fmt->channel_model == GIMG_CHANNEL_RGBA) &&
      fmt->channel_count >= 3 && fmt->bits_per_channel[0] == 16 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    return GIMG_ERR_UNSUPPORTED; // See above: converted to 12-bit earlier.
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
  else if ((fmt->channel_model == GIMG_CHANNEL_CMYK ||
               fmt->channel_model == GIMG_CHANNEL_UNKNOWN) &&
      fmt->channel_count >= 1 && fmt->bits_per_channel[0] == 12 &&
      fmt->layout == GIMG_LAYOUT_INTERLEAVED) {
    // T.81 Table B.2 allows P = 12 in a DCT frame and B.2.2 allows Nf from 1
    // to 255; the two are independent, so this is as legal as the eight-bit
    // form and is what a twelve-bit CMYK file loads and saves back through.
    num_components = (int)fmt->channel_count;
    precision = 12;
  }
  else {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (precision == 12) {
    *out_precision = 12;
    // A four-component frame needs its Adobe marker at either precision; see
    // the note below.
    if (out_adobe_transform && num_components == 4 &&
        fmt->channel_model == GIMG_CHANNEL_CMYK) {
      *out_adobe_transform = (cmyk_transform == 2u) ? 2 : 0;
    }
    else if (out_adobe_transform && num_components == 3 &&
        fmt->channel_model == GIMG_CHANNEL_UNKNOWN) {
      *out_adobe_transform = 0; // see below: three components must say so
    }
    return jpeg_raster_to_scan_data_12bit(alloc, raster, quality,
        chroma_subsampling, progressive, arithmetic, cond, fdct_method,
        restart_interval, out_scan_data,
        out_scan_size, out_coef_buffer, out_total_blocks, quant_luma,
        quant_chroma, out_width, out_height, out_num_components, out_h_samp,
        out_v_samp, out_tbl_sel);
  }
  // A four-component frame is CMYK unless the caller asked for the YCCK
  // transform; either way it needs an Adobe APP14 marker, because that marker
  // is the only thing in the file that says which (see jdapimin.c, and the
  // color section of documentation/formats/jpeg.md).
  int adobe_transform = -1;
  if (num_components == 4 && fmt->channel_model == GIMG_CHANNEL_CMYK) {
    adobe_transform = (cmyk_transform == 2u) ? 2 : 0;
  }
  else if (num_components == 3 &&
      fmt->channel_model == GIMG_CHANNEL_UNKNOWN) {
    // Three components with no color meaning still go out unchanged, and
    // three components is the one count where a decoder will otherwise guess:
    // RGB and YCbCr are both three, and without a marker the convention is
    // YCbCr (jdapimin.c, and jpeg_frame_is_rgb here).  An Adobe APP14 with
    // transform 0 says "not YCbCr", which is how the lossless writer marks the
    // same thing, and without it such a frame does not survive a round trip.
    adobe_transform = 0;
  }

  // Planes, all owned by this function and all freed through jpeg_planes_free.
  unsigned char * plane[GIMG_JPEG_MAX_COMPONENTS];
  size_t plane_stride[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t h_samp[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t v_samp[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t tbl_sel[GIMG_JPEG_MAX_COMPONENTS];
  for (int c = 0; c < (int)GIMG_JPEG_MAX_COMPONENTS; c++) {
    plane[c] = NULL;
    plane_stride[c] = 0;
    h_samp[c] = 1;
    v_samp[c] = 1;
    tbl_sel[c] = (uint8_t)(c == 0 ? 0 : 1);
  }
  // T.81 B.2.2/B.2.3 leave Tq, Td and Ta free per component; these are
  // libjpeg's choices (jcparam.c jpeg_set_colorspace), and they are what makes
  // a CMYK frame use one table set throughout while YCCK keeps the chroma set
  // for its two chrominance components only.
  if (fmt->channel_model == GIMG_CHANNEL_UNKNOWN) {
    // No component of such a frame is chrominance, so none of them wants the
    // chrominance tables; libjpeg gives every JCS_UNKNOWN component table 0.
    for (int c = 0; c < num_components; c++) {
      tbl_sel[c] = 0;
    }
  }
  else if (num_components == 4) {
    if (adobe_transform == 2) {
      tbl_sel[0] = 0;
      tbl_sel[1] = 1;
      tbl_sel[2] = 1;
      tbl_sel[3] = 0;
    }
    else {
      tbl_sel[0] = tbl_sel[1] = tbl_sel[2] = tbl_sel[3] = 0;
    }
  }

  size_t comp_size = 0;
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &comp_size)) {
    return GIMG_ERR_LIMIT;
  }
  for (int c = 0; c < num_components; c++) {
    plane[c] = (unsigned char *)gimg_malloc(alloc, comp_size);
    if (!plane[c]) {
      jpeg_planes_free(alloc, plane, num_components);
      return GIMG_ERR_OOM;
    }
    plane_stride[c] = (size_t)width;
  }

  size_t stride_bytes = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  if (!pixels) {
    jpeg_planes_free(alloc, plane, num_components);
    return GIMG_ERR_UNSUPPORTED;
  }
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  if (num_components == 1) {
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        plane[0][y * (size_t)width + x] = row[x * bpp];
      }
    }
  }
  else if (num_components == 3) {
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        uint8_t r = row[x * bpp + 0];
        uint8_t g = row[x * bpp + 1];
        uint8_t b = row[x * bpp + 2];
        uint8_t yv, cb, cr;
        jpeg_rgb_to_ycbcr(r, g, b, &yv, &cb, &cr);
        plane[0][y * (size_t)width + x] = yv;
        plane[1][y * (size_t)width + x] = cb;
        plane[2][y * (size_t)width + x] = cr;
      }
    }
  }
  else if (fmt->channel_model == GIMG_CHANNEL_UNKNOWN) {
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        for (int c = 0; c < num_components; c++) {
          plane[c][y * (size_t)width + x] =
              row[x * bpp + (size_t)c];
        }
      }
    }
  }
  else {
    // Four components.  For transform 0 the samples go out exactly as they came
    // in - that is what the decoder's raw-CMYK path (jdcolor.c null_convert)
    // reads back, so a load/save round trip is the identity.  For transform 2
    // the first three become YCbCr of the complement, which is what
    // jpeg_emit_four_component undoes on the way in.
    for (uint32_t y = 0; y < height; y++) {
      const unsigned char * row = pixels + y * stride_bytes;
      for (uint32_t x = 0; x < width; x++) {
        uint8_t cc = row[x * bpp + 0];
        uint8_t mm = row[x * bpp + 1];
        uint8_t yy = row[x * bpp + 2];
        uint8_t kk = row[x * bpp + 3];
        if (adobe_transform == 2) {
          uint8_t yv, cb, cr;
          jpeg_rgb_to_ycbcr((uint8_t)(255 - cc), (uint8_t)(255 - mm),
              (uint8_t)(255 - yy), &yv, &cb, &cr);
          plane[0][y * (size_t)width + x] = yv;
          plane[1][y * (size_t)width + x] = cb;
          plane[2][y * (size_t)width + x] = cr;
        }
        else {
          plane[0][y * (size_t)width + x] = cc;
          plane[1][y * (size_t)width + x] = mm;
          plane[2][y * (size_t)width + x] = yy;
        }
        plane[3][y * (size_t)width + x] = kk;
      }
    }
  }

  // Chroma subsampling applies to components 1 and 2 only, and only when the
  // frame actually has chrominance there: three-component YCbCr, or the YCCK
  // form of a four-component frame.  A raw CMYK frame has no chrominance and
  // libjpeg does not subsample one either (jcparam.c gives all four 1x1).
  int has_chroma = (fmt->channel_model != GIMG_CHANNEL_UNKNOWN) &&
      ((num_components == 3) || (adobe_transform == 2));
  if (has_chroma && chroma_subsampling != CHROMA_444) {
    uint32_t cw = 0, ch = 0;
    if (chroma_subsampling == CHROMA_420) {
      // T.81 Annex A: expand chroma to fill integral DCT blocks
      // (output_cols = width_in_blocks*8).
      uint32_t mcu_per_row = (width + 15u) / 16u;
      cw = 8u * mcu_per_row;
      ch = (height + 1u) / 2u;
      if (ch == 0u) {
        ch = 1u;
      }
      h_samp[0] = 2;
      v_samp[0] = 2;
    }
    else {
      // 4:2:2: 2x1 horizontal, 8x1 vertical (one Cb 8x8 block per 16x8 MCU).
      uint32_t mcu_per_row = (width + 15u) / 16u;
      uint32_t mcu_per_col = (height + 7u) / 8u;
      cw = 8u * mcu_per_row;
      ch = 8u * mcu_per_col;
      h_samp[0] = 2;
      v_samp[0] = 1;
    }
    h_samp[1] = 1;
    h_samp[2] = 1;
    v_samp[1] = 1;
    v_samp[2] = 1;
    if (num_components == 4) {
      // T.81 A.1.1: K is not chrominance, so it keeps the luminance grid; this
      // is libjpeg's YCCK layout as well.
      h_samp[3] = h_samp[0];
      v_samp[3] = v_samp[0];
    }
    size_t chroma_size = 0;
    if (!gcu_safe_mul_size((size_t)cw, (size_t)ch, &chroma_size)) {
      jpeg_planes_free(alloc, plane, num_components);
      return GIMG_ERR_LIMIT;
    }
    unsigned char * sub[2] = {NULL, NULL};
    sub[0] = (unsigned char *)gimg_malloc(alloc, chroma_size);
    sub[1] = (unsigned char *)gimg_malloc(alloc, chroma_size);
    if (!sub[0] || !sub[1]) {
      gimg_free(alloc, sub[0]);
      gimg_free(alloc, sub[1]);
      jpeg_planes_free(alloc, plane, num_components);
      return GIMG_ERR_OOM;
    }
    if (chroma_subsampling == CHROMA_420) {
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
          // Ordered-dither rounding for 2x2 box: bias 1,2,1,2 per column
          // (T.81 does not specify the filter).
          unsigned bias = 1u + (cb_x % 2u);
          for (int k = 0; k < 2; k++) {
            const unsigned char * src = plane[1 + k];
            unsigned sum = (unsigned)src[y_lo * (size_t)width + x_lo] +
                (unsigned)src[y_lo * (size_t)width + x1] +
                (unsigned)src[y1 * (size_t)width + x_lo] +
                (unsigned)src[y1 * (size_t)width + x1];
            sub[k][cb_y * (size_t)cw + cb_x] = (unsigned char)((sum + bias) / 4);
          }
        }
      }
    }
    else {
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
          for (int k = 0; k < 2; k++) {
            const unsigned char * src = plane[1 + k];
            unsigned sum = (unsigned)src[row_off + x_lo] +
                (unsigned)src[row_off + x1];
            sub[k][cb_y * (size_t)cw + cb_x] = (unsigned char)((sum + 1) / 2);
          }
        }
      }
    }
    for (int k = 0; k < 2; k++) {
      gimg_free(alloc, plane[1 + k]);
      plane[1 + k] = sub[k];
      plane_stride[1 + k] = (size_t)cw;
    }
  }

  if (out_h_samp && out_v_samp) {
    for (int c = 0; c < num_components; c++) {
      out_h_samp[c] = h_samp[c];
      out_v_samp[c] = v_samp[c];
    }
  }
  if (out_tbl_sel) {
    for (int c = 0; c < num_components; c++) {
      out_tbl_sel[c] = tbl_sel[c];
    }
  }
  if (out_adobe_transform) {
    *out_adobe_transform = adobe_transform;
  }

  if (quality > 100) {
    quality = 100;
  }
  gimg_jpeg_default_quant_scaled(quality, quant_luma, quant_chroma);
  int any_subsampled = 0;
  for (int c = 0; c < num_components; c++) {
    if (h_samp[c] != 1 || v_samp[c] != 1) {
      any_subsampled = 1;
    }
  }
  const uint8_t * h_ptr = any_subsampled ? h_samp : NULL;
  const uint8_t * v_ptr = any_subsampled ? v_samp : NULL;
  const uint8_t * tbl_ptr = tbl_sel;
  GIMG_Result r;
  uint8_t h_max, v_max;
  gimg_jpeg_sampling_max(num_components, h_samp, v_samp, &h_max, &v_max);
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t blocks_per_mcu = 0;
  for (int c = 0; c < num_components; c++) {
    blocks_per_mcu += (size_t)h_samp[c] * (size_t)v_samp[c];
  }
  size_t total_blocks = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &total_blocks) ||
      !gcu_safe_mul_size(total_blocks, blocks_per_mcu, &total_blocks)) {
    jpeg_planes_free(alloc, plane, num_components);
    return GIMG_ERR_LIMIT;
  }
  int16_t * coef_buf =
      (int16_t *)gimg_malloc(alloc, total_blocks * 64 * sizeof(int16_t));
  if (!coef_buf) {
    jpeg_planes_free(alloc, plane, num_components);
    return GIMG_ERR_OOM;
  }
  // The sequential path uses the same coefficient buffer as the progressive
  // one, so that the two decode to identical pixels and only the scan order
  // differs.
  {
    const unsigned char * comps[GIMG_JPEG_MAX_COMPONENTS];
    for (int c = 0; c < (int)GIMG_JPEG_MAX_COMPONENTS; c++) {
      comps[c] = plane[c];
    }
    size_t filled = total_blocks;
    r = gimg_jpeg_progressive_fill_coef_buffer(width, height, num_components,
        comps, plane_stride, h_ptr, v_ptr, tbl_ptr, quant_luma, quant_chroma,
        fdct_method, quant_method, coef_buf, &filled);
    jpeg_planes_free(alloc, plane, num_components);
    if (r != GIMG_OK) {
      gimg_free(alloc, coef_buf);
      return r;
    }
    total_blocks = filled;
  }
#if GIMG_JPEG_DUMP_FIRST_MCU_COEF
    {
      const char * mcu_coef_dir =
          getenv("GIMG_JPEG_DUMP_FIRST_MCU_COEF");
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
#endif
#if GIMG_JPEG_DUMP_FIRST_BLOCK_COEF
    {
      const char * first_block_path =
          getenv("GIMG_JPEG_DUMP_FIRST_BLOCK_COEF");
      if (first_block_path && first_block_path[0] != '\0' && total_blocks > 0) {
        FILE * f = fopen(first_block_path, "wb");
        if (f) {
          (void)fwrite(coef_buf, sizeof(int16_t), 64, f);
          (void)fclose(f);
        }
      }
    }
#endif
#if GIMG_JPEG_DUMP_FIRST_N_BLOCKS_COEF
    // Dump first N coefficient blocks (block_000.bin .. block_(N-1).bin) for comparison with
    // reference encoder. T.81 Annex A block order; 64 int16_t per file, zigzag order.
    {
      const char * n_blocks_dir =
          getenv("GIMG_JPEG_DUMP_FIRST_N_BLOCKS_COEF");
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
#endif
  if (progressive) {
    *out_coef_buffer = coef_buf;
    *out_total_blocks = total_blocks;
    *out_width = width;
    *out_height = height;
    *out_num_components = num_components;
    return GIMG_OK;
  }
  unsigned char * scan_data = NULL;
  size_t scan_size = 0;
  if (arithmetic) {
    // T.81 Annex D in place of Annex F; the coefficients are the same.
    r = gimg_jpeg_encode_arith_scan_from_coef_buffer(width, height,
        num_components, coef_buf, total_blocks, h_ptr, v_ptr, tbl_ptr, cond,
        alloc, restart_interval, 0, &scan_data, &scan_size);
  }
  else {
    r = gimg_jpeg_encode_baseline_scan_from_coef_buffer(width, height,
        num_components, coef_buf, total_blocks, h_ptr, v_ptr, tbl_ptr, alloc,
        restart_interval, &scan_data, &scan_size);
  }
  gimg_free(alloc, coef_buf);
  // An arithmetic scan of zero bytes is a real answer, not a failed
  // allocation: T.81 D.1.8 drops trailing zero bytes on the grounds that a
  // decoder past the end of the data supplies zeros anyway (D.2.9), so a
  // frame whose every decision is the more probable symbol - a 1x1 image,
  // say - needs no bytes at all.  libjpeg writes none for the same file.
  if (r != GIMG_OK || (!scan_data && !(arithmetic && scan_size == 0))) {
    return (r != GIMG_OK) ? r : GIMG_ERR_OOM;
  }
#if GIMG_JPEG_DUMP_SCAN_BASELINE
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
#endif
  *out_scan_data = scan_data;
  *out_scan_size = scan_size;
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
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &strip_size) ||
      !gcu_safe_mul_size(strip_size, (size_t)samples, &strip_size)) {
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
      stream, 0x00, gimg_jpeg_std_dc_lum_bits, gimg_jpeg_std_dc_lum_vals, 12,
      &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x10, gimg_jpeg_std_ac_lum_bits,
      gimg_jpeg_std_ac_lum_vals, GIMG_JPEG_STD_AC_LUM_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(
      stream, 0x01, gimg_jpeg_std_dc_chr_bits, gimg_jpeg_std_dc_chr_vals, 12,
      &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x11, gimg_jpeg_std_ac_chr_bits,
      gimg_jpeg_std_ac_chr_vals, GIMG_JPEG_STD_AC_CHR_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_write_standard_dht_extended(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  GIMG_Result r = jpeg_write_one_dht(stream, 0x00, gimg_jpeg_ext_dc_lum_bits,
      gimg_jpeg_ext_dc_lum_vals, GIMG_JPEG_EXT_DC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x10, gimg_jpeg_ext_ac_lum_bits,
      gimg_jpeg_ext_ac_lum_vals, GIMG_JPEG_EXT_AC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x01, gimg_jpeg_ext_dc_chr_bits,
      gimg_jpeg_ext_dc_chr_vals, GIMG_JPEG_EXT_DC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  r = jpeg_write_one_dht(stream, 0x11, gimg_jpeg_ext_ac_chr_bits,
      gimg_jpeg_ext_ac_chr_vals, GIMG_JPEG_EXT_AC_VALS, &total);
  if (r != GIMG_OK) {
    return r;
  }
  if (out_bytes_written) {
    *out_bytes_written = total;
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_write_ac_refine_dht(
    GIMG_Stream * stream, size_t * out_bytes_written) {
  size_t total = 0;
  // Th=2 (table 2) so SOS Ta=2 selects this refinement table (T.81 B.2.4).
  GIMG_Result r = jpeg_write_one_dht(stream, 0x12, gimg_jpeg_std_ac_refine_bits,
      gimg_jpeg_std_ac_refine_vals, GIMG_JPEG_AC_REFINE_VALS, &total);
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
    int num_components, const uint8_t * tbl_sel,
    const uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
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
  // T.81 B.2.4.1: a table is written because a component names it.
  if (gimg_jpeg_uses_second_table(tbl_sel, num_components)) {
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

/** Write DQT, [DRI if restart_interval>0], the frame header, the table
 * specifications, SOS, scan data and EOI (SOF before DHT to match common
 * decoders). Does not free scan_data.
 *
 * With Huffman coding the frame is SOF0 at 8-bit or SOF1 at 12-bit and the
 * tables are DHT; with arithmetic coding (T.81 Annex D) it is SOF9 at either
 * precision and the tables are DAC. */
/**
 * Write a hierarchical sequence (T.81 B.3.1, Annex J).
 *
 * The order is the one Figure B.13 gives: tables, the DHP segment that declares
 * the size of the completed image, then the frames.  Each frame after the first
 * is preceded by the EXP that doubles the reference it will be differenced
 * against (B.3.3), and carries its own tables - the standard ones for the
 * non-differential frame, and for a differential frame the table its own
 * coefficients generated, because Table J.2's extra AC category is in no Annex
 * K table.
 */
static GIMG_Result jpeg_write_image_body_hierarchical(GIMG_Stream * stream,
    uint32_t width, uint32_t height, int num_components,
    const uint8_t * tbl_sel,
    const uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    const uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES],
    const gimg_jpeg_enc_frame_t * frames, unsigned num_frames,
    uint16_t restart_interval, bool arithmetic,
    const jpeg_arith_cond_t * cond, int precision, int hier_is_color,
    size_t * out_n) {
  size_t n = (out_n ? *out_n : 0);
  size_t written = 0;
  GIMG_Result r;
  // A lossless sequence has no quantization table to write at all: T.81 Annex
  // H has no DCT, so DQT would describe nothing.  Which process the sequence
  // uses is in the frames' own markers.
  const int lossless = (num_frames > 0) &&
      (frames[0].sof_marker == GIMG_JPEG_MARKER_SOF3 ||
          frames[0].sof_marker == GIMG_JPEG_MARKER_SOF11);

  // A lossless sequence carries RGB, because the YCbCr conversion is not
  // reversible and "lossless" would then be a lie - so it needs the Adobe
  // APP14 that says so, exactly as the single-frame lossless writer does.
  // Without it a decoder reads three components as YCbCr (jdapimin.c, and
  // jpeg_frame_is_rgb here) and every pixel comes out a different color.
  // Three components, and not YCbCr: a lossless sequence keeps RGB, and a
  // sequence of channels with no color meaning keeps those.  Three is the
  // count a decoder guesses at, so it has to be told (see the single-frame
  // writers); the marker is harmless for any other count and is written only
  // where it decides something.
  if (num_components == 3 && (lossless || !hier_is_color)) {
    unsigned char app14[12];
    memcpy(app14, "Adobe", 5);
    app14[5] = 0x00;
    app14[6] = 100; // version
    app14[7] = 0x00;
    app14[8] = 0x00; // flags0
    app14[9] = 0x00;
    app14[10] = 0x00; // flags1
    app14[11] = 0x00; // transform: none
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_APP14, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, (uint16_t)(2 + sizeof(app14)), &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = gimg_stream_write(stream, app14, sizeof(app14), &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }

  // One set of quantization tables for the whole sequence: every frame uses
  // the same quality, and B.2.4.1 lets them stand until redefined.
  if (!lossless) {
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
    // T.81 B.2.4.1: a table is written because a component names it, not
    // because the frame has three components - a CMYK frame has four and uses
    // one table.
    if (gimg_jpeg_uses_second_table(tbl_sel, num_components)) {
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

  // B.3.2: DHP has the frame header's syntax and the completed image's size,
  // "except that the quantization table destination selector parameter shall
  // be set to zero".
  {
    uint16_t len = (uint16_t)(8 + 3 * num_components);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DHP, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, len, &n);
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char dhp[6 + 3 * GIMG_JPEG_MAX_COMPONENTS];
    memset(dhp, 0, sizeof(dhp));
    // B.3.1: every frame of the sequence carries the same precision, so DHP
    // states it once.
    dhp[0] = (unsigned char)precision;
    dhp[1] = (unsigned char)(height >> 8);
    dhp[2] = (unsigned char)(height & 0xFF);
    dhp[3] = (unsigned char)(width >> 8);
    dhp[4] = (unsigned char)(width & 0xFF);
    dhp[5] = (unsigned char)num_components;
    for (int c = 0; c < num_components; c++) {
      dhp[6 + c * 3] = (unsigned char)(c + 1);
      dhp[7 + c * 3] = 0x11; // 4:4:4 throughout the sequence
      dhp[8 + c * 3] = 0x00; // B.3.2: Tq shall be zero here
    }
    r = gimg_stream_write(stream, dhp, (size_t)(6 + 3 * num_components),
        &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }

  for (unsigned f = 0; f < num_frames; f++) {
    const gimg_jpeg_enc_frame_t * fr = &frames[f];
    if (fr->exp_h || fr->exp_v) {
      // B.3.3 Table B.11: Le is 3, so the payload is one byte of Eh | Ev.
      r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_EXP, &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = jpeg_write_u16(stream, 3u, &n);
      if (r != GIMG_OK) {
        return r;
      }
      unsigned char exp = (unsigned char)((fr->exp_h << 4) | fr->exp_v);
      r = gimg_stream_write(stream, &exp, 1, &written);
      if (r != GIMG_OK) {
        return r;
      }
      n += written;
    }
    {
      uint16_t sof_len = (uint16_t)(8 + 3 * num_components);
      r = jpeg_write_marker(stream, fr->sof_marker, &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = jpeg_write_u16(stream, sof_len, &n);
      if (r != GIMG_OK) {
        return r;
      }
      unsigned char sof[6 + 3 * GIMG_JPEG_MAX_COMPONENTS];
      memset(sof, 0, sizeof(sof));
      sof[0] = (unsigned char)(fr->precision ? fr->precision : precision);
      sof[1] = (unsigned char)(fr->height >> 8);
      sof[2] = (unsigned char)(fr->height & 0xFF);
      sof[3] = (unsigned char)(fr->width >> 8);
      sof[4] = (unsigned char)(fr->width & 0xFF);
      sof[5] = (unsigned char)num_components;
      for (int c = 0; c < num_components; c++) {
        sof[6 + c * 3] = (unsigned char)(c + 1);
        sof[7 + c * 3] = 0x11;
        // A lossless frame has no quantization, so Tq is zero (B.2.2 still
        // requires the field); a DCT frame uses the selector the tables were
        // written under.
        sof[8 + c * 3] =
            lossless ? 0x00 : (unsigned char)gimg_jpeg_tbl_of(tbl_sel, c);
      }
      r = gimg_stream_write(stream, sof, (size_t)(6 + 3 * num_components),
          &written);
      if (r != GIMG_OK) {
        return r;
      }
      n += written;
    }
    if (arithmetic) {
      // The conditioning, written out rather than left implicit - B.2.4.3's
      // defaults unless the caller asked for something else.
      int tables = gimg_jpeg_uses_second_table(tbl_sel, num_components) ? 2 : 1;
      unsigned char dac[2 * 2 * GIMG_JPEG_ARITH_TABLES];
      size_t dl = gimg_jpeg_build_dac(cond, tables, 0, dac);
      r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DAC, &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = jpeg_write_u16(stream, (uint16_t)(2 + dl), &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = gimg_stream_write(stream, dac, dl, &written);
      if (r != GIMG_OK) {
        return r;
      }
      n += written;
    }
    else if (fr->dht && fr->dht_len > 0) {
      r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DHT, &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = jpeg_write_u16(stream, (uint16_t)(2 + fr->dht_len), &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = gimg_stream_write(stream, fr->dht, fr->dht_len, &written);
      if (r != GIMG_OK) {
        return r;
      }
      n += written;
    }
    else {
      size_t dht_written = 0;
      r = fr->extended_tables
          ? gimg_jpeg_write_standard_dht_extended(stream, &dht_written)
          : gimg_jpeg_write_standard_dht(stream, &dht_written);
      if (r != GIMG_OK) {
        return r;
      }
      n += dht_written;
    }
    r = jpeg_write_dri(stream, restart_interval, &n);
    if (r != GIMG_OK) {
      return r;
    }
    // A frame is one scan for a sequential or lossless process and several for
    // a progressive one (G.1.2.2 puts each AC band in its own scan), so the
    // scan headers come from the frame rather than being assumed here.
    for (unsigned si = 0; si < fr->num_scans; si++) {
      const gimg_jpeg_enc_scan_t * sc = &fr->scans[si];
      uint16_t sos_len = (uint16_t)(6 + 2 * (uint16_t)sc->ns);
      r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_SOS, &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = jpeg_write_u16(stream, sos_len, &n);
      if (r != GIMG_OK) {
        return r;
      }
      unsigned char sos[4 + 2 * GIMG_JPEG_MAX_SCAN_COMPONENTS];
      memset(sos, 0, sizeof(sos));
      sos[0] = sc->ns;
      for (int c = 0; c < (int)sc->ns; c++) {
        sos[1 + c * 2] = (unsigned char)(sc->comp[c] + 1);
        sos[2 + c * 2] = sc->td_ta[c];
      }
      size_t tail = 1 + 2 * (size_t)sc->ns;
      sos[tail] = sc->ss;
      sos[tail + 1] = sc->se;
      sos[tail + 2] = (unsigned char)((sc->ah << 4) | (sc->al & 0x0Fu));
      r = gimg_stream_write(stream, sos, tail + 3, &written);
      if (r != GIMG_OK) {
        return r;
      }
      n += written;
      r = jpeg_write_scan_data_with_stuffing(stream, sc->data, sc->size, &n);
      if (r != GIMG_OK) {
        return r;
      }
    }
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

// Defined with the progressive writer, which needs the same two operations to
// write its non-interleaved AC scans.
static void jpeg_component_block_grid(uint32_t width, uint32_t height,
    const uint8_t * h_samp, const uint8_t * v_samp, int num_components,
    int comp, uint32_t * out_blk_w, uint32_t * out_blk_h);
static void jpeg_gather_component_blocks(int16_t * interleaved,
    int16_t * packed, uint32_t blk_w, uint32_t blk_h, const uint8_t * h_samp,
    const uint8_t * v_samp, int num_components, int comp, uint32_t mcu_per_row,
    int scatter);

/**
 * Write a sequential frame's entropy-coded data as one non-interleaved scan
 * per component (T.81 A.2.3).
 *
 * A.2.2 and A.2.3 are two orders through the same blocks.  An interleaved scan
 * walks MCUs, each holding H_i x V_i blocks of every component; a
 * non-interleaved scan carries one component, and there "the MCU is defined to
 * be one data unit", so the scan is that component's own block grid in raster
 * order with no MCU padding.  That is exactly the shape the one-component
 * encoder already produces, so each component is gathered out of the
 * MCU-interleaved coefficient buffer into its own grid and handed to that
 * encoder as if it were a one-component image - the same route the progressive
 * writer takes for its AC scans, which G.1.2.2 requires to be non-interleaved.
 *
 * Each scan is encoded by its own call, so the DC predictor starts at zero in
 * each of them, which is what F.2.1.3.1 requires: the prediction is reset "at
 * the beginning of the scan", not once for the frame.
 *
 * A restart interval counts MCUs, so in these scans it counts single blocks
 * (A.2.3, B.2.4.4).  Passing it through to the one-component encoder is what
 * gives that meaning, and it is a different placement of RST markers from the
 * interleaved scan of the same image - correctly so.
 */
static GIMG_Result jpeg_write_noninterleaved_scans(GIMG_Stream * stream,
    uint32_t width, uint32_t height, int num_components,
    const uint8_t * h_samp, const uint8_t * v_samp, const uint8_t * tbl_sel,
    const int16_t * coef_buffer,
    int precision, bool arithmetic, const jpeg_arith_cond_t * cond,
    uint16_t restart_interval, const GIMG_Allocator * alloc, size_t * out_n) {
  (void)tbl_sel;
  size_t n = (out_n ? *out_n : 0);
  GIMG_Result r = GIMG_OK;
  size_t written = 0;
  uint8_t samp_ones[GIMG_JPEG_MAX_COMPONENTS];
  h_samp = gimg_jpeg_samp_or_ones(h_samp, samp_ones, num_components);
  v_samp = gimg_jpeg_samp_or_ones(v_samp, samp_ones, num_components);
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  for (int i = 1; i < num_components; i++) {
    if (h_samp[i] > h_max) {
      h_max = h_samp[i];
    }
    if (v_samp[i] > v_max) {
      v_max = v_samp[i];
    }
  }
  uint32_t mcu_per_row =
      (width + (uint32_t)(8 * h_max) - 1u) / (uint32_t)(8 * h_max);
  (void)height;

  for (int comp = 0; comp < num_components; comp++) {
    uint32_t blk_w = 0, blk_h = 0;
    jpeg_component_block_grid(
        width, height, h_samp, v_samp, num_components, comp, &blk_w, &blk_h);
    size_t nblocks = (size_t)blk_w * (size_t)blk_h;
    size_t packed_bytes = 0;
    if (!gcu_safe_mul_size(nblocks, 64u * sizeof(int16_t), &packed_bytes)) {
      return GIMG_ERR_LIMIT;
    }
    int16_t * packed = (int16_t *)gimg_malloc(alloc, packed_bytes);
    if (!packed) {
      return GIMG_ERR_OOM;
    }
    jpeg_gather_component_blocks((int16_t *)coef_buffer, packed, blk_w, blk_h,
        h_samp, v_samp, num_components, comp, mcu_per_row, 0);

    // The component's grid described as a one-component image.  Rounding the
    // dimensions up to whole blocks is not padding the picture: the gathered
    // buffer is blk_w x blk_h blocks, and this is how that same count is
    // spelled in pixels for an encoder that takes pixels.
    uint32_t cw = blk_w * 8u;
    uint32_t ch = blk_h * 8u;
    unsigned char * scan_data = NULL;
    size_t scan_size = 0;
    static const uint8_t one_samp[3] = {1, 1, 1};
    if (arithmetic) {
      r = gimg_jpeg_encode_arith_scan_from_coef_buffer(cw, ch, 1, packed,
          nblocks, one_samp, one_samp, NULL, cond, alloc, restart_interval,
          0, &scan_data, &scan_size);
    }
    else if (precision > 8) {
      r = gimg_jpeg_encode_baseline_scan_from_coef_buffer_extended(cw, ch, 1,
          packed, nblocks, one_samp, one_samp, NULL, alloc,
          restart_interval, &scan_data, &scan_size);
    }
    else {
      r = gimg_jpeg_encode_baseline_scan_from_coef_buffer(cw, ch, 1, packed,
          nblocks, one_samp, one_samp, NULL, alloc, restart_interval,
          &scan_data, &scan_size);
    }
    gimg_free(alloc, packed);
    // A zero-byte arithmetic scan is legitimate (T.81 D.1.8); see
    // jpeg_raster_to_scan_data.
    if (r != GIMG_OK) {
      return r;
    }
    if (!scan_data && !(arithmetic && scan_size == 0)) {
      return GIMG_ERR_OOM;
    }

    // B.2.3: Ls = 6 + 2*Ns, Ns = 1, then Cs/Td/Ta, Ss, Se, Ah/Al.  Ss = 0 and
    // Se = 63 say this scan carries the whole block, which is what makes it a
    // sequential scan rather than one of Annex G's spectral bands.
    unsigned char sos[8];
    sos[0] = 0x01;
    sos[1] = (unsigned char)(comp + 1);
    // Td and Ta must name the tables the entropy coder actually used.  Each of
    // these scans is encoded through the one-component path, which uses the
    // luminance tables whichever component it is handed, so both selectors are
    // 0 for every component.  B.2.3 lets any component select any table, so
    // this is well formed; naming table 1 for chroma while encoding with table
    // 0 is not, and produces a file libjpeg rejects with "bad Huffman code".
    // The progressive writer's AC scans have the same constraint for the same
    // reason.
    sos[2] = 0x00;
    sos[3] = 0x00;
    sos[4] = 0x3F;
    sos[5] = 0x00;
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_SOS, &n);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    r = jpeg_write_u16(stream, 8, &n);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    r = gimg_stream_write(stream, sos, 6, &written);
    if (r != GIMG_OK) {
      gimg_free(alloc, scan_data);
      return r;
    }
    n += written;
    r = jpeg_write_scan_data_with_stuffing(stream, scan_data, scan_size, &n);
    gimg_free(alloc, scan_data);
    if (r != GIMG_OK) {
      return r;
    }
  }
  if (out_n) {
    *out_n = n;
  }
  return GIMG_OK;
}

static GIMG_Result jpeg_write_image_body(GIMG_Stream * stream, uint32_t width,
    uint32_t height, int num_components, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint8_t * tbl_sel,
    const uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    const uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES],
    const unsigned char * scan_data, size_t scan_size, int precision,
    uint16_t restart_interval, bool arithmetic,
    const jpeg_arith_cond_t * cond, const int16_t * coef_buffer,
    const GIMG_Allocator * alloc, unsigned abbreviated, size_t * out_n) {
  size_t n = (out_n ? *out_n : 0);
  GIMG_Result r;
  size_t written = 0;
  // T.81 B.4.  1 leaves the table-specification segments out and keeps the
  // frame; 2 writes them and nothing else.  0 writes both, as usual.
  const int want_tables = (abbreviated != 1u);
  const int want_frame = (abbreviated != 2u);
  if (!want_tables) {
    goto after_tables;
  }
  if (precision > 8) {
    r = jpeg_write_dqt_16bit(
        stream, num_components, tbl_sel, quant_luma, quant_chroma, &n);
    if (r != GIMG_OK) {
      return r;
    }
    if (!arithmetic) { // DAC replaces DHT in an arithmetic frame
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
    // T.81 B.2.4.1: a table is written because a component names it, not
    // because the frame has three components - a CMYK frame has four and uses
    // one table.
    if (gimg_jpeg_uses_second_table(tbl_sel, num_components)) {
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
after_tables:
  // T.81 B.2.2: frame header (SOF) before table specifications (DHT).
  // Skipped entirely for B.4's table-specification stream, which has no frame;
  // the DHT, DAC and DRI below are table data and are written there.
  if (want_frame) {
    // T.81 Table B.1: the marker says which coding process and which entropy
    // coder.  SOF9 is the arithmetic counterpart of SOF1, and serves 8-bit data
    // too - there is no arithmetic equivalent of the baseline process, because
    // baseline is by definition Huffman.
    uint8_t sof_marker = GIMG_JPEG_MARKER_SOF0;
    if (arithmetic) {
      sof_marker = GIMG_JPEG_MARKER_SOF9;
    }
    else if (precision == 12) {
      sof_marker = GIMG_JPEG_MARKER_SOF1;
    }
    uint8_t prec_byte = (uint8_t)(precision < 8 ? 8 : precision);
    // SOF Lf = 8 + 3*Nc (T.81 B.2.2); payload = Lf - 2 = 6 + 3*Nc bytes.
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
    unsigned char sof[6 + 3 * GIMG_JPEG_MAX_COMPONENTS];
    memset(sof, 0, sizeof(sof));
    sof[0] = prec_byte;
    sof[1] = (unsigned char)(height >> 8);
    sof[2] = (unsigned char)(height & 0xFF);
    sof[3] = (unsigned char)(width >> 8);
    sof[4] = (unsigned char)(width & 0xFF);
    sof[5] = (unsigned char)num_components;
    for (int c = 0; c < num_components; c++) {
      uint8_t h = h_samp ? h_samp[c] : 1;
      uint8_t v = v_samp ? v_samp[c] : 1;
      if (h == 0) {
        h = 1;
      }
      if (v == 0) {
        v = 1;
      }
      sof[6 + c * 3] = (unsigned char)(c + 1);
      sof[7 + c * 3] = (unsigned char)((h << 4) | v);
      sof[8 + c * 3] = gimg_jpeg_tbl_of(tbl_sel, c);
    }
    r = gimg_stream_write(stream, sof, sof_payload, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  if (!want_tables) {
    // Left out with the rest of the table-specification data (B.4).
  }
  else if (arithmetic) {
    // T.81 B.2.4.3: DAC in place of DHT.  These are the values B.2.4.3 gives as
    // defaults - L = 0 and U = 1 for DC, Kx = 5 for AC - written out rather
    // than left implicit, which is what libjpeg does as well.
    int tables = gimg_jpeg_uses_second_table(tbl_sel, num_components) ? 2 : 1;
    unsigned char dac[2 * 2 * GIMG_JPEG_ARITH_TABLES];
    size_t dl = gimg_jpeg_build_dac(cond, tables, 0, dac);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DAC, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, (uint16_t)(2 + dl), &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = gimg_stream_write(stream, dac, dl, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  else if (precision <= 8) {
    size_t dht_written = 0;
    r = gimg_jpeg_write_standard_dht(stream, &dht_written);
    if (r != GIMG_OK) {
      return r;
    }
    n += dht_written;
  }
  // T.81: DRI after SOF, before SOS.  DRI is table-specification data too
  // (B.2.4.4), so an abbreviated image leaves it to the tables stream.
  r = want_tables ? jpeg_write_dri(stream, restart_interval, &n) : GIMG_OK;
  if (r != GIMG_OK) {
    return r;
  }
  if (!want_frame) {
    // B.4's abbreviated format for table-specification data: SOI, every table
    // segment, EOI.  Nothing of the picture reaches such a stream.
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_EOI, &n);
    if (r != GIMG_OK) {
      return r;
    }
    if (out_n) {
      *out_n = n;
    }
    return GIMG_OK;
  }
  // T.81 A.2.3: the same blocks, written one component at a time.  Everything
  // above this point - the tables, the frame header, the restart interval - is
  // the same file either way; only the scans differ.
  if (coef_buffer) {
    r = jpeg_write_noninterleaved_scans(stream, width, height, num_components,
        h_samp, v_samp, tbl_sel, coef_buffer, precision, arithmetic, cond,
        restart_interval, alloc, &n);
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
    unsigned char sos[4 + 2 * GIMG_JPEG_MAX_COMPONENTS];
    memset(sos, 0, sizeof(sos));
    sos[0] = (unsigned char)num_components;
    // T.81 B.2.3: Cs, then Td in the high nibble and Ta in the low one.  Both
    // come from the same per-component selector the frame header used, so the
    // tables a scan names are the tables its coefficients were coded with.
    for (int c = 0; c < num_components; c++) {
      uint8_t t = gimg_jpeg_tbl_of(tbl_sel, c);
      sos[1 + c * 2] = (unsigned char)(c + 1);
      sos[2 + c * 2] = (unsigned char)((t << 4) | t);
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
  // T.81 G.1.1.1.2: a coefficient is sent once at some point transform Al and
  // then refined one bit at a time, so a scan's Ah has to be the Al of the
  // scan that last touched the same coefficients, and its own Al one lower.
  //
  // This is checked because it cannot be recovered from: a script that
  // refines a bit no earlier scan held back describes a file no decoder can
  // read.  This library's own decoder refuses one with GIMG_ERR_CORRUPT, and
  // so does libjpeg, which is the right answer - but it is the wrong place to
  // find out, because by then the caller has been told the save succeeded.
  //
  // `sent[k]` is the point transform the last scan covering coefficient k
  // used, or -1 for a coefficient no scan has sent yet.  The DC coefficient
  // is index 0 and its scans are the ones with Ss = Se = 0, so the same array
  // covers both without a special case.  This subsumes the older check that
  // initial AC bands must not overlap: a second initial scan over a
  // coefficient already sent is exactly a coefficient whose sent[k] is not -1.
  int sent[64];
  for (int k = 0; k < 64; k++) {
    sent[k] = -1;
  }
  for (unsigned i = 0; i < config->scan_count; i++) {
    const unsigned Ss = config->scans[i].Ss;
    const unsigned Se = config->scans[i].Se;
    const int Ah = (int)config->scans[i].Ah;
    const int Al = (int)config->scans[i].Al;
    if (Ah != 0 && Al != Ah - 1) {
      return GIMG_ERR_UNSUPPORTED;
    }
    for (unsigned k = Ss; k <= Se && k < 64u; k++) {
      if (Ah == 0) {
        if (sent[k] != -1) {
          return GIMG_ERR_UNSUPPORTED; // sent twice for the first time
        }
      }
      else if (sent[k] != Ah) {
        return GIMG_ERR_UNSUPPORTED; // refining a bit nothing held back
      }
      sent[k] = Al;
    }
  }
  return GIMG_OK;
}

/**
 * Number of blocks a component owns in its own grid.
 *
 * T.81 A.2.2: a non-interleaved scan walks the component's own grid, which is
 * ceil(X_i/8) by ceil(Y_i/8) blocks - not the MCU-padded grid an interleaved
 * scan walks.
 */
static void jpeg_component_block_grid(uint32_t width, uint32_t height,
    const uint8_t * h_samp, const uint8_t * v_samp, int num_components,
    int comp, uint32_t * out_blk_w, uint32_t * out_blk_h) {
  uint8_t h_max = h_samp[0];
  uint8_t v_max = v_samp[0];
  for (int i = 1; i < num_components; i++) {
    if (h_samp[i] > h_max)
      h_max = h_samp[i];
    if (v_samp[i] > v_max)
      v_max = v_samp[i];
  }
  uint32_t comp_w =
      (width * (uint32_t)h_samp[comp] + h_max - 1u) / (uint32_t)h_max;
  uint32_t comp_h =
      (height * (uint32_t)v_samp[comp] + v_max - 1u) / (uint32_t)v_max;
  *out_blk_w = (comp_w + 7u) / 8u;
  *out_blk_h = (comp_h + 7u) / 8u;
}

/**
 * Index of one component block inside the MCU-interleaved coefficient buffer.
 *
 * The buffer is filled MCU by MCU (T.81 A.2.3), so a component block at raster
 * position (row, col) of its own grid lives in the MCU that covers it, at the
 * sub-position that MCU gives it.
 */
static size_t jpeg_interleaved_block_index(uint32_t row, uint32_t col,
    const uint8_t * h_samp, const uint8_t * v_samp, int num_components,
    int comp, uint32_t mcu_per_row) {
  size_t blocks_per_mcu = 0;
  size_t comp_off = 0;
  for (int i = 0; i < num_components; i++) {
    if (i == comp) {
      comp_off = blocks_per_mcu;
    }
    blocks_per_mcu += (size_t)h_samp[i] * (size_t)v_samp[i];
  }
  uint32_t mcu_row = row / v_samp[comp];
  uint32_t mcu_col = col / h_samp[comp];
  uint32_t sub =
      (row % v_samp[comp]) * (uint32_t)h_samp[comp] + (col % h_samp[comp]);
  return ((size_t)mcu_row * (size_t)mcu_per_row + mcu_col) * blocks_per_mcu +
      comp_off + (size_t)sub;
}

/**
 * Copy one component's blocks out of the interleaved buffer into its own
 * raster order (or back again when scatter is set), so that a single-component
 * scan can be encoded through the ordinary one-component path.
 */
static void jpeg_gather_component_blocks(int16_t * interleaved,
    int16_t * packed, uint32_t blk_w, uint32_t blk_h, const uint8_t * h_samp,
    const uint8_t * v_samp, int num_components, int comp, uint32_t mcu_per_row,
    int scatter) {
  for (uint32_t row = 0; row < blk_h; row++) {
    for (uint32_t col = 0; col < blk_w; col++) {
      size_t src = jpeg_interleaved_block_index(
          row, col, h_samp, v_samp, num_components, comp, mcu_per_row);
      size_t dst = (size_t)row * (size_t)blk_w + col;
      if (scatter) {
        memcpy(interleaved + src * 64, packed + dst * 64,
            64 * sizeof(int16_t));
      }
      else {
        memcpy(packed + dst * 64, interleaved + src * 64,
            64 * sizeof(int16_t));
      }
    }
  }
}


/**
 * Write the body of a lossless frame (T.81 Annex H).
 *
 * Shorter than the DCT-based writers, and different in shape: there is no DQT,
 * because nothing is quantized.  The frame header is SOF3 and the scan header
 * carries the predictor selection value in Ss, zero in Se, and the point
 * transform in Al (H.1).
 *
 * Color is written as RGB, with the component identifiers 'R', 'G' and 'B' and
 * an Adobe APP14 saying transform 0, which is how libjpeg marks the same thing.
 * A YCbCr conversion would make the result not lossless.
 */
static GIMG_Result jpeg_write_image_body_lossless(GIMG_Stream * stream,
    uint32_t width, uint32_t height, int num_components, int precision,
    int psv, int arithmetic, const jpeg_arith_cond_t * cond,
    const gimg_jpeg_lossless_scan_t * scans, unsigned num_scans,
    uint16_t restart_interval, size_t * out_n) {
  size_t n = (out_n ? *out_n : 0);
  size_t written = 0;
  GIMG_Result r;

  if (num_components == 3) {
    // APP14 Adobe, transform 0: the components are not YCbCr.
    unsigned char app14[12];
    memcpy(app14, "Adobe", 5);
    app14[5] = 0x00;
    app14[6] = 100; // version
    app14[7] = 0x00;
    app14[8] = 0x00; // flags0
    app14[9] = 0x00;
    app14[10] = 0x00; // flags1
    app14[11] = 0x00; // transform: none
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_APP14, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, (uint16_t)(2 + sizeof(app14)), &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = gimg_stream_write(stream, app14, sizeof(app14), &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }

  // SOF3, or SOF11 for the arithmetic coder (Table B.1), with 1x1 sampling: a
  // lossless MCU is made of samples, and subsampling would discard them.
  {
    uint16_t sof_len = (uint16_t)(8 + 3 * num_components);
    r = jpeg_write_marker(stream,
        arithmetic ? GIMG_JPEG_MARKER_SOF11 : GIMG_JPEG_MARKER_SOF3, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, sof_len, &n);
    if (r != GIMG_OK) {
      return r;
    }
    // Sized for T.81 B.2.2's widest frame.  At 6 + 3 * 4 this wrote past its
    // own end for any frame of more than four components, which the lossless
    // writer can now be handed.
    unsigned char sof[6 + 3 * GIMG_JPEG_MAX_COMPONENTS];
    static const unsigned char rgb_ids[3] = {'R', 'G', 'B'};
    memset(sof, 0, sizeof(sof));
    sof[0] = (unsigned char)precision;
    sof[1] = (unsigned char)(height >> 8);
    sof[2] = (unsigned char)(height & 0xFF);
    sof[3] = (unsigned char)(width >> 8);
    sof[4] = (unsigned char)(width & 0xFF);
    sof[5] = (unsigned char)num_components;
    for (int c = 0; c < num_components; c++) {
      // B.2.2: the identifiers must be distinct, since a scan header selects
      // components by identifier.  Three components carry 'R', 'G', 'B' to say
      // they are RGB (see above); any other count numbers them from 1.
      sof[6 + c * 3] =
          (num_components == 3) ? rgb_ids[c] : (unsigned char)(c + 1);
      sof[7 + c * 3] = 0x11; // H = V = 1
      sof[8 + c * 3] = 0x00; // no quantization table
    }
    r = gimg_stream_write(stream, sof, 6 + 3 * (size_t)num_components, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }

  if (arithmetic) {
    // B.2.4.3: DAC in place of DHT.  A lossless scan codes only differences,
    // which use the DC conditioning (H.1.2.3.3 gives the defaults L = 0 and
    // U = 1), so there is no AC entry to write.  One table serves every
    // component because the scan names table 0 for all of them.
    unsigned char dac[2 * 2 * GIMG_JPEG_ARITH_TABLES];
    size_t dl = gimg_jpeg_build_dac(cond, 1, 1, dac);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DAC, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, (uint16_t)(2 + dl), &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = gimg_stream_write(stream, dac, dl, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  // The Huffman tables are per scan now and are written with them below; an
  // arithmetic frame's DAC is written once, above, because Annex D's
  // conditioning does not vary between scans here.

  r = jpeg_write_dri(stream, restart_interval, &n);
  if (r != GIMG_OK) {
    return r;
  }

  // One interleaved scan for a frame of up to four components, one scan per
  // component beyond that (B.2.3 Table B.3 caps Ns at 4).  Each scan's own
  // Huffman table is written before it; B.2.4.2 lets a table at the same
  // destination stand until redefined, which is what makes that legal.
  for (unsigned si = 0; si < num_scans; si++) {
    const gimg_jpeg_lossless_scan_t * sc = &scans[si];
    if (!arithmetic && sc->dht && sc->dht_len > 0) {
      r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DHT, &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = jpeg_write_u16(stream, (uint16_t)(2 + sc->dht_len), &n);
      if (r != GIMG_OK) {
        return r;
      }
      r = gimg_stream_write(stream, sc->dht, sc->dht_len, &written);
      if (r != GIMG_OK) {
        return r;
      }
      n += written;
    }
    int ns = (sc->component == 0xFFu) ? num_components : 1;
    uint16_t sos_len = (uint16_t)(6 + 2 * ns);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_SOS, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, sos_len, &n);
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char sos[4 + 2 * GIMG_JPEG_MAX_SCAN_COMPONENTS];
    static const unsigned char rgb_ids2[3] = {'R', 'G', 'B'};
    memset(sos, 0, sizeof(sos));
    sos[0] = (unsigned char)ns;
    for (int c = 0; c < ns; c++) {
      int idx = (sc->component == 0xFFu) ? c : (int)sc->component;
      sos[1 + c * 2] = (num_components == 3) ? rgb_ids2[idx]
                                             : (unsigned char)(idx + 1);
      sos[2 + c * 2] = 0x00; // Td = 0, Ta unused
    }
    size_t tail = 1 + 2 * (size_t)ns;
    sos[tail] = (unsigned char)psv; // Ss: predictor selection (H.1)
    sos[tail + 1] = 0x00;           // Se
    sos[tail + 2] = 0x00;           // Ah = 0, Al = point transform 0
    r = gimg_stream_write(stream, sos, tail + 3, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
    // The encoder has already stuffed its own 0x00 after every 0xFF and
    // written the restart markers, so the bytes go out as they stand.
    if (sc->size > 0) {
      r = gimg_stream_write(stream, sc->data, sc->size, &written);
      if (r != GIMG_OK) {
        return r;
      }
      n += written;
    }
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

/** Write DQT, the table specifications, [DRI if restart_interval>0], the frame
 * header, then for each scan: SOS (Ss,Se,Ah,Al) + scan data; then EOI.
 * precision 8 or 12.  Frees scan data after each write.
 *
 * With Huffman coding the frame is SOF2 and the tables are DHT; with arithmetic
 * coding it is SOF10 and the tables are DAC. */
static GIMG_Result jpeg_write_image_body_progressive(GIMG_Stream * stream,
    uint32_t width, uint32_t height, int num_components, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint8_t * tbl_sel,
    const uint16_t quant_luma[GIMG_JPEG_DQT_ENTRIES],
    const uint16_t quant_chroma[GIMG_JPEG_DQT_ENTRIES],
    const int16_t * coef_buffer, size_t total_blocks,
    const GIMG_JPEG_Progressive_Scan * scans, unsigned scan_count,
    int precision, bool arithmetic, const jpeg_arith_cond_t * cond,
    const GIMG_Allocator * alloc, uint16_t restart_interval,
    unsigned abbreviated, size_t * out_n) {
  size_t n = (out_n ? *out_n : 0);
  GIMG_Result r;
  size_t written = 0;
  // T.81 B.4, as in jpeg_write_image_body: 1 leaves the tables out, 2 writes
  // nothing but them.
  const int want_tables = (abbreviated != 1u);
  const int want_frame = (abbreviated != 2u);
  if (!want_tables) {
    goto after_prog_tables;
  }
  if (precision > 8) {
    r = jpeg_write_dqt_16bit(
        stream, num_components, tbl_sel, quant_luma, quant_chroma, &n);
    if (r != GIMG_OK) {
      return r;
    }
    if (!arithmetic) { // DAC replaces DHT in an arithmetic frame
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
    // T.81 B.2.4.1: a table is written because a component names it, not
    // because the frame has three components - a CMYK frame has four and uses
    // one table.
    if (gimg_jpeg_uses_second_table(tbl_sel, num_components)) {
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
    if (!arithmetic) {
      size_t dht_written = 0;
      r = gimg_jpeg_write_standard_dht(stream, &dht_written);
      if (r != GIMG_OK) {
        return r;
      }
      n += dht_written;
    }
  }
  if (arithmetic) {
    // T.81 B.2.4.3: DAC in place of DHT, with the conditioning written out
    // explicitly - the defaults unless the caller asked for something else.
    int tables = gimg_jpeg_uses_second_table(tbl_sel, num_components) ? 2 : 1;
    unsigned char dac[2 * 2 * GIMG_JPEG_ARITH_TABLES];
    size_t dl = gimg_jpeg_build_dac(cond, tables, 0, dac);
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_DAC, &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, (uint16_t)(2 + dl), &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = gimg_stream_write(stream, dac, dl, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  else {
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
  // DRI is table-specification data too (B.2.4.4), so an abbreviated image
  // leaves it to the tables stream.
  r = want_tables ? jpeg_write_dri(stream, restart_interval, &n) : GIMG_OK;
  if (r != GIMG_OK) {
    return r;
  }
after_prog_tables:
  if (!want_frame) {
    // B.4's abbreviated format for table-specification data: the tables and
    // nothing else.
    r = jpeg_write_marker(stream, GIMG_JPEG_MARKER_EOI, &n);
    if (r != GIMG_OK) {
      return r;
    }
    if (out_n) {
      *out_n = n;
    }
    return GIMG_OK;
  }
  {
    uint8_t prec_byte = (uint8_t)(precision > 8 ? precision : 8);
    uint16_t sof_len = (uint16_t)(8 + 3 * (uint16_t)num_components);
    size_t sof_payload = 6 + 3 * (size_t)num_components;
    // T.81 Table B.1: SOF10 is the arithmetic counterpart of SOF2.
    r = jpeg_write_marker(
        stream, arithmetic ? GIMG_JPEG_MARKER_SOF10 : GIMG_JPEG_MARKER_SOF2,
        &n);
    if (r != GIMG_OK) {
      return r;
    }
    r = jpeg_write_u16(stream, sof_len, &n);
    if (r != GIMG_OK) {
      return r;
    }
    unsigned char sof[6 + 3 * GIMG_JPEG_MAX_COMPONENTS];
    memset(sof, 0, sizeof(sof));
    sof[0] = prec_byte;
    sof[1] = (unsigned char)(height >> 8);
    sof[2] = (unsigned char)(height & 0xFF);
    sof[3] = (unsigned char)(width >> 8);
    sof[4] = (unsigned char)(width & 0xFF);
    sof[5] = (unsigned char)num_components;
    for (int c = 0; c < num_components; c++) {
      uint8_t h = h_samp ? h_samp[c] : 1;
      uint8_t v = v_samp ? v_samp[c] : 1;
      if (h == 0) {
        h = 1;
      }
      if (v == 0) {
        v = 1;
      }
      sof[6 + c * 3] = (unsigned char)(c + 1);
      sof[7 + c * 3] = (unsigned char)((h << 4) | v);
      sof[8 + c * 3] = gimg_jpeg_tbl_of(tbl_sel, c);
    }
    r = gimg_stream_write(stream, sof, sof_payload, &written);
    if (r != GIMG_OK) {
      return r;
    }
    n += written;
  }
  // No scan carries state forward to the next.  A refinement scan works out
  // which coefficients its predecessor already sent from the coefficients and
  // the two point transforms (see gimg_jpeg_encode_progressive_scan), so a
  // buffer the size of the whole frame used to be allocated here for nothing
  // - and the scans had to be ordered so that each refinement followed the
  // initial scan of its own band, which is not an order T.81 requires and not
  // the order libjpeg writes.
  // T.81 G.1.2.2: "In a scan with Ss not equal to zero, Ns shall be one" - an
  // AC scan is always non-interleaved.  A DC scan (Ss = Se = 0) may carry all
  // the components together.  This encoder used to write every scan with every
  // component, so any multi-component progressive file it produced was not a
  // JPEG: libjpeg reports "broken data stream" on them.  Our own decoder read
  // them back because it shared the misunderstanding, so round-trip tests
  // passed throughout.
  //
  // An AC scan is therefore written once per component.  The coefficient buffer
  // is MCU-interleaved (A.2.3), so each component's blocks are gathered into
  // their own raster order (A.2.2) first, encoded through the ordinary
  // one-component path, and the refinement state scattered back.
  // The sampling factors are optional at this interface; the scan encoders
  // substitute 1x1 when they are absent, so do the same here rather than
  // dereferencing a null pointer.
  uint8_t samp_ones[GIMG_JPEG_MAX_COMPONENTS];
  h_samp = gimg_jpeg_samp_or_ones(h_samp, samp_ones, num_components);
  v_samp = gimg_jpeg_samp_or_ones(v_samp, samp_ones, num_components);
  uint32_t mcu_per_row_enc = 1;
  {
    uint8_t hm = h_samp[0];
    uint8_t vm = v_samp[0];
    for (int i = 1; i < num_components; i++) {
      if (h_samp[i] > hm)
        hm = h_samp[i];
      if (v_samp[i] > vm)
        vm = v_samp[i];
    }
    mcu_per_row_enc = (width + (uint32_t)(8 * hm) - 1u) / (uint32_t)(8 * hm);
    (void)vm;
  }
  for (unsigned s = 0; s < scan_count; s++) {
    int is_ac_scan = (scans[s].Ss != 0 || scans[s].Se != 0);
    int ac_refine = (scans[s].Ah != 0 && is_ac_scan);

    // Components written in this script entry: one scan each for an AC scan
    // (G.1.2.2 requires it), and all of them together for a DC scan - unless
    // there are more of them than B.2.3 lets one scan name, in which case the
    // DC scan splits the same way.  A frame of five or more components has no
    // other legal arrangement (A.2.3).
    int split =
        is_ac_scan || num_components > (int)GIMG_JPEG_MAX_SCAN_COMPONENTS;
    int first_comp = 0;
    int last_comp = split ? (num_components - 1) : 0;
    for (int comp = first_comp; comp <= last_comp; comp++) {
      unsigned char * scan_data = NULL;
      size_t scan_size = 0;
      int scan_components = split ? 1 : num_components;
      int16_t * packed = NULL;
      const int16_t * enc_coef = coef_buffer;
      size_t enc_blocks = total_blocks;
      uint8_t one_samp[3] = {1, 1, 1};
      const uint8_t * enc_h = h_samp;
      const uint8_t * enc_v = v_samp;
      uint32_t enc_w = width;
      uint32_t enc_h_px = height;
      uint32_t blk_w = 0;
      uint32_t blk_h = 0;

      if (split && num_components > 1) {
        jpeg_component_block_grid(width, height, h_samp, v_samp,
            num_components, comp, &blk_w, &blk_h);
        size_t nblocks = (size_t)blk_w * (size_t)blk_h;
        packed = (int16_t *)gimg_malloc(alloc, nblocks * 64 * sizeof(int16_t));
        if (!packed) {
          return GIMG_ERR_OOM;
        }
        jpeg_gather_component_blocks((int16_t *)coef_buffer, packed, blk_w,
            blk_h, h_samp, v_samp, num_components, comp, mcu_per_row_enc, 0);
        enc_coef = packed;
        enc_blocks = nblocks;
        enc_h = one_samp;
        enc_v = one_samp;
        // One block per MCU: present the component's grid as its own image.
        enc_w = blk_w * 8u;
        enc_h_px = blk_h * 8u;
      }

      // T.81 B.2.3: the tables this scan's SOS names must be the tables the
      // entropy coder used.  An AC scan is always one component encoded
      // through the one-component path, which reads tbl_sel[0]; a DC scan
      // carries every component and reads the frame's own selector.
      uint8_t ac_scan_tbl[GIMG_JPEG_MAX_SCAN_COMPONENTS] = {0};
      const uint8_t * scan_tbl = tbl_sel;
      if (split && num_components > 1) {
        scan_tbl = ac_scan_tbl;
      }
      if (arithmetic) {
        // T.81 Annex D and G.2.  The same coder serves both precisions, and it
        // needs no previous-scan state: the point transform is a shift.
        r = gimg_jpeg_encode_arith_progressive_scan(enc_w, enc_h_px,
            scan_components, enc_coef, enc_blocks, enc_h, enc_v, scan_tbl, 0,
            scans[s].Ss, scans[s].Se, scans[s].Ah, scans[s].Al, cond, alloc,
            restart_interval, &scan_data, &scan_size);
      }
      else if (precision > 8) {
        r = gimg_jpeg_encode_progressive_scan_extended(enc_w, enc_h_px,
            scan_components, enc_coef, enc_blocks, enc_h, enc_v, scan_tbl, 0,
            scans[s].Ss, scans[s].Se, scans[s].Ah, scans[s].Al, alloc,
            restart_interval, &scan_data, &scan_size);
      }
      else {
        r = gimg_jpeg_encode_progressive_scan(enc_w, enc_h_px, scan_components,
            enc_coef, enc_blocks, enc_h, enc_v, scan_tbl, scans[s].Ss,
            scans[s].Se, scans[s].Ah, scans[s].Al, alloc, restart_interval,
            &scan_data, &scan_size);
      }
      gimg_free(alloc, packed);
      if (r != GIMG_OK) {
        return r;
      }

      uint16_t sos_len = (uint16_t)(6 + 2 * (uint16_t)scan_components);
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
      // A DC scan names every component, so this is sized for B.2.3's widest
      // scan rather than for the three-component case it used to assume.
      unsigned char sos[4 + 2 * GIMG_JPEG_MAX_SCAN_COMPONENTS];
      memset(sos, 0, sizeof(sos));
      sos[0] = (unsigned char)scan_components;
      // Td is the high nibble, Ta the low one (T.81 B.2.3).  A DC scan needs
      // only Td; an AC scan only Ta.  Component ids are 1..Nf in SOF order.
      if (split) {
        // Td and Ta must name the tables the entropy coder actually used.  A
        // single-component scan is encoded through the one-component path,
        // which uses the luminance tables for whichever component it is given,
        // so both are 0 here regardless of the component - T.81 B.2.3 lets any
        // component select any table, so this is well formed, and the
        // alternative (naming table 1 for chroma while encoding with table 0)
        // produces a file that decodes to nonsense.  An AC refinement scan
        // uses the dedicated refinement table written at Th=2.
        unsigned char td_ta = is_ac_scan ? (ac_refine ? 0x02 : 0x00) : 0x00;
        sos[1] = (unsigned char)(comp + 1);
        sos[2] = td_ta;
      }
      else {
        for (int i = 0; i < num_components; i++) {
          sos[1 + i * 2] = (unsigned char)(i + 1);
          sos[2 + i * 2] =
              (unsigned char)(gimg_jpeg_tbl_of(tbl_sel, i) << 4);
        }
      }
      size_t tail = 1 + 2 * (size_t)scan_components;
      sos[tail] = scans[s].Ss;
      sos[tail + 1] = scans[s].Se;
      sos[tail + 2] =
          (unsigned char)((scans[s].Ah << 4) | (scans[s].Al & 0x0F));
      r = gimg_stream_write(stream, sos, tail + 3, &written);
      if (r != GIMG_OK) {
        gimg_free(alloc, scan_data);
        return r;
      }
      n += written;
      r = jpeg_write_scan_data_with_stuffing(stream, scan_data, scan_size, &n);
      gimg_free(alloc, scan_data);
      if (r != GIMG_OK) {
        return r;
      }
    }
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

static GIMG_Result jpeg_save_body(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report,
    GCOL_Color_Info * out_color, void ** out_icc_copy) {
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

  // Prefer a raster already attached to the item - the load, modify,
  // set_raster, save case - and otherwise decode one and own it for the
  // duration of the save.
  //
  // This used to decode only when this codec had loaded the document, which
  // made every conversion into a JPEG fail: a PNG or BMP loaded and saved as
  // a JPEG has no raster on its item and was refused with
  // GIMG_ERR_UNSUPPORTED unless the caller decoded by hand first.  Nothing
  // needed the guard - gimg_item_decode dispatches to whichever codec loaded
  // the document - and the PNG and BMP writers never had it.
  GIMG_Raster * raster = gimg_item_raster(item);
  int raster_owned = 0;
  if (!raster) {
    GIMG_Result r = gimg_item_decode(item, NULL, &raster);
    if (r != GIMG_OK || !raster) {
      return (r != GIMG_OK) ? r : GIMG_ERR_UNSUPPORTED;
    }
    raster_owned = 1;
  }

  // T.81 Table B.2: a DCT-based frame carries 8- or 12-bit samples.  Precision
  // up to 16 exists only in a lossless frame (SOF3), so a request for 16
  // without jpeg_lossless_predictor is refused rather than quietly downgraded.
  // A lossless frame is not bound by the DCT precision rules below; declared
  // here because they need to know.
  int lossless_psv = (options && options->jpeg_lossless_predictor)
      ? (int)options->jpeg_lossless_predictor
      : 0;
  // T.81 Annex J.  Refused rather than silently ignored when combined with
  // something it cannot be: a hierarchical sequence here is built out of the
  // sequential DCT process at 8 bits, and each of these asks for a different
  // process for the frames.
  // T.81 B.2.4.3: the arithmetic coder's conditioning, built once here and
  // handed to every writer and every entropy call below.  It used to be built
  // in nine separate places from the defaults, with four more places spelling
  // the same two bytes into a DAC payload by hand, so nothing a caller said
  // could reach it.  Validated here, before a byte is written: conditioning a
  // conforming decoder would reject is refused rather than clamped.
  jpeg_arith_cond_t arith_cond;
  {
    GIMG_Result cr = gimg_jpeg_cond_from_options(options, &arith_cond);
    if (cr != GIMG_OK) {
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      return cr;
    }
  }

  int hier_levels = (options && options->jpeg_hierarchical_levels)
      ? (int)options->jpeg_hierarchical_levels
      : 0;
  // A hierarchical sequence is built out of the sequential, progressive or
  // lossless process (B.3.1 requires every frame to use the same one), and all
  // three are written here.  What it cannot do is change precision: the
  // pyramid's reconstruction is built at 8 bits.
  if (hier_levels != 0 &&
      (options && options->jpeg_precision != 0 &&
          options->jpeg_precision != 8)) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  // T.81 A.2.3: one non-interleaved scan per component instead of one
  // interleaved scan.  Refused where the standard already fixes the scan
  // arrangement or where a different writer owns it: Annex G's progressive
  // scans carry their own script and its AC scans are non-interleaved by
  // G.1.2.2 already, a lossless frame goes through the Annex H writer, and a
  // hierarchical sequence writes its frames itself.
  int non_interleaved =
      (options && options->jpeg_non_interleaved) ? 1 : 0;
  if (non_interleaved &&
      (lossless_psv != 0 || hier_levels != 0 ||
          (options && options->jpeg_progressive))) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  // B.2.3 Table B.3 caps Ns at 4 however wide the frame is, so a frame of more
  // than four components cannot be written as one interleaved scan at all: it
  // is split whether or not the caller asked for it (A.2.3).  Not a refusal,
  // because there is nothing wrong with the request - there is only one way to
  // honor it.
  {
    const GIMG_Pixel_Format * wide_fmt = gimg_raster_format(raster);
    // Not for a lossless frame or a hierarchical sequence: each of those
    // writes its own scans and splits them itself where B.2.3 requires it.
    if (wide_fmt &&
        wide_fmt->channel_count > (uint8_t)GIMG_JPEG_MAX_SCAN_COMPONENTS &&
        wide_fmt->channel_model == GIMG_CHANNEL_UNKNOWN &&
        !(options && options->jpeg_progressive) && lossless_psv == 0 &&
        hier_levels == 0) {
      non_interleaved = 1;
    }
  }
  if (hier_levels < 0 || hier_levels + 1 > (int)GIMG_JPEG_MAX_FRAMES) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  if (options && options->jpeg_precision == 16 && lossless_psv == 0) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }

  // Convert when the caller asked for a precision the raster is not already in.
  // A 16-bit raster has no matching JPEG precision in a DCT-based frame, so it
  // becomes 12-bit even when the caller expressed no preference.
  //
  // None of that applies to a lossless frame: T.81 Table B.2 allows P from 2 to
  // 16 there, so a 16-bit raster is written at 16 bits and narrowing it would
  // throw away exactly what the caller asked to keep.
  if (lossless_psv == 0) {
    uint8_t want_bits = 0;
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    uint8_t have_bits = fmt && fmt->channel_count > 0
        ? fmt->bits_per_channel[0]
        : 0;
    if (options && (options->jpeg_precision == 8 ||
                       options->jpeg_precision == 12)) {
      want_bits = options->jpeg_precision;
    }
    else if (have_bits == 16) {
      want_bits = 12;
    }
    if (want_bits != 0 && have_bits != want_bits &&
        (have_bits == 8 || have_bits == 12 || have_bits == 16)) {
      GIMG_Raster * converted = NULL;
      GIMG_Result r_conv =
          gimg_ops_convert_bit_depth(raster, want_bits, &converted);
      // A conversion that failed used to be dropped here, with no else: the
      // frame was then written at the precision the raster already had, and
      // the save reported GIMG_OK.  A caller who asked for eight bits got
      // twelve and was told nothing - and the way to reach it is an
      // allocation failure, which is to say the case where the caller most
      // needs to be told.
      if (r_conv != GIMG_OK || !converted) {
        if (raster_owned) {
          gimg_raster_destroy(raster);
        }
        return r_conv != GIMG_OK ? r_conv : GIMG_ERR_OOM;
      }
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      raster = converted;
      raster_owned = 1;
    }
  }

  // A CMYK raster that says its samples are the other way round is
  // complemented here, because a JPEG's four components are the Adobe
  // convention and writing the samples as they stand would produce a
  // photographic negative of the picture the caller labelled.
  {
    GIMG_Raster * flipped = NULL;
    GIMG_Result fr = gimg_jpeg_cmyk_to_file_polarity(raster, &flipped);
    if (fr != GIMG_OK) {
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      return fr;
    }
    if (flipped) {
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      raster = flipped;
      raster_owned = 1;
    }
  }

  const GIMG_Allocator * alloc = codec->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // What the APP2 writer will say about colour, captured here because the
  // raster does not live that long: the encode below produces the scan data
  // and then destroys the raster, and the APP segments are written after
  // that.  ASan caught the difference as a use-after-free the first time this
  // writer read a raster's colour, on a lossless file whose raster the save
  // had decoded for itself.
  //
  // Only the profile needs copying; the rest of GCOL_Color_Info is values.
  // The copy belongs to gimg_jpeg_save, which frees it however this returns -
  // this function has thirty-nine exits and is no place for a second thing to
  // remember.
  {
    const GCOL_Color_Info * raster_color = gimg_raster_color_info_const(raster);
    if (raster_color) {
      *out_color = *raster_color;
      out_color->icc_bytes = NULL;
      out_color->icc_size = 0;
      if (raster_color->icc_bytes && raster_color->icc_size > 0) {
        void * copy = gimg_malloc(alloc, raster_color->icc_size);
        if (!copy) {
          if (raster_owned) {
            gimg_raster_destroy(raster);
          }
          return GIMG_ERR_OOM;
        }
        memcpy(copy, raster_color->icc_bytes, raster_color->icc_size);
        *out_icc_copy = copy;
        out_color->icc_bytes = copy;
        out_color->icc_size = raster_color->icc_size;
      }
    }
  }

  unsigned quality = GIMG_JPEG_DEFAULT_QUALITY;
  if (options && options->jpeg_quality != 0) {
    quality = options->jpeg_quality;
    if (quality > 100) {
      quality = 100;
    }
  }

  unsigned chroma_subsampling =
      (options && options->jpeg_chroma_subsampling <= 2)
      ? options->jpeg_chroma_subsampling
      : (unsigned)CHROMA_420;
  // T.81 puts no color space in a frame; for four components the Adobe APP14
  // marker is the whole of it.  See GIMG_Save_Options.jpeg_cmyk_transform.
  unsigned cmyk_transform =
      (options && options->jpeg_cmyk_transform) ? options->jpeg_cmyk_transform
                                                : 0u;
  if (cmyk_transform != 0u && cmyk_transform != 2u) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  // T.81 B.4: the abbreviated formats.  Refused where they cannot mean
  // anything: a hierarchical sequence's frames carry their own tables (B.3.1),
  // and a lossless frame's Huffman table is generated from the very
  // coefficients it codes, so it cannot be written before them.
  unsigned abbreviated =
      (options && options->jpeg_abbreviated) ? options->jpeg_abbreviated : 0u;
  if (abbreviated > 2u ||
      (abbreviated != 0u && (hier_levels != 0 || lossless_psv != 0))) {
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    return GIMG_ERR_UNSUPPORTED;
  }
  bool progressive = (options && options->jpeg_progressive) ? true : false;
  bool arithmetic = (options && options->jpeg_arithmetic) ? true : false;
  GIMG_Result r;
  if (lossless_psv != 0) {
    if (lossless_psv > 7) {
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      return GIMG_ERR_UNSUPPORTED; // T.81 Table H.1 defines 1..7
    }
    if (progressive) {
      // T.81 puts progression in the DCT-based processes only: there is no
      // progressive lossless frame to write.  Arithmetic is fine - that is
      // SOF11, Table B.1.
      if (raster_owned) {
        gimg_raster_destroy(raster);
      }
      return GIMG_ERR_UNSUPPORTED;
    }
  }
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
  uint8_t h_samp[GIMG_JPEG_MAX_COMPONENTS] = {1, 1, 1, 1, 1, 1, 1, 1};
  uint8_t v_samp[GIMG_JPEG_MAX_COMPONENTS] = {1, 1, 1, 1, 1, 1, 1, 1};
  // T.81 B.2.2/B.2.3: which table set each component names.  Filled by
  // jpeg_raster_to_scan_data and used by every writer below, so that the
  // frame header, the scan header and the entropy coder agree.
  uint8_t tbl_sel[GIMG_JPEG_MAX_COMPONENTS] = {0, 1, 1, 1, 1, 1, 1, 1};
  // -1 unless the frame needs an Adobe APP14 marker (four components).
  int adobe_transform = -1;
  // Set when the metadata pass re-emitted a preserved Adobe marker, so that
  // the fallback below does not write a second one.
  int adobe_marker_written = 0;
  int precision = 8;
  // T.81 Annex H: one interleaved scan, or one per component when the frame is
  // wider than B.2.3 lets a scan name.
  gimg_jpeg_lossless_scan_t lossless_scans[GIMG_JPEG_MAX_COMPONENTS];
  unsigned lossless_num_scans = 0;
  memset(lossless_scans, 0, sizeof(lossless_scans));
  // T.81 Annex J: a pyramid of frames rather than one.  Built here, before the
  // markers are written, because each frame depends on the reconstruction of
  // the one before it and they cannot be produced as they are emitted.
  gimg_jpeg_enc_frame_t hier_frames[GIMG_JPEG_MAX_FRAMES];
  unsigned hier_num_frames = 0;
  // Whether the sequence's three components are color, which decides whether
  // it needs an Adobe marker saying they are not.
  int hier_is_color = 1;
  memset(hier_frames, 0, sizeof(hier_frames));
  if (hier_levels != 0) {
    gimg_jpeg_default_quant_scaled(quality, quant_luma, quant_chroma);
    width = gimg_raster_width(raster);
    height = gimg_raster_height(raster);
    precision = 8;
    // B.3.1: every frame of a sequence uses the same process, so the option
    // names it once for the whole pyramid.
    {
      const GIMG_Pixel_Format * hf = gimg_raster_format(raster);
      hier_is_color = hf && (hf->channel_model == GIMG_CHANNEL_RGB ||
                                 hf->channel_model == GIMG_CHANNEL_RGBA);
    }
    gimg_jpeg_hier_process_t hier_process = GIMG_JPEG_HIER_SEQUENTIAL;
    if (lossless_psv != 0) {
      hier_process = GIMG_JPEG_HIER_LOSSLESS;
    }
    else if (options && options->jpeg_progressive) {
      hier_process = GIMG_JPEG_HIER_PROGRESSIVE;
    }
    r = gimg_jpeg_encode_hierarchical(alloc, raster, hier_levels, arithmetic,
        hier_process, lossless_psv, &arith_cond, fdct_method,
        restart_interval, quant_luma, quant_chroma, hier_frames,
        &hier_num_frames, &num_components, &precision);
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    if (r != GIMG_OK) {
      return r;
    }
    goto have_scan;
  }
  if (lossless_psv != 0) {
    // T.81 does not say a restart interval must begin at the start of a row,
    // but a lossless interval resets the prediction, and what "the first row of
    // the interval" means when an interval starts mid-row is not defined
    // anywhere.  Implementations resolve that by requiring alignment - libjpeg
    // refuses to decode an unaligned one outright - so round down to whole
    // rows rather than write a file the most widely used decoder rejects.
    if (restart_interval > 0) {
      uint32_t rw = gimg_raster_width(raster);
      if (rw > 0) {
        uint32_t rows = restart_interval / rw;
        uint32_t snapped = rows > 0 ? rows * rw : rw;
        restart_interval =
            (snapped > 0xFFFFu) ? (uint16_t)0 : (uint16_t)snapped;
      }
    }
    r = gimg_jpeg_encode_lossless(alloc, raster, lossless_psv, restart_interval,
        arithmetic, &arith_cond, lossless_scans, &lossless_num_scans, &width,
        &height, &num_components, &precision);
    if (raster_owned) {
      gimg_raster_destroy(raster);
    }
    if (r != GIMG_OK) {
      return r;
    }
    goto have_scan;
  }
  // A non-interleaved frame is written from the coefficient buffer rather than
  // from a finished scan, because it needs the blocks again, one component at
  // a time.  That is the same buffer the progressive path asks for.
  r = jpeg_raster_to_scan_data(alloc, raster, quality, chroma_subsampling,
      progressive || non_interleaved, arithmetic, &arith_cond, restart_interval,
      fdct_method, quant_method, cmyk_transform,
      &scan_data,
      &scan_size, &coef_buffer, &total_blocks, quant_luma, quant_chroma, &width,
      &height, &num_components, h_samp, v_samp, tbl_sel, &precision,
      &adobe_transform);
  if (raster_owned) {
    gimg_raster_destroy(raster);
  }
  if (r != GIMG_OK) {
    return r;
  }
have_scan:
  // Use progressive image body when we have coefficient buffer (8-bit progressive, or 12/16-bit
  // which use coef path for both baseline and progressive).
  bool use_progressive_body = (coef_buffer != NULL) && !non_interleaved;
  if (use_progressive_body) {
    if (!coef_buffer) {
      return GIMG_ERR_OOM;
    }
  }
  else if (non_interleaved) {
    // The scans have not been encoded yet; the coefficients are what must have
    // survived.
    if (!coef_buffer) {
      return GIMG_ERR_OOM;
    }
  }
  else if (hier_levels == 0 && lossless_psv == 0) {
    // A zero-byte arithmetic scan is legitimate; see jpeg_raster_to_scan_data.
    if (!scan_data && !(arithmetic && scan_size == 0)) {
      return GIMG_ERR_OOM;
    }
  }
  // The lossless path keeps its scans in lossless_scans and frees them itself;
  // scan_data is NULL there and there is nothing for this to own.
  void * to_free = (use_progressive_body || non_interleaved)
      ? (void *)coef_buffer
      : (void *)scan_data;

  size_t written = 0;
  r = gimg_stream_write(
      stream, gimg_jpeg_signature, GIMG_JPEG_SIGNATURE_LEN, &written);
  if (r != GIMG_OK) {
    goto done;
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
                goto done;
              }
              offset += plen;
            }
          }
          gimg_free(alloc, com_buf);
        }
        if (r != GIMG_OK) {
          goto done;
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
              goto done;
            }
          }
        }
      }
    }

    // APP0: from meta_raw when preserving and present, else minimal (JFIF).
    //
    // Not for a three-component lossless frame, though.  JFIF declares
    // three-component data to be YCbCr, and a decoder that sees a JFIF marker
    // takes it at its word ahead of anything the Adobe marker says - libjpeg
    // does exactly that in jdmaster.c.  A lossless frame stores RGB, because
    // the YCbCr conversion is not reversible, so a JFIF marker here is simply
    // false: it made libjpeg refuse the file with "unsupported color
    // conversion request".  libjpeg's own lossless RGB output carries the
    // Adobe marker and no JFIF, and so does ours now.
    // A four-component frame gets no JFIF marker either.  JFIF describes
    // grayscale or YCbCr data and says nothing about four components; libjpeg
    // writes none for a CMYK or YCCK file (jcparam.c sets write_JFIF_header
    // for JCS_GRAYSCALE and JCS_YCbCr only), and the Adobe marker below is
    // what carries the color instead.
    // JFIF declares three-component data to be YCbCr and says nothing about
    // any other count, so it is written only for the frames it describes.
    // adobe_transform >= 0 marks a frame that carries its meaning in an Adobe
    // marker instead, and the two must not contradict each other: a decoder
    // that sees JFIF takes it at its word ahead of Adobe.
    int suppress_jfif = (lossless_psv != 0 && num_components == 3) ||
        num_components == 4 || num_components == 2 || num_components > 4 ||
        adobe_transform >= 0 ||
        // A hierarchical sequence writes its own Adobe marker where its three
        // components are not YCbCr, and JFIF beside it would contradict it: a
        // decoder that sees JFIF takes it at its word first.
        (hier_levels != 0 && !hier_is_color);
    size_t app0_len = 0;
    bool have_app0 = !suppress_jfif &&
        (policy != GIMG_META_DROP_ALL &&
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
          jpeg_apply_aspect_to_app0(app0_buf, app0_len, doc, x_dpi, y_dpi);
          r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP0, app0_buf,
              app0_len, &report->bytes_written);
        }
        gimg_free(alloc, app0_buf);
      }
      if (r != GIMG_OK) {
        goto done;
      }
    }
    else if (!suppress_jfif) {
      unsigned char app0[14];
      jpeg_build_minimal_app0(app0, x_dpi, y_dpi);
      jpeg_apply_aspect_to_app0(app0, sizeof(app0), doc, x_dpi, y_dpi);
      r = jpeg_write_app_segment(
          stream, GIMG_JPEG_MARKER_APP0, app0, 14, &report->bytes_written);
      if (r != GIMG_OK) {
        goto done;
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
          goto done;
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
          // The second item is a thumbnail only in a document this codec
          // loaded - where it came out of IFD1 - or one built by hand for
          // that purpose, which has no loading codec at all.  In a GIF or an
          // APNG it is frame two, and encoding an animation frame into an
          // EXIF thumbnail is a claim the source never made.
          //
          // This used to gate only the *decode*, and take an already-attached
          // raster from any document at all.  A caller who decoded the frames
          // before saving - the ordinary way to walk an animation - therefore
          // got frame two embedded as a full-resolution "thumbnail", and one
          // who did not got a clean file from the same document and the same
          // options.  On a 49-frame 1200x1200 GIF that was an 87 KB JPEG
          // against a 35 KB one, the difference being a 51,905-byte APP1; on
          // eight larger animations the segment passed the 65533 bytes APP1
          // can hold and the whole save failed with GIMG_ERR_LIMIT.
          //
          // So the rule the comment always described now gates the use and
          // not just the decode.
          const bool second_item_is_a_thumbnail =
              doc->loaded_by_codec == NULL ||
              doc->loaded_by_codec == (struct GIMG_Codec *)codec;
          GIMG_Item * thumb_item = second_item_is_a_thumbnail
              ? gimg_doc_item((GIMG_Doc *)doc, 1)
              : NULL;
          GIMG_Raster * thumb_raster =
              thumb_item ? gimg_item_raster(thumb_item) : NULL;
          bool thumb_raster_owned = false;
          if (!thumb_raster && thumb_item) {
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
              // The JFIF thumbnail stays Huffman whatever the main image
              // uses: it is read by viewers that may know nothing of Annex D,
              // and it is too small for the difference to matter.
              // Huffman, so the conditioning never reaches the bytes; the
              // thumbnail is read by viewers that may know nothing of Annex D.
              r = jpeg_raster_to_scan_data(alloc, thumb_raster, thumb_quality,
                  CHROMA_444, false, false, &arith_cond, 0, fdct_method,
                  quant_method, 0u,
                  &thumb_scan,
                  &thumb_scan_size, NULL, NULL, tq_luma, tq_chroma, &tw, &th,
                  &tnc, NULL, NULL, NULL, &thumb_prec, NULL);
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
                    // The thumbnail is Huffman-coded, so its conditioning
                    // is never written; it is passed for the signature's sake.
                    r = jpeg_write_image_body(mem_stream, tw, th, tnc, NULL,
                        NULL, NULL, tq_luma, tq_chroma, thumb_scan,
                        thumb_scan_size, 8, 0, false, &arith_cond, NULL, alloc,
                        0u, &mem_n);
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
                // An Exif policy we cannot apply fails the save: writing
                // the original through would hand back exactly what the
                // caller asked to remove, and dropping the segment would
                // destroy metadata whose only fault is being unparseable.
                //
                // The stored APP1 payload is "Exif\0\0" followed by the TIFF
                // structure, and the policies parse TIFF - so the identifier
                // comes off before the call and goes back on after. Handing
                // over the whole payload made every policy read buf[2] as 'i'
                // rather than 42 and answer CORRUPT, which the `if (... ==
                // GIMG_OK)` this replaced then discarded. STRIP_GPS has never
                // actually run on a JPEG.
                static const unsigned char exif_prefix[] = {
                    'E', 'x', 'i', 'f', 0, 0};
                unsigned char * prefixed = NULL;
                if (policy == GIMG_META_STRIP_GPS ||
                    policy == GIMG_META_NORMALIZE_EXIF) {
                  if (exif_size <= sizeof(exif_prefix) ||
                      memcmp(exif_buf, exif_prefix, sizeof(exif_prefix)) != 0) {
                    // The loader only files a payload here when it begins
                    // with the identifier, so this is unreachable from a
                    // loaded file; if it ever is reached, the payload is not
                    // what this code believes and it must not be edited.
                    r = GIMG_ERR_CORRUPT;
                  }
                  else if (policy == GIMG_META_STRIP_GPS) {
                    r = gimg_exif_strip_gps(alloc, exif_buf + sizeof(exif_prefix),
                        exif_size - sizeof(exif_prefix), &modified,
                        &modified_size);
                  }
                  else {
                    r = gimg_exif_normalize(alloc, exif_buf + sizeof(exif_prefix),
                        exif_size - sizeof(exif_prefix), &modified,
                        &modified_size);
                  }
                  if (r == GIMG_OK && modified) {
                    const size_t n = sizeof(exif_prefix) + modified_size;
                    prefixed = (unsigned char *)gimg_malloc(alloc, n);
                    if (!prefixed) {
                      r = GIMG_ERR_OOM;
                    }
                    else {
                      memcpy(prefixed, exif_prefix, sizeof(exif_prefix));
                      memcpy(prefixed + sizeof(exif_prefix), modified,
                          modified_size);
                      to_write = prefixed;
                      to_write_size = n;
                    }
                  }
                }
                if (r == GIMG_OK) {
                  r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP1,
                      to_write, to_write_size, &report->bytes_written);
                }
                if (prefixed) {
                  gimg_free(alloc, prefixed);
                }
                if (modified) {
                  gimg_free(alloc, modified);
                }
              }
              gimg_free(alloc, exif_buf);
            }
          }
        }
        if (r != GIMG_OK) {
          goto done;
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
          goto done;
        }
      }

      // APP2 ICC: multi-segment (CHUNKS) or single segment (APP2_ICC).
      bool wrote_icc_from_raw = false;
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
          goto done;
        }
        wrote_icc_from_raw = true;
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
            goto done;
          }
          wrote_icc_from_raw = true;
        }
      }

      // A profile the raster carries but the file did not.  The segment the
      // file came with wins, the same way the APP0 density does; this is only
      // a way of not losing a profile that arrived from somewhere else, such
      // as a BMP's V5 embedded one or a PNG's iCCP.
      if (!wrote_icc_from_raw && policy != GIMG_META_KEEP_RAW_ONLY) {
        r = jpeg_write_icc_from_info(
            stream, out_color, num_components, alloc, report);
        if (r != GIMG_OK) {
          goto done;
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
          goto done;
        }
      }
      // APP14: preserved from the source, but its transform byte describes the
      // color space of the frame it accompanies, and that is this encoder's
      // frame now, not the one it came from.  A source that carried RGB with
      // transform 0 re-encodes here as YCbCr, and copying the marker across
      // unchanged leaves the file saying two contradictory things at once -
      // harmless to a decoder that reads JFIF first, as this one and libjpeg
      // do, but wrong for one that trusts Adobe.  A three-component lossless
      // frame is skipped entirely: it keeps RGB, and its body writes the
      // matching marker itself, so re-emitting this one would put two Adobe
      // segments in the file.
      size_t app14_size = 0;
      const int lossless_rgb_writes_its_own =
          (lossless_psv != 0 && num_components == 3);
      if (!lossless_rgb_writes_its_own && meta_raw &&
          gimg_meta_raw_get(meta_raw, "jpeg", GIMG_JPEG_RAW_APP14, NULL,
              &app14_size) == GIMG_OK &&
          app14_size > 0) {
        unsigned char * app14_buf =
            (unsigned char *)gimg_malloc(alloc, app14_size);
        if (app14_buf) {
          r = gimg_meta_raw_get(
              meta_raw, "jpeg", GIMG_JPEG_RAW_APP14, app14_buf, &app14_size);
          if (r == GIMG_OK) {
            // The payload is "Adobe\0", version, flags0, flags1, transform -
            // the transform is the twelfth byte (index 11).
            if (app14_size >= 12u && memcmp(app14_buf, "Adobe\0", 6) == 0) {
              if (num_components == 3) {
                app14_buf[11] = 1u; // YCbCr, which is what was written above.
              }
              else if (adobe_transform >= 0) {
                // Four components: the transform this frame was actually
                // written with, which is the only thing that says whether the
                // first three are CMY or YCbCr.
                app14_buf[11] = (unsigned char)adobe_transform;
              }
            }
            r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP14,
                app14_buf, app14_size, &report->bytes_written);
            adobe_marker_written = 1;
          }
          gimg_free(alloc, app14_buf);
        }
        if (r != GIMG_OK) {
          goto done;
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
                  goto done;
                }
                off += plen;
              }
            }
            gimg_free(alloc, unknown_buf);
          }
        }
        if (r != GIMG_OK) {
          goto done;
        }
      }
    }
    else if (policy == GIMG_META_KEEP_COMMON_ONLY) {
      // KEEP_COMMON_ONLY drops the segments the file arrived with, but a
      // color space is not one of them: it describes what the samples mean,
      // and the PNG writer keeps it under this policy for the same reason.
      // APP2 is the only place a JPEG can say it.
      r = jpeg_write_icc_from_info(
          stream, out_color, num_components, alloc, report);
      if (r != GIMG_OK) {
        goto done;
      }
    }
  }

  // A four-component frame that did not bring an Adobe marker with it gets one
  // written here.  T.81 describes no color space, so for four components this
  // marker is the whole of it: without it a decoder has only libjpeg's
  // fallback to go on (jdapimin.c reads four components with no Adobe marker
  // as CMYK), and a YCCK frame would then be read as CMYK and come out wrong.
  // It is written whatever the metadata policy says, because it is not
  // metadata - dropping it changes what the pixels mean.  B.2.4 allows a
  // table-or-misc segment anywhere before the frame header.
  if (!adobe_marker_written && adobe_transform >= 0) {
    unsigned char app14_new[12];
    memcpy(app14_new, "Adobe", 5);
    app14_new[5] = 0x00;
    app14_new[6] = 100; // version, as libjpeg writes it
    app14_new[7] = 0x00;
    app14_new[8] = 0x00; // flags0
    app14_new[9] = 0x00;
    app14_new[10] = 0x00; // flags1
    app14_new[11] = (unsigned char)adobe_transform;
    r = jpeg_write_app_segment(stream, GIMG_JPEG_MARKER_APP14, app14_new,
        sizeof(app14_new), &report->bytes_written);
    if (r != GIMG_OK) {
      goto done;
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
        num_components > 1 ? h_samp : NULL,
        num_components > 1 ? v_samp : NULL, tbl_sel, quant_luma, quant_chroma,
        coef_buffer, total_blocks, scans, scan_count, precision, arithmetic,
        &arith_cond, alloc, restart_interval, abbreviated,
        &report->bytes_written);
  }
  else if (hier_levels != 0) {
    r = jpeg_write_image_body_hierarchical(stream, width, height,
        num_components, tbl_sel, quant_luma, quant_chroma, hier_frames,
        hier_num_frames, restart_interval, arithmetic, &arith_cond,
        precision, hier_is_color, &report->bytes_written);
  }
  else if (lossless_psv != 0) {
    r = jpeg_write_image_body_lossless(stream, width, height, num_components,
        precision, lossless_psv, arithmetic, &arith_cond, lossless_scans,
        lossless_num_scans, restart_interval, &report->bytes_written);
  }
  else {
    r = jpeg_write_image_body(stream, width, height, num_components,
        num_components > 1 ? h_samp : NULL,
        num_components > 1 ? v_samp : NULL, tbl_sel, quant_luma, quant_chroma,
        scan_data, scan_size, precision, restart_interval, arithmetic,
        &arith_cond, non_interleaved ? coef_buffer : NULL, alloc, abbreviated,
        &report->bytes_written);
  }

done:
  // One exit, holding everything the encode step produced.
  //
  // This used to be a `gimg_free(alloc, to_free)` repeated at each of the
  // eighteen points where writing a segment could fail, and each of the four
  // writer branches freeing its own state at the end.  Between them they
  // covered the DCT paths and nothing else: `to_free` is NULL for a lossless
  // frame and for a hierarchical sequence, whose scans and per-frame buffers
  // were freed only by the branch that wrote them - so a sink that failed
  // anywhere in the header, a marker before the frame, leaked the entire
  // encoded image.  A budget-limited stream finds it at the first byte.
  gimg_free(alloc, to_free);
  gimg_jpeg_free_enc_frames(alloc, hier_frames, hier_num_frames);
  for (unsigned si = 0; si < lossless_num_scans; si++) {
    gimg_free(alloc, lossless_scans[si].data);
    gimg_free(alloc, lossless_scans[si].dht);
  }
  return r;
}

/**
 * Save a document as a JPEG.
 *
 * A thin owner around jpeg_save_body: the colour the APP2 writer states has
 * to outlive the raster it came from, because the raster is destroyed as soon
 * as the scan data exists and the APP segments are written after that.  The
 * body fills in the colour and hands back the one allocation behind it, which
 * is freed here however the body returned.
 */
GIMG_Result gimg_jpeg_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  GCOL_Color_Info color;
  gcol_color_info_default(&color);
  void * icc_copy = NULL;
  GIMG_Result r = jpeg_save_body(
      codec, doc, stream, format_name, options, report, &color, &icc_copy);
  gimg_free(codec ? gimg_alloc_or_default(codec->allocator) : NULL, icc_copy);
  return r;
}
