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
 * Hierarchical mode of operation (ITU-T T.81 / ISO 10918-1 Annex J).
 *
 * A hierarchical JPEG is not one image but a sequence of them.  A DHP segment
 * (B.3.2) declares the size the sequence adds up to; the first frame carries a
 * small version of the picture, and each frame after it either repeats the
 * picture at a new resolution or - the usual case - codes the two's complement
 * difference between the picture and what has been reconstructed so far.  An
 * EXP segment (B.3.3) before a frame doubles the reconstruction first, so a
 * pyramid can climb.
 *
 * Only the coding model changes for a differential frame (J.1.3, J.2.3): the
 * IDCT is taken without the level shift, the DC coefficient is coded directly
 * rather than as a difference from the previous block, and a lossless frame
 * codes its difference with no predictor at all.  Everything else - the
 * bitstream, the blocks, the restart intervals - is the machinery of Annexes F,
 * G and H, which is why this file calls into the same block decoders the
 * single-frame paths use rather than repeating them.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <stdint.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "jpeg_internal.h"

/**
 * One component's samples, at that component's own resolution.
 *
 * Held as int32 rather than at the frame's precision because a differential
 * frame produces two's complement differences, which are signed and which
 * J.2.1 adds to the reference modulo 2^16.  Narrowing happens once, when the
 * finished components become a raster.
 */
typedef struct {
  int32_t * s;
  uint32_t w, h;
} hier_plane_t;

static void hier_plane_free(const GIMG_Allocator * alloc, hier_plane_t * p) {
  if (p->s) {
    gimg_free(alloc, p->s);
    p->s = NULL;
  }
  p->w = 0;
  p->h = 0;
}

static GIMG_Result hier_plane_alloc(
    const GIMG_Allocator * alloc, hier_plane_t * p, uint32_t w, uint32_t h) {
  size_t n = 0;
  p->s = NULL;
  p->w = 0;
  p->h = 0;
  if (w == 0 || h == 0) {
    return GIMG_ERR_FORMAT;
  }
  if (!gcu_safe_mul_size((size_t)w, (size_t)h, &n) ||
      !gcu_safe_mul_size(n, sizeof(int32_t), &n)) {
    return GIMG_ERR_LIMIT;
  }
  p->s = (int32_t *)gimg_malloc(alloc, n);
  if (!p->s) {
    return GIMG_ERR_OOM;
  }
  memset(p->s, 0, n);
  p->w = w;
  p->h = h;
  return GIMG_OK;
}

/**
 * Expand a reference component by two (T.81 J.1.1.2).
 *
 * "Px = (Ra + Rb) / 2 ... The division indicates truncation, not rounding.
 * The left-most column of the upsampled image matches the left-most column of
 * the lower resolution image.  The top line of the upsampled image matches the
 * top line of the lower resolution image.  The right column and the bottom line
 * of the lower resolution image are replicated to provide the values required
 * for the right column edge and bottom line interpolations."  And: "If both
 * horizontal and vertical expansions are signaled, they are done in sequence -
 * first the horizontal expansion and then the vertical."
 *
 * So output column 2i is input column i, and output column 2i+1 is the mean of
 * input columns i and i+1 (i+1 clamped to the last column).  Truncation of a
 * sum of two non-negative samples is a plain shift; a reference component is
 * always a reconstructed sample and so never negative.
 */
static GIMG_Result hier_expand(
    const GIMG_Allocator * alloc, hier_plane_t * p, int do_h, int do_v) {
  if (!p->s) {
    return GIMG_ERR_FORMAT;
  }
  if (do_h) {
    hier_plane_t out;
    uint32_t nw = 0;
    if (p->w > UINT32_MAX / 2u) {
      return GIMG_ERR_LIMIT;
    }
    nw = p->w * 2u;
    GIMG_Result r = hier_plane_alloc(alloc, &out, nw, p->h);
    if (r != GIMG_OK) {
      return r;
    }
    for (uint32_t y = 0; y < p->h; y++) {
      const int32_t * src = p->s + (size_t)y * p->w;
      int32_t * dst = out.s + (size_t)y * nw;
      for (uint32_t x = 0; x < p->w; x++) {
        int32_t a = src[x];
        int32_t b = src[(x + 1u < p->w) ? (x + 1u) : x];
        dst[x * 2u] = a;
        dst[x * 2u + 1u] = (a + b) / 2;
      }
    }
    hier_plane_free(alloc, p);
    *p = out;
  }
  if (do_v) {
    hier_plane_t out;
    uint32_t nh = 0;
    if (p->h > UINT32_MAX / 2u) {
      return GIMG_ERR_LIMIT;
    }
    nh = p->h * 2u;
    GIMG_Result r = hier_plane_alloc(alloc, &out, p->w, nh);
    if (r != GIMG_OK) {
      return r;
    }
    for (uint32_t y = 0; y < p->h; y++) {
      const int32_t * a = p->s + (size_t)y * p->w;
      const int32_t * b =
          p->s + (size_t)((y + 1u < p->h) ? (y + 1u) : y) * p->w;
      int32_t * d0 = out.s + (size_t)(y * 2u) * p->w;
      int32_t * d1 = out.s + (size_t)(y * 2u + 1u) * p->w;
      for (uint32_t x = 0; x < p->w; x++) {
        d0[x] = a[x];
        d1[x] = (a[x] + b[x]) / 2;
      }
    }
    hier_plane_free(alloc, p);
    *p = out;
  }
  return GIMG_OK;
}

/**
 * Narrow a reference component to a smaller frame's size, in place.
 *
 * T.81 J.1.1.2: "The upsampling process always doubles the line length or the
 * number of lines."  Doubling an odd dimension overshoots by one, so a
 * reference that has just been expanded can be a row or a column larger than
 * the frame it is about to be differenced against.  The surplus lies outside
 * the picture and is dropped; rows are moved up in place, which is safe
 * because the destination row always starts at or before the source row.
 */
static void hier_crop(hier_plane_t * p, uint32_t w, uint32_t h) {
  if (w > p->w || h > p->h) {
    return;
  }
  for (uint32_t y = 0; y < h; y++) {
    memmove(p->s + (size_t)y * w, p->s + (size_t)y * p->w,
        (size_t)w * sizeof(int32_t));
  }
  p->w = w;
  p->h = h;
}

