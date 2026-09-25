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
 * Writing a hierarchical JPEG (ITU-T T.81 / ISO 10918-1 Annex J, J.1).
 *
 * The encoder is the decoder run backwards, and it has to contain the decoder
 * to work at all: a differential frame codes the difference between the
 * picture and what has been reconstructed so far, so at every step the encoder
 * must reconstruct exactly what a decoder will, quantization loss included.
 * That is why each frame here is followed by a dequantize-and-inverse-DCT pass
 * over the very coefficients that were written, rather than by anything
 * cheaper: what the encoder believes the decoder has must be what the decoder
 * actually gets, or the differences drift.
 *
 * The shape is fixed and simple.  A sequence of N levels writes one
 * non-differential frame at the smallest resolution and then N differential
 * frames, each preceded by an EXP(1,1) that doubles the reference, and every
 * frame uses 4:4:4 sampling - the pyramid is already doing the scaling, and
 * subsampling the chroma on top of it would only add a second, differently
 * aligned one.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/raster.h>
#include <stdint.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "jpeg_internal.h"

/** A component plane of signed samples at one level of the pyramid. */
typedef struct {
  int32_t * s;
  uint32_t w, h;
} henc_plane_t;

static void henc_plane_free(const GIMG_Allocator * alloc, henc_plane_t * p) {
  if (p->s) {
    gimg_free(alloc, p->s);
  }
  p->s = NULL;
  p->w = 0;
  p->h = 0;
}

