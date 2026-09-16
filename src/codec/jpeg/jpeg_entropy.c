/**
 * @file
 *
 * JPEG entropy decoding (Huffman), dezigzag, dequantise, inverse DCT.
 * Used for baseline decode.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../raster/raster_internal.h"
#include "jpeg_debug_internal.h"
#include "jpeg_huffman_tables_internal.h"
#include <ghoti.io/image/bitdepth.h>
#include "jpeg_internal.h"

/** When GIMG_JPEG_TRACE_ALL=1, dump block[0..63] (zigzag order) to stderr as
 * 64 space-separated values in natural order. Prefix with label (e.g.
 * "BLOCK_BEFORE"). */
static void jpeg_trace_all_dump_block(const char * label, unsigned int scan_idx,
    uint32_t mcu_x, uint32_t mcu_y, unsigned int comp_idx, size_t block_in_mcu,
    size_t byte_off, unsigned int bit_off, const int16_t * block) {
  (void)fprintf(stderr,
      "%s scan%u mcu=(%u,%u) comp=%u block=%zu byte_off=%zu bit_off=%u coeffs:",
      label, scan_idx, (unsigned)mcu_x, (unsigned)mcu_y, comp_idx, block_in_mcu,
      (size_t)byte_off, (unsigned)bit_off);
  for (int nat = 0; nat < 64; nat++) {
    (void)fprintf(stderr, " %d", (int)block[gimg_jpeg_inv_zigzag[nat]]);
  }
  (void)fprintf(stderr, "\n");
  (void)fflush(stderr);
}

/** When GIMG_JPEG_DEBUG_PROG_SYNC=1, log every encoder/decoder step with
 * PROG_SYNC_ENC / PROG_SYNC_DEC scan=N block=B byte=X bit=Y op=... so you can
 * diff enc vs dec and find first divergence. */
#define PROG_SYNC_DEBUG() (GIMG_JPEG_DEBUG_PROG_SYNC)

static GIMG_Result jpeg_decode_progressive_extended(
    const gimg_jpeg_doc_state_t * state, const GIMG_Decode_Options * options,
    GIMG_Raster ** out_raster);

/** Assemble a four-component frame; see the definition below. */
static GIMG_Result jpeg_emit_four_component(const GIMG_Allocator * alloc,
    int adobe_transform, uint32_t width, uint32_t height, int precision,
    int planes_wide, const void * const * comp_buf, const size_t * comp_stride,
    const uint32_t * comp_w, const uint32_t * comp_h, const uint8_t * h_samp,
    const uint8_t * v_samp, int fancy, GIMG_Raster ** out_raster);

static GIMG_Result jpeg_decode_baseline_extended(
    const gimg_jpeg_doc_state_t * state, const GIMG_Decode_Options * options,
    GIMG_Raster ** out_raster) {
  const gimg_jpeg_sof_t * sof = &state->sof;
  uint8_t precision = sof->precision;
  if (precision != 12) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint16_t width = sof->width;
  uint16_t height = sof->height;
  uint8_t num_comp = sof->num_components;
  // T.81 Table B.2 allows P=12 in a DCT-based frame and B.2.2 allows Nf from 1
  // to 255; the two are independent, so a twelve-bit four-component frame is a
  // legal file.  This walk assembles one and three components itself and hands
  // four to jpeg_emit_four_component, which writes GIMG_PIXEL_CMYK16.
  if (num_comp != 1 && num_comp != 3 && num_comp != 4) {
    return GIMG_ERR_UNSUPPORTED;
  }

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

  const gimg_jpeg_scan_t * scan0 = &state->scans[0];
  gimg_jpeg_huff_table_t dc_tables[4];
  gimg_jpeg_huff_table_t ac_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  memset(ac_tables, 0, sizeof(ac_tables));
  for (uint8_t c = 0; c < scan0->comp_count; c++) {
    if (state->is_arithmetic) {
      // An arithmetic frame (SOF9 or SOF10) carries no DHT segments at all.
      // Its "tables" are the adaptive statistics of T.81 F.1.4, built as it
      // decodes, and the scan header's Td and Ta select conditioning (B.2.4.3)
      // rather than Huffman tables.  Asking for Huffman tables here would
      // reject every such frame.
      break;
    }
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

  uint8_t h_max = 0;
  uint8_t v_max = 0;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (sof->h_samp[i] > h_max)
      h_max = sof->h_samp[i];
    if (sof->v_samp[i] > v_max)
      v_max = sof->v_samp[i];
  }
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;
  size_t mcu_total = 0;
  if (!gcu_safe_mul_size(
          (size_t)mcu_per_row, (size_t)mcu_per_col, &mcu_total)) {
    return GIMG_ERR_LIMIT;
  }
  size_t blocks_per_mcu_ext = 0;
  for (uint8_t s = 0; s < scan0->comp_count; s++) {
    uint8_t comp_idx = 0;
    for (; comp_idx < num_comp; comp_idx++) {
      if (sof->comp_id[comp_idx] == scan0->comp_id[s])
        break;
    }
    if (comp_idx < num_comp)
      blocks_per_mcu_ext +=
          (size_t)sof->h_samp[comp_idx] * (size_t)sof->v_samp[comp_idx];
  }
  size_t total_blocks_ext = 0;
  if (!gcu_safe_mul_size(mcu_total, blocks_per_mcu_ext, &total_blocks_ext)) {
    return GIMG_ERR_LIMIT;
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // Per-component dimensions in samples (T.81): X_i = ceil(X*H_i/H_max).
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_w[i] = (width * (uint32_t)sof->h_samp[i] + (uint32_t)h_max - 1) /
        (uint32_t)h_max;
    comp_h[i] = (height * (uint32_t)sof->v_samp[i] + (uint32_t)v_max - 1) /
        (uint32_t)v_max;
    if (comp_w[i] == 0)
      comp_w[i] = 8;
    if (comp_h[i] == 0)
      comp_h[i] = 8;
  }

  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_stride_el[GIMG_JPEG_MAX_COMPONENTS];
  uint16_t * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride_el[i] = (size_t)comp_w[i];
    if (!gcu_safe_mul_size(
            comp_stride_el[i], (size_t)comp_h[i], &comp_size[i]) ||
        !gcu_safe_mul_size(comp_size[i], sizeof(uint16_t), &comp_size[i])) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      return GIMG_ERR_LIMIT;
    }
    comp_buf[i] = (uint16_t *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      return GIMG_ERR_OOM;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  // T.81 A.3.3 (equation 8.3): the inverse DCT is
  //     s(x) = 1/2 * sum_u C(u) S(u) cos((2x+1) u pi / 16),  C(0) = 1/sqrt(2).
  // jpeg_idct_1d_32 accumulates sum_u S(u) * c_u * cos(...) with c_u = 256, or
  // 181 for u = 0 (181/256 is 1/sqrt(2)), then divides by this scale.  One pass
  // therefore yields (512 / scale) * s(x), and the separable two-pass transform
  // squares that, so only scale = 512 reproduces the transform.  It was 4096,
  // which is a factor of (512/4096)^2 = 1/64: every 12-bit image decoded to a
  // band roughly 1/64 of its true contrast, clustered around mid-grey, and no
  // test noticed because none compared 12-bit sample values.
  (void)precision;
  int level_shift = (precision == 12) ? 2048 : 32768;
  int max_val = (precision == 12) ? 4095 : 65535;

  gimg_jpeg_bitstream_t bs;
  jpeg_bitstream_init(&bs, scan0->data, scan0->data_size);
  // SOF9 at P=12 takes the same route through this function as SOF1; only the
  // entropy coder differs (T.81 Annex D in place of Annex F).
  jpeg_arith_decoder_t ad;
  jpeg_arith_stats_t astats;
  if (state->is_arithmetic) {
    jpeg_arith_decoder_init(&ad, scan0->data, scan0->data_size);
    jpeg_arith_stats_reset(&astats);
  }
  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));
  int16_t block_zig[64];
  int16_t block_rz[64];
  int32_t block_q[64];
  int32_t block_idct[64];

  // The interval in force for this scan, not the frame's latest (B.2.4.4).
  uint16_t restart_interval = scan0->restart_interval;
  size_t block_counter = 0;
  const int trace_baseline_sync =
      (GIMG_JPEG_TRACE_BASELINE_SYNC)
      ? 1
      : 0;
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0) {
        // T.81 B.2.1 and F.2.1.3.1: a restart marker byte-aligns the entropy
        // data and resets the DC prediction of every component.  Consume it
        // here, before any of this MCU's blocks, so all the predictors can be
        // reset together - doing it inside the first block reset only that
        // block's component.
        // Set expect_rst before MCU ri, 2*ri, ... so align_skip_rst skips there.
        if (mcu_index > 0 && mcu_index % (uint32_t)restart_interval == 0) {
          if (state->is_arithmetic) {
            // T.81 F.2.4.1: restart the coder and forget what it had learned.
            if (jpeg_arith_restart(&ad, &astats) != GIMG_OK) {
              goto ext_fail;
            }
          }
          else {
            bs.expect_rst = 1; // T.81 3.1.110: next 0xFF 0xD0..0xD7 is RST
            jpeg_bitstream_align_skip_rst(&bs);
            if (bs.rst_just_skipped) {
              memset(dc_pred, 0, sizeof(dc_pred));
              bs.rst_just_skipped = 0;
            }
          }
        }
      }
      for (uint8_t s = 0; s < scan0->comp_count; s++) {
        uint8_t comp_idx = 0;
        for (; comp_idx < num_comp; comp_idx++) {
          if (sof->comp_id[comp_idx] == scan0->comp_id[s])
            break;
        }
        if (comp_idx >= num_comp) {
          goto ext_fail;
        }
        uint8_t h_samp = sof->h_samp[comp_idx];
        uint8_t v_samp = sof->v_samp[comp_idx];
        uint8_t qid = sof->quant_tbl_id[comp_idx];
        if (qid >= GIMG_JPEG_MAX_QUANT_TABLES ||
            !state->quant_tbl_present[qid]) {
          goto ext_fail;
        }
        const uint16_t * quant = state->quant_tbl[qid];
        const gimg_jpeg_huff_table_t * dc_tbl = &dc_tables[scan0->dc_tbl[s]];
        const gimg_jpeg_huff_table_t * ac_tbl = &ac_tables[scan0->ac_tbl[s]];

        for (uint8_t by = 0; by < v_samp; by++) {
          for (uint8_t bx = 0; bx < h_samp; bx++) {
            int is_last =
                (total_blocks_ext > 0 && block_counter == total_blocks_ext - 1)
                ? 1
                : 0;
            GIMG_Result r;
            if (state->is_arithmetic) {
              r = jpeg_arith_decode_block_sequential(&ad, &astats,
                  &state->arith_cond, comp_idx, scan0->dc_tbl[s],
                  scan0->ac_tbl[s], 63, block_zig);
            }
            else {
              r = jpeg_decode_block(&bs, dc_tbl, ac_tbl, block_zig,
                  &dc_pred[comp_idx], is_last);
            }
            if (r != GIMG_OK) {
              goto ext_fail;
            }
            if (trace_baseline_sync) {
              (void)fprintf(stderr,
                  "DEC_BLOCK_AFTER block=%zu byte=%zu bit=%u\n", block_counter,
                  (size_t)bs.byte_off, (unsigned)bs.bit_off);
              (void)fflush(stderr);
            }
            block_counter++;
            jpeg_dezigzag(block_zig, block_rz);
            jpeg_dequantise_32(block_rz, quant, block_q);
            // pass1_bits 1 at P=12: one fewer fractional bit between passes,
            // for the headroom the wider samples need (libjpeg jidctint.c).
            jpeg_idct_8x8_islow(block_q, block_idct, 1);

            uint32_t dst_x = mcu_x * (uint32_t)(8 * h_samp) + (uint32_t)bx * 8;
            uint32_t dst_y = mcu_y * (uint32_t)(8 * v_samp) + (uint32_t)by * 8;
            uint32_t cw = comp_w[comp_idx];
            uint32_t ch = comp_h[comp_idx];
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = dst_y + (uint32_t)dy;
              if (y >= ch)
                break;
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = dst_x + (uint32_t)dx;
                if (x >= cw)
                  break;
                int32_t v = block_idct[dy * 8 + dx] + level_shift;
                if (v < 0)
                  v = 0;
                if (v > max_val)
                  v = max_val;
                // Keep the component plane at its native precision.  The
                // widening to 16-bit output happens once, at the end, through
                // gimg_bitdepth_12_to_16 - the library's rule for widening a
                // sample - rather than being open-coded as a shift here and
                // undone by a shift when the plane is read back.
                comp_buf[comp_idx][y * comp_stride_el[comp_idx] + x] =
                    (uint16_t)v;
              }
            }
          }
        }
      }
    }
    if (restart_interval > 0 && bs.rst_just_skipped) {
      bs.rst_just_skipped = 0; // clear after all components of this MCU
    }
  }

  GIMG_Result r;
  if (num_comp == 4) {
    // A twelve-bit CMYK or YCCK frame; see jpeg_emit_four_component.
    int use_fancy_4 = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    r = jpeg_emit_four_component(alloc, state->adobe_transform, (uint32_t)width,
        (uint32_t)height, (int)precision, 1, (const void * const *)comp_buf,
        comp_stride_el, comp_w, comp_h, sof->h_samp, sof->v_samp, use_fancy_4,
        out_raster);
    if (r != GIMG_OK) {
      goto ext_fail;
    }
  }
  else if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto ext_fail;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 2;
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        uint16_t v = comp_buf[0][jpeg_component_index(comp_w[0], comp_h[0],
            comp_stride_el[0], x, y, width, height)];
        pixels[y * stride_el + x] =
            (precision == 12) ? gimg_bitdepth_12_to_16(v) : v;
      }
    }
  }
  else {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto ext_fail;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    // uint16 elements, not pixels: the row is indexed as
    // pixels[y * stride_el + x * 4 + c] through a uint16_t *, so the divisor is
    // sizeof(uint16_t) and not the 8 bytes an RGBA16 pixel occupies.  Dividing
    // by 8 made the stride a quarter of a row, so every 12-bit colour frame was
    // written into the first quarter of its own raster and the rest left blank.
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 2;
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    // Fancy unless the caller explicitly asked for SIMPLE, so that NULL
    // options and zero-initialised options agree (GIMG_JPEG_CHROMA_UPSAMPLE_
    // DEFAULT is 0 and means FANCY).
    int use_fancy = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    const int frame_is_rgb = jpeg_frame_is_rgb(state, sof);
    jpeg_plane_t pl_cb = {comp_buf[1], comp_stride_el[1], 1};
    jpeg_plane_t pl_cr = {comp_buf[2], comp_stride_el[2], 1};
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        int yy = (int)comp_buf[0][jpeg_component_index(comp_w[0], comp_h[0],
            comp_stride_el[0], x, y, width, height)];
        // The same filters the 8-bit path uses.  This used to read the chroma
        // planes with a nearest-neighbour index of its own, so a 12-bit 4:2:0
        // or 4:2:2 frame was always box-filtered no matter what the caller
        // asked for.
        int cb = jpeg_chroma_sample(&pl_cb, cw1, ch1, x, y, width, height,
            sof->h_samp[1], sof->v_samp[1], h_max, v_max, use_fancy);
        int cr = jpeg_chroma_sample(&pl_cr, cw2, ch2, x, y, width, height,
            sof->h_samp[2], sof->v_samp[2], h_max, v_max, use_fancy);
        // Convert and clamp at the frame's own precision (T.81 A.3.1: a
        // reconstructed sample is in 0..2^P-1), then widen once to the 16-bit
        // raster.
        int r_val, g_val, b_val;
        // T.81 describes no colour space at all; jpeg_frame_is_rgb reads the
        // conventions that do (JFIF, Adobe APP14, the component identifiers).
        // A frame that already carries R, G, B is passed through: converting it
        // as though it were YCbCr turns every pixel into a different colour.
        if (frame_is_rgb) {
          r_val = yy;
          g_val = cb;
          b_val = cr;
        }
        else {
          jpeg_ycbcr_to_rgb(
              yy, cb, cr, level_shift, max_val, &r_val, &g_val, &b_val);
        }
        if (precision == 12) {
          r_val = (int)gimg_bitdepth_12_to_16((uint16_t)r_val);
          g_val = (int)gimg_bitdepth_12_to_16((uint16_t)g_val);
          b_val = (int)gimg_bitdepth_12_to_16((uint16_t)b_val);
        }
        pixels[y * stride_el + x * 4 + 0] = (uint16_t)r_val;
        pixels[y * stride_el + x * 4 + 1] = (uint16_t)g_val;
        pixels[y * stride_el + x * 4 + 2] = (uint16_t)b_val;
        pixels[y * stride_el + x * 4 + 3] = (uint16_t)65535;
      }
    }
  }

  if (state->app2_icc && state->app2_icc_len > 0u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

ext_fail:
  if (*out_raster) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_ERR_CORRUPT;
}

