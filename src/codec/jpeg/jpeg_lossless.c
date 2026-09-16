/**
 * @file
 *
 * Lossless JPEG: the predictive coding process of ITU-T T.81 Annex H (SOF3 with
 * Huffman coding, SOF11 with arithmetic).
 *
 * This is a different coding process from everything else in this codec, not a
 * variation on it.  There is no DCT, no quantisation and no 8x8 block: each
 * sample is predicted from its already-decoded neighbours, and the difference
 * between the prediction and the sample is what gets entropy-coded.  The
 * reconstruction is exact, which is the point.
 *
 * It is also where sample precisions other than 8 and 12 live.  T.81 Table B.2
 * allows P from 2 to 16 in a lossless frame, against exactly 8 or 12 in a
 * DCT-based one - so a 16-bit JPEG is a real thing, just not a 16-bit *DCT*
 * JPEG.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <stdlib.h>
#include <string.h>

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "jpeg_internal.h"

/**
 * Widen a P-bit sample to @p to bits by replicating its high bits.
 *
 * The same rule the rest of the library uses (src/ops/bitdepth.c), generalised:
 * a lossless frame may declare any precision from 2 to 16, so the fixed 8-to-16
 * and 12-to-16 helpers are not enough.  Replication maps the full source range
 * onto the full destination range - all-ones stays all-ones - which
 * left-justification does not.
 */
static uint32_t jpeg_lossless_widen(uint32_t v, int from, int to) {
  if (from >= to) {
    return v >> (from - to);
  }
  uint32_t r = v;
  int have = from;
  while (have < to) {
    int take = to - have;
    if (take > from) {
      take = from;
    }
    r = (r << take) | (v >> (from - take));
    have += take;
  }
  return r;
}

/** Predict a sample from its neighbours (T.81 H.1.2.1, Table H.1). */
static int32_t jpeg_lossless_predict(
    int psv, int32_t ra, int32_t rb, int32_t rc) {
  switch (psv) {
  case 1:
    return ra;
  case 2:
    return rb;
  case 3:
    return rc;
  case 4:
    return ra + rb - rc;
  case 5:
    return ra + ((rb - rc) >> 1);
  case 6:
    return rb + ((ra - rc) >> 1);
  case 7:
    return (ra + rb) >> 1;
  default:
    return 0; // psv 0 is differential-frame only (H.1.2.1); rejected earlier
  }
}

/**
 * Decode one difference value (T.81 H.1.2.2).
 *
 * The categories are the DC ones of F.1.2.1 extended by one: SSSS runs to 16,
 * and 16 is a special case that carries no additional bits and always means
 * 32768.  That is the only value which would otherwise need seventeen bits to
 * distinguish from its negative counterpart.
 */