/** Per-component sample dimensions of a frame (T.81 A.1.1). */
static void hier_component_dims(const gimg_jpeg_sof_t * sof, uint8_t h_max,
    uint8_t v_max, uint32_t * cw, uint32_t * ch) {
  for (uint8_t i = 0; i < sof->num_components; i++) {
    cw[i] = ((uint32_t)sof->width * sof->h_samp[i] + h_max - 1u) / h_max;
    ch[i] = ((uint32_t)sof->height * sof->v_samp[i] + v_max - 1u) / v_max;
    if (cw[i] == 0u) {
      cw[i] = 1u;
    }
    if (ch[i] == 0u) {
      ch[i] = 1u;
    }
  }
}

static void hier_sampling_max(
    const gimg_jpeg_sof_t * sof, uint8_t * h_max, uint8_t * v_max) {
  *h_max = 0;
  *v_max = 0;
  for (uint8_t i = 0; i < sof->num_components; i++) {
    if (sof->h_samp[i] > *h_max) {
      *h_max = sof->h_samp[i];
    }
    if (sof->v_samp[i] > *v_max) {
      *v_max = sof->v_samp[i];
    }
  }
}

/** The Huffman table a scan selected, preferring the snapshot the loader took
 * at that scan's SOS over whatever the document last saw (T.81 B.2.4). */
static const unsigned char * hier_scan_huff(const gimg_jpeg_doc_state_t * state,
    const gimg_jpeg_scan_t * scan, int is_ac, uint8_t id, size_t * out_len) {
  if (id >= 4u) {
    *out_len = 0;
    return NULL;
  }
  const unsigned char * p = is_ac ? scan->huff_ac[id] : scan->huff_dc[id];
  size_t len = is_ac ? scan->huff_ac_len[id] : scan->huff_dc_len[id];
  if (!p || len == 0) {
    p = is_ac ? state->huff_ac[id] : state->huff_dc[id];
    len = is_ac ? state->huff_ac_len[id] : state->huff_dc_len[id];
  }
  *out_len = len;
  return p;
}

/**
 * Decode one DCT-based frame of the sequence into signed component planes
 * (T.81 Annex F for the non-differential case, J.1.3.1/J.2.3.1 for the
 * differential one).
 *
 * Two lines differ between the two cases, and both come straight from J.2.3.1:
 * "First, the IDCT of the differential output is calculated without the level
 * shift.  Second, the DC coefficient of the DCT is decoded directly - without
 * prediction."  Decoding the DC directly is spelled here as clearing the
 * predictor before each block, which is the same thing for both entropy coders:
 * the Huffman path adds the predictor to the decoded difference, and the
 * arithmetic path of F.1.4.4.1 adds its own carried DC value, so a zero
 * predictor leaves the decoded value standing as the coefficient.
 */
// Defined below; the sequential frame decoder hands A.2.3 frames to it.
static GIMG_Result hier_decode_progressive_frame(
    const gimg_jpeg_doc_state_t * state, const gimg_jpeg_frame_t * f,
    hier_plane_t * out, int sequential);