/**
 * Assemble a four-component frame into a CMYK raster.
 *
 * T.81 puts no colour space in the frame at all and allows Nf up to 255;
 * four components in practice means CMYK, or YCCK when an Adobe APP14 says
 * transform 2, and that marker is the only thing that distinguishes them.
 *
 * Shared by the baseline walk and the coefficient-buffer walk.  The second one
 * refused four components outright, so a progressive CMYK file - which Pillow
 * and libjpeg both write without comment - decoded as UNSUPPORTED while the
 * same image saved sequentially decoded fine.  Refusing it there and accepting
 * it here was not a decision, just the two paths having been written at
 * different times; they now assemble the picture with the same code.
 *
 * Upsampling is the same as for three components.  This used to resample by
 * nearest neighbour on the grounds that "four-component files are not YCbCr
 * and the filter does not apply to them", which was wrong: libjpeg chooses its
 * upsampler from the sampling factors alone (jdsample.c jinit_upsampler never
 * looks at the colour space), so a subsampled YCCK frame gets the triangle
 * filter there.  Nearest neighbour disagreed with libjpeg on every 4:2:0 and
 * 4:2:2 YCCK file; nothing caught it because every four-component fixture was
 * 4:4:4, where the two agree exactly.
 *
 * @param comp_buf   8-bit component planes, whichever walk produced them.
 * @param h_samp,v_samp  Per-component Hi and Vi from the frame header.
 * @param fancy      0 selects the box filter, as GIMG_JPEG_CHROMA_UPSAMPLE_
 *                   SIMPLE asks for.
 */