static GIMG_Result henc_plane_alloc(
    const GIMG_Allocator * alloc, henc_plane_t * p, uint32_t w, uint32_t h) {
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
 * Halve a component plane (T.81 K.5).
 *
 * K.5 gives a 1-2-1 low-pass filter, "normalized by the sum of the
 * neighborhood weights", applied horizontally and then vertically: "The
 * center sample ... should be aligned with the left column or top line of the
 * high resolution image when calculating the left column or top line of the
 * low resolution image.  Sample values which are situated outside of the image
 * boundary are replicated from the sample values at the boundary", and "If the
 * image being downsampled has an odd width or length, the odd dimension is
 * increased by 1 by sample replication ... before downsampling."
 *
 * So output column i takes input columns 2i-1, 2i, 2i+1 with weights 1, 2, 1,
 * each clamped into the image, and the output is (w + 1) / 2 wide - which is
 * exactly the width J.1.1.2's doubling upsampler turns back into w or w + 1,
 * the surplus column being dropped.  K.5 is only an example, and J.1.1.1 leaves
 * the filter to the encoder, but it is the example written to match the
 * normative upsampler, so it is the one to use.
 */
static GIMG_Result henc_downsample(
    const GIMG_Allocator * alloc, const henc_plane_t * in, henc_plane_t * out) {
  uint32_t nw = (in->w + 1u) / 2u;
  uint32_t nh = (in->h + 1u) / 2u;
  GIMG_Result r = henc_plane_alloc(alloc, out, nw, nh);
  if (r != GIMG_OK) {
    return r;
  }
  // Horizontal first, into a scratch plane of the original height.
  henc_plane_t mid;
  r = henc_plane_alloc(alloc, &mid, nw, in->h);
  if (r != GIMG_OK) {
    henc_plane_free(alloc, out);
    return r;
  }
  for (uint32_t y = 0; y < in->h; y++) {
    const int32_t * src = in->s + (size_t)y * in->w;
    int32_t * dst = mid.s + (size_t)y * nw;
    for (uint32_t x = 0; x < nw; x++) {
      // The three taps are clamped to the edges, but only two of them can
      // reach one.  x runs below nw = (w + 1) / 2, so the centre tap 2x is at
      // most w - 1 for every width: a clamp stood here and could not fire.
      // Its neighbours can and do - the left tap at x = 0 and the right tap
      // at the last column - which is why the pair is kept and this is not.
      int64_t c = (int64_t)(2u * x);
      int64_t l = c - 1, rr = c + 1;
      if (l < 0) {
        l = 0;
      }
      if (rr > (int64_t)in->w - 1) {
        rr = (int64_t)in->w - 1;
      }
      dst[x] = (int32_t)((src[l] + 2 * src[c] + src[rr]) / 4);
    }
  }
  for (uint32_t y = 0; y < nh; y++) {
    // Same three taps down the column, and the same one of them unreachable:
    // y runs below nh = (h + 1) / 2, so the centre tap 2y is at most h - 1.
    int64_t c = (int64_t)(2u * y);
    int64_t t = c - 1, b = c + 1;
    if (t < 0) {
      t = 0;
    }
    if (b > (int64_t)mid.h - 1) {
      b = (int64_t)mid.h - 1;
    }
    const int32_t * rt = mid.s + (size_t)t * nw;
    const int32_t * rc = mid.s + (size_t)c * nw;
    const int32_t * rb = mid.s + (size_t)b * nw;
    int32_t * dst = out->s + (size_t)y * nw;
    for (uint32_t x = 0; x < nw; x++) {
      dst[x] = (int32_t)((rt[x] + 2 * rc[x] + rb[x]) / 4);
    }
  }
  henc_plane_free(alloc, &mid);
  return GIMG_OK;
}

/**
 * Double a component plane (T.81 J.1.1.2), then trim to the frame it is about
 * to be differenced against.
 *
 * The same filter the decoder applies, and it has to be: the encoder is
 * predicting what the decoder will hold.  See hier_expand in
 * jpeg_hierarchical.c for the rule quoted in full.
 */
static GIMG_Result henc_expand_to(const GIMG_Allocator * alloc,
    henc_plane_t * p, uint32_t want_w, uint32_t want_h) {
  henc_plane_t out;
  uint32_t nw = p->w * 2u, nh = p->h * 2u;
  GIMG_Result r = henc_plane_alloc(alloc, &out, nw, nh);
  if (r != GIMG_OK) {
    return r;
  }
  for (uint32_t y = 0; y < p->h; y++) {
    const int32_t * a = p->s + (size_t)y * p->w;
    const int32_t * b =
        p->s + (size_t)((y + 1u < p->h) ? (y + 1u) : y) * p->w;
    int32_t * d0 = out.s + (size_t)(y * 2u) * nw;
    int32_t * d1 = out.s + (size_t)(y * 2u + 1u) * nw;
    for (uint32_t x = 0; x < p->w; x++) {
      uint32_t xr = (x + 1u < p->w) ? (x + 1u) : x;
      // Horizontal first, then vertical, as J.1.1.2 requires; doing both in
      // one pass here is the same arithmetic because each output is a mean of
      // means in the same order.
      int32_t a0 = a[x], a1 = (a[x] + a[xr]) / 2;
      int32_t b0 = b[x], b1 = (b[x] + b[xr]) / 2;
      d0[x * 2u] = a0;
      d0[x * 2u + 1u] = a1;
      d1[x * 2u] = (a0 + b0) / 2;
      d1[x * 2u + 1u] = (a1 + b1) / 2;
    }
  }
  if (want_w > nw || want_h > nh) {
    henc_plane_free(alloc, &out);
    return GIMG_ERR_INTERNAL;
  }
  // Trim the row or column that doubling an odd dimension overshoots by.
  for (uint32_t y = 0; y < want_h; y++) {
    memmove(out.s + (size_t)y * want_w, out.s + (size_t)y * nw,
        (size_t)want_w * sizeof(int32_t));
  }
  out.w = want_w;
  out.h = want_h;
  henc_plane_free(alloc, p);
  *p = out;
  return GIMG_OK;
}

/**
 * Dequantize, inverse-transform and level-shift one block, exactly as the
 * decoder will: the encoder's reference has to be what the decoder holds.
 *
 * The dequantization is spelled out here rather than handed to
 * jpeg_dequantize_32 because the two sides of the codec hold the table in
 * different orders.  A DQT segment stores its elements in zigzag order
 * (B.2.4.1), so that is the order the decoder keeps and the order
 * jpeg_dequantize_32 indexes; the encoder's table is in natural order, because
 * that is what jpeg_quantize_block wants and what the DQT writer converts from.
 * Passing one to the other multiplies every coefficient by the wrong element,
 * and the reconstruction it produces bears no relation to the frame - which,
 * in a hierarchical encode, is not a cosmetic error but the reference every
 * later frame is differenced against.
 */
static void henc_reconstruct_block(const int16_t * coef_zig,
    const uint16_t * quant_natural, int32_t level_shift, int32_t out[64]) {
  int16_t rz[64];
  int32_t q[64];
  jpeg_dezigzag(coef_zig, rz); // zigzag order in, natural order out
  for (int i = 0; i < 64; i++) {
    q[i] = (int32_t)rz[i] * (int32_t)quant_natural[i];
  }
  jpeg_idct_8x8_islow(q, out, 2);
  for (int i = 0; i < 64; i++) {
    out[i] += level_shift;
  }
}

/**
 * Reconstruct a whole frame from the coefficients that were written.
 *
 * @p add non-zero accumulates into the planes (a differential frame, J.2.1)
 * rather than replacing them (a non-differential one).
 */
static void henc_reconstruct_frame(const int16_t * coef, int num_components,
    const uint16_t * quant_luma, const uint16_t * quant_chroma,
    int32_t level_shift, int add, henc_plane_t * planes) {
  uint32_t blocks_w = (planes[0].w + 7u) / 8u;
  uint32_t blocks_h = (planes[0].h + 7u) / 8u;
  int32_t block[64];
  const int16_t * in = coef;
  for (uint32_t by = 0; by < blocks_h; by++) {
    for (uint32_t bx = 0; bx < blocks_w; bx++) {
      for (int c = 0; c < num_components; c++) {
        const uint16_t * quant = (c == 0) ? quant_luma : quant_chroma;
        henc_reconstruct_block(in, quant, level_shift, block);
        in += 64;
        for (int row = 0; row < 8; row++) {
          uint32_t y = by * 8u + (uint32_t)row;
          if (y >= planes[c].h) {
            break;
          }
          for (int col = 0; col < 8; col++) {
            uint32_t x = bx * 8u + (uint32_t)col;
            if (x >= planes[c].w) {
              break;
            }
            int32_t * dst = &planes[c].s[(size_t)y * planes[c].w + x];
            *dst = add ? (*dst + block[row * 8 + col]) : block[row * 8 + col];
          }
        }
      }
    }
  }
}

/**
 * Fill in the scan header of a frame written as one sequential scan.
 *
 * T.81 B.2.3: Ss = 0 and Se = 63 say the scan carries the whole block, which
 * is what makes it sequential rather than one of Annex G's spectral bands.
 * A differential frame's own Huffman tables both live at destination 0 (Table
 * J.2's extra category is in no Annex K table, so it generates them), and the
 * non-differential frame uses the standard luminance/chrominance split.
 */
static void henc_set_whole_block_scan(
    gimg_jpeg_enc_frame_t * fr, int num_components, int own_tables) {
  fr->num_scans = 1;
  fr->scans[0].ns = (uint8_t)num_components;
  for (int c = 0; c < num_components && c < (int)GIMG_JPEG_MAX_SCAN_COMPONENTS;
      c++) {
    fr->scans[0].comp[c] = (uint8_t)c;
    fr->scans[0].td_ta[c] = (own_tables || c == 0) ? 0x00u : 0x11u;
  }
  fr->scans[0].ss = 0;
  fr->scans[0].se = 63;
  fr->scans[0].ah = 0;
  fr->scans[0].al = 0;
}

/**
 * Write one frame of the pyramid as one scan per component (T.81 A.2.3).
 *
 * B.2.3 Table B.3 caps Ns at 4 whatever Nf is, so a sequence of more than four
 * components cannot put a frame in one interleaved scan.  Sampling is 4:4:4
 * throughout a pyramid, so a component's blocks are every num_components'th in
 * the interleaved coefficient buffer and gathering one is a strided copy.
 *
 * @param differential  Selects the differential entropy coder, whose DC
 *   coefficient is coded directly (J.1.3.1) and whose AC categories reach past
 *   Annex K's tables - which is why a differential scan generates its own.
 */
/*
 * Nothing calls this today.  gimg_jpeg_encode_hierarchical() refuses a frame
 * of more than GIMG_JPEG_MAX_SCAN_COMPONENTS components before it gets here,
 * deliberately and for the reason given at that check: there is no second
 * implementation to check a wide hierarchical sequence against.  This
 * function, and every `num_components > GIMG_JPEG_MAX_SCAN_COMPONENTS` branch
 * below it, is the code that would run if that gate were ever opened - about
 * thirty-five lines of it, which is most of what coverage reports as untested
 * in this file.  It is untested because it is unreachable, not the other way
 * round.
 *
 * Kept deliberately rather than deleted (Corey, 2026-09-23).  The gate is
 * about the absence of an oracle, not about the code being wrong, and an
 * oracle is the sort of thing that arrives later; throwing the implementation
 * away would mean writing it again from the spec when one does.  So it stays,
 * and this note is here so that a coverage sweep does not read it as work
 * outstanding.
 */
static GIMG_Result henc_split_scans(const GIMG_Allocator * alloc,
    uint32_t width, uint32_t height, int num_components, const int16_t * coef,
    size_t total_blocks, int arithmetic, const jpeg_arith_cond_t * cond,
    uint16_t restart_interval, int differential,
    gimg_jpeg_enc_frame_t * fr) {
  if (num_components < 1 ||
      (unsigned)num_components > GIMG_JPEG_MAX_HIER_SCANS) {
    return GIMG_ERR_UNSUPPORTED;
  }
  (void)total_blocks;
  GIMG_Result r = GIMG_OK;
  uint32_t blk_w = (width + 7u) / 8u;
  uint32_t blk_h = (height + 7u) / 8u;
  size_t nblocks = (size_t)blk_w * (size_t)blk_h;
  fr->num_scans = 0;
  for (int comp = 0; comp < num_components && r == GIMG_OK; comp++) {
    gimg_jpeg_enc_scan_t * sc = &fr->scans[comp];
    int16_t * packed =
        (int16_t *)gimg_malloc(alloc, nblocks * 64u * sizeof(int16_t));
    if (!packed) {
      return GIMG_ERR_OOM;
    }
    for (size_t b = 0; b < nblocks; b++) {
      memcpy(packed + b * 64,
          coef + (b * (size_t)num_components + (size_t)comp) * 64,
          64 * sizeof(int16_t));
    }
    unsigned char * dht = NULL;
    size_t dht_len = 0;
    if (arithmetic) {
      r = gimg_jpeg_encode_arith_scan_from_coef_buffer(blk_w * 8u, blk_h * 8u,
          1, packed, nblocks, NULL, NULL, NULL, cond, alloc, restart_interval,
          differential, &sc->data, &sc->size);
    }
    else if (differential) {
      r = gimg_jpeg_encode_differential_scan(blk_w * 8u, blk_h * 8u, 1, packed,
          nblocks, NULL, NULL, alloc, restart_interval, &sc->data, &sc->size,
          &dht, &dht_len);
    }
    else {
      r = gimg_jpeg_encode_baseline_scan_from_coef_buffer(blk_w * 8u,
          blk_h * 8u, 1, packed, nblocks, NULL, NULL, NULL, alloc,
          restart_interval, &sc->data, &sc->size);
    }
    gimg_free(alloc, packed);
    if (r != GIMG_OK) {
      gimg_free(alloc, dht);
      return r;
    }
    // Every scan of a differential frame generates the same kind of table at
    // the same destination; B.2.4.2 lets the last one written stand, so the
    // frame keeps one and the rest are dropped.
    if (dht && !fr->dht) {
      fr->dht = dht;
      fr->dht_len = dht_len;
    }
    else {
      gimg_free(alloc, dht);
    }
    sc->ns = 1;
    sc->comp[0] = (uint8_t)comp;
    sc->td_ta[0] = 0x00; // the one-component path uses the luminance tables
    sc->ss = 0;
    sc->se = 63;
    sc->ah = 0;
    sc->al = 0;
    fr->num_scans = (unsigned)(comp + 1);
  }
  return r;
}

/**
 * Write one frame of the pyramid as a progressive scan script (T.81 Annex G).
 *
 * The script is the same two-pass one a single-frame progressive JPEG uses
 * here: an interleaved DC scan over every component, then one AC scan per
 * component, because G.1.2.2 says "In a scan with Ss not equal to zero, Ns
 * shall be one".  The coefficients are already in the buffer; only the order
 * they leave in changes, so the reconstruction the next differential frame is
 * built on is untouched.
 *
 * A differential progressive frame (SOF6, SOF14) is the same script over
 * differential coefficients; J.1.3.1 changes what the coefficients mean, not
 * how a progressive scan is arranged.
 */
static GIMG_Result henc_progressive_scans(const GIMG_Allocator * alloc,
    uint32_t width, uint32_t height, int num_components,
    const int16_t * coef, size_t total_blocks, int arithmetic,
    const jpeg_arith_cond_t * cond, uint16_t restart_interval,
    int differential, gimg_jpeg_enc_frame_t * fr) {
  if (num_components < 1 ||
      (unsigned)(1 + num_components) > GIMG_JPEG_MAX_HIER_SCANS) {
    return GIMG_ERR_UNSUPPORTED;
  }
  GIMG_Result r = GIMG_OK;
  fr->num_scans = 0;
  fr->extended_tables = arithmetic ? 0u : 1u;
  // Scan 0: DC, every component, interleaved (G.1.2 allows Ns > 1 only here).
  {
    gimg_jpeg_enc_scan_t * sc = &fr->scans[0];
    if (arithmetic) {
      r = gimg_jpeg_encode_arith_progressive_scan(width, height,
          num_components, coef, total_blocks, NULL, NULL, NULL, differential,
          0, 0, 0, 0, cond, alloc, restart_interval, &sc->data, &sc->size);
    }
    else {
      r = gimg_jpeg_encode_progressive_scan_extended(width, height,
          num_components, coef, total_blocks, NULL, NULL, NULL, differential,
          0, 0, 0, 0, alloc, restart_interval, &sc->data, &sc->size);
    }
    if (r != GIMG_OK) {
      return r;
    }
    sc->ns = (uint8_t)num_components;
    for (int c = 0;
        c < num_components && c < (int)GIMG_JPEG_MAX_SCAN_COMPONENTS; c++) {
      sc->comp[c] = (uint8_t)c;
      // The scan encoder reads tbl_sel, which is NULL here, so it uses the
      // luminance table for component 0 and the chrominance one for the rest;
      // Td must say the same.  Ta is not read in a DC scan (B.2.3).
      sc->td_ta[c] = (uint8_t)(c == 0 ? 0x00 : 0x10);
    }
    sc->ss = 0;
    sc->se = 0;
    sc->ah = 0;
    sc->al = 0;
    fr->num_scans = 1;
  }
  // One AC scan per component, each over that component's own block grid.
  for (int comp = 0; comp < num_components; comp++) {
    gimg_jpeg_enc_scan_t * sc = &fr->scans[1 + comp];
    const int16_t * enc_coef = coef;
    size_t enc_blocks = total_blocks;
    int16_t * packed = NULL;
    uint32_t enc_w = width, enc_h = height;
    if (num_components > 1) {
      // Sampling is 4:4:4 throughout a pyramid, so a component's grid is the
      // frame's grid and its blocks are every num_components'th in the
      // interleaved buffer.
      uint32_t blk_w = (width + 7u) / 8u;
      uint32_t blk_h = (height + 7u) / 8u;
      size_t nblocks = (size_t)blk_w * (size_t)blk_h;
      packed = (int16_t *)gimg_malloc(alloc, nblocks * 64u * sizeof(int16_t));
      if (!packed) {
        return GIMG_ERR_OOM;
      }
      for (size_t b = 0; b < nblocks; b++) {
        memcpy(packed + b * 64,
            coef + (b * (size_t)num_components + (size_t)comp) * 64,
            64 * sizeof(int16_t));
      }
      enc_coef = packed;
      enc_blocks = nblocks;
      enc_w = blk_w * 8u;
      enc_h = blk_h * 8u;
    }
    if (arithmetic) {
      r = gimg_jpeg_encode_arith_progressive_scan(enc_w, enc_h, 1, enc_coef,
          enc_blocks, NULL, NULL, NULL, differential, 1, 63, 0, 0, cond,
          alloc, restart_interval, &sc->data, &sc->size);
    }
    else {
      r = gimg_jpeg_encode_progressive_scan_extended(enc_w, enc_h, 1,
          enc_coef, enc_blocks, NULL, NULL, NULL, differential, 1, 63, 0, 0,
          alloc, restart_interval, &sc->data, &sc->size);
    }
    gimg_free(alloc, packed);
    if (r != GIMG_OK) {
      return r;
    }
    sc->ns = 1;
    sc->comp[0] = (uint8_t)comp;
    sc->td_ta[0] = 0x00;
    sc->ss = 1;
    sc->se = 63;
    sc->ah = 0;
    sc->al = 0;
    fr->num_scans = (unsigned)(2 + comp);
  }
  return r;
}

void gimg_jpeg_free_enc_frames(const GIMG_Allocator * alloc,
    gimg_jpeg_enc_frame_t * frames, unsigned num_frames) {
  if (!frames) {
    return;
  }
  alloc = gimg_alloc_or_default(alloc);
  for (unsigned i = 0; i < num_frames; i++) {
    for (unsigned k = 0; k < frames[i].num_scans &&
        k < GIMG_JPEG_MAX_HIER_SCANS;
        k++) {
      gimg_free(alloc, frames[i].scans[k].data);
      frames[i].scans[k].data = NULL;
    }
    frames[i].num_scans = 0;
    gimg_free(alloc, frames[i].dht);
    frames[i].dht = NULL;
  }
}

GIMG_Result gimg_jpeg_encode_hierarchical(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, int levels, int arithmetic,
    gimg_jpeg_hier_process_t process, int lossless_psv,
    const jpeg_arith_cond_t * cond, uint16_t restart_interval,
    const uint16_t * quant_luma, const uint16_t * quant_chroma,
    gimg_jpeg_enc_frame_t * frames, unsigned * out_num_frames,
    int * out_num_components, int * out_precision) {
  if (!raster || !frames || !out_num_frames || !out_num_components ||
      !quant_luma || !quant_chroma) {
    return GIMG_ERR_INTERNAL;
  }
  const int lossless = (process == GIMG_JPEG_HIER_LOSSLESS);
  const int progressive = (process == GIMG_JPEG_HIER_PROGRESSIVE);
  if (lossless && (lossless_psv < 1 || lossless_psv > 7)) {
    return GIMG_ERR_UNSUPPORTED; // T.81 Table H.1 defines 1..7
  }
  alloc = gimg_alloc_or_default(alloc);
  *out_num_frames = 0;
  // Bound before clearing: the caller's array is GIMG_JPEG_MAX_FRAMES long,
  // and clearing levels + 1 entries of it is only safe once that is known to
  // fit.  jpeg_save.c checks this too; the one here is so that the function is
  // safe to call on its own terms.
  if (levels < 1 || levels + 1 > (int)GIMG_JPEG_MAX_FRAMES) {
    return GIMG_ERR_UNSUPPORTED;
  }
  memset(frames, 0, sizeof(*frames) * (size_t)(levels + 1));
  uint32_t width = gimg_raster_width(raster);
  uint32_t height = gimg_raster_height(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  if (!fmt || width == 0 || height == 0) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // Eight-bit gray or color.  A DCT pyramid is built at 8 bits because the
  // twelve-bit process would need its own reconstruction, which is the part
  // that cannot be approximated; a lossless pyramid is built at 8 bits because
  // that is the precision the color step below produces.
  if (fmt->bits_per_channel[0] != 8) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // T.81 B.2.2 counts a sequence's components as it counts any frame's.  Three
  // channels are color and convert; CMYK and channels with no color meaning
  // go through as they are; an RGBA raster drops its alpha, which a JPEG frame
  // has nowhere to put.
  const int is_color = (fmt->channel_model == GIMG_CHANNEL_RGB ||
      fmt->channel_model == GIMG_CHANNEL_RGBA);
  int num_components;
  if (fmt->channel_model == GIMG_CHANNEL_GRAY) {
    num_components = 1;
  }
  else if (is_color) {
    num_components = 3;
  }
  else if (fmt->channel_model == GIMG_CHANNEL_CMYK ||
      fmt->channel_model == GIMG_CHANNEL_UNKNOWN) {
    num_components = (int)fmt->channel_count;
  }
  else {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (num_components < 1) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // B.2.3 caps Ns at 4, so a wider sequence needs every frame of the pyramid
  // split into one scan per component, in three entropy coders and two
  // processes, and reconstructed from the split.  That is written but not
  // trusted: there is no other implementation to check a wide hierarchical
  // sequence against - libjpeg has no hierarchical mode at all and the ISO
  // reference codec does not accept one this wide - and a round trip through
  // this library alone cannot tell a private misreading from a correct one.
  // Four components and fewer are checked against the reference codec, so that
  // is what is offered.
  if (num_components > (int)GIMG_JPEG_MAX_SCAN_COMPONENTS) {
    return GIMG_ERR_UNSUPPORTED;
  }
  *out_num_components = num_components;
  const int precision = 8;
  if (out_precision) {
    *out_precision = precision;
  }

  // Every frame of the pyramid must still be at least one block, or the
  // smallest one has nothing in it.
  {
    uint32_t w = width, h = height;
    for (int i = 0; i < levels; i++) {
      w = (w + 1u) / 2u;
      h = (h + 1u) / 2u;
    }
    if (w == 0 || h == 0) {
      return GIMG_ERR_UNSUPPORTED;
    }
  }

  GIMG_Result r = GIMG_OK;
  // pyramid[k] holds the source at level k; level `levels` is full size.
  henc_plane_t pyr[GIMG_JPEG_MAX_FRAMES][GIMG_JPEG_MAX_COMPONENTS];
  henc_plane_t ref[GIMG_JPEG_MAX_COMPONENTS];
  henc_plane_t diff[GIMG_JPEG_MAX_COMPONENTS];
  memset(pyr, 0, sizeof(pyr));
  memset(ref, 0, sizeof(ref));
  memset(diff, 0, sizeof(diff));
  int16_t * coef = NULL;
  unsigned char * base_planes[GIMG_JPEG_MAX_COMPONENTS];
  memset(base_planes, 0, sizeof(base_planes));

  size_t stride_bytes = gimg_raster_stride_bytes(raster);
  const unsigned char * pixels =
      (const unsigned char *)gimg_raster_pixels_const(raster);
  if (!pixels) {
    return GIMG_ERR_UNSUPPORTED;
  }
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  for (int c = 0; c < num_components; c++) {
    r = henc_plane_alloc(alloc, &pyr[levels][c], width, height);
    if (r != GIMG_OK) {
      goto done;
    }
  }
  for (uint32_t y = 0; y < height; y++) {
    const unsigned char * row = pixels + (size_t)y * stride_bytes;
    for (uint32_t x = 0; x < width; x++) {
      if (num_components == 1) {
        pyr[levels][0].s[(size_t)y * width + x] = row[x * bpp];
      }
      else if (lossless || !is_color) {
        // A lossless sequence keeps RGB - the YCbCr conversion is not
        // reversible, so converting here would make "lossless" a lie, the same
        // reason gimg_jpeg_encode_lossless stores RGB and marks it with an
        // Adobe APP14 saying transform 0 - and components with no color
        // meaning have nothing to convert either way.
        for (int c = 0; c < num_components; c++) {
          pyr[levels][c].s[(size_t)y * width + x] =
              row[x * bpp + (size_t)c];
        }
      }
      else {
        uint8_t yv, cb, cr;
        jpeg_rgb_to_ycbcr(
            row[x * bpp + 0], row[x * bpp + 1], row[x * bpp + 2], &yv, &cb, &cr);
        pyr[levels][0].s[(size_t)y * width + x] = yv;
        pyr[levels][1].s[(size_t)y * width + x] = cb;
        pyr[levels][2].s[(size_t)y * width + x] = cr;
      }
    }
  }
  for (int k = levels; k > 0; k--) {
    for (int c = 0; c < num_components; c++) {
      r = henc_downsample(alloc, &pyr[k][c], &pyr[k - 1][c]);
      if (r != GIMG_OK) {
        goto done;
      }
    }
  }

  // Frame 0 of a lossless sequence: an ordinary lossless frame at the smallest
  // resolution (T.81 SOF3, or SOF11 for the arithmetic coder), predicted and
  // coded by Annex H.  There is no DCT and no quantizer here, so the
  // reconstruction the next frame is differenced against is the frame itself.
  if (lossless) {
    uint32_t w0 = pyr[0][0].w, h0 = pyr[0][0].h;
    int32_t * pl[GIMG_JPEG_MAX_COMPONENTS];
    size_t pls[GIMG_JPEG_MAX_COMPONENTS];
    for (int c = 0; c < num_components; c++) {
      pl[c] = pyr[0][c].s;
      pls[c] = pyr[0][c].w;
    }
    // B.2.3 caps Ns at 4, so a wider sequence carries one component per scan.
    gimg_jpeg_lossless_scan_t ll_scans[GIMG_JPEG_MAX_COMPONENTS];
    unsigned ll_num_scans = 0;
    memset(ll_scans, 0, sizeof(ll_scans));
    if (num_components > (int)GIMG_JPEG_MAX_SCAN_COMPONENTS) {
      for (int c = 0; c < num_components && r == GIMG_OK; c++) {
        const int32_t * one = pl[c];
        size_t one_stride = pls[c];
        r = gimg_jpeg_encode_lossless_planes(alloc, &one, &one_stride, w0, h0,
            1, precision, lossless_psv, restart_interval, arithmetic, cond,
            &ll_scans[c].data, &ll_scans[c].size, &ll_scans[c].dht,
            &ll_scans[c].dht_len);
        ll_scans[c].component = (uint8_t)c;
      }
      ll_num_scans = (unsigned)num_components;
    }
    else {
      r = gimg_jpeg_encode_lossless_planes(alloc, (const int32_t * const *)pl,
          pls, w0, h0, num_components, precision, lossless_psv,
          restart_interval, arithmetic, cond, &ll_scans[0].data,
          &ll_scans[0].size,
          &ll_scans[0].dht, &ll_scans[0].dht_len);
      ll_scans[0].component = 0xFFu;
      ll_num_scans = 1u;
    }
    for (unsigned si = 0; si < ll_num_scans; si++) {
      frames[0].scans[si].data = ll_scans[si].data;
      frames[0].scans[si].size = ll_scans[si].size;
      // Every scan generated a table at the same destination; B.2.4.2 lets the
      // last one written stand, so the frame keeps one and the rest go.
      if (ll_scans[si].dht && !frames[0].dht) {
        frames[0].dht = ll_scans[si].dht;
        frames[0].dht_len = ll_scans[si].dht_len;
      }
      else {
        gimg_free(alloc, ll_scans[si].dht);
      }
    }
    if (r != GIMG_OK) {
      goto done;
    }
    frames[0].sof_marker =
        arithmetic ? GIMG_JPEG_MARKER_SOF11 : GIMG_JPEG_MARKER_SOF3;
    frames[0].precision = (uint8_t)precision;
    frames[0].width = (uint16_t)w0;
    frames[0].height = (uint16_t)h0;
    // H.1: Ss carries the predictor selection value, Se is zero, and Al is the
    // point transform, which is zero here.  B.2.3 caps Ns at 4, so a wider
    // sequence carries one component per scan; that is what the lossless
    // encoder produced above, and each scan says which component it is.
    frames[0].num_scans = ll_num_scans;
    for (unsigned si = 0; si < ll_num_scans; si++) {
      int one = (ll_scans[si].component != 0xFFu);
      frames[0].scans[si].ns = one ? 1u : (uint8_t)num_components;
      for (int c = 0; c < (int)frames[0].scans[si].ns; c++) {
        frames[0].scans[si].comp[c] =
            one ? ll_scans[si].component : (uint8_t)c;
        frames[0].scans[si].td_ta[c] = 0x00;
      }
      frames[0].scans[si].ss = (uint8_t)lossless_psv;
      frames[0].scans[si].se = 0;
      frames[0].scans[si].ah = 0;
      frames[0].scans[si].al = 0;
    }
    for (int c = 0; c < num_components; c++) {
      r = henc_plane_alloc(alloc, &ref[c], w0, h0);
      if (r != GIMG_OK) {
        goto done;
      }
      memcpy(ref[c].s, pyr[0][c].s,
          (size_t)w0 * (size_t)h0 * sizeof(int32_t));
    }
    *out_num_frames = 1;
    goto differential_frames;
  }

  // Frame 0 of a DCT sequence: an ordinary sequential or progressive frame at
  // the smallest resolution, encoded by the same path any single-frame JPEG
  // takes.
  {
    uint32_t w0 = pyr[0][0].w, h0 = pyr[0][0].h;
    size_t n = (size_t)w0 * h0;
    for (int c = 0; c < num_components; c++) {
      base_planes[c] = (unsigned char *)gimg_malloc(alloc, n);
      if (!base_planes[c]) {
        r = GIMG_ERR_OOM;
        goto done;
      }
      for (size_t i = 0; i < n; i++) {
        int32_t v = pyr[0][c].s[i];
        base_planes[c][i] = (unsigned char)(v < 0 ? 0 : (v > 255 ? 255 : v));
      }
    }
    size_t blocks = (size_t)((w0 + 7u) / 8u) * (size_t)((h0 + 7u) / 8u) *
        (size_t)num_components;
    coef = (int16_t *)gimg_malloc(alloc, blocks * 64u * sizeof(int16_t));
    if (!coef) {
      r = GIMG_ERR_OOM;
      goto done;
    }
    size_t total_blocks = 0;
    const unsigned char * base_ptr[GIMG_JPEG_MAX_COMPONENTS];
    size_t base_stride[GIMG_JPEG_MAX_COMPONENTS];
    for (int c = 0; c < (int)GIMG_JPEG_MAX_COMPONENTS; c++) {
      base_ptr[c] = base_planes[c];
      base_stride[c] = (size_t)w0;
    }
    r = gimg_jpeg_progressive_fill_coef_buffer(w0, h0, num_components,
        base_ptr, base_stride, NULL, NULL, NULL, quant_luma, quant_chroma,
        GIMG_JPEG_FDCT_LOEFFLER, GIMG_JPEG_QUANT_RECIP, coef, &total_blocks);
    if (r != GIMG_OK) {
      goto done;
    }
    if (progressive) {
      r = henc_progressive_scans(alloc, w0, h0, num_components, coef,
          total_blocks, arithmetic, cond, restart_interval, 0, &frames[0]);
    }
    else if (num_components > (int)GIMG_JPEG_MAX_SCAN_COMPONENTS) {
      r = henc_split_scans(alloc, w0, h0, num_components, coef, total_blocks,
          arithmetic, cond, restart_interval, 0, &frames[0]);
    }
    else if (arithmetic) {
      r = gimg_jpeg_encode_arith_scan_from_coef_buffer(w0, h0, num_components,
          coef, total_blocks, NULL, NULL, NULL, cond, alloc,
          restart_interval, 0,
          &frames[0].scans[0].data, &frames[0].scans[0].size);
      henc_set_whole_block_scan(&frames[0], num_components, 0);
    }
    else {
      r = gimg_jpeg_encode_baseline_scan_from_coef_buffer(w0, h0,
          num_components, coef, total_blocks, NULL, NULL, NULL, alloc,
          restart_interval, &frames[0].scans[0].data,
          &frames[0].scans[0].size);
      henc_set_whole_block_scan(&frames[0], num_components, 0);
    }
    if (r != GIMG_OK) {
      goto done;
    }
    // Table B.1: the extended sequential process, the progressive process, or
    // the arithmetic counterpart of either.  Not SOF0: baseline may not be
    // mixed with arithmetic frames and a hierarchical sequence is not a
    // baseline file.
    frames[0].sof_marker = progressive
        ? (arithmetic ? GIMG_JPEG_MARKER_SOF10 : GIMG_JPEG_MARKER_SOF2)
        : (arithmetic ? GIMG_JPEG_MARKER_SOF9 : GIMG_JPEG_MARKER_SOF1);
    frames[0].precision = (uint8_t)precision;
    frames[0].width = (uint16_t)w0;
    frames[0].height = (uint16_t)h0;
    for (int c = 0; c < num_components; c++) {
      r = henc_plane_alloc(alloc, &ref[c], w0, h0);
      if (r != GIMG_OK) {
        goto done;
      }
    }
    henc_reconstruct_frame(
        coef, num_components, quant_luma, quant_chroma, 128, 0, ref);
    gimg_free(alloc, coef);
    coef = NULL;
  }
  *out_num_frames = 1;

differential_frames:
  for (int k = 1; k <= levels; k++) {
    uint32_t w = pyr[k][0].w, h = pyr[k][0].h;
    for (int c = 0; c < num_components; c++) {
      r = henc_expand_to(alloc, &ref[c], pyr[k][c].w, pyr[k][c].h);
      if (r != GIMG_OK) {
        goto done;
      }
      henc_plane_free(alloc, &diff[c]);
      r = henc_plane_alloc(alloc, &diff[c], pyr[k][c].w, pyr[k][c].h);
      if (r != GIMG_OK) {
        goto done;
      }
      size_t n = (size_t)pyr[k][c].w * pyr[k][c].h;
      for (size_t i = 0; i < n; i++) {
        diff[c].s[i] = pyr[k][c].s[i] - ref[c].s[i];
      }
    }
    const int32_t * plane_ptr[GIMG_JPEG_MAX_COMPONENTS];
    size_t plane_stride[GIMG_JPEG_MAX_COMPONENTS];
    for (int c = 0; c < num_components; c++) {
      plane_ptr[c] = diff[c].s;
      plane_stride[c] = diff[c].w;
    }
    frames[k].precision = (uint8_t)precision;
    frames[k].width = (uint16_t)w;
    frames[k].height = (uint16_t)h;
    frames[k].exp_h = 1;
    frames[k].exp_v = 1;
    if (lossless) {
      // T.81 SOF7, or SOF15 for the arithmetic coder.  J.1.3.2 fixes the
      // prediction selection value at zero for a differential lossless frame,
      // so the scan codes the difference image itself and reconstruction is
      // exact - which is the whole point of a lossless pyramid.
      const int split_ll =
          (num_components > (int)GIMG_JPEG_MAX_SCAN_COMPONENTS);
      unsigned nsc = split_ll ? (unsigned)num_components : 1u;
      for (unsigned si = 0; si < nsc && r == GIMG_OK; si++) {
        unsigned char * one_dht = NULL;
        size_t one_dht_len = 0;
        if (split_ll) {
          const int32_t * one = plane_ptr[si];
          size_t one_stride = plane_stride[si];
          r = gimg_jpeg_encode_lossless_differential(alloc, &one, &one_stride,
              w, h, 1, restart_interval, arithmetic, cond,
              &frames[k].scans[si].data, &frames[k].scans[si].size, &one_dht,
              &one_dht_len);
        }
        else {
          r = gimg_jpeg_encode_lossless_differential(alloc, plane_ptr,
              plane_stride, w, h, num_components, restart_interval, arithmetic,
              cond, &frames[k].scans[si].data, &frames[k].scans[si].size,
              &one_dht, &one_dht_len);
        }
        if (one_dht && !frames[k].dht) {
          frames[k].dht = one_dht;
          frames[k].dht_len = one_dht_len;
        }
        else {
          gimg_free(alloc, one_dht);
        }
        frames[k].scans[si].ns =
            split_ll ? 1u : (uint8_t)num_components;
        for (int c = 0; c < (int)frames[k].scans[si].ns; c++) {
          frames[k].scans[si].comp[c] = split_ll ? (uint8_t)si : (uint8_t)c;
          frames[k].scans[si].td_ta[c] = 0x00;
        }
        frames[k].scans[si].ss = 0; // J.1.3.2: "shall be set to zero"
        frames[k].scans[si].se = 0;
        frames[k].scans[si].ah = 0;
        frames[k].scans[si].al = 0;
        frames[k].num_scans = si + 1u;
      }
      if (r != GIMG_OK) {
        goto done;
      }
      frames[k].sof_marker =
          arithmetic ? GIMG_JPEG_MARKER_SOF15 : GIMG_JPEG_MARKER_SOF7;
      // J.2.1: the reference for the next frame is this one added to it, and
      // there is nothing to lose on the way.
      for (int c = 0; c < num_components; c++) {
        size_t n = (size_t)diff[c].w * diff[c].h;
        for (size_t i = 0; i < n; i++) {
          ref[c].s[i] += diff[c].s[i];
        }
      }
      *out_num_frames = (unsigned)(k + 1);
      continue;
    }
    size_t blocks = (size_t)((w + 7u) / 8u) * (size_t)((h + 7u) / 8u) *
        (size_t)num_components;
    coef = (int16_t *)gimg_malloc(alloc, blocks * 64u * sizeof(int16_t));
    if (!coef) {
      r = GIMG_ERR_OOM;
      goto done;
    }
    size_t total_blocks = 0;
    r = gimg_jpeg_fill_coef_buffer_differential(w, h, num_components, plane_ptr,
        plane_stride, NULL, quant_luma, quant_chroma, coef, &total_blocks);
    if (r != GIMG_OK) {
      goto done;
    }
    if (progressive) {
      r = henc_progressive_scans(alloc, w, h, num_components, coef,
          total_blocks, arithmetic, cond, restart_interval, 1, &frames[k]);
    }
    else if (num_components > (int)GIMG_JPEG_MAX_SCAN_COMPONENTS) {
      r = henc_split_scans(alloc, w, h, num_components, coef, total_blocks,
          arithmetic, cond, restart_interval, 1, &frames[k]);
    }
    else if (arithmetic) {
      r = gimg_jpeg_encode_arith_scan_from_coef_buffer(w, h, num_components,
          coef, total_blocks, NULL, NULL, NULL, cond, alloc,
          restart_interval, 1,
          &frames[k].scans[0].data, &frames[k].scans[0].size);
      henc_set_whole_block_scan(&frames[k], num_components, 0);
    }
    else {
      r = gimg_jpeg_encode_differential_scan(w, h, num_components, coef,
          total_blocks, NULL, NULL, alloc, restart_interval,
          &frames[k].scans[0].data, &frames[k].scans[0].size, &frames[k].dht,
          &frames[k].dht_len);
      // Table J.2's extra AC category is in no Annex K table, so this frame
      // generated its own and both live at destination 0.
      henc_set_whole_block_scan(&frames[k], num_components, 1);
    }
    if (r != GIMG_OK) {
      goto done;
    }
    frames[k].sof_marker = progressive
        ? (arithmetic ? GIMG_JPEG_MARKER_SOF14 : GIMG_JPEG_MARKER_SOF6)
        : (arithmetic ? GIMG_JPEG_MARKER_SOF13 : GIMG_JPEG_MARKER_SOF5);
    // J.2.1: the reference for the next frame is this one added to it.
    henc_reconstruct_frame(
        coef, num_components, quant_luma, quant_chroma, 0, 1, ref);
    gimg_free(alloc, coef);
    coef = NULL;
    *out_num_frames = (unsigned)(k + 1);
  }

done:
  gimg_free(alloc, coef);
  for (int c = 0; c < (int)GIMG_JPEG_MAX_COMPONENTS; c++) {
    gimg_free(alloc, base_planes[c]);
    henc_plane_free(alloc, &ref[c]);
    henc_plane_free(alloc, &diff[c]);
    for (int k = 0; k < (int)GIMG_JPEG_MAX_FRAMES; k++) {
      henc_plane_free(alloc, &pyr[k][c]);
    }
  }
  if (r != GIMG_OK) {
    gimg_jpeg_free_enc_frames(alloc, frames, (unsigned)(levels + 1));
    *out_num_frames = 0;
  }
  return r;
}
