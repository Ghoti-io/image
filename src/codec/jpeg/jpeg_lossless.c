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
uint32_t jpeg_sample_widen(uint32_t v, int from, int to) {
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
int32_t jpeg_lossless_predict(
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
GIMG_Result jpeg_lossless_decode_diff(gimg_jpeg_bitstream_t * bs,
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
  // A.2.2: when a scan names one component it is non-interleaved, the data
  // units are "left-to-right, top-to-bottom within the component", and the
  // sampling factors say nothing about the walk - the scan simply covers that
  // component's own samples in raster order.  Treating such a scan as a grid
  // of Hi x Vi blocks visits the wrong samples, and for a one-component frame
  // with Hi above one it visits samples that are not in the image at all.
  const int scan_interleaved = (scan->comp_count > 1);

  uint32_t comp_w[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t comp_h[GIMG_JPEG_MAX_COMPONENTS];
  uint16_t * plane[GIMG_JPEG_MAX_COMPONENTS];
  memset(plane, 0, sizeof(plane));
  // Declared here rather than beside the arithmetic setup below, because the
  // allocation failures between here and there jump straight to the cleanup,
  // which frees these.
  uint8_t * db_cat[GIMG_JPEG_MAX_COMPONENTS];
  int da_cat[GIMG_JPEG_MAX_COMPONENTS];
  memset(db_cat, 0, sizeof(db_cat));
  memset(da_cat, 0, sizeof(da_cat));
  /**
   * Whether each line is predicted one-dimensionally: one byte per line of
   * each component, rather than one flag per component.
   *
   * It is a property of the line - H.1.2.1 speaks of "the first line of
   * samples at the start of the scan and at the beginning of each restart
   * interval" - and a single carried flag only behaves like one when the
   * samples arrive in raster order.  They do not.  An interleaved scan walks
   * MCUs, so with Hi above one it finishes the MCU's lines before returning to
   * the first line in the next MCU along, by which time a flag set for the
   * last line of the previous MCU is being read on the first line of this one.
   * On the first line that asks for the sample above it, which is off the
   * front of the plane: the fuzzer found a 110-byte file that read wild memory
   * that way, on a 17x9 frame with Hi = 2.
   */
  unsigned char * row_1d[GIMG_JPEG_MAX_COMPONENTS];
  memset(row_1d, 0, sizeof(row_1d));
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

  // T.81 Table B.1: SOF11 is this same predictive process with the arithmetic
  // coder of Annex D in place of the Huffman coder.  Only the entropy layer
  // differs - predictors, point transform, restart handling and the sample
  // raster below are shared - so the two paths diverge at exactly one point,
  // where a difference is read.
  const int is_arith = state->is_arithmetic;

  gimg_jpeg_huff_table_t dc_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  if (!is_arith) {
    for (uint8_t c = 0; c < scan->comp_count; c++) {
      uint8_t id = scan->dc_tbl[c];
      if (id >= 4 || !state->huff_dc[id] ||
          jpeg_build_huff_table(
              state->huff_dc[id], state->huff_dc_len[id], &dc_tables[id]) != 0) {
        r = GIMG_ERR_CORRUPT;
        goto fail;
      }
    }
  }

  gimg_jpeg_bitstream_t bs;
  jpeg_bitstream_init(&bs, scan->data, scan->data_size);

  // H.1.2.3.1: the conditioning needs the category of the difference coded for
  // the sample above, so one row of categories per component is carried from
  // line to line.  Categories, not the differences themselves - five values is
  // all the model looks at.
  jpeg_arith_decoder_t ad;
  jpeg_arith_lossless_stats_t astats;
  if (is_arith) {
    jpeg_arith_decoder_init(&ad, scan->data, scan->data_size);
    jpeg_arith_lossless_stats_reset(&astats);
    for (uint8_t i = 0; i < num_comp; i++) {
      db_cat[i] = (uint8_t *)gimg_malloc(alloc, comp_w[i]);
      if (!db_cat[i]) {
        r = GIMG_ERR_OOM;
        goto fail;
      }
      memset(db_cat[i], 0, comp_w[i]);
    }
  }

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
  // H.1.2.1 describes the predictor as a per-line state: the first line of the
  // scan is predicted one-dimensionally from Ra, later lines use the selected
  // predictor and take Rb at their start.  A restart puts the coder back into
  // that first-line state - libjpeg does it by calling start_pass again from
  // its restart handler, and the ISO reference codec behaves the same way.
  //
  // Because the state is per line, a restart only changes the prediction when
  // it falls on a line boundary.  libjpeg refuses a restart interval that is
  // not a whole number of MCU rows ("must be an integer multiple of the number
  // of MCUs in an MCU row"), so for everything it will read, it always does.
  // A mid-row interval is left predicting as though no restart had happened,
  // which is what the reference codec produces and the only reading under
  // which such a file decodes at all.
  for (uint8_t i = 0; i < num_comp; i++) {
    row_1d[i] = (unsigned char *)gimg_malloc(alloc, comp_h[i]);
    if (!row_1d[i]) {
      r = GIMG_ERR_OOM;
      goto fail;
    }
    memset(row_1d[i], 1, comp_h[i]); // until a line says otherwise
  }
  int restart_now = 0;

  // The scan's own grid: the image's MCU grid for an interleaved scan, the
  // single component's sample grid for a non-interleaved one.
  uint32_t solo = 0;
  if (!scan_interleaved) {
    for (; solo < num_comp; solo++) {
      if (sof->comp_id[solo] == scan->comp_id[0]) {
        break;
      }
    }
    if (solo >= num_comp) {
      r = GIMG_ERR_CORRUPT;
      goto fail;
    }
  }
  const uint32_t scan_mcus_x = scan_interleaved
      ? mcu_per_row
      : ((width * sof->h_samp[solo] + h_max - 1u) / h_max);
  const uint32_t scan_mcus_y = scan_interleaved
      ? mcu_per_col
      : ((height * sof->v_samp[solo] + v_max - 1u) / v_max);

  for (uint32_t mcu_y = 0; mcu_y < scan_mcus_y; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < scan_mcus_x; mcu_x++) {
      uint32_t mcu_index = mcu_y * scan_mcus_x + mcu_x;
      if (restart_interval > 0 && mcu_index > 0 &&
          mcu_index % (uint32_t)restart_interval == 0) {
        if (is_arith) {
          // D.2.9 and H.1.2.3.4: the decoder is primed afresh past the marker
          // and every statistics bin goes back to its initial state.
          r = jpeg_arith_lossless_restart(&ad, &astats);
          if (r != GIMG_OK) {
            goto fail;
          }
          // H.1.2.3.1: "At the beginning of the scan and each restart interval
          // the conditioning derived from the line above is set to zero."
          for (uint8_t i = 0; i < num_comp; i++) {
            if (db_cat[i]) {
              memset(db_cat[i], 0, comp_w[i]);
            }
            da_cat[i] = 0;
          }
        }
        else {
          bs.expect_rst = 1;
          jpeg_bitstream_align_skip_rst(&bs);
          if (bs.rst_just_skipped) {
            bs.rst_just_skipped = 0;
          }
        }
        restart_now = 1;
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
        // A non-interleaved scan contributes one sample per MCU, at the MCU's
        // own position in the component's grid (A.2.2).
        uint8_t hs = scan_interleaved ? sof->h_samp[ci] : 1u;
        uint8_t vs = scan_interleaved ? sof->v_samp[ci] : 1u;
        for (uint8_t sy = 0; sy < vs; sy++) {
          for (uint8_t sx = 0; sx < hs; sx++) {
            uint32_t x = mcu_x * hs + sx;
            uint32_t y = mcu_y * vs + sy;
            int32_t diff = 0;
            if (is_arith) {
              // H.1.2.3.1: at the start of each line the difference to the
              // left is taken as zero for conditioning purposes.
              if (x == 0) {
                da_cat[ci] = 0;
              }
              int cat = 0;
              r = jpeg_arith_lossless_decode_diff(&ad, &astats,
                  &state->arith_cond, scan->dc_tbl[s], da_cat[ci],
                  (int)db_cat[ci][x], &diff, &cat);
              if (r != GIMG_OK) {
                goto fail;
              }
              // This difference becomes Da for the next sample on this line
              // and Db for the sample below it.  db_cat[x] is read above and
              // overwritten here, in that order.
              da_cat[ci] = cat;
              db_cat[ci][x] = (uint8_t)cat;
            }
            else {
              r = jpeg_lossless_decode_diff(&bs, tbl, &diff);
              if (r != GIMG_OK) {
                goto fail;
              }
            }
            int32_t pred;
            if (x == 0) {
              // H.1.2.1: 2^(P-Pt-1) begins the first line, and a restart makes
              // the line it lands on a first line again.  Otherwise a line
              // starts from the sample above it, whatever the frame's
              // predictor selection says.
              if (y == 0 || restart_now) {
                pred = initial_pred;
                row_1d[ci][y] = 1;
              }
              else {
                pred = (int32_t)plane[ci][(size_t)(y - 1) * cw];
                row_1d[ci][y] = 0;
              }
            }
            else if (row_1d[ci][y]) {
              pred = (int32_t)plane[ci][(size_t)y * cw + (x - 1)]; // Ra
            }
            else {
              int32_t ra = (int32_t)plane[ci][(size_t)y * cw + (x - 1)];
              int32_t rb = (int32_t)plane[ci][(size_t)(y - 1) * cw + x];
              int32_t rc = (int32_t)plane[ci][(size_t)(y - 1) * cw + (x - 1)];
              pred = jpeg_lossless_predict(psv, ra, rb, rc);
            }
            // H.1.2.1: the reconstruction is modulo 2^16.
            plane[ci][(size_t)y * cw + x] =
                (uint16_t)((uint32_t)(pred + diff) & 0xFFFFu);
          }
        }
      }
      restart_now = 0;
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
    const int frame_is_rgb = jpeg_frame_is_rgb(state, sof);
    // A lossless frame may subsample (T.81 Table B.2 bounds Hi and Vi the same
    // way it does for a DCT frame), in which case a component holds fewer
    // samples than the image has pixels and has to be read through the map
    // rather than indexed directly.  Reading it directly walked off the end of
    // the plane - two samples per row for a 17-wide image with 9-wide chroma,
    // and eventually past the allocation entirely.
    //
    // Which map: the same choice libjpeg makes.  A chroma component of a YCbCr
    // frame gets the triangle filter, because that is what the rest of this
    // codec does with chroma and what the caller's option selects; anything
    // else - an RGB frame's components, or a grey one's - gets replication,
    // which is libjpeg's int_upsample and the only defensible thing to do to a
    // component that is not chroma.
    int use_fancy = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    jpeg_plane_t pl[GIMG_JPEG_MAX_COMPONENTS];
    for (uint8_t c = 0; c < num_comp; c++) {
      pl[c].data = plane[c];
      pl[c].stride = comp_w[c];
      pl[c].wide = 1; // the lossless planes are uint16 whatever P says
    }
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        uint32_t v[3] = {0, 0, 0};
        for (uint8_t c = 0; c < num_comp; c++) {
          uint32_t sv;
          if (c > 0 && num_comp == 3 && !frame_is_rgb) {
            sv = (uint32_t)jpeg_chroma_sample(&pl[c], comp_w[c], comp_h[c], x,
                y, width, height, sof->h_samp[c], sof->v_samp[c], h_max, v_max,
                use_fancy);
          }
          else {
            sv = plane[c][jpeg_component_index(
                comp_w[c], comp_h[c], comp_w[c], x, y, width, height)];
          }
          sv = (uint32_t)((uint64_t)sv << pt); // undo the point transform
          if (sv > max_val) {
            sv = max_val;
          }
          v[c] = sv;
        }
        // A lossless frame's three components are no more inherently RGB than
        // any other frame's: T.81 describes no colour space, and the same
        // conventions decide it here as in the DCT paths.  Most lossless files
        // in the wild do carry RGB - they say so with an Adobe APP14 whose
        // transform is zero, or with 'R', 'G', 'B' as the component
        // identifiers - but a JFIF one is YCbCr and has to be converted.
        if (num_comp == 3 && !frame_is_rgb) {
          int rr = 0, gg = 0, bb = 0;
          jpeg_ycbcr_to_rgb((int)v[0], (int)v[1], (int)v[2],
              1 << (sample_bits - 1), (int)max_val, &rr, &gg, &bb);
          v[0] = (uint32_t)rr;
          v[1] = (uint32_t)gg;
          v[2] = (uint32_t)bb;
        }
        for (uint8_t c = 0; c < num_comp; c++) {
          v[c] = jpeg_sample_widen(v[c], sample_bits, out_bits);
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
    if (db_cat[i]) {
      gimg_free(alloc, db_cat[i]);
    }
    if (row_1d[i]) {
      gimg_free(alloc, row_1d[i]);
    }
  }
  if (r != GIMG_OK && *out_raster) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
  return r;
}

/* ------------------------------------------------------------------------
 * Encoder
 * ------------------------------------------------------------------------ */

/** T.81 H.1.2.2: SSSS runs from 0 to 16 for a lossless difference, so the
 * table has seventeen symbols rather than the DC table's twelve. */
#define JPEG_LL_SYMBOLS 17


/** Number of bits needed to hold the magnitude of a lossless difference
 * (T.81 H.1.2.2): the DC categories of F.1.2.1 with one more at the top. */
static int jpeg_lossless_category(int32_t diff) {
  if (diff == 32768) {
    return 16; // the one value that carries no additional bits
  }
  int32_t m = diff < 0 ? -diff : diff;
  int s = 0;
  while (m) {
    s++;
    m >>= 1;
  }
  return s;
}

/** Bit writer for the lossless scan.  Separate from the DCT encoder's because
 * that one lives in jpeg_encode.c; the stuffing rule is the same (B.1.1.5). */
typedef struct {
  unsigned char * buf;
  size_t cap;
  size_t len;
  uint32_t acc;
  int nbits;
  const GIMG_Allocator * alloc;
  int oom;
} jpeg_ll_writer;

typedef struct {
  jpeg_ll_writer * w;
  const GIMG_Allocator * alloc;
  int oom;
} jpeg_arith_sink_t;

static int jpeg_ll_ensure(jpeg_ll_writer * w, size_t extra) {
  if (w->len + extra <= w->cap) {
    return 1;
  }
  size_t cap = w->cap ? w->cap * 2 : 4096;
  if (cap < w->len + extra) {
    cap = w->len + extra;
  }
  unsigned char * p = (unsigned char *)gimg_realloc(w->alloc, w->buf, cap);
  if (!p) {
    w->oom = 1;
    return 0;
  }
  w->buf = p;
  w->cap = cap;
  return 1;
}

/** Byte sink for the arithmetic coder: D.1.6 already applies the B.1.1.5
 * stuffing, so these bytes go straight into the scan. */
static void jpeg_arith_lossless_sink_emit(void * ctx, unsigned char b) {
  jpeg_arith_sink_t * sink = (jpeg_arith_sink_t *)ctx;
  if (!jpeg_ll_ensure(sink->w, 1)) {
    sink->oom = 1;
    return;
  }
  sink->w->buf[sink->w->len++] = b;
}

static void jpeg_ll_put_bits(jpeg_ll_writer * w, uint32_t code, int n) {
  if (n <= 0) {
    return;
  }
  w->acc = (w->acc << n) | (code & (n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u)));
  w->nbits += n;
  while (w->nbits >= 8) {
    w->nbits -= 8;
    unsigned char b = (unsigned char)(w->acc >> w->nbits);
    if (!jpeg_ll_ensure(w, 2)) {
      return;
    }
    w->buf[w->len++] = b;
    if (b == 0xFF) {
      w->buf[w->len++] = 0x00; // B.1.1.5 byte stuffing
    }
  }
}

/** B.2.2: pad the last byte with ones; the value is the encoder's choice. */
static void jpeg_ll_flush(jpeg_ll_writer * w) {
  if (w->nbits > 0) {
    jpeg_ll_put_bits(w, 0x7F, 8 - w->nbits);
  }
  w->acc = 0;
  w->nbits = 0;
}

GIMG_Result gimg_jpeg_encode_lossless(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, int psv, uint16_t restart_interval,
    int arithmetic, unsigned char ** out_scan_data, size_t * out_scan_size,
    unsigned char ** out_dht, size_t * out_dht_len, uint32_t * out_width,
    uint32_t * out_height, int * out_num_components, int * out_precision) {
  if (!alloc || !raster || !out_scan_data || !out_scan_size || !out_dht ||
      !out_dht_len) {
    return GIMG_ERR_INTERNAL;
  }
  *out_scan_data = NULL;
  *out_scan_size = 0;
  *out_dht = NULL;
  *out_dht_len = 0;

  if (psv < 1 || psv > 7) {
    return GIMG_ERR_UNSUPPORTED;
  }
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || width == 0 || height == 0) {
    return GIMG_ERR_UNSUPPORTED;
  }

  // The frame's precision follows the raster: Table B.2 permits 2 to 16 in a
  // lossless frame, so there is no need to convert anything.
  int precision = (int)fmt->bits_per_channel[0];
  int channels = (int)fmt->channel_count;
  int num_comp = (fmt->channel_model == GIMG_CHANNEL_GRAY) ? 1 : 3;
  if (precision != 8 && precision != 12 && precision != 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (num_comp == 3 && channels < 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (fmt->channel_model != GIMG_CHANNEL_GRAY &&
      fmt->channel_model != GIMG_CHANNEL_RGB &&
      fmt->channel_model != GIMG_CHANNEL_RGBA) {
    return GIMG_ERR_UNSUPPORTED; // CMYK lossless is not handled here
  }

  size_t n_samples = 0;
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, &n_samples)) {
    return GIMG_ERR_LIMIT;
  }
  size_t n_diffs = 0;
  if (!gcu_safe_mul_size(n_samples, (size_t)num_comp, &n_diffs) ||
      n_diffs > SIZE_MAX / sizeof(int32_t)) {
    return GIMG_ERR_LIMIT;
  }
  int32_t * diffs = (int32_t *)gimg_malloc(alloc, n_diffs * sizeof(int32_t));
  if (!diffs) {
    return GIMG_ERR_OOM;
  }

  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  int wide = (precision > 8);
  const int32_t initial_pred = (int32_t)1 << (precision - 1);
  uint32_t freq[JPEG_LL_SYMBOLS + 1];
  memset(freq, 0, sizeof(freq));

  // Sample at (x, y) of component c, straight from the raster: a lossless frame
  // stores colour as RGB, because YCbCr is not reversible and "lossless" would
  // then be a lie.
  #define LL_SAMPLE(cc, xx, yy)                                                \
    (wide ? (int32_t)((const uint16_t *)(pixels +                              \
                (size_t)(yy) * stride))[(size_t)(xx) * (size_t)channels + (cc)] \
          : (int32_t)(pixels + (size_t)(yy) * stride)[(size_t)(xx) *           \
                (size_t)channels + (cc)])

  // The same per-line predictor state the decoder keeps.  One flag per
  // component is enough here, where the decoder needs one per line, because
  // this walk is in raster order: sampling is 1x1, so an MCU is one sample and
  // x = 0 of a line is always reached before the rest of it.
  int row_1d[GIMG_JPEG_MAX_COMPONENTS];
  for (unsigned i = 0; i < GIMG_JPEG_MAX_COMPONENTS; i++) {
    row_1d[i] = 1;
  }
  int restart_now = 0;
  // Sampling is 1x1 for every component, so an MCU is one sample of each
  // (T.81 H.1.1) and the MCU index is the raster index.
  size_t di = 0;
  for (uint32_t y = 0; y < height; y++) {
    for (uint32_t x = 0; x < width; x++) {
      size_t mcu_index = (size_t)y * width + x;
      restart_now = (restart_interval > 0 && mcu_index > 0 &&
          mcu_index % (size_t)restart_interval == 0);
      for (int c = 0; c < num_comp; c++) {
        int32_t pred;
        if (x == 0) {
          if (y == 0 || restart_now) {
            pred = initial_pred;
            row_1d[c] = 1;
          }
          else {
            pred = LL_SAMPLE(c, 0, y - 1);
            row_1d[c] = 0;
          }
        }
        else if (row_1d[c]) {
          pred = LL_SAMPLE(c, x - 1, y);
        }
        else {
          pred = jpeg_lossless_predict(psv, LL_SAMPLE(c, x - 1, y),
              LL_SAMPLE(c, x, y - 1), LL_SAMPLE(c, x - 1, y - 1));
        }
        // H.1.2.1: the difference is taken modulo 2^16, so that it always fits
        // the categories of H.1.2.2 whatever the prediction was.
        int32_t d = (int32_t)(((uint32_t)LL_SAMPLE(c, x, y) - (uint32_t)pred) &
            0xFFFFu);
        if (d > 32768) {
          d -= 65536;
        }
        diffs[di++] = d;
        freq[jpeg_lossless_category(d)]++;
      }
    }
  }
  #undef LL_SAMPLE

  // T.81 SOF11: the same differences, coded with Annex D instead of Annex F.
  // Both passes read one buffer, so a disagreement between them is a defect in
  // one of the two rather than something a decoder has to be built to notice.
  if (arithmetic) {
    jpeg_ll_writer aw;
    memset(&aw, 0, sizeof(aw));
    aw.alloc = alloc;
    jpeg_arith_sink_t sink = {&aw, alloc, 0};
    jpeg_arith_encoder_t e;
    jpeg_arith_lossless_stats_t astats;
    jpeg_arith_cond_t cond;
    jpeg_arith_cond_defaults(&cond);
    jpeg_arith_encoder_init(&e, jpeg_arith_lossless_sink_emit, &sink);
    jpeg_arith_lossless_stats_reset(&astats);

    // H.1.2.3.1: one row of difference categories per component, plus the
    // category to the left, both cleared where the spec says to clear them.
    uint8_t * db = (uint8_t *)gimg_malloc(
        alloc, (size_t)width * (size_t)num_comp);
    if (!db) {
      gimg_free(alloc, diffs);
      return GIMG_ERR_OOM;
    }
    memset(db, 0, (size_t)width * (size_t)num_comp);
    int da[GIMG_JPEG_MAX_COMPONENTS];
    memset(da, 0, sizeof(da));

    GIMG_Result ar = GIMG_OK;
    for (size_t i = 0; i < n_diffs; i++) {
      size_t mcu_index = i / (size_t)num_comp;
      int c = (int)(i % (size_t)num_comp);
      uint32_t x = (uint32_t)(mcu_index % width);
      if (restart_interval > 0 && c == 0 && mcu_index > 0 &&
          mcu_index % (size_t)restart_interval == 0) {
        // D.1.8 then B.2.1: flush the coder, align, write the marker, and
        // start again with every bin at its initial state (H.1.2.3.4).
        jpeg_arith_encoder_flush(&e);
        if (!jpeg_ll_ensure(&aw, 2)) {
          ar = GIMG_ERR_OOM;
          break;
        }
        aw.buf[aw.len++] = 0xFF;
        aw.buf[aw.len++] = (unsigned char)(0xD0 +
            ((mcu_index / (size_t)restart_interval - 1u) & 7u));
        jpeg_arith_encoder_init(&e, jpeg_arith_lossless_sink_emit, &sink);
        jpeg_arith_lossless_stats_reset(&astats);
        memset(db, 0, (size_t)width * (size_t)num_comp);
        memset(da, 0, sizeof(da));
      }
      if (x == 0) {
        da[c] = 0; // H.1.2.3.1: Da is zero at the start of every line.
      }
      int cat = 0;
      jpeg_arith_lossless_encode_diff(&e, &astats, &cond, 0, da[c],
          (int)db[(size_t)x * (size_t)num_comp + (size_t)c], diffs[i], &cat);
      da[c] = cat;
      db[(size_t)x * (size_t)num_comp + (size_t)c] = (uint8_t)cat;
      if (sink.oom) {
        ar = GIMG_ERR_OOM;
        break;
      }
    }
    gimg_free(alloc, db);
    gimg_free(alloc, diffs);
    if (ar == GIMG_OK) {
      jpeg_arith_encoder_flush(&e);
      if (sink.oom || aw.oom) {
        ar = GIMG_ERR_OOM;
      }
    }
    if (ar != GIMG_OK) {
      gimg_free(alloc, aw.buf);
      return ar;
    }
    *out_scan_data = aw.buf;
    *out_scan_size = aw.len;
    *out_dht = NULL; // an arithmetic frame carries no Huffman tables
    *out_dht_len = 0;
    if (out_width) {
      *out_width = width;
    }
    if (out_height) {
      *out_height = height;
    }
    if (out_num_components) {
      *out_num_components = num_comp;
    }
    if (out_precision) {
      *out_precision = precision;
    }
    return GIMG_OK;
  }

  unsigned char bits[17];
  unsigned char vals[JPEG_LL_SYMBOLS];
  int nvals = 0;
  jpeg_gen_huff_table(freq, JPEG_LL_SYMBOLS, bits, vals, &nvals);

  // Build the encoding table: canonical codes in code-length order (C.2).
  unsigned int code_of[JPEG_LL_SYMBOLS];
  int len_of[JPEG_LL_SYMBOLS];
  memset(code_of, 0, sizeof(code_of));
  memset(len_of, 0, sizeof(len_of));
  {
    unsigned int code = 0;
    int k = 0;
    for (int L = 1; L <= 16; L++) {
      for (int i = 0; i < (int)bits[L]; i++, k++) {
        code_of[vals[k]] = code++;
        len_of[vals[k]] = L;
      }
      code <<= 1;
    }
  }

  jpeg_ll_writer w;
  memset(&w, 0, sizeof(w));
  w.alloc = alloc;
  for (size_t i = 0; i < n_diffs; i++) {
    if (restart_interval > 0) {
      size_t mcu_index = i / (size_t)num_comp;
      if (i % (size_t)num_comp == 0 && mcu_index > 0 &&
          mcu_index % (size_t)restart_interval == 0) {
        jpeg_ll_flush(&w);
        if (!jpeg_ll_ensure(&w, 2)) {
          gimg_free(alloc, diffs);
          gimg_free(alloc, w.buf);
          return GIMG_ERR_OOM;
        }
        w.buf[w.len++] = 0xFF;
        w.buf[w.len++] = (unsigned char)(0xD0 +
            ((mcu_index / (size_t)restart_interval - 1u) & 7u));
      }
    }
    int32_t d = diffs[i];
    int s = jpeg_lossless_category(d);
    if (len_of[s] == 0) {
      gimg_free(alloc, diffs);
      gimg_free(alloc, w.buf);
      return GIMG_ERR_INTERNAL; // the table was built from these very symbols
    }
    jpeg_ll_put_bits(&w, code_of[s], len_of[s]);
    if (s > 0 && s < 16) {
      // F.1.2.1: a negative value is sent as its one's complement.
      int32_t v = d < 0 ? d - 1 : d;
      jpeg_ll_put_bits(&w, (uint32_t)v & ((1u << s) - 1u), s);
    }
    if (w.oom) {
      gimg_free(alloc, diffs);
      gimg_free(alloc, w.buf);
      return GIMG_ERR_OOM;
    }
  }
  jpeg_ll_flush(&w);
  gimg_free(alloc, diffs);
  if (w.oom) {
    gimg_free(alloc, w.buf);
    return GIMG_ERR_OOM;
  }

  // DHT payload: Tc|Th, sixteen counts, then the symbols (B.2.4.2).
  size_t dht_len = 1u + 16u + (size_t)nvals;
  unsigned char * dht = (unsigned char *)gimg_malloc(alloc, dht_len);
  if (!dht) {
    gimg_free(alloc, w.buf);
    return GIMG_ERR_OOM;
  }
  dht[0] = 0x00; // class 0 (DC/lossless), destination 0
  for (int L = 1; L <= 16; L++) {
    dht[L] = bits[L];
  }
  memcpy(dht + 17, vals, (size_t)nvals);

  *out_scan_data = w.buf;
  *out_scan_size = w.len;
  *out_dht = dht;
  *out_dht_len = dht_len;
  if (out_width) *out_width = width;
  if (out_height) *out_height = height;
  if (out_num_components) *out_num_components = num_comp;
  if (out_precision) *out_precision = precision;
  return GIMG_OK;
}