static GIMG_Result hier_decode_dct_frame(const gimg_jpeg_doc_state_t * state,
    const gimg_jpeg_frame_t * f, hier_plane_t * out) {
  const gimg_jpeg_sof_t * sof = &f->sof;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);
  uint8_t num_comp = sof->num_components;
  int precision = (int)sof->precision;
  if (precision != 8 && precision != 12) {
    // T.81 Table B.2: a DCT-based frame is 8- or 12-bit.
    return GIMG_ERR_UNSUPPORTED;
  }
  if (f->num_scans == 0u) {
    return GIMG_ERR_CORRUPT;
  }
  // T.81 A.2.3: a frame of a hierarchical sequence may be coded as several
  // non-interleaved scans, one per component, just as a single-frame image
  // may.  Annex J says nothing to forbid it - J.1 changes the coding model,
  // not the scan arrangement - and the ISO reference codec reads such a file.
  //
  // That order is a different walk through the same blocks, and the walk that
  // knows how to read it is the one Annex G already needed, so such a frame
  // takes the coefficient-buffer path with `sequential` set.  This is the same
  // delegation the single-frame sequential decoder makes, for the same reason;
  // it used to be refused here only because nothing had needed it yet.
  if (f->num_scans > 1u || f->scans[0].comp_count != num_comp) {
    return hier_decode_progressive_frame(state, f, out, 1);
  }
  const gimg_jpeg_scan_t * scan = &f->scans[0];
  if (!f->is_arithmetic && (!scan->data || scan->data_size == 0)) {
    return GIMG_ERR_CORRUPT;
  }

  uint8_t h_max = 0, v_max = 0;
  hier_sampling_max(sof, &h_max, &v_max);
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t cw[GIMG_JPEG_MAX_COMPONENTS], ch[GIMG_JPEG_MAX_COMPONENTS];
  hier_component_dims(sof, h_max, v_max, cw, ch);

  gimg_jpeg_huff_table_t dc_tables[4], ac_tables[4];
  memset(dc_tables, 0, sizeof(dc_tables));
  memset(ac_tables, 0, sizeof(ac_tables));
  if (!f->is_arithmetic) {
    for (uint8_t c = 0; c < scan->comp_count; c++) {
      size_t len = 0;
      const unsigned char * p =
          hier_scan_huff(state, scan, 0, scan->dc_tbl[c], &len);
      if (!p || jpeg_build_huff_table(p, len, &dc_tables[scan->dc_tbl[c]]) != 0) {
        return GIMG_ERR_CORRUPT;
      }
      p = hier_scan_huff(state, scan, 1, scan->ac_tbl[c], &len);
      if (!p || jpeg_build_huff_table(p, len, &ac_tables[scan->ac_tbl[c]]) != 0) {
        return GIMG_ERR_CORRUPT;
      }
    }
  }

  GIMG_Result r = GIMG_OK;
  for (uint8_t i = 0; i < num_comp; i++) {
    r = hier_plane_alloc(alloc, &out[i], cw[i], ch[i]);
    if (r != GIMG_OK) {
      for (uint8_t j = 0; j < i; j++) {
        hier_plane_free(alloc, &out[j]);
      }
      return r;
    }
  }

  uint32_t mcu_per_row = ((uint32_t)sof->width + 8u * h_max - 1u) / (8u * h_max);
  uint32_t mcu_per_col =
      ((uint32_t)sof->height + 8u * v_max - 1u) / (8u * v_max);

  gimg_jpeg_bitstream_t bs;
  jpeg_bitstream_init(&bs, scan->data, scan->data_size);
  jpeg_arith_decoder_t ad;
  jpeg_arith_stats_t astats;
  if (f->is_arithmetic) {
    jpeg_arith_decoder_init(&ad, scan->data, scan->data_size);
    jpeg_arith_stats_reset(&astats);
  }
  int16_t dc_pred[GIMG_JPEG_MAX_COMPONENTS];
  memset(dc_pred, 0, sizeof(dc_pred));

  // A.3.1's level shift, which a differential frame does not apply: J.2.3.1 -
  // "the IDCT of the differential output is calculated without the level
  // shift".
  //
  // Neither case clamps to 0..2^P-1 here.  A frame's output is about to become
  // a reference component that a later differential frame is added to, and
  // J.2.1 makes that addition modulo 2^16 - a rule that would be pointless if
  // the intermediate were clipped to the sample range first.  It is the
  // clipping that is wrong: a sample that the DCT undershoots to -9 at black
  // and a differential frame then corrects by +13 must end at 4, which it
  // cannot do if the -9 was flattened to 0 on the way.  The range of A.3.1 is
  // imposed once, on the finished components, in hier_emit_raster.
  const int32_t level_shift =
      f->is_differential ? 0 : ((int32_t)1 << (precision - 1));
  const int pass1_bits = (precision == 12) ? 1 : 2;
  const uint16_t restart_interval = scan->restart_interval;

  int16_t block_zig[64], block_rz[64];
  int32_t block_q[64], block_idct[64];

  for (uint32_t mcu_y = 0; mcu_y < mcu_per_col; mcu_y++) {
    for (uint32_t mcu_x = 0; mcu_x < mcu_per_row; mcu_x++) {
      uint32_t mcu_index = mcu_y * mcu_per_row + mcu_x;
      if (restart_interval > 0 && mcu_index > 0 &&
          mcu_index % (uint32_t)restart_interval == 0) {
        if (f->is_arithmetic) {
          r = jpeg_arith_restart(&ad, &astats);
          if (r != GIMG_OK) {
            goto fail;
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
      for (uint8_t s = 0; s < scan->comp_count; s++) {
        uint8_t ci = 0;
        for (; ci < num_comp; ci++) {
          if (sof->comp_id[ci] == scan->comp_id[s]) {
            break;
          }
        }
        if (ci >= num_comp) {
          r = GIMG_ERR_FORMAT;
          goto fail;
        }
        uint8_t qid = sof->quant_tbl_id[ci];
        if (qid >= GIMG_JPEG_MAX_QUANT_TABLES || !f->quant_tbl_present[qid]) {
          r = GIMG_ERR_CORRUPT;
          goto fail;
        }
        const uint16_t * quant = f->quant_tbl[qid];
        for (uint8_t by = 0; by < sof->v_samp[ci]; by++) {
          for (uint8_t bx = 0; bx < sof->h_samp[ci]; bx++) {
            if (f->is_differential) {
              // J.2.3.1: the DC coefficient is decoded directly.
              dc_pred[ci] = 0;
              if (f->is_arithmetic) {
                astats.dc_pred[ci] = 0;
              }
            }
            if (f->is_arithmetic) {
              r = jpeg_arith_decode_block_sequential(&ad, &astats,
                  &f->arith_cond, ci, scan->dc_tbl[s], scan->ac_tbl[s], 63,
                  block_zig);
            }
            else {
              r = jpeg_decode_block(&bs, &dc_tables[scan->dc_tbl[s]],
                  &ac_tables[scan->ac_tbl[s]], block_zig, &dc_pred[ci], 0);
            }
            if (r != GIMG_OK) {
              goto fail;
            }
            jpeg_dezigzag(block_zig, block_rz);
            jpeg_dequantize_32(block_rz, quant, block_q);
            jpeg_idct_8x8_islow(block_q, block_idct, pass1_bits);

            uint32_t dst_x =
                mcu_x * (uint32_t)(8 * sof->h_samp[ci]) + (uint32_t)bx * 8u;
            uint32_t dst_y =
                mcu_y * (uint32_t)(8 * sof->v_samp[ci]) + (uint32_t)by * 8u;
            for (int dy = 0; dy < 8; dy++) {
              uint32_t y = dst_y + (uint32_t)dy;
              if (y >= ch[ci]) {
                break;
              }
              for (int dx = 0; dx < 8; dx++) {
                uint32_t x = dst_x + (uint32_t)dx;
                if (x >= cw[ci]) {
                  break;
                }
                out[ci].s[(size_t)y * cw[ci] + x] =
                    block_idct[dy * 8 + dx] + level_shift;
              }
            }
          }
        }
      }
    }
  }
  return GIMG_OK;

fail:
  for (uint8_t i = 0; i < num_comp; i++) {
    hier_plane_free(alloc, &out[i]);
  }
  return r;
}

/**
 * Decode one lossless frame of the sequence into signed component planes
 * (T.81 Annex H, with the modification of J.2.3.2 for a differential frame).
 *
 * J.1.3.2: "One modification is made to the lossless coding models.  The
 * difference is coded directly - without prediction.  The prediction selection
 * parameter in the scan header shall be set to zero."  So a differential frame
 * is the same entropy decode with the predictor replaced by nothing, which is
 * why the two share this walk rather than being written twice.
 *
 * The point transform is undone here rather than at the end, because a
 * reference component in a hierarchical sequence is a sample, not a
 * point-transformed one: A.4 divides the input by 2^Pt, so the decoder
 * multiplies back before the frame joins the sequence.
 */
static GIMG_Result hier_decode_lossless_frame(
    const gimg_jpeg_doc_state_t * state, const gimg_jpeg_frame_t * f,
    hier_plane_t * out) {
  const gimg_jpeg_sof_t * sof = &f->sof;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);
  uint8_t num_comp = sof->num_components;
  int precision = (int)sof->precision;
  if (precision < 2 || precision > 16) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (f->num_scans == 0u) {
    return GIMG_ERR_CORRUPT;
  }

  uint8_t h_max = 0, v_max = 0;
  hier_sampling_max(sof, &h_max, &v_max);
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  // H.1.1: in a lossless frame an MCU is made of samples, not 8x8 blocks.
  uint32_t mcu_per_row = ((uint32_t)sof->width + h_max - 1u) / h_max;
  uint32_t mcu_per_col = ((uint32_t)sof->height + v_max - 1u) / v_max;

  // Two geometries per component, and the difference is the whole of A.2.3.
  // cw/chh is the MCU-padded grid an interleaved scan walks and the plane is
  // allocated at; ow/oh is the component's own size, ceil(X x H_i / H_max) by
  // ceil(Y x V_i / V_max), which is what a non-interleaved scan covers - A.2.2
  // puts its data units "left-to-right, top-to-bottom" over that and codes no
  // padding at all.
  uint32_t cw[GIMG_JPEG_MAX_COMPONENTS], chh[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t ow[GIMG_JPEG_MAX_COMPONENTS], oh[GIMG_JPEG_MAX_COMPONENTS];
  GIMG_Result r = GIMG_OK;
  uint8_t * db_cat[GIMG_JPEG_MAX_COMPONENTS];
  int da_cat[GIMG_JPEG_MAX_COMPONENTS];
  // One byte per line of each component, not one flag per component: see the
  // note in jpeg_lossless.c.  An interleaved scan walks MCUs, so with Hi above
  // one a single flag is read on a line other than the one it was set for, and
  // on a first line that asks for the sample above the image.
  unsigned char * row_1d[GIMG_JPEG_MAX_COMPONENTS];
  // A.4 gives the point transform per scan, so a frame written as several
  // scans may use a different Pt in each.  It is undone once, at the end, and
  // so has to be remembered per component rather than held in one variable.
  int pt_of[GIMG_JPEG_MAX_COMPONENTS];
  memset(db_cat, 0, sizeof(db_cat));
  memset(da_cat, 0, sizeof(da_cat));
  memset(row_1d, 0, sizeof(row_1d));
  memset(pt_of, 0, sizeof(pt_of));
  for (uint8_t i = 0; i < num_comp; i++) {
    cw[i] = mcu_per_row * sof->h_samp[i];
    chh[i] = mcu_per_col * sof->v_samp[i];
    ow[i] = ((uint32_t)sof->width * sof->h_samp[i] + h_max - 1u) / h_max;
    oh[i] = ((uint32_t)sof->height * sof->v_samp[i] + v_max - 1u) / v_max;
    if (ow[i] > cw[i]) {
      ow[i] = cw[i];
    }
    if (oh[i] > chh[i]) {
      oh[i] = chh[i];
    }
    r = hier_plane_alloc(alloc, &out[i], cw[i], chh[i]);
    if (r != GIMG_OK) {
      goto fail;
    }
    db_cat[i] = (uint8_t *)gimg_malloc(alloc, cw[i]);
    row_1d[i] = (unsigned char *)gimg_malloc(alloc, chh[i]);
    if (!db_cat[i] || !row_1d[i]) {
      r = GIMG_ERR_OOM;
      goto fail;
    }
  }

  // T.81 A.2.3: a lossless frame may be written as one scan per component
  // rather than as a single interleaved scan, in a hierarchical sequence just
  // as outside one.  Each scan carries its own predictor, point transform,
  // tables and restart interval (B.2.3), so all of that is per scan here and
  // not per frame; H.1.2.1's prediction starts again in each of them, which
  // falls out of resetting the line state below.
  for (unsigned si = 0; si < f->num_scans; si++) {
    const gimg_jpeg_scan_t * scan = &f->scans[si];
    if (scan->comp_count == 0 || scan->comp_count > num_comp) {
      r = GIMG_ERR_FORMAT;
      goto fail;
    }
    if (!f->is_arithmetic && (!scan->data || scan->data_size == 0)) {
      r = GIMG_ERR_CORRUPT;
      goto fail;
    }
    int psv = (int)scan->ss;
    int pt = (int)scan->al;
    if (pt < 0 || pt >= precision) {
      r = GIMG_ERR_UNSUPPORTED;
      goto fail;
    }
    // T.81 H.1.2 Table H.1 and J.1.3.2: selection value 0 means "no
    // prediction" and "shall only be used for differential coding in the
    // hierarchical mode"; 1 to 7 are the predictors, and a differential frame
    // may not use them.
    if (f->is_differential) {
      if (psv != 0) {
        r = GIMG_ERR_FORMAT;
        goto fail;
      }
    }
    else if (psv < 1 || psv > 7) {
      r = GIMG_ERR_FORMAT;
      goto fail;
    }

    gimg_jpeg_huff_table_t dc_tables[4];
    memset(dc_tables, 0, sizeof(dc_tables));
    if (!f->is_arithmetic) {
      for (uint8_t c = 0; c < scan->comp_count; c++) {
        size_t len = 0;
        const unsigned char * p =
            hier_scan_huff(state, scan, 0, scan->dc_tbl[c], &len);
        if (!p ||
            jpeg_build_huff_table(p, len, &dc_tables[scan->dc_tbl[c]]) != 0) {
          r = GIMG_ERR_CORRUPT;
          goto fail;
        }
      }
    }

    gimg_jpeg_bitstream_t bs;
    jpeg_bitstream_init(&bs, scan->data, scan->data_size);
    jpeg_arith_decoder_t ad;
    jpeg_arith_lossless_stats_t astats;
    if (f->is_arithmetic) {
      jpeg_arith_decoder_init(&ad, scan->data, scan->data_size);
      jpeg_arith_lossless_stats_reset(&astats);
    }
    for (uint8_t i = 0; i < num_comp; i++) {
      memset(db_cat[i], 0, cw[i]);
      da_cat[i] = 0;
      memset(row_1d[i], 1, chh[i]);
    }

    const uint16_t restart_interval = scan->restart_interval;
    const int32_t initial_pred = (int32_t)1 << (precision - pt - 1);

    // A.2.2: with one component the MCU is a single sample and the scan covers
    // that component's own grid.  A.2.3: with more than one it is H_i x V_i
    // samples of each, over the frame's MCU grid.
    int interleaved = (scan->comp_count > 1);
    uint8_t solo = 0;
    if (!interleaved) {
      for (; solo < num_comp; solo++) {
        if (sof->comp_id[solo] == scan->comp_id[0]) {
          break;
        }
      }
      if (solo >= num_comp) {
        r = GIMG_ERR_FORMAT;
        goto fail;
      }
      pt_of[solo] = pt;
    }
    else {
      for (uint8_t c = 0; c < scan->comp_count; c++) {
        for (uint8_t ci = 0; ci < num_comp; ci++) {
          if (sof->comp_id[ci] == scan->comp_id[c]) {
            pt_of[ci] = pt;
            break;
          }
        }
      }
    }
    uint32_t nx = interleaved ? mcu_per_row : ow[solo];
    uint32_t ny = interleaved ? mcu_per_col : oh[solo];
    int restart_now = 0;

    for (uint32_t mcu_y = 0; mcu_y < ny; mcu_y++) {
      for (uint32_t mcu_x = 0; mcu_x < nx; mcu_x++) {
        uint32_t mcu_index = mcu_y * nx + mcu_x;
        if (restart_interval > 0 && mcu_index > 0 &&
            mcu_index % (uint32_t)restart_interval == 0) {
          if (f->is_arithmetic) {
            r = jpeg_arith_lossless_restart(&ad, &astats);
            if (r != GIMG_OK) {
              goto fail;
            }
            for (uint8_t i = 0; i < num_comp; i++) {
              memset(db_cat[i], 0, cw[i]);
              da_cat[i] = 0;
            }
          }
          else {
            bs.expect_rst = 1;
            jpeg_bitstream_align_skip_rst(&bs);
            bs.rst_just_skipped = 0;
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
          uint8_t nsy = interleaved ? sof->v_samp[ci] : 1u;
          uint8_t nsx = interleaved ? sof->h_samp[ci] : 1u;
          for (uint8_t sy = 0; sy < nsy; sy++) {
            for (uint8_t sx = 0; sx < nsx; sx++) {
              uint32_t x = interleaved
                  ? mcu_x * sof->h_samp[ci] + sx
                  : mcu_x;
              uint32_t y = interleaved
                  ? mcu_y * sof->v_samp[ci] + sy
                  : mcu_y;
              int32_t diff = 0;
              if (f->is_arithmetic) {
                if (x == 0) {
                  da_cat[ci] = 0;
                }
                int cat = 0;
                r = jpeg_arith_lossless_decode_diff(&ad, &astats,
                    &f->arith_cond, scan->dc_tbl[s], da_cat[ci],
                    (int)db_cat[ci][x], &diff, &cat);
                if (r != GIMG_OK) {
                  goto fail;
                }
                da_cat[ci] = cat;
                db_cat[ci][x] = (uint8_t)cat;
              }
              else {
                r = jpeg_lossless_decode_diff(
                    &bs, &dc_tables[scan->dc_tbl[s]], &diff);
                if (r != GIMG_OK) {
                  goto fail;
                }
              }
              int32_t v;
              if (f->is_differential) {
                // J.2.3.2: the difference is the output; there is nothing to
                // predict from, since the reference it belongs to is added
                // later.
                v = diff;
              }
              else {
                int32_t pred;
                if (x == 0) {
                  if (y == 0 || restart_now) {
                    pred = initial_pred;
                    row_1d[ci][y] = 1;
                  }
                  else {
                    pred = out[ci].s[(size_t)(y - 1) * cw[ci]];
                    row_1d[ci][y] = 0;
                  }
                }
                else if (row_1d[ci][y]) {
                  pred = out[ci].s[(size_t)y * cw[ci] + (x - 1)];
                }
                else {
                  int32_t ra = out[ci].s[(size_t)y * cw[ci] + (x - 1)];
                  int32_t rb = out[ci].s[(size_t)(y - 1) * cw[ci] + x];
                  int32_t rc = out[ci].s[(size_t)(y - 1) * cw[ci] + (x - 1)];
                  pred = jpeg_lossless_predict(psv, ra, rb, rc);
                }
                // H.1.2.1: the reconstruction is modulo 2^16.
                v = (int32_t)((uint32_t)(pred + diff) & 0xFFFFu);
              }
              out[ci].s[(size_t)y * cw[ci] + x] = v;
            }
          }
        }
        restart_now = 0;
      }
    }
  }

  // A.4: the point transform divided the input by 2^Pt, so undo it once the
  // whole frame is decoded - the prediction above works in transformed space,
  // as H.1.2 intends, and only what leaves this function is a sample.
  for (uint8_t i = 0; i < num_comp; i++) {
    if (pt_of[i] > 0) {
      size_t n = (size_t)cw[i] * chh[i];
      for (size_t k = 0; k < n; k++) {
        out[i].s[k] = (int32_t)((uint32_t)out[i].s[k] << pt_of[i]);
      }
    }
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, db_cat[i]);
    gimg_free(alloc, row_1d[i]);
  }
  return GIMG_OK;

fail:
  for (uint8_t i = 0; i < num_comp; i++) {
    hier_plane_free(alloc, &out[i]);
    gimg_free(alloc, db_cat[i]);
    gimg_free(alloc, row_1d[i]);
  }
  return r;
}

/**
 * Decode one progressive frame of the sequence into signed component planes
 * (T.81 Annex G, with the modifications of J.2.3.1 when it is differential).
 *
 * The scans go through the same reader the single-frame progressive path uses;
 * what is left here is the geometry, and the pass that turns the finished
 * coefficients back into samples.  That pass is where the second half of
 * J.2.3.1 lands: "the IDCT of the differential output is calculated without the
 * level shift".
 */
static GIMG_Result hier_decode_progressive_frame(
    const gimg_jpeg_doc_state_t * state, const gimg_jpeg_frame_t * f,
    hier_plane_t * out, int sequential) {
  const gimg_jpeg_sof_t * sof = &f->sof;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);
  uint8_t num_comp = sof->num_components;
  int precision = (int)sof->precision;
  if (precision != 8 && precision != 12) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (f->num_scans == 0u) {
    return GIMG_ERR_CORRUPT;
  }

  uint8_t h_max = 0, v_max = 0;
  hier_sampling_max(sof, &h_max, &v_max);
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  uint32_t cw[GIMG_JPEG_MAX_COMPONENTS], chh[GIMG_JPEG_MAX_COMPONENTS];
  hier_component_dims(sof, h_max, v_max, cw, chh);
  uint32_t mcu_per_row = ((uint32_t)sof->width + 8u * h_max - 1u) / (8u * h_max);
  uint32_t mcu_per_col =
      ((uint32_t)sof->height + 8u * v_max - 1u) / (8u * v_max);

  // A.2: a component holds ceil(X_i/8) x ceil(Y_i/8) blocks of real samples,
  // but an interleaved scan walks whole MCUs and so addresses a grid padded out
  // at the right and bottom edges.  The buffer is the larger of the two, and
  // grid_w is the row stride throughout - the same split the single-frame
  // progressive path makes, and for the same reason.
  uint32_t blk_w[GIMG_JPEG_MAX_COMPONENTS], blk_h[GIMG_JPEG_MAX_COMPONENTS];
  uint32_t grid_w[GIMG_JPEG_MAX_COMPONENTS], grid_h[GIMG_JPEG_MAX_COMPONENTS];
  for (uint8_t i = 0; i < num_comp; i++) {
    blk_w[i] = (cw[i] + 7u) / 8u;
    blk_h[i] = (chh[i] + 7u) / 8u;
    grid_w[i] = mcu_per_row * (uint32_t)sof->h_samp[i];
    grid_h[i] = mcu_per_col * (uint32_t)sof->v_samp[i];
    if (blk_w[i] > grid_w[i]) {
      grid_w[i] = blk_w[i];
    }
    if (blk_h[i] > grid_h[i]) {
      grid_h[i] = blk_h[i];
    }
  }

  int16_t * coef[GIMG_JPEG_MAX_COMPONENTS];
  memset(coef, 0, sizeof(coef));
  GIMG_Result r = GIMG_OK;
  for (uint8_t i = 0; i < num_comp; i++) {
    size_t n = 0;
    if (!gcu_safe_mul_size((size_t)grid_w[i], (size_t)grid_h[i], &n) ||
        !gcu_safe_mul_size(n, 64u * sizeof(int16_t), &n)) {
      r = GIMG_ERR_LIMIT;
      goto fail;
    }
    coef[i] = (int16_t *)gimg_malloc(alloc, n);
    if (!coef[i]) {
      r = GIMG_ERR_OOM;
      goto fail;
    }
    memset(coef[i], 0, n);
    r = hier_plane_alloc(alloc, &out[i], cw[i], chh[i]);
    if (r != GIMG_OK) {
      goto fail;
    }
  }

  r = jpeg_decode_progressive_scans(state, sof, f->scans, f->num_scans,
      f->is_arithmetic, &f->arith_cond, f->is_differential, sequential,
      mcu_per_row, mcu_per_col, blk_w, blk_h, grid_w, coef);
  if (r != GIMG_OK) {
    goto fail;
  }

  {
    const int32_t level_shift =
        f->is_differential ? 0 : ((int32_t)1 << (precision - 1));
    const int pass1_bits = (precision == 12) ? 1 : 2;
    int16_t block_rz[64];
    int32_t block_q[64], block_idct[64];
    for (uint8_t ci = 0; ci < num_comp; ci++) {
      uint8_t qid = sof->quant_tbl_id[ci];
      if (qid >= GIMG_JPEG_MAX_QUANT_TABLES || !f->quant_tbl_present[qid]) {
        r = GIMG_ERR_CORRUPT;
        goto fail;
      }
      const uint16_t * quant = f->quant_tbl[qid];
      for (uint32_t by = 0; by < blk_h[ci]; by++) {
        for (uint32_t bx = 0; bx < blk_w[ci]; bx++) {
          const int16_t * block =
              coef[ci] + ((size_t)by * grid_w[ci] + bx) * 64u;
          jpeg_dezigzag(block, block_rz);
          jpeg_dequantize_32(block_rz, quant, block_q);
          jpeg_idct_8x8_islow(block_q, block_idct, pass1_bits);
          for (int dy = 0; dy < 8; dy++) {
            uint32_t y = by * 8u + (uint32_t)dy;
            if (y >= chh[ci]) {
              break;
            }
            for (int dx = 0; dx < 8; dx++) {
              uint32_t x = bx * 8u + (uint32_t)dx;
              if (x >= cw[ci]) {
                break;
              }
              // Unclamped, as in the sequential path: the bound of A.3.1 is
              // imposed once, on the finished components.
              out[ci].s[(size_t)y * cw[ci] + x] =
                  block_idct[dy * 8 + dx] + level_shift;
            }
          }
        }
      }
    }
  }
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef[i]);
  }
  return GIMG_OK;

