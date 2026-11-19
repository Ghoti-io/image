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
 * WebP Phase E: ANMF header parse and canvas compositing (dispose / blend)
 * matching libwebp's anim_dump.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>

#include <string.h>

#include "../../core/alloc_internal.h"
#include "webp_internal.h"

static uint32_t webp_u24(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

GIMG_Result gimg_webp_parse_anmf_header(const unsigned char * header16,
    gimg_webp_frame_t * frame) {
  if (!header16 || !frame) {
    return GIMG_ERR_INTERNAL;
  }
  memset(frame, 0, sizeof(*frame));
  // On-disk X/Y are half-pixels; multiply by two (libwebp demux).
  frame->x = webp_u24(header16) * 2u;
  frame->y = webp_u24(header16 + 3) * 2u;
  frame->width = webp_u24(header16 + 6) + 1u;
  frame->height = webp_u24(header16 + 9) + 1u;
  frame->duration_ms = webp_u24(header16 + 12);
  const uint8_t bits = header16[15];
  frame->dispose_background = (uint8_t)(bits & 1u);
  frame->blend_source = (uint8_t)((bits & 2u) ? 1u : 0u);
  if (frame->width == 0u || frame->height == 0u) {
    return GIMG_ERR_CORRUPT;
  }
  return GIMG_OK;
}

/** Write @a alpha (width x height samples) into the A bytes of @a raster. */
static void webp_apply_alpha_plane(GIMG_Raster * raster, const uint8_t * alpha) {
  const uint32_t w = gimg_raster_width(raster);
  const uint32_t h = gimg_raster_height(raster);
  uint8_t * px = (uint8_t *)gimg_raster_pixels(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      px[(size_t)y * stride + (size_t)x * 4u + 3u] = alpha[(size_t)y * w + x];
    }
  }
}