static GIMG_Result jpeg_emit_four_component(const GIMG_Allocator * alloc,
    int adobe_transform, uint32_t width, uint32_t height, int precision,
    int planes_wide, const void * const * comp_buf, const size_t * comp_stride,
    const uint32_t * comp_w, const uint32_t * comp_h, const uint8_t * h_samp,
    const uint8_t * v_samp, int fancy, GIMG_Raster ** out_raster) {
  GIMG_Result r;
  // The raster's width follows the frame's precision; the planes' width
  // follows the walk that produced them - the coefficient-buffer walk carries
  // even an 8-bit frame in uint16_t so that a 12-bit one fits.
  const int wide = (precision > 8);
  // T.81 A.3.1: a reconstructed sample lies in 0..2^P-1, and the centre the
  // chrominance components are offset about is 2^(P-1).
  const int max_val = (1 << precision) - 1;
  const int centre = 1 << (precision - 1);
  uint8_t h_max = 1, v_max = 1;
  gimg_jpeg_sampling_max(4, h_samp, v_samp, &h_max, &v_max);
  jpeg_plane_t pl[4];
  for (int i = 0; i < 4; i++) {
    pl[i].data = comp_buf[i];
    pl[i].stride = comp_stride[i];
    pl[i].wide = planes_wide;
  }
  r = gimg_raster_create_with_allocator(alloc, width, height,
      wide ? &GIMG_PIXEL_CMYK16 : &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL,
      0, out_raster);
  if (r != GIMG_OK) {
    return r;
  }
  unsigned char * pix8 = (unsigned char *)gimg_raster_pixels(*out_raster);
  uint16_t * pix16 = (uint16_t *)gimg_raster_pixels(*out_raster);
  size_t stride8 = gimg_raster_stride_bytes(*out_raster);
  size_t stride16 = stride8 / sizeof(uint16_t);
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      int s[4];
      for (int i = 0; i < 4; i++) {
        s[i] = jpeg_chroma_sample(&pl[i], comp_w[i], comp_h[i], x, y, width,
            height, h_samp[i], v_samp[i], h_max, v_max, fancy);
      }
      int out[4];
      if (adobe_transform == 2) {
        // YCCK -> CMYK: YCbCr -> RGB, then C = max - R, M = max - G,
        // Y = max - B, K unchanged.  This is jdcolor.c's ycck_cmyk_convert,
        // and the YCbCr step is the one the three-component path uses, so the
        // two agree by construction at either precision.
        int r_val, g_val, b_val;
        jpeg_ycbcr_to_rgb(
            s[0], s[1], s[2], centre, max_val, &r_val, &g_val, &b_val);
        out[0] = max_val - r_val;
        out[1] = max_val - g_val;
        out[2] = max_val - b_val;
      }
      else {
        // Raw CMYK: the components go out unchanged, to match libjpeg
        // (jdcolor.c null_convert for JCS_CMYK).  libjpeg does not invert
        // Adobe CMYK; Pillow does, which is why Pillow is not the oracle here.
        out[0] = s[0];
        out[1] = s[1];
        out[2] = s[2];
      }
      out[3] = s[3];
      if (wide) {
        for (int i = 0; i < 4; i++) {
          // Left-justify into the 16-bit raster, as GRAY16 and RGBA16 do.
          pix16[y * stride16 + x * 4 + (size_t)i] = (precision == 12)
              ? gimg_bitdepth_12_to_16((uint16_t)out[i])
              : (uint16_t)out[i];
        }
      }
      else {
        for (int i = 0; i < 4; i++) {
          pix8[y * stride8 + x * 4 + (size_t)i] = (unsigned char)out[i];
        }
      }
    }
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_decode_baseline(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  GIMG_Result r = GIMG_ERR_CORRUPT;
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
  // An arithmetic scan is allowed to be empty.  T.81 D.2.9 has the decoder
  // supply zero bytes once it has run past the compressed data, so a scan whose
  // decisions all resolve to the more probable symbol - a 1x1 image whose only
  // block is a zero DC difference and an immediate end of block, say - needs no
  // bytes at all, and libjpeg writes exactly none.  A Huffman scan always has
  // at least one byte, so the check still applies there.
  if (state->is_arithmetic ? (scan0->data_size != 0 && !scan0->data)
                           : (!scan0->data || scan0->data_size == 0)) {
    return GIMG_ERR_CORRUPT;
  }

  // T.81 A.2.3: a sequential frame may be coded as several non-interleaved
  // scans, one per component, instead of one interleaved scan.  That is a
  // different order through the same blocks, and the walk that knows how to
  // read it is the one Annex G already needed, so such a frame goes through
  // the coefficient-buffer path rather than either single-scan walk.  Before
  // the precision branch, because that path takes 8- and 12-bit frames alike.
  if (state->num_scans > 1u) {
    return jpeg_decode_progressive_extended(state, options, out_raster);
  }
  if (sof->precision != 8) {
    return jpeg_decode_baseline_extended(state, options, out_raster);
  }

  // Build Huffman tables for scan components.
  gimg_jpeg_huff_table_t dc_tables[4];
  gimg_jpeg_huff_table_t ac_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  memset(ac_tables, 0, sizeof(ac_tables));
  for (uint8_t c = 0; c < scan0->comp_count; c++) {
    if (state->is_arithmetic) {
      // An arithmetic frame (SOF9 or SOF10) carries no DHT segments at all.
      // Its "tables" are the adaptive statistics of T.81 F.1.4, built as it
      // decodes, and the scan header's Td and Ta select conditioning (B.2.4.3)
      // rather than Huffman tables.  Asking for Huffman tables here would
      // reject every such frame.
      break;
    }
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
    if (GIMG_JPEG_DEBUG_DHT_AC && state->huff_ac[ac_id] &&
        state->huff_ac_len[ac_id] >= 18) {
      const unsigned char * bits = state->huff_ac[ac_id] + 1;
      size_t n_sym = 0;
      for (int i = 0; i < 16; i++) {
        n_sym += bits[i];
      }
      (void)fprintf(stderr,
          "BASELINE_DHT_AC ac_id=%u len=%zu num_syms=%zu bits[15]=%u "
          "min_code[16]=0x%04x\n",
          (unsigned)ac_id, (size_t)state->huff_ac_len[ac_id], (size_t)n_sym,
          (unsigned)bits[15], (unsigned)ac_tables[ac_id].min_code[16]);
      (void)fflush(stderr);
    }
  }
  if (GIMG_JPEG_DEBUG_BASELINE_FAIL) {
    // Sanity: AC Th=1 first 3-bit symbol should be EOB (0x00) per T.81 Table K.6.
    if (state->huff_ac[1] && state->huff_ac_len[1] >= 17 + 2) {
      const unsigned char * bits = state->huff_ac[1] + 1;
      if (bits[2] >= 1) {
        (void)fprintf(stderr,
            "BASELINE_DEBUG AC1 first 3-bit symbol=0x%02x (expect 0x00 EOB)\n",
            (unsigned)state->huff_ac[1][17]);
      }
    }
    (void)fprintf(stderr, "BASELINE_DEBUG scan_data_size=%zu first 16 bytes:",
        (size_t)scan0->data_size);
    for (size_t i = 0; i < 16 && i < scan0->data_size; i++) {
      (void)fprintf(stderr, " %02x", (unsigned)scan0->data[i]);
    }
    (void)fprintf(stderr, "\n");
    (void)fflush(stderr);
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
  if (!gcu_safe_mul_size(
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
  size_t total_blocks = 0;
  if (!gcu_safe_mul_size(mcu_total, blocks_per_mcu, &total_blocks)) {
    return GIMG_ERR_LIMIT;
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // Per-component dimensions in samples (T.81): X_i = ceil(X*H_i/H_max).
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_w[i] = (width * (uint32_t)sof->h_samp[i] + (uint32_t)h_max - 1) /
        (uint32_t)h_max;
    comp_h[i] = (height * (uint32_t)sof->v_samp[i] + (uint32_t)v_max - 1) /
        (uint32_t)v_max;
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
    if (!gcu_safe_mul_size(comp_stride[i], (size_t)comp_h[i], &comp_size[i])) {
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

  // Step 2: log first 80 bytes of scan data as seen by decoder (compare to encoder buffer).
  {
#if GIMG_JPEG_DEBUG_SCAN_LOADED
    const char * dbg_scan = "1";
#else
    const char * dbg_scan = NULL;
#endif
    if (dbg_scan && dbg_scan[0] == '1' && scan0->data && scan0->data_size > 0) {
      (void)fprintf(stderr, "DECODER scan_data (first 80 bytes) size=%zu:\n",
          (size_t)scan0->data_size);
      for (size_t i = 0; i < 80u && i < scan0->data_size; i++) {
        (void)fprintf(stderr, " %zu:0x%02x", i, (unsigned)scan0->data[i]);
        if ((i + 1) % 16 == 0) {
          (void)fprintf(stderr, "\n");
        }
      }
      if (scan0->data_size < 80u) {
        (void)fprintf(stderr, "\n");
      }
      (void)fflush(stderr);
    }
  }

  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  // Arithmetic frames (SOF9) use the coder of T.81 Annex D in place of the
  // bitstream reader above; the surrounding MCU walk is identical, because the
  // entropy coder is the only thing Annex D changes.
  jpeg_arith_decoder_t ad;
  jpeg_arith_stats_t astats;
  if (state->is_arithmetic) {
    jpeg_arith_decoder_init(&ad, scan0->data, scan0->data_size);
    jpeg_arith_stats_reset(&astats);
  }

  int16_t block_zig[64];
  int16_t block_rz[64];
  int32_t block_q[64];
  int32_t block_idct[64];
  memset(block_zig, 0, sizeof(block_zig));
  memset(block_rz, 0, sizeof(block_rz));
  memset(block_q, 0, sizeof(block_q));
  memset(block_idct, 0, sizeof(block_idct));

  // Decode MCU by MCU. At each restart boundary (DRI), reset DC predictors.
  // The interval in force for this scan, not the frame's latest (B.2.4.4).
  uint16_t restart_interval = scan0->restart_interval;
  size_t block_counter_8 = 0;
  const int trace_baseline_sync_8 =
      (GIMG_JPEG_TRACE_BASELINE_SYNC)
      ? 1
      : 0;
  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0) {
        // Same as the 8-bit path: consume the restart marker before the MCU and
        // reset every component's DC prediction (T.81 B.2.1, F.2.1.3.1).
        if (mcu_index > 0 && mcu_index % (uint32_t)restart_interval == 0) {
          if (state->is_arithmetic) {
            // T.81 F.2.4.1: restart the coder and forget what it had learned.
            GIMG_Result rr = jpeg_arith_restart(&ad, &astats);
            if (rr != GIMG_OK) {
              goto fail_comp;
            }
          }
          else {
            bs.expect_rst = 1;
            jpeg_bitstream_align_skip_rst(&bs);
            if (bs.rst_just_skipped) {
              memset(dc_pred, 0, sizeof(dc_pred));
              bs.rst_just_skipped = 0;
            }
          }
        }
      }
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
        const gimg_jpeg_huff_table_t * dc_tbl = &dc_tables[scan0->dc_tbl[s]];
        const gimg_jpeg_huff_table_t * ac_tbl = &ac_tables[scan0->ac_tbl[s]];

        for (uint8_t by = 0; by < v_samp; by++) {
          for (uint8_t bx = 0; bx < h_samp; bx++) {
            (void)block_idx;
            if (GIMG_JPEG_TRACE_DEC_BLOCK_POS) {
              size_t linear_block =
                  (size_t)mcu_y * (size_t)mcu_per_row + (size_t)mcu_x;
              if (linear_block <= 40u ||
                  (linear_block >= 1195u && linear_block <= 1225u)) {
                size_t pos = (size_t)bs.byte_off * 8u + (unsigned)bs.bit_off;
                (void)fprintf(stderr,
                    "DEC_BLOCK_POS block=%zu pos_bits=%zu byte_off=%zu "
                    "bit_off=%u\n",
                    linear_block, pos, (size_t)bs.byte_off,
                    (unsigned)bs.bit_off);
                (void)fflush(stderr);
              }
            }
            int is_last_8 =
                (total_blocks > 0 && block_counter_8 == total_blocks - 1) ? 1
                                                                          : 0;
            GIMG_Result r;
            if (state->is_arithmetic) {
              r = jpeg_arith_decode_block_sequential(&ad, &astats,
                  &state->arith_cond, comp_idx, scan0->dc_tbl[s],
                  scan0->ac_tbl[s], 63, block_zig);
            }
            else {
              r = jpeg_decode_block(&bs, dc_tbl, ac_tbl, block_zig,
                  &dc_pred[comp_idx], is_last_8);
            }
            if (r != GIMG_OK) {
              if (GIMG_JPEG_DEBUG_BIT_POS || GIMG_JPEG_DEBUG_BASELINE_FAIL) {
                (void)fprintf(stderr,
                    "BASELINE_FAIL mcu=(%u,%u) comp=%u (comp_idx=%u) dc_tbl=%u "
                    "ac_tbl=%u byte_off=%zu bit_off=%u\n",
                    (unsigned)mcu_x, (unsigned)mcu_y, (unsigned)s,
                    (unsigned)comp_idx, (unsigned)scan0->dc_tbl[s],
                    (unsigned)scan0->ac_tbl[s], (size_t)bs.byte_off,
                    (unsigned)bs.bit_off);
                if (bs.byte_off < scan0->data_size) {
                  size_t n = (scan0->data_size - bs.byte_off) > 8u
                      ? 8u
                      : (scan0->data_size - bs.byte_off);
                  (void)fprintf(stderr, " next %zu bytes:", n);
                  for (size_t i = 0; i < n; i++)
                    (void)fprintf(stderr, " %02x",
                        (unsigned)scan0->data[bs.byte_off + i]);
                  (void)fprintf(stderr, "\n");
                }
                (void)fflush(stderr);
              }
              goto fail_decode;
            }
            if (trace_baseline_sync_8) {
              (void)fprintf(stderr,
                  "DEC_BLOCK_AFTER block=%zu byte=%zu bit=%u\n",
                  block_counter_8, (size_t)bs.byte_off, (unsigned)bs.bit_off);
              (void)fflush(stderr);
            }
            if (block_counter_8 == 0 && GIMG_JPEG_TRACE_FIRST_BLOCK) {
              (void)fprintf(stderr, "DEC_FIRST_BLOCK comp=%u DC=%d",
                  (unsigned)comp_idx, (int)block_zig[0]);
              for (int ki = 1; ki < 64; ki++) {
                if (block_zig[ki] != 0) {
                  (void)fprintf(stderr, " k=%d val=%d", ki, (int)block_zig[ki]);
                }
              }
              (void)fprintf(stderr, "\n");
              (void)fflush(stderr);
            }
            block_counter_8++;
            if (GIMG_JPEG_DEBUG_BIT_POS && mcu_x == 0 && mcu_y == 0 &&
                s == 0 && by == 0 && bx == 0) {
              (void)fprintf(stderr,
                  "DEC after block0: byte_off=%zu bit_off=%u (next byte(s):",
                  (size_t)bs.byte_off, (unsigned)bs.bit_off);
              for (size_t i = 0; i < 6u && bs.byte_off + i < scan0->data_size;
                   i++)
                (void)fprintf(
                    stderr, " %02x", (unsigned)scan0->data[bs.byte_off + i]);
              (void)fprintf(stderr, ")\n");
              (void)fflush(stderr);
            }
            jpeg_dezigzag(block_zig, block_rz);
            jpeg_dequantise_32(block_rz, quant, block_q);
            // pass1_bits 2: the 8-bit setting (libjpeg jidctint.c).
            jpeg_idct_8x8_islow(block_q, block_idct, 2);

            if (comp_idx == 1 && by == 0 && bx == 0 &&
                GIMG_JPEG_TRACE_FIRST_CB) {
              (void)fprintf(stderr,
                  "BASELINE first Cb: quant[0]=%u dequant[0]=%d idct[0]=%d "
                  "stored=%d\n",
                  (unsigned)quant[0], (int)block_q[0], (int)block_idct[0],
                  (int)block_idct[0] + 128);
              (void)fflush(stderr);
            }

            // Bit-by-bit debug: first MCU coefficients and pipeline (compare to encoder dump).
            if (mcu_x == 0 && mcu_y == 0 &&
                GIMG_JPEG_TRACE_BASELINE_FIRST_MCU) {
              unsigned linear = (unsigned)block_counter_8 -
                  1u; // 0-based to match encoder block_0.bin
              (void)fprintf(stderr,
                  "BLOCK_DEC linear=%u comp=%u by=%u bx=%u ZIG:", linear,
                  (unsigned)comp_idx, (unsigned)by, (unsigned)bx);
              for (int i = 0; i < 64; i++)
                (void)fprintf(stderr, " %d", (int)block_zig[i]);
              (void)fprintf(stderr, "\n");
              (void)fprintf(stderr, "BLOCK_DEC linear=%u DEQUANT:", linear);
              for (int i = 0; i < 64; i++)
                (void)fprintf(stderr, " %d", (int)block_q[i]);
              (void)fprintf(stderr, "\n");
              (void)fprintf(stderr, "BLOCK_DEC linear=%u IDCT:", linear);
              for (int i = 0; i < 64; i++)
                (void)fprintf(stderr, " %d", (int)block_idct[i]);
              (void)fprintf(stderr, "\n");
              (void)fprintf(stderr, "BLOCK_DEC linear=%u STORED:", linear);
              for (int i = 0; i < 64; i++) {
                int v = block_idct[i] + 128;
                if (v < 0)
                  v = 0;
                if (v > 255)
                  v = 255;
                (void)fprintf(stderr, " %d", v);
              }
              (void)fprintf(stderr, "\n");
              (void)fflush(stderr);
            }

            if (mcu_x == 0 && mcu_y == 0) {
#if GIMG_JPEG_DUMP_JPEG_COMPONENTS
              const char * dump_dir =
                  getenv("GIMG_JPEG_DUMP_JPEG_COMPONENTS");
              if (dump_dir && dump_dir[0] != '\0') {
                char path[1024];
                int n = -1;
                if (comp_idx == 0) {
                  unsigned block_no =
                      (unsigned)by * (unsigned)h_samp + (unsigned)bx;
                  n = snprintf(path, sizeof(path), "%s/Y_block%u.bin", dump_dir,
                      block_no);
                }
                else if (comp_idx == 1 && by == 0 && bx == 0) {
                  n = snprintf(
                      path, sizeof(path), "%s/Cb_block0.bin", dump_dir);
                }
                else if (comp_idx == 2 && by == 0 && bx == 0) {
                  n = snprintf(
                      path, sizeof(path), "%s/Cr_block0.bin", dump_dir);
                }
                if (n > 0 && (size_t)n < sizeof(path)) {
                  FILE * f = fopen(path, "wb");
                  if (f) {
                    fwrite(block_zig, 2, 64, f);
                    fwrite(quant, 2, 64, f);
                    for (int i = 0; i < 64; i++) {
                      int v = block_idct[i] + 128;
                      if (v < 0)
                        v = 0;
                      if (v > 255)
                        v = 255;
                      unsigned char b = (unsigned char)v;
                      fwrite(&b, 1, 1, f);
                    }
                    fclose(f);
                  }
                }
              }
#endif
            }

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
                int v = block_idct[dy * 8 + dx] + 128;
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
    // Optional debug dump: Y, Cb, Cr component buffers (see
    // compare_ycbcr_components.py).
#if GIMG_JPEG_DUMP_JPEG_COMPONENTS
    {
      const char * dir = getenv("GIMG_JPEG_DUMP_JPEG_COMPONENTS");
      if (dir && dir[0] != '\0') {
        const char * names[] = {"Y.raw", "Cb.raw", "Cr.raw"};
        const uint32_t cw[] = {comp_w[0], comp_w[1], comp_w[2]};
        const uint32_t ch[] = {comp_h[0], comp_h[1], comp_h[2]};
        for (int i = 0; i < 3; i++) {
          char path[1024];
          int n = snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
          if (n > 0 && (size_t)n < sizeof(path)) {
            FILE * f = fopen(path, "wb");
            if (f) {
              uint32_t w = cw[i];
              uint32_t h = ch[i];
              fwrite(&w, 4, 1, f);
              fwrite(&h, 4, 1, f);
              for (uint32_t row = 0; row < h; row++) {
                fwrite(comp_buf[i] + row * comp_stride[i], 1, (size_t)w, f);
              }
              fclose(f);
            }
          }
        }
      }
    }
#endif
    // YCbCr -> RGB per Rec. ITU-R BT.601 (JFIF/JPEG).
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
    // Fancy unless the caller explicitly asked for SIMPLE, so that NULL
    // options and zero-initialised options agree (GIMG_JPEG_CHROMA_UPSAMPLE_
    // DEFAULT is 0 and means FANCY).
    int use_fancy = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    const int frame_is_rgb = jpeg_frame_is_rgb(state, sof);
    jpeg_plane_t pl_cb = {comp_buf[1], comp_stride[1], 0};
    jpeg_plane_t pl_cr = {comp_buf[2], comp_stride[2], 0};
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        // jpeg_component_index returns a flat offset into the plane, so this
        // one is indexed directly rather than through the (x, y) accessor.
        int yy = (int)comp_buf[0][jpeg_component_index(
            comp_w[0], comp_h[0], comp_stride[0], x, y, width, height)];
        int cb = jpeg_chroma_sample(&pl_cb, cw1, ch1, x, y, width, height,
            sof->h_samp[1], sof->v_samp[1], h_max, v_max, use_fancy);
        int cr = jpeg_chroma_sample(&pl_cr, cw2, ch2, x, y, width, height,
            sof->h_samp[2], sof->v_samp[2], h_max, v_max, use_fancy);
        int r_val, g_val, b_val;
        // T.81 describes no colour space at all; jpeg_frame_is_rgb reads the
        // conventions that do (JFIF, Adobe APP14, the component identifiers).
        // A frame that already carries R, G, B is passed through: converting it
        // as though it were YCbCr turns every pixel into a different colour.
        if (frame_is_rgb) {
          r_val = yy;
          g_val = cb;
          b_val = cr;
        }
        else {
          jpeg_ycbcr_to_rgb(yy, cb, cr, 128, 255, &r_val, &g_val, &b_val);
        }
        pixels[y * stride + x * 4 + 0] = (unsigned char)r_val;
        pixels[y * stride + x * 4 + 1] = (unsigned char)g_val;
        pixels[y * stride + x * 4 + 2] = (unsigned char)b_val;
        pixels[y * stride + x * 4 + 3] = 255;
      }
    }
  }
  else if (num_comp == 4) {
    // GIMG_JPEG_CHROMA_UPSAMPLE_DEFAULT is 0 and means FANCY, so absent
    // options and zero-initialised options agree.
    int use_fancy_4 = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    r = jpeg_emit_four_component(alloc, state->adobe_transform,
        (uint32_t)width, (uint32_t)height, 8, 0,
        (const void * const *)comp_buf, comp_stride, comp_w, comp_h,
        sof->h_samp, sof->v_samp, use_fancy_4, out_raster);
    if (r != GIMG_OK) {
      goto fail_decode;
    }
  }
  else {
    r = GIMG_ERR_UNSUPPORTED;
    goto fail_decode;
  }

  // Attach ICC profile from APP2 to raster color info (single or
  // multi-segment). For CMYK, always set color info so cmyk_polarity is set.
  if (num_comp == 4u ||
      (state->app2_icc && state->app2_icc_len > 0u)) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (num_comp == 4u) {
      color_info.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
    }
    if (state->app2_icc && state->app2_icc_len > 0u) {
      if (state->app2_icc_num_chunks > 0) {
        color_info.icc_bytes = state->app2_icc;
        color_info.icc_size = state->app2_icc_len;
      }
      else {
        color_info.icc_bytes = state->app2_icc + 14;
        color_info.icc_size = state->app2_icc_len - 14u;
      }
    }
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

fail_decode:
  if (*out_raster) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
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

/** Progressive decode for 8- or 12-bit precision. Grayscale and YCbCr only. */
/**
 * Decode every scan of a progressive frame into its coefficient buffers
 * (T.81 Annex G).
 *
 * Shared by the single-frame progressive decoders and the hierarchical one,
 * because the scans of a differential progressive frame (SOF6, SOF14) are read
 * exactly as an ordinary frame's are.  J.2.3.1 changes two things about such a
 * frame, and only one of them happens here: "the DC coefficient of the DCT is
 * decoded directly - without prediction", which @p differential asks for by
 * clearing the predictor before each first-pass DC block.  The other - the IDCT
 * taken without the level shift - belongs to whoever turns these coefficients
 * back into samples.
 *
 * The caller owns @p coef_blocks and frees them whatever this returns.  The
 * geometry arguments are the caller's, because it has already had to work them
 * out to size those buffers.
 */
GIMG_Result jpeg_decode_progressive_scans(const gimg_jpeg_doc_state_t * state,
    const gimg_jpeg_sof_t * sof, const gimg_jpeg_scan_t * scans,
    unsigned num_scans, int is_arithmetic, const jpeg_arith_cond_t * cond,
    int differential, int sequential, uint32_t mcu_per_row,
    uint32_t mcu_per_col, const uint32_t * blk_w, const uint32_t * blk_h,
    const uint32_t * grid_w, int16_t * const * coef_blocks) {
  const uint8_t num_comp = sof->num_components;
  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  for (unsigned scan_idx = 0; scan_idx < num_scans; scan_idx++) {
    const gimg_jpeg_scan_t * scan = &scans[scan_idx];
    // An arithmetic scan may legitimately be empty (T.81 D.2.9); see the
    // sequential path for why.
    if (is_arithmetic ? (scan->data_size != 0 && !scan->data)
                             : (!scan->data || scan->data_size == 0)) {
      return GIMG_ERR_CORRUPT;
    }
    // F.2.1.3.1 and G.1.2.1: the DC prediction starts again at every scan, not
    // only at every restart.  With one scan per frame that never showed; a
    // sequential frame coded as one scan per component has three.
    memset(dc_pred, 0, sizeof(dc_pred));
    int is_dc = (scan->ss == 0 && scan->se == 0);
    gimg_jpeg_huff_table_t dc_tables[4];
    gimg_jpeg_huff_table_t ac_tables[4];
    gimg_jpeg_huff_table_t ac_refine_tables[4];
    memset(dc_tables, 0, sizeof(dc_tables));
    memset(ac_tables, 0, sizeof(ac_tables));
    memset(ac_refine_tables, 0, sizeof(ac_refine_tables));
    for (uint8_t c = 0; c < scan->comp_count; c++) {
      if (is_arithmetic) {
        // SOF10 carries no DHT segments; see the sequential path.
        break;
      }
      uint8_t dc_id = scan->dc_tbl[c];
      uint8_t ac_id = scan->ac_tbl[c];
      const unsigned char * dc_src =
          (scan->huff_dc[dc_id] && scan->huff_dc_len[dc_id] > 0)
          ? scan->huff_dc[dc_id]
          : state->huff_dc[dc_id];
      size_t dc_len = (scan->huff_dc[dc_id] && scan->huff_dc_len[dc_id] > 0)
          ? scan->huff_dc_len[dc_id]
          : state->huff_dc_len[dc_id];
      // T.81 B.2.4: the table for this scan is the one most recently defined
      // before its entropy-coded segment, which is exactly what the SOS
      // snapshot holds.  Fall back to the Annex K.4 default only when the file
      // never defined one.  Refinement scans take the same table; there is no
      // second kind.
      const unsigned char * ac_src =
          (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0)
          ? scan->huff_ac[ac_id]
          : state->huff_ac[ac_id];
      size_t ac_len = (scan->huff_ac[ac_id] && scan->huff_ac_len[ac_id] > 0)
          ? scan->huff_ac_len[ac_id]
          : state->huff_ac_len[ac_id];
      if (!ac_src || ac_len == 0) {
        ac_src = jpeg_default_ac_dht_payload(&ac_len);
      }
      if (dc_id >= 4 || !dc_src || dc_len == 0 ||
          jpeg_build_huff_table(dc_src, dc_len, &dc_tables[dc_id]) != 0) {
        return GIMG_ERR_CORRUPT;
      }
      if (ac_id >= 4 || !ac_src || ac_len == 0 ||
          jpeg_build_huff_table(ac_src, ac_len, &ac_tables[ac_id]) != 0) {
        return GIMG_ERR_CORRUPT;
      }
    }
    // T.81 B.2.2: segment ends at next marker; padding bit value unspecified.
    // We track expected block count and treat underflow in the last block as EOB/0
    // so we do not assume 0 or 1 for padding (spec compliance, third-party files).
    // Do not set pad_at_eob — use last-block underflow handling instead.
    // T.81 A.2: what an MCU *is* depends on how many components the scan has.
    //
    // With one component (A.2.2) the scan is non-interleaved and an MCU is a
    // single block, so the scan covers that component's own block grid:
    // ceil(X_i/8) x ceil(Y_i/8) MCUs, in raster order, with no MCU padding.
    // With several (A.2.3) an MCU is H_i x V_i blocks of each component and the
    // scan covers mcu_per_row x mcu_per_col MCUs.
    //
    // Every progressive AC scan is non-interleaved - G.1.2.2 allows no other
    // arrangement - so the one-component case is the common one, not the
    // exception.  Walking the image MCU grid for those scans decoded blocks in
    // the wrong order and in the wrong quantity, which is why progressive
    // decode failed or returned wrong pixels on anything past a single MCU row.
    int scan_interleaved = (scan->comp_count > 1);
    uint8_t solo_comp = 0;
    if (!scan_interleaved) {
      for (; solo_comp < num_comp; solo_comp++) {
        if (sof->comp_id[solo_comp] == scan->comp_id[0])
          break;
      }
      if (solo_comp >= num_comp) {
        return GIMG_ERR_CORRUPT;
      }
    }
    uint32_t scan_mcus_x =
        scan_interleaved ? mcu_per_row : blk_w[solo_comp];
    uint32_t scan_mcus_y =
        scan_interleaved ? mcu_per_col : blk_h[solo_comp];

    size_t blocks_per_mcu_prog = 0;
    if (scan_interleaved) {
      for (uint8_t s = 0; s < scan->comp_count; s++) {
        uint8_t comp_idx = 0;
        for (; comp_idx < num_comp; comp_idx++) {
          if (sof->comp_id[comp_idx] == scan->comp_id[s])
            break;
        }
        if (comp_idx < num_comp)
          blocks_per_mcu_prog +=
              (size_t)sof->h_samp[comp_idx] * (size_t)sof->v_samp[comp_idx];
      }
    }
    else {
      blocks_per_mcu_prog = 1;
    }
    size_t mcu_total_prog = 0;
    if (!gcu_safe_mul_size(
            (size_t)scan_mcus_x, (size_t)scan_mcus_y, &mcu_total_prog)) {
      return GIMG_ERR_CORRUPT;
    }
    size_t total_blocks_prog = 0;
    if (!gcu_safe_mul_size(
            mcu_total_prog, blocks_per_mcu_prog, &total_blocks_prog)) {
      return GIMG_ERR_CORRUPT;
    }

    gimg_jpeg_bitstream_t bs;
    jpeg_bitstream_init(&bs, scan->data, scan->data_size);
    // SOF10 is the same progressive process as SOF2 with the arithmetic coder
    // of Annex D.  Each scan starts its statistics afresh (T.81 F.2.4.1): the
    // model is per scan, not per frame, because successive scans of the same
    // band carry quite different decisions.
    jpeg_arith_decoder_t ad;
    jpeg_arith_stats_t astats;
    if (is_arithmetic) {
      jpeg_arith_decoder_init(&ad, scan->data, scan->data_size);
      jpeg_arith_stats_reset(&astats);
    }
    {
      const char * e = getenv("GIMG_JPEG_RECOVER_STUFF_ZERO");
      if (e && e[0] == '1') {
        bs.recover_stuff_zero = 1; // Opt-in recovery only; not from T.81.
      }
    }
    // The interval in force for this scan, not the frame's latest (B.2.4.4).
    uint16_t restart_interval = scan->restart_interval;
    int ss = (int)scan->ss;
    int se = (int)scan->se;
    int ah = (int)scan->ah;
    int al = (int)scan->al;
    unsigned int eobrun = 0;
    size_t block_counter_prog = 0;

    for (uint32_t mcu_y = 0; mcu_y < scan_mcus_y; mcu_y++) {
      for (uint32_t mcu_x = 0; mcu_x < scan_mcus_x; mcu_x++) {
        uint32_t mcu_index = mcu_y * scan_mcus_x + mcu_x;
        if (restart_interval > 0 && mcu_index > 0 &&
            mcu_index % (uint32_t)restart_interval == 0) {
          if (is_arithmetic) {
            // T.81 F.2.4.1: restart the coder and forget what it had learned.
            if (jpeg_arith_restart(&ad, &astats) != GIMG_OK) {
              return GIMG_ERR_CORRUPT;
            }
          }
          else {
            // Consume the restart marker here, at the MCU boundary, by
            // byte-aligning and reading it (T.81 B.2.1) - the same correction
            // the sequential path needed.  Setting expect_rst and leaving the
            // bitstream reader to notice the marker on its own does not work:
            // the reader only looks when it next needs a byte, which is after
            // it has already consumed bits belonging to the wrong side of the
            // boundary, and the scan desynchronises from there on.  That is why
            // every progressive file with a restart interval failed to decode.
            bs.expect_rst = 1; // T.81 3.1.110: next 0xFF 0xD0..0xD7 is RST
            jpeg_bitstream_align_skip_rst(&bs);
            if (bs.rst_just_skipped) {
              memset(dc_pred, 0, sizeof(dc_pred));
              bs.rst_just_skipped = 0;
            }
            // G.1.2.3: an EOB run counts blocks within one restart interval and
            // never continues across the marker.
            eobrun = 0;
          }
        }
        for (uint8_t s = 0; s < scan->comp_count; s++) {
          uint8_t comp_idx = 0;
          for (; comp_idx < num_comp; comp_idx++) {
            if (sof->comp_id[comp_idx] == scan->comp_id[s])
              break;
          }
          if (comp_idx >= num_comp) {
            return GIMG_ERR_CORRUPT;
          }
          // A non-interleaved scan contributes one block per MCU, at the MCU's
          // own raster position in this component's grid.  An interleaved scan
          // contributes H_i x V_i blocks, the MCU's top-left corner being at
          // (mcu_x*H_i, mcu_y*V_i).
          uint8_t h_samp = scan_interleaved ? sof->h_samp[comp_idx] : 1;
          uint8_t v_samp = scan_interleaved ? sof->v_samp[comp_idx] : 1;
          uint32_t blk_col0 = scan_interleaved ? mcu_x * h_samp : mcu_x;
          uint32_t blk_row0 = scan_interleaved ? mcu_y * v_samp : mcu_y;
          uint32_t row_stride = grid_w[comp_idx];

          for (uint8_t by = 0; by < v_samp; by++) {
            for (uint8_t bx = 0; bx < h_samp; bx++) {
              int is_last_prog =
                  (total_blocks_prog > 0 &&
                      block_counter_prog == total_blocks_prog - 1)
                  ? 1
                  : 0;
              size_t block_idx =
                  (size_t)(blk_row0 + (uint32_t)by) * (size_t)row_stride +
                  (size_t)(blk_col0 + (uint32_t)bx);
              int16_t * block = coef_blocks[comp_idx] + block_idx * 64;
              if (sequential) {
                // A sequential scan carries the whole block - Ss = 0, Se = 63,
                // no successive approximation (B.2.3) - so there is no band to
                // split and the ordinary block decoder does it in one go.  The
                // walk around it is the same one an Annex G scan needs, which
                // is why this lives here rather than in a second copy: A.2.3's
                // non-interleaved order is not a progressive idea, it is what
                // any scan naming one component uses.
                GIMG_Result r;
                if (differential) {
                  dc_pred[comp_idx] = 0; // J.2.3.1: DC decoded directly
                  if (is_arithmetic) {
                    astats.dc_pred[comp_idx] = 0;
                  }
                }
                if (is_arithmetic) {
                  r = jpeg_arith_decode_block_sequential(&ad, &astats, cond,
                      comp_idx, scan->dc_tbl[s], scan->ac_tbl[s], 63, block);
                }
                else {
                  r = jpeg_decode_block(&bs, &dc_tables[scan->dc_tbl[s]],
                      &ac_tables[scan->ac_tbl[s]], block, &dc_pred[comp_idx],
                      is_last_prog);
                }
                if (r != GIMG_OK) {
                  return GIMG_ERR_CORRUPT;
                }
              }
              else if (is_dc) {
                GIMG_Result r;
                // J.2.3.1: in a differential frame the DC coefficient is
                // decoded directly.  Only the first pass predicts - a
                // refinement scan (Ah != 0) merely appends a bit - so this is
                // the one place the predictor has to be taken out of play.
                if (differential && ah == 0) {
                  dc_pred[comp_idx] = 0;
                  if (is_arithmetic) {
                    astats.dc_pred[comp_idx] = 0;
                  }
                }
                if (is_arithmetic) {
                  r = (ah == 0)
                      ? jpeg_arith_decode_block_prog_dc_first(&ad, &astats,
                            cond, comp_idx, scan->dc_tbl[s], al,
                            block)
                      : jpeg_arith_decode_block_prog_dc_refine(
                            &ad, &astats, al, block);
                }
                else if (ah == 0) {
                  r = jpeg_decode_block_progressive_dc(&bs,
                      &dc_tables[scan->dc_tbl[s]], block, &dc_pred[comp_idx],
                      al, NULL, NULL, 0, is_last_prog);
                }
                else {
                  r = jpeg_decode_block_progressive_dc_refine(&bs, block,
                      &dc_pred[comp_idx], (unsigned int)al, NULL, 0,
                      is_last_prog);
                }
                if (r != GIMG_OK) {
                  return GIMG_ERR_CORRUPT;
                }
                if (block_counter_prog < 6 &&
                    GIMG_JPEG_TRACE_PROG_FIRST_DC) {
                  (void)fprintf(stderr,
                      "PROG_DEC_DC block=%zu comp=%u block[0]=%d\n",
                      block_counter_prog, (unsigned)comp_idx, (int)block[0]);
                  (void)fflush(stderr);
                }
              }
              else if (is_arithmetic) {
                GIMG_Result r = (ah == 0)
                    ? jpeg_arith_decode_block_prog_ac_first(&ad, &astats,
                          cond, scan->ac_tbl[s], ss, se, al,
                          block)
                    : jpeg_arith_decode_block_prog_ac_refine(
                          &ad, &astats, scan->ac_tbl[s], ss, se, al, block);
                if (r != GIMG_OK) {
                  return GIMG_ERR_CORRUPT;
                }
              }
              else {
                if (ah == 0) {
                  int trace_blk = GIMG_JPEG_TRACE_PROG_FIRST_AC
                      ? (int)block_counter_prog
                      : -1;
                  GIMG_Result r = jpeg_decode_block_progressive_ac_initial(&bs,
                      &ac_tables[scan->ac_tbl[s]], block, ss, se, al, 0,
                      trace_blk, 0u, &eobrun, 0, is_last_prog);
                  if (r != GIMG_OK) {
                    return GIMG_ERR_CORRUPT;
                  }
                }
                else {
                  // T.81 G.1.1.2.2: a refinement scan uses the AC table its
                  // SOS names through Ta, which is the same table any other
                  // scan would get.  The table is usually small - refinement
                  // only ever emits symbols with s in {0,1} - but small is not
                  // a different kind of table.
                  const gimg_jpeg_huff_table_t * ac_ref_tbl =
                      &ac_tables[scan->ac_tbl[s]];
                  if (ac_ref_tbl->num_values == 0) {
                    return GIMG_ERR_CORRUPT;
                  }
                  GIMG_Result r =
                      jpeg_decode_block_progressive_ac_refine(&bs, ac_ref_tbl,
                          block, ss, se, al, 0, -1, 0, 0, -1, &eobrun,
                          is_last_prog);
                  if (r != GIMG_OK) {
                    return GIMG_ERR_CORRUPT;
                  }
                }
              }
              block_counter_prog++;
            }
          }
        }
      }
    }
  }

  return GIMG_OK;
}

static GIMG_Result jpeg_decode_progressive_extended(
    const gimg_jpeg_doc_state_t * state, const GIMG_Decode_Options * options,
    GIMG_Raster ** out_raster) {
  const gimg_jpeg_sof_t * sof = &state->sof;
  uint8_t precision = sof->precision;
  if (precision != 8 && precision != 12) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint16_t width = sof->width;
  uint16_t height = sof->height;
  uint8_t num_comp = sof->num_components;
  // Four is CMYK or YCCK; see jpeg_emit_four_component.  This walk serves
  // progressive frames, sequential frames written as several scans, and 12-bit
  // frames, and used to refuse all three of those with four components while
  // the baseline walk accepted them.
  if (num_comp != 1 && num_comp != 3 && num_comp != 4) {
    return GIMG_ERR_UNSUPPORTED;
  }

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
    if (sof->h_samp[i] > h_max)
      h_max = sof->h_samp[i];
    if (sof->v_samp[i] > v_max)
      v_max = sof->v_samp[i];
  }
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t mcu_w = (uint32_t)(8 * h_max);
  uint32_t mcu_h = (uint32_t)(8 * v_max);
  uint32_t mcu_per_row = (width + mcu_w - 1) / mcu_w;
  uint32_t mcu_per_col = (height + mcu_h - 1) / mcu_h;

  // Per-component dimensions in samples (T.81): X_i = ceil(X*H_i/H_max).
  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_w[i] = (width * (uint32_t)sof->h_samp[i] + (uint32_t)h_max - 1) /
        (uint32_t)h_max;
    comp_h[i] = (height * (uint32_t)sof->v_samp[i] + (uint32_t)v_max - 1) /
        (uint32_t)v_max;
    if (comp_w[i] == 0)
      comp_w[i] = 8;
    if (comp_h[i] == 0)
      comp_h[i] = 8;
  }

  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);

  // One block grid per component, in raster order, used by every scan and by
  // the IDCT alike.  T.81 A.2: a component covers ceil(X_i/8) x ceil(Y_i/8)
  // blocks of real samples (blk_w/blk_h), but an interleaved scan walks whole
  // MCUs and so addresses mcu_per_row*H_i x mcu_per_col*V_i blocks, padding the
  // grid out at the right and bottom edges.  The array is allocated to the
  // larger of the two (grid_w/grid_h) and grid_w is the row stride everywhere.
  //
  // Keeping these separate matters: the blocks the IDCT reads are blk_w x
  // blk_h, while the stride between rows is grid_w.  Conflating them is what
  // made this decoder write past the end of the buffer and read coefficients
  // from the wrong blocks.
  uint32_t blk_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t blk_h[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t grid_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t grid_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    blk_w[i] = (comp_w[i] + 7u) / 8u;
    blk_h[i] = (comp_h[i] + 7u) / 8u;
    grid_w[i] = mcu_per_row * (uint32_t)sof->h_samp[i];
    grid_h[i] = mcu_per_col * (uint32_t)sof->v_samp[i];
    if (blk_w[i] > grid_w[i])
      grid_w[i] = blk_w[i];
    if (blk_h[i] > grid_h[i])
      grid_h[i] = blk_h[i];
  }

  size_t blocks_per_comp[GIMG_JPEG_MAX_COMPONENTS];
  int16_t * coef_blocks[GIMG_JPEG_MAX_COMPONENTS];
  memset(coef_blocks, 0, sizeof(coef_blocks));
  for (uint8_t i = 0; i < num_comp; i++) {
    size_t bw = (size_t)grid_w[i];
    size_t bh = (size_t)grid_h[i];
    if (!gcu_safe_mul_size(bw, bh, &blocks_per_comp[i])) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, coef_blocks[j]);
      return GIMG_ERR_LIMIT;
    }
    size_t coef_size = 0;
    if (!gcu_safe_mul_size(
            blocks_per_comp[i], 64 * sizeof(int16_t), &coef_size)) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, coef_blocks[j]);
      return GIMG_ERR_LIMIT;
    }
    coef_blocks[i] = (int16_t *)gimg_malloc(alloc, coef_size);
    if (!coef_blocks[i]) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, coef_blocks[j]);
      return GIMG_ERR_OOM;
    }
    memset(coef_blocks[i], 0, coef_size);
  }

  {
    GIMG_Result rr = jpeg_decode_progressive_scans(state, sof, state->scans,
        state->num_scans, state->is_arithmetic, &state->arith_cond, 0,
        !state->is_progressive, mcu_per_row, mcu_per_col, blk_w, blk_h, grid_w,
        coef_blocks);
    if (rr != GIMG_OK) {
      goto prog_ext_fail;
    }
  }

  // T.81 A.3.3 (equation 8.3): the inverse DCT is
  //     s(x) = 1/2 * sum_u C(u) S(u) cos((2x+1) u pi / 16),  C(0) = 1/sqrt(2).
  // jpeg_idct_1d_32 accumulates sum_u S(u) * c_u * cos(...) with c_u = 256, or
  // 181 for u = 0 (181/256 is 1/sqrt(2)), then divides by this scale.  One pass
  // therefore yields (512 / scale) * s(x), and the separable two-pass transform
  // squares that, so only scale = 512 reproduces the transform.  It was 4096,
  // which is a factor of (512/4096)^2 = 1/64: every 12-bit image decoded to a
  // band roughly 1/64 of its true contrast, clustered around mid-grey, and no
  // test noticed because none compared 12-bit sample values.
  (void)precision;
  int level_shift = (precision == 12) ? 2048 : 32768;
  int max_val = (precision == 12) ? 4095 : 65535;

  size_t comp_stride_el[GIMG_JPEG_MAX_COMPONENTS];
  size_t comp_size[GIMG_JPEG_MAX_COMPONENTS];
  uint16_t * comp_buf[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    comp_stride_el[i] = (size_t)comp_w[i];
    if (!gcu_safe_mul_size(
            comp_stride_el[i], (size_t)comp_h[i], &comp_size[i]) ||
        !gcu_safe_mul_size(comp_size[i], sizeof(uint16_t), &comp_size[i])) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      goto prog_ext_fail;
    }
    comp_buf[i] = (uint16_t *)gimg_malloc(alloc, comp_size[i]);
    if (!comp_buf[i]) {
      for (uint8_t j = 0; j < i; j++)
        gimg_free(alloc, comp_buf[j]);
      goto prog_ext_fail;
    }
    memset(comp_buf[i], 0, comp_size[i]);
  }

  int16_t block_rz[64];
  int32_t block_q[64];
  int32_t block_idct[64];
  for (uint8_t comp_idx = 0; comp_idx < num_comp; comp_idx++) {
    uint8_t qid = sof->quant_tbl_id[comp_idx];
    if (qid >= GIMG_JPEG_MAX_QUANT_TABLES || !state->quant_tbl_present[qid]) {
      goto prog_ext_fail_buf;
    }
    const uint16_t * quant = state->quant_tbl[qid];
    // Read the blocks that hold real samples, addressed with the grid's row
    // stride.  These are two different numbers whenever the component is not a
    // whole number of MCUs wide, and the truncating comp_w/8 used here before
    // dropped the last partial block column entirely.
    uint32_t blocks_w = blk_w[comp_idx];
    uint32_t blocks_h = blk_h[comp_idx];
    uint32_t row_stride = grid_w[comp_idx];
    for (uint32_t by = 0; by < blocks_h; by++) {
      for (uint32_t bx = 0; bx < blocks_w; bx++) {
        size_t block_idx = (size_t)by * (size_t)row_stride + (size_t)bx;
        const int16_t * block_zig = coef_blocks[comp_idx] + block_idx * 64;
        jpeg_dezigzag(block_zig, block_rz);
        uint32_t dst_x = bx * 8;
        uint32_t dst_y = by * 8;
        if (precision == 8) {
          // Same pipeline as baseline 8-bit: dequant (int16_t) + islow IDCT.
          // Chroma may underflow/overflow (TBD: match baseline or use 32-bit).
          jpeg_dequantise_32(block_rz, quant, block_q);
          // pass1_bits 2: the 8-bit setting (libjpeg jidctint.c).
          jpeg_idct_8x8_islow(block_q, block_idct, 2);
          if (comp_idx == 1 && by == 0 && bx == 0 &&
              GIMG_JPEG_TRACE_FIRST_CB) {
            (void)fprintf(stderr,
                "PROGRESSIVE first Cb: quant[0]=%u dequant[0]=%d idct[0]=%d "
                "stored=%d\n",
                (unsigned)quant[0], (int)block_q[0], (int)block_idct[0],
                (int)block_idct[0] + 128);
            (void)fflush(stderr);
          }
          for (int dy = 0; dy < 8; dy++) {
            uint32_t y = dst_y + (uint32_t)dy;
            if (y >= comp_h[comp_idx])
              break;
            for (int dx = 0; dx < 8; dx++) {
              uint32_t x = dst_x + (uint32_t)dx;
              if (x >= comp_w[comp_idx])
                break;
              int v = block_idct[dy * 8 + dx] + 128;
              if (v < 0)
                v = 0;
              if (v > 255)
                v = 255;
              comp_buf[comp_idx][y * comp_stride_el[comp_idx] + x] =
                  (uint16_t)(unsigned)v;
            }
          }
        }
        else {
          jpeg_dequantise_32(block_rz, quant, block_q);
          // pass1_bits 1 at P=12: one fewer fractional bit between passes,
          // for the headroom the wider samples need (libjpeg jidctint.c).
          jpeg_idct_8x8_islow(block_q, block_idct, 1);
          for (int dy = 0; dy < 8; dy++) {
            uint32_t y = dst_y + (uint32_t)dy;
            if (y >= comp_h[comp_idx])
              break;
            for (int dx = 0; dx < 8; dx++) {
              uint32_t x = dst_x + (uint32_t)dx;
              if (x >= comp_w[comp_idx])
                break;
              int32_t v = block_idct[dy * 8 + dx] + level_shift;
              if (v < 0)
                v = 0;
              if (v > max_val)
                v = max_val;
              // Native precision in the plane; widened once on output.
              comp_buf[comp_idx][y * comp_stride_el[comp_idx] + x] =
                  (uint16_t)v;
            }
          }
        }
      }
    }
  }

  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
  }

  // For 8-bit precision we used islow IDCT and stored 0..255 in comp_buf;
  // output 8-bit raster (GRAY8/RGBA8) to match baseline decode.
  const int prog_8bit = (precision == 8);

  GIMG_Result r;
  if (prog_8bit && num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto prog_ext_fail_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        uint16_t v = comp_buf[0][y * comp_stride_el[0] + x];
        pixels[y * stride + x] = (unsigned char)(v > 255u ? 255u : v);
      }
    }
  }
  else if (prog_8bit && num_comp == 3) {
    // 8-bit colour: comp_buf holds 0..255; use same chroma and RGB as baseline.
    unsigned char * comp_buf_8[GIMG_JPEG_MAX_COMPONENTS];
    size_t comp_size_8[GIMG_JPEG_MAX_COMPONENTS];
    for (uint8_t i = 0; i < num_comp; i++) {
      comp_size_8[i] = comp_stride_el[i] * (size_t)comp_h[i];
      comp_buf_8[i] = (unsigned char *)gimg_malloc(alloc, comp_size_8[i]);
      if (!comp_buf_8[i]) {
        for (uint8_t j = 0; j < i; j++)
          gimg_free(alloc, comp_buf_8[j]);
        goto prog_ext_fail_buf;
      }
      for (size_t k = 0; k < comp_size_8[i]; k++) {
        uint16_t v = comp_buf[i][k];
        comp_buf_8[i][k] = (unsigned char)(v > 255u ? 255u : v);
      }
    }
    // Fancy unless the caller explicitly asked for SIMPLE, so that NULL
    // options and zero-initialised options agree (GIMG_JPEG_CHROMA_UPSAMPLE_
    // DEFAULT is 0 and means FANCY).
    int use_fancy = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    const int frame_is_rgb = jpeg_frame_is_rgb(state, sof);
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      for (uint8_t i = 0; i < num_comp; i++)
        gimg_free(alloc, comp_buf_8[i]);
      goto prog_ext_fail_buf;
    }
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    jpeg_plane_t pl_cb = {comp_buf_8[1], comp_stride_el[1], 0};
    jpeg_plane_t pl_cr = {comp_buf_8[2], comp_stride_el[2], 0};
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        // Flat offset, as above.
        int yy = (int)comp_buf_8[0][jpeg_component_index(comp_w[0], comp_h[0],
            comp_stride_el[0], x, y, width, height)];
        int cb = jpeg_chroma_sample(&pl_cb, cw1, ch1, x, y, width, height,
            sof->h_samp[1], sof->v_samp[1], h_max, v_max, use_fancy);
        int cr = jpeg_chroma_sample(&pl_cr, cw2, ch2, x, y, width, height,
            sof->h_samp[2], sof->v_samp[2], h_max, v_max, use_fancy);
        int r_val, g_val, b_val;
        // T.81 describes no colour space at all; jpeg_frame_is_rgb reads the
        // conventions that do (JFIF, Adobe APP14, the component identifiers).
        // A frame that already carries R, G, B is passed through: converting it
        // as though it were YCbCr turns every pixel into a different colour.
        if (frame_is_rgb) {
          r_val = yy;
          g_val = cb;
          b_val = cr;
        }
        else {
          jpeg_ycbcr_to_rgb(yy, cb, cr, 128, 255, &r_val, &g_val, &b_val);
        }
        pixels[y * stride + x * 4 + 0] = (unsigned char)r_val;
        pixels[y * stride + x * 4 + 1] = (unsigned char)g_val;
        pixels[y * stride + x * 4 + 2] = (unsigned char)b_val;
        pixels[y * stride + x * 4 + 3] = 255;
      }
    }
    for (uint8_t i = 0; i < num_comp; i++) {
      gimg_free(alloc, comp_buf_8[i]);
    }
  }
  else if (num_comp == 4) {
    // The planes go straight to the shared assembly, at either precision.
    // They used to be narrowed to 8 bits first, which is why a twelve-bit
    // four-component frame was refused outright: T.81 Table B.2 allows P=12 in
    // a DCT frame and B.2.2 allows Nf=4, so the two together are a legal file
    // that had nowhere to be decoded to until GIMG_PIXEL_CMYK16 existed.
    int use_fancy_4 = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    r = jpeg_emit_four_component(alloc, state->adobe_transform, (uint32_t)width,
        (uint32_t)height, (int)precision, 1, (const void * const *)comp_buf,
        comp_stride_el, comp_w, comp_h, sof->h_samp, sof->v_samp, use_fancy_4,
        out_raster);
    if (r != GIMG_OK) {
      goto prog_ext_fail_buf;
    }
  }
  else if (num_comp == 1) {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto prog_ext_fail_buf;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 2;
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        uint16_t v = comp_buf[0][jpeg_component_index(comp_w[0], comp_h[0],
            comp_stride_el[0], x, y, width, height)];
        pixels[y * stride_el + x] =
            (precision == 12) ? gimg_bitdepth_12_to_16(v) : v;
      }
    }
  }
  else {
    r = gimg_raster_create_with_allocator(alloc, (uint32_t)width,
        (uint32_t)height, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED, NULL, 0,
        out_raster);
    if (r != GIMG_OK) {
      goto prog_ext_fail_buf;
    }
    uint16_t * pixels = (uint16_t *)gimg_raster_pixels(*out_raster);
    // uint16 elements, not pixels: the row is indexed as
    // pixels[y * stride_el + x * 4 + c] through a uint16_t *, so the divisor is
    // sizeof(uint16_t) and not the 8 bytes an RGBA16 pixel occupies.  Dividing
    // by 8 made the stride a quarter of a row, so every 12-bit colour frame was
    // written into the first quarter of its own raster and the rest left blank.
    size_t stride_el = gimg_raster_stride_bytes(*out_raster) / 2;
    uint32_t cw1 = comp_w[1];
    uint32_t ch1 = comp_h[1];
    uint32_t cw2 = comp_w[2];
    uint32_t ch2 = comp_h[2];
    // Fancy unless the caller explicitly asked for SIMPLE, so that NULL
    // options and zero-initialised options agree (GIMG_JPEG_CHROMA_UPSAMPLE_
    // DEFAULT is 0 and means FANCY).
    int use_fancy = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    const int frame_is_rgb = jpeg_frame_is_rgb(state, sof);
    jpeg_plane_t pl_cb = {comp_buf[1], comp_stride_el[1], 1};
    jpeg_plane_t pl_cr = {comp_buf[2], comp_stride_el[2], 1};
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        int yy = (int)comp_buf[0][jpeg_component_index(comp_w[0], comp_h[0],
            comp_stride_el[0], x, y, width, height)];
        // The same filters the 8-bit path uses; see the baseline extended path.
        int cb = jpeg_chroma_sample(&pl_cb, cw1, ch1, x, y, width, height,
            sof->h_samp[1], sof->v_samp[1], h_max, v_max, use_fancy);
        int cr = jpeg_chroma_sample(&pl_cr, cw2, ch2, x, y, width, height,
            sof->h_samp[2], sof->v_samp[2], h_max, v_max, use_fancy);
        // T.81 A.3.1: a reconstructed sample lies in 0..2^P-1.  Clamp there,
        // then widen once to the 16-bit raster.
        int r_val, g_val, b_val;
        // T.81 describes no colour space at all; jpeg_frame_is_rgb reads the
        // conventions that do (JFIF, Adobe APP14, the component identifiers).
        // A frame that already carries R, G, B is passed through: converting it
        // as though it were YCbCr turns every pixel into a different colour.
        if (frame_is_rgb) {
          r_val = yy;
          g_val = cb;
          b_val = cr;
        }
        else {
          jpeg_ycbcr_to_rgb(
              yy, cb, cr, level_shift, max_val, &r_val, &g_val, &b_val);
        }
        if (precision == 12) {
          r_val = (int)gimg_bitdepth_12_to_16((uint16_t)r_val);
          g_val = (int)gimg_bitdepth_12_to_16((uint16_t)g_val);
          b_val = (int)gimg_bitdepth_12_to_16((uint16_t)b_val);
        }
        pixels[y * stride_el + x * 4 + 0] = (uint16_t)r_val;
        pixels[y * stride_el + x * 4 + 1] = (uint16_t)g_val;
        pixels[y * stride_el + x * 4 + 2] = (uint16_t)b_val;
        pixels[y * stride_el + x * 4 + 3] = (uint16_t)65535;
      }
    }
  }

  if (state->app2_icc && state->app2_icc_len > 0u) {
    GIMG_Color_Info color_info;
    gimg_color_info_default(&color_info);
    if (state->app2_icc_num_chunks > 0) {
      color_info.icc_bytes = state->app2_icc;
      color_info.icc_size = state->app2_icc_len;
    }
    else {
      color_info.icc_bytes = state->app2_icc + 14;
      color_info.icc_size = state->app2_icc_len - 14u;
    }
    (void)gimg_raster_set_color_info(*out_raster, &color_info);
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
  return GIMG_OK;

prog_ext_fail_buf:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, comp_buf[i]);
  }
prog_ext_fail:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef_blocks[i]);
  }
  return GIMG_ERR_CORRUPT;
}

/** Progressive decode entry: run all scans (DC, AC initial, AC/DC refinement)
 * into coefficient buffers, then dequant, IDCT, chroma upsample, assemble raster. */
GIMG_Result gimg_jpeg_decode_progressive(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!state || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (!state->is_progressive || state->num_scans == 0) {
    return GIMG_ERR_CORRUPT;
  }
  return jpeg_decode_progressive_extended(state, options, out_raster);
}