fail:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, coef[i]);
    hier_plane_free(alloc, &out[i]);
  }
  return r;
}

/**
 * Turn the finished reference components into a raster.
 *
 * The components are narrowed to the frame precision first, so that the
 * upsampling and color-conversion helpers the single-frame decoders use can
 * be applied here unchanged: they read a plane of bytes or of 16-bit words,
 * which is what a reconstructed component is once it stops being a running sum.
 */
static GIMG_Result hier_emit_raster(const gimg_jpeg_doc_state_t * state,
    const hier_plane_t * ref, const GIMG_Decode_Options * options,
    int lossless_sequence, GIMG_Raster ** out_raster) {
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);
  const gimg_jpeg_sof_t * dhp = &state->dhp;
  uint32_t width = dhp->width;
  uint32_t height = dhp->height;
  uint8_t num_comp = dhp->num_components;
  int precision = (int)dhp->precision;
  const int wide = (precision > 8);
  const int out_bits = wide ? 16 : 8;
  const int32_t max_val = ((int32_t)1 << precision) - 1;

  // T.81 B.2.2 counts a hierarchical sequence's components exactly as it counts
  // any other frame's, and Annex H has no color concept at all, so neither a
  // wide sequence nor a lossless four-component one is anything but ordinary.
  // Both used to be refused here, the second of them because the single-frame
  // lossless path refused it too.
  // One, three and four are the counts a hierarchical sequence is written and
  // checked at here; the assembly below handles any of them and the raster can
  // hold more, but nothing available produces a wider sequence to test the
  // reading of, so a wider one is refused rather than guessed at.
  if (num_comp != 1u && num_comp != 3u && num_comp != 4u) {
    return GIMG_ERR_UNSUPPORTED;
  }
  (void)lossless_sequence;

  uint8_t h_max = 0, v_max = 0;
  hier_sampling_max(dhp, &h_max, &v_max);
  if (h_max == 0 || v_max == 0) {
    return GIMG_ERR_FORMAT;
  }
  // The size each component would have if the sequence reached the completed
  // image (A.1.1 applied to the DHP header).
  uint32_t want_w[GIMG_JPEG_MAX_COMPONENTS], want_h[GIMG_JPEG_MAX_COMPONENTS];
  hier_component_dims(dhp, h_max, v_max, want_w, want_h);

  void * cbuf[GIMG_JPEG_MAX_COMPONENTS];
  memset(cbuf, 0, sizeof(cbuf));
  GIMG_Result r = GIMG_OK;
  for (uint8_t i = 0; i < num_comp; i++) {
    if (!ref[i].s) {
      r = GIMG_ERR_CORRUPT; // A component no frame ever coded.
      goto done;
    }
    size_t n = (size_t)ref[i].w * ref[i].h;
    cbuf[i] = gimg_malloc(alloc, n * (size_t)(wide ? 2 : 1));
    if (!cbuf[i]) {
      r = GIMG_ERR_OOM;
      goto done;
    }
    // T.81 A.3.1: a reconstructed sample lies in 0..2^P-1.  The frames were
    // summed without that bound, because J.2.1's modulo-2^16 addition is what
    // holds between them; this is where the sum becomes a sample again.
    //
    // At 16-bit precision the modulo is the answer - J.1.1: "the reconstructed
    // components calculated from the reconstructed differential components are
    // also calculated modulo 2^16" - and it coincides with the sample range.
    // Below that, reducing first would turn an undershoot of -1 into 65535 and
    // so into white, where clamping gives the black the sample actually is.
    for (size_t k = 0; k < n; k++) {
      int32_t v = ref[i].s[k];
      if (precision == 16) {
        v = (int32_t)((uint32_t)v & 0xFFFFu);
      }
      if (v < 0) {
        v = 0;
      }
      if (v > max_val) {
        v = max_val;
      }
      if (wide) {
        ((uint16_t *)cbuf[i])[k] = (uint16_t)v;
      }
      else {
        ((unsigned char *)cbuf[i])[k] = (unsigned char)v;
      }
    }
  }

  const GIMG_Pixel_Format * fmt;
  GIMG_Pixel_Format fmt_n;
  if (num_comp == 1u) {
    fmt = wide ? &GIMG_PIXEL_GRAY16 : &GIMG_PIXEL_GRAY8;
  }
  else if (num_comp == 3u) {
    fmt = wide ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_RGBA8;
  }
  else {
    // Four components are CMYK; any other count carries no color meaning at
    // all.  Either way the samples go out as they came in.
    r = gimg_pixel_format_multichannel(
        num_comp, (uint8_t)out_bits, &fmt_n);
    if (r != GIMG_OK) {
      goto done;
    }
    fmt = &fmt_n;
  }
  r = gimg_raster_create_with_allocator(
      alloc, width, height, fmt, GIMG_RASTER_OWNED, NULL, 0, out_raster);
  if (r != GIMG_OK) {
    goto done;
  }
  {
    unsigned char * pixels = (unsigned char *)gimg_raster_pixels(*out_raster);
    size_t stride = gimg_raster_stride_bytes(*out_raster);
    // Fancy unless the caller explicitly asked for SIMPLE, as everywhere else.
    int use_fancy = (!options ||
        options->jpeg_chroma_upsampling != GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE);
    const int frame_is_rgb = jpeg_frame_is_rgb(state, dhp);
    jpeg_plane_t pl[GIMG_JPEG_MAX_COMPONENTS];
    for (uint8_t i = 0; i < num_comp; i++) {
      pl[i].data = cbuf[i];
      pl[i].stride = ref[i].w;
      pl[i].wide = wide;
    }
    for (uint32_t y = 0; y < height; y++) {
      for (uint32_t x = 0; x < width; x++) {
        int sample[GIMG_JPEG_MAX_COMPONENTS];
        // Component 0 carries the full resolution, so it is read directly;
        // jpeg_component_index still maps the image grid onto the plane, which
        // matters when the sequence stopped short of the DHP size.
        {
          size_t idx = jpeg_component_index(
              ref[0].w, ref[0].h, ref[0].w, x, y, width, height);
          sample[0] = wide ? (int)((const uint16_t *)cbuf[0])[idx]
                           : (int)((const unsigned char *)cbuf[0])[idx];
        }
        for (uint8_t i = 1; i < num_comp; i++) {
          // The chroma filters of jpeg_upsample.c for a subsampled component,
          // which is also the right answer when it is not subsampled - but
          // only when the component is the size the DHP header implies.  A
          // sequence may stop before it reaches the completed image: B.3.1
          // bounds a frame by the DHP size and does not require any frame to
          // attain it, and a truncated file simply has fewer frames than it
          // meant to.  The filter works from the sampling factors and the
          // image size, so pointing it at a plane of some other size reads
          // past the end of it, which is how the fuzzer found this with a
          // 17x9 DHP whose only frame was 9x5.  Falling back to the map keeps
          // such a sequence decodable - it is the smaller picture, scaled -
          // and the map is in bounds for a plane of any size.
          // Four components are CMYK, where the first three are not chroma
          // and the fourth is ink, so the triangle filter has nothing to say
          // about any of them and they all take the map.  It changes nothing
          // for an unsubsampled frame, which is what such a file would be.
          if (num_comp == 3u && ref[i].w == want_w[i] &&
              ref[i].h == want_h[i]) {
            sample[i] = jpeg_chroma_sample(&pl[i], ref[i].w, ref[i].h, x, y,
                width, height, dhp->h_samp[i], dhp->v_samp[i], h_max, v_max,
                use_fancy);
          }
          else {
            size_t idx = jpeg_component_index(
                ref[i].w, ref[i].h, ref[i].w, x, y, width, height);
            sample[i] = wide ? (int)((const uint16_t *)cbuf[i])[idx]
                             : (int)((const unsigned char *)cbuf[i])[idx];
          }
        }
        if (num_comp == 1u) {
          uint32_t v = jpeg_sample_widen(
              (uint32_t)sample[0], precision, out_bits);
          if (wide) {
            ((uint16_t *)(pixels + (size_t)y * stride))[x] = (uint16_t)v;
          }
          else {
            (pixels + (size_t)y * stride)[x] = (unsigned char)v;
          }
        }
        else if (num_comp == 3u) {
          int rv, gv, bv;
          if (frame_is_rgb) {
            // T.81 describes no color space; jpeg_frame_is_rgb reads the
            // conventions that do.  A sequence whose components are already
            // R, G, B is passed through.
            rv = sample[0];
            gv = sample[1];
            bv = sample[2];
          }
          else {
            jpeg_ycbcr_to_rgb(sample[0], sample[1], sample[2],
                1 << (precision - 1), max_val, &rv, &gv, &bv);
          }
          uint32_t r8 = jpeg_sample_widen((uint32_t)rv, precision, out_bits);
          uint32_t g8 = jpeg_sample_widen((uint32_t)gv, precision, out_bits);
          uint32_t b8 = jpeg_sample_widen((uint32_t)bv, precision, out_bits);
          if (wide) {
            uint16_t * p = (uint16_t *)(pixels + (size_t)y * stride);
            p[x * 4 + 0] = (uint16_t)r8;
            p[x * 4 + 1] = (uint16_t)g8;
            p[x * 4 + 2] = (uint16_t)b8;
            p[x * 4 + 3] = 65535u;
          }
          else {
            unsigned char * p = pixels + (size_t)y * stride;
            p[x * 4 + 0] = (unsigned char)r8;
            p[x * 4 + 1] = (unsigned char)g8;
            p[x * 4 + 2] = (unsigned char)b8;
            p[x * 4 + 3] = 255u;
          }
        }
        else {
          // Raw CMYK, or components with no color meaning: unchanged, as the
          // single-frame paths leave them.
          if (wide) {
            uint16_t * p = (uint16_t *)(pixels + (size_t)y * stride);
            for (uint8_t i = 0; i < num_comp; i++) {
              p[(size_t)x * num_comp + i] = (uint16_t)jpeg_sample_widen(
                  (uint32_t)sample[i], precision, out_bits);
            }
          }
          else {
            unsigned char * p = pixels + (size_t)y * stride;
            for (uint8_t i = 0; i < num_comp; i++) {
              p[(size_t)x * num_comp + i] = (unsigned char)jpeg_sample_widen(
                  (uint32_t)sample[i], precision, out_bits);
            }
          }
        }
      }
    }
  }
  gimg_jpeg_attach_color(state, num_comp, *out_raster);