GIMG_Result gimg_webp_decode_picture(const unsigned char * vp8, size_t vp8_size,
    const unsigned char * vp8l, size_t vp8l_size, const unsigned char * alph,
    size_t alph_size, const GIMG_Allocator * alloc, gimg_vp8_seq * seq,
    GIMG_Raster ** out_raster) {
  if (!out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if ((vp8l != NULL) == (vp8 != NULL)) {
    return GIMG_ERR_UNSUPPORTED;
  }

  // ALPH beside VP8L is unusual for still files; ANMF may still carry it.
  //
  // The alpha plane is decoded first when the bitstream header says how big
  // it is, so its working set (four bytes a pixel for the lossless decoder
  // that makes it) is gone before the colour picture's is built, and the two
  // never stand together.  A header that does not say - a VP8 frame that is
  // not a key frame - is decoded in the old order.
  uint8_t * alpha = NULL;
  uint32_t want_w = 0u;
  uint32_t want_h = 0u;
  const int have_alph = alph && alph_size > 0u;
  const int sized = have_alph &&
      (vp8l ? gimg_webp_peek_vp8l_dims(vp8l, vp8l_size, &want_w, &want_h, NULL)
            : gimg_webp_peek_vp8_dims(vp8, vp8_size, &want_w, &want_h));
  if (sized && vp8) {
    // The alpha plane is the size the frame declares, which a few bytes of
    // header can make as large as the limits allow.  Make sure the colour
    // data holds that many macroblocks before spending a plane on it.
    const GIMG_Result mr = gimg_webp_vp8_check_modes(vp8, vp8_size, alloc);
    if (mr != GIMG_OK) {
      return mr;
    }
  }
  if (sized) {
    const GIMG_Result ar = gimg_webp_alpha_decode(
        alph, alph_size, want_w, want_h, alloc, &alpha);
    if (ar != GIMG_OK) {
      return ar;
    }
  }

  GIMG_Result r;
  if (vp8l) {
    r = gimg_webp_vp8l_decode(vp8l, vp8l_size, alloc, out_raster);
  }
  else {
    r = seq ? gimg_webp_vp8_decode_frame(vp8, vp8_size, alloc, seq, out_raster)
            : gimg_webp_vp8_decode(vp8, vp8_size, alloc, out_raster);
  }
  if (r != GIMG_OK) {
    gimg_free(alloc, alpha);
    return r;
  }
  if (have_alph && !sized) {
    r = gimg_webp_alpha_decode(alph, alph_size, gimg_raster_width(*out_raster),
        gimg_raster_height(*out_raster), alloc, &alpha);
    if (r != GIMG_OK) {
      gimg_raster_destroy(*out_raster);
      *out_raster = NULL;
      return r;
    }
  }
  else if (sized && (gimg_raster_width(*out_raster) != want_w ||
                        gimg_raster_height(*out_raster) != want_h)) {
    gimg_free(alloc, alpha);
    gimg_raster_destroy(*out_raster);
    *out_raster = NULL;
    return GIMG_ERR_CORRUPT;
  }
  if (alpha) {
    webp_apply_alpha_plane(*out_raster, alpha);
    gimg_free(alloc, alpha);
  }
  return GIMG_OK;
}

/** Non-premultiplied OVER, matching libwebp BlendPixelNonPremult (RGBA). */
static void webp_blend_pixel(uint8_t * dst, const uint8_t * src) {
  const uint8_t sa = src[3];
  if (sa == 0) {
    return;
  }
  if (sa == 255) {
    dst[0] = src[0];
    dst[1] = src[1];
    dst[2] = src[2];
    dst[3] = 255;
    return;
  }
  const uint8_t da = dst[3];
  const uint8_t dst_factor_a = (uint8_t)((da * (256u - sa)) >> 8);
  const uint8_t blend_a = (uint8_t)(sa + dst_factor_a);
  if (blend_a == 0) {
    dst[0] = dst[1] = dst[2] = dst[3] = 0;
    return;
  }
  const uint32_t scale = (1u << 24) / (uint32_t)blend_a;
  for (int c = 0; c < 3; ++c) {
    const uint32_t blend_unscaled =
        (uint32_t)src[c] * sa + (uint32_t)dst[c] * dst_factor_a;
    dst[c] = (uint8_t)((blend_unscaled * scale) >> 24);
  }
  dst[3] = blend_a;
}

static void webp_blit_frame(uint8_t * canvas, size_t canvas_stride,
    uint32_t canvas_w, uint32_t canvas_h, const uint8_t * frame,
    size_t frame_stride, uint32_t fx, uint32_t fy, uint32_t fw, uint32_t fh,
    int blend_source) {
  if (fx >= canvas_w || fy >= canvas_h) {
    return;
  }
  if (fw > canvas_w - fx) {
    fw = canvas_w - fx;
  }
  if (fh > canvas_h - fy) {
    fh = canvas_h - fy;
  }
  for (uint32_t y = 0; y < fh; ++y) {
    uint8_t * dst =
        canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * 4u;
    const uint8_t * src = frame + (size_t)y * frame_stride;
    if (blend_source) {
      memcpy(dst, src, (size_t)fw * 4u);
    }
    else {
      for (uint32_t x = 0; x < fw; ++x) {
        webp_blend_pixel(dst + (size_t)x * 4u, src + (size_t)x * 4u);
      }
    }
  }
}

static void webp_clear_rect(uint8_t * canvas, size_t canvas_stride,
    uint32_t canvas_w, uint32_t canvas_h, uint32_t fx, uint32_t fy, uint32_t fw,
    uint32_t fh) {
  if (fx >= canvas_w || fy >= canvas_h) {
    return;
  }
  if (fw > canvas_w - fx) {
    fw = canvas_w - fx;
  }
  if (fh > canvas_h - fy) {
    fh = canvas_h - fy;
  }
  for (uint32_t y = 0; y < fh; ++y) {
    memset(canvas + (size_t)(fy + y) * canvas_stride + (size_t)fx * 4u, 0,
        (size_t)fw * 4u);
  }
}

/**
 * Match libwebp anim_decode.c IsKeyFrame: a keyframe resets the canvas and
 * is blitted without alpha blending (the bitstream pixels are the canvas).
 * Frame 0 is always a keyframe; later frames are when they fully cover the
 * canvas without blending (or after a full dispose).
 */
static int webp_is_key_frame(const gimg_webp_doc_state_t * st, size_t index,
    int prev_was_key_frame) {
  const gimg_webp_frame_t * fr = &st->frames[index];
  if (index == 0u) {
    return 1;
  }
  const int full = (fr->width == st->canvas_width &&
      fr->height == st->canvas_height && fr->x == 0u && fr->y == 0u);
  // blend_source != 0 means ANMF "do not blend" (WEBP_MUX_NO_BLEND). Do not
  // treat "no ALPH chunk" as opaque: VP8L carries its own alpha bit, and a
  // full-canvas OVER frame with in-bitstream alpha is not a keyframe.
  if (full && fr->blend_source) {
    return 1;
  }
  // has_alpha is not stored separately; a full-frame SOURCE blit already
  // returned. A full-frame OVER blit is only a keyframe when the previous
  // frame disposed the whole canvas (or was itself a keyframe that covered
  // everything and then disposed).
  const gimg_webp_frame_t * prev = &st->frames[index - 1u];
  if (!prev->dispose_background) {
    return 0;
  }
  const int prev_full = (prev->width == st->canvas_width &&
      prev->height == st->canvas_height && prev->x == 0u && prev->y == 0u);
  return prev_full || prev_was_key_frame;
}

static GIMG_Result webp_decode_one_frame(const gimg_webp_doc_state_t * st,
    const gimg_webp_frame_t * fr, gimg_vp8_seq * seq, GIMG_Raster ** out) {
  const unsigned char * vp8 = NULL;
  size_t vp8_size = 0;
  const unsigned char * vp8l = NULL;
  size_t vp8l_size = 0;
  const unsigned char * alph = NULL;
  size_t alph_size = 0;
  if (fr->vp8_payload_size > 0u) {
    if (fr->vp8_payload_off + (size_t)fr->vp8_payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    vp8 = st->file_bytes + fr->vp8_payload_off;
    vp8_size = fr->vp8_payload_size;
  }
  if (fr->vp8l_payload_size > 0u) {
    if (fr->vp8l_payload_off + (size_t)fr->vp8l_payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    vp8l = st->file_bytes + fr->vp8l_payload_off;
    vp8l_size = fr->vp8l_payload_size;
  }
  if (fr->alph_payload_size > 0u) {
    if (fr->alph_payload_off + (size_t)fr->alph_payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    alph = st->file_bytes + fr->alph_payload_off;
    alph_size = fr->alph_payload_size;
  }
  {
    const GIMG_Result sr = gimg_webp_check_bitstream_size(
        vp8, vp8_size, vp8l, vp8l_size, fr->width, fr->height);
    if (sr != GIMG_OK) {
      return sr;
    }
  }
  if (vp8l && seq) {
    /* A lossless frame is not a VP8 reference. The next VP8 interframe
     * has to follow a keyframe again. */
    gimg_vp8_seq_reset(st->allocator, seq);
  }
  return gimg_webp_decode_picture(
      vp8, vp8_size, vp8l, vp8l_size, alph, alph_size, st->allocator, seq, out);
}

/**
 * Whether decoding frame @p index depends on nothing the frames before it
 * left behind in the VP8 sequence state.  A lossless frame resets it, and a
 * lossy key frame restores every default it holds, so either can be decoded
 * from a fresh sequence; an inter frame cannot.  Anything that cannot be
 * classified is treated as dependent, which only costs a replay.
 */
static int webp_frame_starts_fresh(
    const gimg_webp_doc_state_t * st, size_t index) {
  const gimg_webp_frame_t * fr = &st->frames[index];
  if (fr->vp8l_payload_size > 0u) {
    return 1;
  }
  if (fr->vp8_payload_size == 0u ||
      fr->vp8_payload_off + (size_t)fr->vp8_payload_size > st->file_size) {
    return 0;
  }
  uint32_t w = 0;
  uint32_t h = 0;
  return gimg_webp_peek_vp8_dims(
      st->file_bytes + fr->vp8_payload_off, fr->vp8_payload_size, &w, &h);
}

/**
 * Whether frame @p index exists and is a lossy frame that is not a key frame,
 * which is the only kind that reads the reference pictures the frame before it
 * left.  The tag's first bit is the frame type (RFC 6386 section 9.1: 0 is a
 * key frame).  Anything that cannot be read is not one: such a frame fails
 * when it is decoded, and then nothing was waiting on a reference.
 */
static int webp_frame_is_vp8_inter(
    const gimg_webp_doc_state_t * st, size_t index) {
  if (index >= st->frame_count) {
    return 0;
  }
  const gimg_webp_frame_t * fr = &st->frames[index];
  if (fr->vp8l_payload_size > 0u || fr->vp8_payload_size == 0u ||
      fr->vp8_payload_off + (size_t)fr->vp8_payload_size > st->file_size) {
    return 0;
  }
  return (st->file_bytes[fr->vp8_payload_off] & 1u) != 0u;
}

/**
 * Record the canvas frame @p index + 1 starts from, which is the picture frame
 * @p index returned, and with it @p seq, the VP8 sequence state that frame
 * left (reference pictures, probabilities, segment map) when frame @p index + 1
 * is a lossy inter frame and needs it.  Failing to is not a decode failure;
 * only the head start for the next frame is lost.  The cache only moves
 * forward, except that frame 0 always replaces it: a looping player starts over
 * there, and a cache left at the end of the last pass would make every frame of
 * the next one replay all those before it.
 *
 * @p seq is the decoder's own sequence, which is finished with, so it is
 * handed over rather than copied: the cache holds it and not a second copy of
 * the references.
 *
 * @return Whether @p seq was taken.  When not, it is still the caller's.
 */
static int webp_cache_store(gimg_webp_doc_state_t * st, size_t index,
    const uint8_t * canvas, size_t stride, size_t bytes, int key,
    gimg_vp8_seq * seq) {
  GCU_MUTEX_LOCK(st->cache_lock);
  const bool worth_taking = !st->cache_canvas || st->cache_bytes != bytes ||
      st->cache_next <= index || index == 0u;
  GCU_MUTEX_UNLOCK(st->cache_lock);
  if (!worth_taking) {
    return 0;
  }
  uint8_t * snapshot = (uint8_t *)gimg_malloc(st->allocator, bytes);
  if (!snapshot) {
    return 0;
  }
  memcpy(snapshot, canvas, bytes);
  int taken = 0;
  gimg_vp8_seq * displaced = NULL;
  GCU_MUTEX_LOCK(st->cache_lock);
  if (!st->cache_canvas || st->cache_bytes != bytes ||
      st->cache_next <= index || index == 0u) {
    gimg_free(st->allocator, st->cache_canvas);
    displaced = st->cache_seq;
    st->cache_canvas = snapshot;
    st->cache_bytes = bytes;
    st->cache_stride = stride;
    st->cache_next = index + 1u;
    st->cache_prev_key = key;
    st->cache_seq = seq;
    taken = seq != NULL;
    snapshot = NULL;
  }
  GCU_MUTEX_UNLOCK(st->cache_lock);
  gimg_free(st->allocator, snapshot);
  gimg_vp8_seq_destroy(st->allocator, displaced);
  return taken;
}

GIMG_Result gimg_webp_decode_animation_frame(const gimg_webp_doc_state_t * st,
    size_t index, GIMG_Raster ** out_raster) {
  if (!st || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  if (index >= st->frame_count) {
    return GIMG_ERR_UNSUPPORTED;
  }

  const uint32_t canvas_w = st->canvas_width;
  const uint32_t canvas_h = st->canvas_height;
  GIMG_Raster * canvas = NULL;
  gimg_vp8_seq * seq = NULL;
  GIMG_Result r = gimg_raster_create_with_allocator(st->allocator, canvas_w,
      canvas_h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &canvas);
  if (r != GIMG_OK) {
    return r;
  }
  uint8_t * canvas_px = (uint8_t *)gimg_raster_pixels(canvas);
  const size_t canvas_stride = gimg_raster_stride_bytes(canvas);
  memset(canvas_px, 0, canvas_stride * (size_t)canvas_h);

  // The cache is the one mutable thing in the document state, and decode is
  // handed it as const.  `codec_private` is a plain `void *`, so the const is
  // on this view of the state and not on the state, and the lock is what makes
  // writing through it safe.  A one-frame file has nobody to hand a head start
  // to.
  gimg_webp_doc_state_t * cache_st =
      (st->frame_count > 1u && st->cache_lock_ready)
      ? (gimg_webp_doc_state_t *)st
      : NULL;
  const size_t canvas_bytes = canvas_stride * (size_t)canvas_h;
  int prev_was_key_frame = 0;
  size_t start = 0;
  if (cache_st) {
    GCU_MUTEX_LOCK(cache_st->cache_lock);
    if (cache_st->cache_canvas && cache_st->cache_next <= index &&
        cache_st->cache_bytes == canvas_bytes &&
        cache_st->cache_stride == canvas_stride) {
      const int fresh = webp_frame_starts_fresh(st, cache_st->cache_next);
      // A lossy inter frame continues from the sequence state the frame before
      // it left, which the cache holds when that frame was the one it was
      // taken at.  Another decode may have the state out at the moment (it is
      // lent, not copied), and then this one replays, which is slower and
      // gives the same pixels.
      if (fresh || cache_st->cache_seq) {
        memcpy(canvas_px, cache_st->cache_canvas, canvas_bytes);
        start = cache_st->cache_next;
        prev_was_key_frame = cache_st->cache_prev_key;
        if (!fresh) {
          seq = cache_st->cache_seq;
          cache_st->cache_seq = NULL;
        }
      }
    }
    GCU_MUTEX_UNLOCK(cache_st->cache_lock);
  }
  if (!seq) {
    r = gimg_vp8_seq_create(st->allocator, &seq);
    if (r != GIMG_OK) {
      gimg_raster_destroy(canvas);
      return r;
    }
  }
  int last_key = 0;
  for (size_t i = start; i <= index; ++i) {
    const gimg_webp_frame_t * fr = &st->frames[i];
    const int key = webp_is_key_frame(st, i, prev_was_key_frame);
    if (key) {
      memset(canvas_px, 0, canvas_stride * (size_t)canvas_h);
    }
    else if (i > 0u) {
      const gimg_webp_frame_t * prev = &st->frames[i - 1u];
      if (prev->dispose_background) {
        webp_clear_rect(canvas_px, canvas_stride, canvas_w, canvas_h, prev->x,
            prev->y, prev->width, prev->height);
      }
    }
    if ((uint64_t)fr->x + fr->width > canvas_w ||
        (uint64_t)fr->y + fr->height > canvas_h) {
      gimg_vp8_seq_destroy(st->allocator, seq);
      gimg_raster_destroy(canvas);
      return GIMG_ERR_CORRUPT;
    }
    GIMG_Raster * frame_ras = NULL;
    // Keep this frame's picture as a reference only when the frame after it
    // will read it.  The last frame asked for is kept for the cache's sake.
    gimg_vp8_seq_keep_refs(
        seq, (i < index || cache_st) && webp_frame_is_vp8_inter(st, i + 1u));
    r = webp_decode_one_frame(st, fr, seq, &frame_ras);
    if (r != GIMG_OK) {
      gimg_vp8_seq_destroy(st->allocator, seq);
      gimg_raster_destroy(canvas);
      return r;
    }
    // Frame bitstream size must match the ANMF rectangle; otherwise the blit
    // would read past the decoded buffer or leave an incomplete patch.
    if (gimg_raster_width(frame_ras) != fr->width ||
        gimg_raster_height(frame_ras) != fr->height) {
      gimg_raster_destroy(frame_ras);
      gimg_vp8_seq_destroy(st->allocator, seq);
      gimg_raster_destroy(canvas);
      return GIMG_ERR_CORRUPT;
    }
    // Keyframes and ANMF "do not blend" replace the rectangle. Otherwise
    // alpha-blend over the (possibly disposed) canvas, matching libwebp's
    // BlendPixelNonPremult path. Keyframe must not OVER onto clear: the
    // integer blend scale rounds RGB by ±1 against the bitstream.
    const int replace = key || fr->blend_source;
    webp_blit_frame(canvas_px, canvas_stride, canvas_w, canvas_h,
        (const uint8_t *)gimg_raster_pixels(frame_ras),
        gimg_raster_stride_bytes(frame_ras), fr->x, fr->y, fr->width,
        fr->height, replace);
    gimg_raster_destroy(frame_ras);
    prev_was_key_frame = key;
    last_key = key;
  }

  if (!cache_st ||
      !webp_cache_store(cache_st, index, canvas_px, canvas_stride,
          canvas_bytes, last_key,
          webp_frame_is_vp8_inter(st, index + 1u) ? seq : NULL)) {
    gimg_vp8_seq_destroy(st->allocator, seq);
  }
  *out_raster = canvas;
  return GIMG_OK;
}