static GIMG_Result jpeg_lossless_decode_diff(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * tbl, int32_t * out_diff) {
  int s = jpeg_huff_decode(bs, tbl, 0, 1, 0);
  if (s < 0 || s > 16) {
    return GIMG_ERR_CORRUPT;
  }
  if (s == 0) {
    *out_diff = 0;
    return GIMG_OK;
  }
  if (s == 16) {
    *out_diff = 32768;
    return GIMG_OK;
  }
  int32_t bits = (int32_t)jpeg_bitstream_read_bits(bs, s);
  // EXTEND (T.81 Figure F.12): a value whose top bit is clear is negative.
  if (bits < (1 << (s - 1))) {
    bits += (int32_t)(((uint32_t)-1) << s) + 1;
  }
  *out_diff = bits;
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_decode_lossless(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!state || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  const gimg_jpeg_sof_t * sof = &state->sof;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);
  uint32_t width = sof->width;
  uint32_t height = sof->height;
  uint8_t num_comp = sof->num_components;
  int precision = (int)sof->precision;

  if (state->num_scans == 0) {
    return GIMG_ERR_CORRUPT;
  }
  const gimg_jpeg_scan_t * scan = &state->scans[0];
  if (!scan->data || scan->data_size == 0) {
    return GIMG_ERR_CORRUPT;
  }
  // T.81 H.1: the predictor is carried in Ss and the point transform in Al.
  int psv = (int)scan->ss;
  int pt = (int)scan->al;
  if (psv < 1 || psv > 7) {
    return GIMG_ERR_UNSUPPORTED; // 0 is differential-frame only
  }
  if (precision < 2 || precision > 16 || pt < 0 || pt >= precision) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (num_comp < 1 || num_comp > GIMG_JPEG_MAX_COMPONENTS) {
    return GIMG_ERR_CORRUPT;
  }
  if (scan->comp_count != num_comp) {
    // A lossless scan that does not carry every component would need the
    // multi-scan machinery the DCT path has; refuse rather than guess.
    return GIMG_ERR_UNSUPPORTED;
  }

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count(width, height, &pixel_count) != GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Limits * limits = options && options->limits ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  uint8_t h_max = 1, v_max = 1;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (sof->h_samp[i] > h_max) {
      h_max = sof->h_samp[i];
    }
    if (sof->v_samp[i] > v_max) {
      v_max = sof->v_samp[i];
    }
  }

  // In a lossless frame an MCU is made of samples, not 8x8 blocks (T.81 H.1.1),
  // so the MCU grid is the sample grid divided by the sampling factors rather
  // than by eight times them.
  uint32_t mcu_per_row = (width + h_max - 1u) / h_max;
  uint32_t mcu_per_col = (height + v_max - 1u) / v_max;

  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  uint16_t * plane[GIMG_JPEG_MAX_COMPONENTS];
  memset(plane, 0, sizeof(plane));
  GIMG_Result r = GIMG_OK;
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_w[i] = mcu_per_row * sof->h_samp[i];
    comp_h[i] = mcu_per_col * sof->v_samp[i];
    size_t n = 0;
    if (!gcu_safe_mul_size((size_t)comp_w[i], (size_t)comp_h[i], &n) ||
        n > SIZE_MAX / sizeof(uint16_t)) {
      r = GIMG_ERR_LIMIT;
      goto fail;
    }
    plane[i] = (uint16_t *)gimg_malloc(alloc, n * sizeof(uint16_t));
    if (!plane[i]) {
      r = GIMG_ERR_OOM;
      goto fail;
    }
    memset(plane[i], 0, n * sizeof(uint16_t));
  }

  gimg_jpeg_huff_table_t dc_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  for (uint8_t c = 0; c < scan->comp_count; c++) {
    uint8_t id = scan->dc_tbl[c];
    if (id >= 4 || !state->huff_dc[id] ||
        jpeg_build_huff_table(
            state->huff_dc[id], state->huff_dc_len[id], &dc_tables[id]) != 0) {
      r = GIMG_ERR_CORRUPT;
      goto fail;
    }
  }

  gimg_jpeg_bitstream_t bs;
  jpeg_bitstream_init(&bs, scan->data, scan->data_size);
  uint16_t restart_interval = scan->restart_interval;

  // H.1.2.1: the first sample of the image predicts from 2^(P-Pt-1), and so
  // does the first sample after every restart interval.
  const int32_t initial_pred = (int32_t)1 << (precision - pt - 1);
  // A restart interval begins a fresh predictive context, not merely a fresh
  // entropy-coded segment.  The row the interval starts on has no row above it
  // that the interval may refer to, so that row is predicted one-dimensionally
  // - first sample from the constant, the rest from Ra - exactly as the first
  // row of the image is.  Only from the next row on does the frame's own
  // predictor come into play.  Resetting just the single first sample, and
  // then reading Rb and Rc from across the boundary, decodes the first row of
  // every interval after the first as noise.
  // Per component: in an interleaved scan every component starts its own
  // predictive context afresh at a restart, and each has its own first sample.
  int restart_pending[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t restart_row[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < GIMG_JPEG_MAX_COMPONENTS; i++) {
    restart_pending[i] = 1;
    restart_row[i] = 0;
  }

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0 && mcu_index > 0 &&
          mcu_index % (uint32_t)restart_interval == 0) {
        bs.expect_rst = 1;
        jpeg_bitstream_align_skip_rst(&bs);
        if (bs.rst_just_skipped) {
          bs.rst_just_skipped = 0;
        }
        for (uint8_t i = 0; i < GIMG_JPEG_MAX_COMPONENTS; i++) {
          restart_pending[i] = 1;
        }
      }
      for (uint8_t s = 0; s < scan->comp_count; s++) {
        uint8_t ci = 0;
        for (; ci < num_comp; ci++) {
          if (sof->comp_id[ci] == scan->comp_id[s]) {
            break;
          }
        }
        if (ci >= num_comp) {
          r = GIMG_ERR_CORRUPT;
          goto fail;
        }
        const gimg_jpeg_huff_table_t * tbl = &dc_tables[scan->dc_tbl[s]];
        uint32_t cw = comp_w[ci];
        for (uint8_t sy = 0; sy < sof->v_samp[ci]; sy++) {
          for (uint8_t sx = 0; sx < sof->h_samp[ci]; sx++) {
            uint32_t x = mcu_x * sof->h_samp[ci] + sx;
            uint32_t y = mcu_y * sof->v_samp[ci] + sy;
            int32_t diff = 0;
            r = jpeg_lossless_decode_diff(&bs, tbl, &diff);
            if (r != GIMG_OK) {
              goto fail;
            }
            int32_t pred;
            if (restart_pending[ci]) {
              // The first sample of the image or of a restart interval.
              pred = initial_pred;
              restart_row[ci] = y;
            }
            else if (y == restart_row[ci]) {
              // Still on the row the interval began: one-dimensional
              // prediction, because there is no row above it to refer to.
              pred = (int32_t)plane[ci][(size_t)y * cw + (x - 1)];
            }
            else if (x == 0) {
              // H.1.2.1: the first sample of a line predicts from the sample
              // above it, whatever the frame's predictor selection says.
              pred = (int32_t)plane[ci][(size_t)(y - 1) * cw];
            }
            else {
              int32_t ra = (int32_t)plane[ci][(size_t)y * cw + (x - 1)];
              int32_t rb = (int32_t)plane[ci][(size_t)(y - 1) * cw + x];
              int32_t rc = (int32_t)plane[ci][(size_t)(y - 1) * cw + (x - 1)];
              pred = jpeg_lossless_predict(psv, ra, rb, rc);
            }
            restart_pending[ci] = 0;
            // H.1.2.1: the reconstruction is modulo 2^16.
            plane[ci][(size_t)y * cw + x] =
                (uint16_t)((uint32_t)(pred + diff) & 0xFFFFu);
          }
        }
      }
    }
  }

  // Undo the point transform (H.1.2) and widen to the raster's depth.
  int out_bits = (precision <= 8) ? 8 : 16;
  int sample_bits = precision; // after the point transform is undone
  const GIMG_Pixel_Format * fmt;
  if (num_comp == 1) {
    fmt = (out_bits == 8) ? &GIMG_PIXEL_GRAY8 : &GIMG_PIXEL_GRAY16;
  }
  else if (num_comp == 3) {
    fmt = (out_bits == 8) ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_RGBA16;
  }
  else {
    r = GIMG_ERR_UNSUPPORTED; // CMYK lossless is not handled here
    goto fail;
  }
  r = gimg_raster_create_with_allocator(
      alloc, width, height, fmt, GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    goto fail;
  }
  {
    void * pixels = gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    uint32_t max_val = (sample_bits >= 32) ? 0xFFFFFFFFu
                                           : ((1u << sample_bits) - 1u);
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        uint32_t v[3] = {0, 0, 0};
        for (uint8_t c = 0; c < num_comp; c++) {
          uint32_t sv = plane[c][(size_t)y * comp_w[c] + x];
          sv = (uint32_t)((uint64_t)sv << pt); // undo the point transform
          if (sv > max_val) {
            sv = max_val;
          }
          v[c] = jpeg_lossless_widen(sv, sample_bits, out_bits);
        }
        if (out_bits == 8) {
          unsigned char * p = (unsigned char *)pixels + (size_t)y * stride;
          if (num_comp == 1) {
            p[x] = (unsigned char)v[0];
          }
          else {
            p[x * 4 + 0] = (unsigned char)v[0];
            p[x * 4 + 1] = (unsigned char)v[1];
            p[x * 4 + 2] = (unsigned char)v[2];
            p[x * 4 + 3] = 255;
          }
        }
        else {
          uint16_t * p =
              (uint16_t *)((unsigned char *)pixels + (size_t)y * stride);
          if (num_comp == 1) {
            p[x] = (uint16_t)v[0];
          }
          else {
            p[x * 4 + 0] = (uint16_t)v[0];
            p[x * 4 + 1] = (uint16_t)v[1];
            p[x * 4 + 2] = (uint16_t)v[2];
            p[x * 4 + 3] = 65535;
          }
        }
      }
    }
  }

fail:
  for (uint8_t i = 0; i < GIMG_JPEG_MAX_COMPONENTS; i++) {
    if (plane[i]) {
      gimg_free(alloc, plane[i]);
    }
  }
  if (r != GIMG_OK && *out_raster) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
  return r;
}