done:
  for (uint8_t i = 0; i < num_comp; i++) {
    gimg_free(alloc, cbuf[i]);
  }
  return r;
}

GIMG_Result gimg_jpeg_decode_hierarchical(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!state || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (state->num_frames == 0u) {
    return GIMG_ERR_CORRUPT;
  }
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);

  size_t pixel_count = 0;
  if (gimg_safe_pixel_count(state->dhp.width, state->dhp.height,
          &pixel_count) != GIMG_OK) {
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Limits * limits =
      options && options->limits ? options->limits : NULL;
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    return GIMG_ERR_LIMIT;
  }

  // The reference components, indexed by their position in the DHP header so
  // that a frame coding a subset of the components lands in the right place.
  hier_plane_t ref[GIMG_JPEG_MAX_COMPONENTS];
  hier_plane_t cur[GIMG_JPEG_MAX_COMPONENTS];
  memset(ref, 0, sizeof(ref));
  GIMG_Result r = GIMG_OK;

  for (unsigned fi = 0; fi < state->num_frames; fi++) {
    const gimg_jpeg_frame_t * f = state->frames[fi];
    memset(cur, 0, sizeof(cur));

    // Map this frame's components onto the DHP's, by identifier (B.2.2: a
    // component is selected by its identifier, not by its position).
    uint8_t gi[GIMG_JPEG_MAX_COMPONENTS];
    for (uint8_t i = 0; i < f->sof.num_components; i++) {
      uint8_t g = 0;
      for (; g < state->dhp.num_components; g++) {
        if (state->dhp.comp_id[g] == f->sof.comp_id[i]) {
          break;
        }
      }
      if (g >= state->dhp.num_components) {
        r = GIMG_ERR_FORMAT; // A component DHP never declared.
        goto done;
      }
      gi[i] = g;
    }

    // B.3.3 and J.2.1: an EXP before this frame expands the reference
    // components it is about to be differenced against, before the frame is
    // decoded.
    if (f->exp_h || f->exp_v) {
      for (uint8_t i = 0; i < f->sof.num_components; i++) {
        if (!ref[gi[i]].s) {
          r = GIMG_ERR_FORMAT; // Nothing to expand: EXP before any reference.
          goto done;
        }
        r = hier_expand(alloc, &ref[gi[i]], f->exp_h, f->exp_v);
        if (r != GIMG_OK) {
          goto done;
        }
      }
    }

    if (f->is_lossless) {
      r = hier_decode_lossless_frame(state, f, cur);
    }
    else if (f->is_progressive) {
      r = hier_decode_progressive_frame(state, f, cur, 0);
    }
    else {
      r = hier_decode_dct_frame(state, f, cur);
    }
    if (r != GIMG_OK) {
      goto done;
    }

    for (uint8_t i = 0; i < f->sof.num_components; i++) {
      hier_plane_t * dst = &ref[gi[i]];
      if (!f->is_differential) {
        // A non-differential frame is the reference from here on (J.1.1).
        hier_plane_free(alloc, dst);
        *dst = cur[i];
        cur[i].s = NULL;
        continue;
      }
      // J.1.1.2: "The upsampling process always doubles the line length or the
      // number of lines."  A frame whose size is not exactly twice the one
      // below it therefore has a reference an odd row or column too large -
      // the 9x5 base of a 17x9 image expands to 18x10 - and the surplus is not
      // part of the picture.  Trim it rather than refusing the file.
      if (dst->s && (dst->w > cur[i].w || dst->h > cur[i].h)) {
        hier_crop(dst, cur[i].w, cur[i].h);
      }
      if (!dst->s || dst->w != cur[i].w || dst->h != cur[i].h) {
        // J.1.1: "When the resolution of a reference component does not match
        // the resolution of the component input to a differential frame, an
        // upsampling filter shall be used" - and B.3.3 makes EXP the only way
        // to ask for one.  A reference that is still too small here means the
        // file asked for a difference between two things that are not the same
        // shape and gave no EXP that would have made them match.
        r = GIMG_ERR_FORMAT;
        goto done;
      }
      // J.2.1: "These differential components shall be added, modulo 2^16, to
      // the upsampled reference components.  This creates a new set of
      // reference components."
      //
      // The sum is kept exactly, in a wider type than the modulus.  Addition
      // commutes with reduction modulo 2^16, so deferring the reduction to the
      // one place that needs it - the emit, and only at P=16, where J.1.1 says
      // the wrap is what is meant - gives the same answer for every sequence
      // the modulo was written for, while leaving the intermediate signed for
      // the far commoner case of a DCT undershoot that a later frame corrects.
      // Thirty-two frames of 16-bit samples cannot overflow int32.
      size_t n = (size_t)dst->w * dst->h;
      for (size_t k = 0; k < n; k++) {
        dst->s[k] += cur[i].s[k];
      }
    }
    for (uint8_t i = 0; i < GIMG_JPEG_MAX_COMPONENTS; i++) {
      hier_plane_free(alloc, &cur[i]);
    }
  }

  r = hier_emit_raster(
      state, ref, options, state->frames[0]->is_lossless, out_raster);

done:
  for (uint8_t i = 0; i < GIMG_JPEG_MAX_COMPONENTS; i++) {
    hier_plane_free(alloc, &ref[i]);
    hier_plane_free(alloc, &cur[i]);
  }
  if (r != GIMG_OK && *out_raster) {
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
  }
  return r;
}
