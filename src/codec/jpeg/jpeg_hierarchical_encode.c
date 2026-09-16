/**
 * @file
 *
 * Writing a hierarchical JPEG (ITU-T T.81 / ISO 10918-1 Annex J, J.1).
 *
 * The encoder is the decoder run backwards, and it has to contain the decoder
 * to work at all: a differential frame codes the difference between the
 * picture and what has been reconstructed so far, so at every step the encoder
 * must reconstruct exactly what a decoder will, quantisation loss included.
 * That is why each frame here is followed by a dequantise-and-inverse-DCT pass
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
 *
 * Copyright 2026 by Corey Pennycuff
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
 * neighbourhood weights", applied horizontally and then vertically: "The
 * centre sample ... should be aligned with the left column or top line of the
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
      int64_t c = (int64_t)(2u * x);
      int64_t l = c - 1, rr = c + 1;
      if (l < 0) {
        l = 0;
      }
      if (c > (int64_t)in->w - 1) {
        c = (int64_t)in->w - 1;
      }
      if (rr > (int64_t)in->w - 1) {
        rr = (int64_t)in->w - 1;
      }
      dst[x] = (int32_t)((src[l] + 2 * src[c] + src[rr]) / 4);
    }
  }
  for (uint32_t y = 0; y < nh; y++) {
    int64_t c = (int64_t)(2u * y);
    int64_t t = c - 1, b = c + 1;
    if (t < 0) {
      t = 0;
    }
    if (c > (int64_t)mid.h - 1) {
      c = (int64_t)mid.h - 1;
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
 * Dequantise, inverse-transform and level-shift one block, exactly as the
 * decoder will: the encoder's reference has to be what the decoder holds.
 *
 * The dequantisation is spelled out here rather than handed to
 * jpeg_dequantise_32 because the two sides of the codec hold the table in
 * different orders.  A DQT segment stores its elements in zigzag order
 * (B.2.4.1), so that is the order the decoder keeps and the order
 * jpeg_dequantise_32 indexes; the encoder's table is in natural order, because
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

void gimg_jpeg_free_enc_frames(const GIMG_Allocator * alloc,
    gimg_jpeg_enc_frame_t * frames, unsigned num_frames) {
  if (!frames) {
    return;
  }
  alloc = gimg_alloc_or_default(alloc);
  for (unsigned i = 0; i < num_frames; i++) {
    gimg_free(alloc, frames[i].scan_data);
    gimg_free(alloc, frames[i].dht);
    frames[i].scan_data = NULL;
    frames[i].dht = NULL;
  }
}

GIMG_Result gimg_jpeg_encode_hierarchical(const GIMG_Allocator * alloc,
    const GIMG_Raster * raster, int levels, int arithmetic,
    uint16_t restart_interval, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, gimg_jpeg_enc_frame_t * frames,
    unsigned * out_num_frames, int * out_num_components) {
  if (!raster || !frames || !out_num_frames || !out_num_components ||
      !quant_luma || !quant_chroma) {
    return GIMG_ERR_INTERNAL;
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
  // Eight-bit grey or colour only.  A pyramid is built out of the sequential
  // DCT process, and the twelve-bit and lossless ones would each need their
  // own reconstruction, which is the part that cannot be approximated.
  if (fmt->bits_per_channel[0] != 8) {
    return GIMG_ERR_UNSUPPORTED;
  }
  int num_components = (fmt->channel_model == GIMG_CHANNEL_GRAY) ? 1 : 3;
  if (fmt->channel_count != 1 && fmt->channel_count != 3 &&
      fmt->channel_count != 4) {
    return GIMG_ERR_UNSUPPORTED;
  }
  *out_num_components = num_components;

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
  henc_plane_t pyr[GIMG_JPEG_MAX_FRAMES][3];
  henc_plane_t ref[3];
  henc_plane_t diff[3];
  memset(pyr, 0, sizeof(pyr));
  memset(ref, 0, sizeof(ref));
  memset(diff, 0, sizeof(diff));
  int16_t * coef = NULL;
  unsigned char * base_planes[3] = {NULL, NULL, NULL};

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

  // Frame 0: an ordinary sequential frame at the smallest resolution, encoded
  // by the same path any single-frame JPEG takes.
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
    const unsigned char * base_ptr[GIMG_JPEG_MAX_COMPONENTS] = {
        base_planes[0], base_planes[1], base_planes[2]};
    size_t base_stride[GIMG_JPEG_MAX_COMPONENTS] = {
        (size_t)w0, (size_t)w0, (size_t)w0};
    r = gimg_jpeg_progressive_fill_coef_buffer(w0, h0, num_components,
        base_ptr, base_stride, NULL, NULL, NULL, quant_luma, quant_chroma,
        GIMG_JPEG_FDCT_LOEFFLER, GIMG_JPEG_QUANT_RECIP, coef, &total_blocks);
    if (r != GIMG_OK) {
      goto done;
    }
    if (arithmetic) {
      jpeg_arith_cond_t cond;
      jpeg_arith_cond_defaults(&cond);
      r = gimg_jpeg_encode_arith_scan_from_coef_buffer(w0, h0, num_components,
          coef, total_blocks, NULL, NULL, NULL, &cond, alloc,
          restart_interval, 0,
          &frames[0].scan_data, &frames[0].scan_size);
    }
    else {
      r = gimg_jpeg_encode_baseline_scan_from_coef_buffer(w0, h0,
          num_components, coef, total_blocks, NULL, NULL, NULL, alloc,
          restart_interval, &frames[0].scan_data, &frames[0].scan_size);
    }
    if (r != GIMG_OK) {
      goto done;
    }
    // Table B.1: the extended sequential process, or its arithmetic
    // counterpart.  Not SOF0: baseline may not be mixed with arithmetic frames
    // and a hierarchical sequence is not a baseline file.
    frames[0].sof_marker =
        arithmetic ? GIMG_JPEG_MARKER_SOF9 : GIMG_JPEG_MARKER_SOF1;
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
    size_t blocks = (size_t)((w + 7u) / 8u) * (size_t)((h + 7u) / 8u) *
        (size_t)num_components;
    coef = (int16_t *)gimg_malloc(alloc, blocks * 64u * sizeof(int16_t));
    if (!coef) {
      r = GIMG_ERR_OOM;
      goto done;
    }
    const int32_t * plane_ptr[3] = {diff[0].s, diff[1].s, diff[2].s};
    size_t plane_stride[3] = {diff[0].w, diff[1].w, diff[2].w};
    size_t total_blocks = 0;
    r = gimg_jpeg_fill_coef_buffer_differential(w, h, num_components, plane_ptr,
        plane_stride, NULL, quant_luma, quant_chroma, coef, &total_blocks);
    if (r != GIMG_OK) {
      goto done;
    }
    if (arithmetic) {
      jpeg_arith_cond_t cond;
      jpeg_arith_cond_defaults(&cond);
      r = gimg_jpeg_encode_arith_scan_from_coef_buffer(w, h, num_components,
          coef, total_blocks, NULL, NULL, NULL, &cond, alloc,
          restart_interval, 1,
          &frames[k].scan_data, &frames[k].scan_size);
    }
    else {
      r = gimg_jpeg_encode_differential_scan(w, h, num_components, coef,
          total_blocks, NULL, NULL, alloc, restart_interval,
          &frames[k].scan_data, &frames[k].scan_size, &frames[k].dht,
          &frames[k].dht_len);
    }
    if (r != GIMG_OK) {
      goto done;
    }
    frames[k].sof_marker =
        arithmetic ? GIMG_JPEG_MARKER_SOF13 : GIMG_JPEG_MARKER_SOF5;
    frames[k].width = (uint16_t)w;
    frames[k].height = (uint16_t)h;
    frames[k].exp_h = 1;
    frames[k].exp_v = 1;
    // J.2.1: the reference for the next frame is this one added to it.
    henc_reconstruct_frame(
        coef, num_components, quant_luma, quant_chroma, 0, 1, ref);
    gimg_free(alloc, coef);
    coef = NULL;
    *out_num_frames = (unsigned)(k + 1);
  }

done:
  gimg_free(alloc, coef);
  for (int c = 0; c < 3; c++) {
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
