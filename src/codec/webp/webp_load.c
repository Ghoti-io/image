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
 * WebP Phase A load: RIFF walk, VP8X canvas, ICCP/EXIF/XMP carriage.
 * Phase B: decode simple VP8L and VP8X+VP8L (alpha inside VP8L).
 * Phase C/D: VP8 lossy (+ optional ALPH plane).
 * Phase E: ANIM/ANMF items and composited frame decode.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/limits_internal.h"
#include "../../meta/exif_internal.h"
#include "../codec_internal.h"
#include "webp_internal.h"

static void webp_load_diag(GIMG_Diagnostics * d, size_t offset,
    GIMG_Diag_Severity severity, const char * action) {
  if (!d) {
    return;
  }
  (void)gimg_diagnostics_append(d, "webp", offset, 0u, severity, action);
}

static uint32_t webp_u32(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}

static uint32_t webp_u24(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

static GIMG_Result webp_push_chunk(gimg_webp_doc_state_t * st, uint32_t fourcc,
    size_t header_offset, uint32_t size, const GIMG_Limits * limits,
    GIMG_Diagnostics * diagnostics) {
  // A RIFF file is nothing but length-prefixed chunks, which is the one
  // structure max_chunk_size was written for, and this codec did not read it
  // until 2026-10-02: a one-byte cap refused a PNG, a JPEG and a GIF and let
  // every WebP fixture through.  The check lives here rather than in either
  // walk because both walks push through this function - the top-level RIFF
  // loop and the nested one inside an ANMF payload - and a cap enforced in
  // only one of them is a cap an animation steps around.  The bound is the
  // declared payload length, which is what png_chunk.c and the JPEG segment
  // reader compare against as well.
  if (limits && limits->max_chunk_size != 0u &&
      (size_t)size > limits->max_chunk_size) {
    webp_load_diag(diagnostics, header_offset, GIMG_DIAG_ERROR,
        "chunk exceeds max_chunk_size");
    return GIMG_ERR_LIMIT;
  }
  if (st->chunk_count >= GIMG_WEBP_MAX_CHUNKS) {
    webp_load_diag(diagnostics, header_offset, GIMG_DIAG_ERROR,
        "too many chunks");
    return GIMG_ERR_LIMIT;
  }
  st->chunks[st->chunk_count].fourcc = fourcc;
  st->chunks[st->chunk_count].offset = header_offset;
  st->chunks[st->chunk_count].payload_size = size;
  st->chunks[st->chunk_count].size = size + 8u + (size & 1u);
  st->chunk_count++;
  return GIMG_OK;
}

/**
 * List nested bitstream chunks inside an ANMF payload the way webpinfo does:
 * after the 16-byte ANMF header, optional ALPH then VP8 / VP8L.
 * When @a frame is non-NULL, also records payload spans on that frame.
 */
static GIMG_Result webp_walk_anmf_payload(gimg_webp_doc_state_t * st,
    size_t anmf_payload_off, uint32_t anmf_payload_size,
    gimg_webp_frame_t * frame, const GIMG_Limits * limits,
    GIMG_Diagnostics * diagnostics) {
  if (anmf_payload_size < GIMG_WEBP_ANMF_HEADER_SIZE) {
    webp_load_diag(diagnostics, anmf_payload_off, GIMG_DIAG_ERROR,
        "ANMF shorter than its header");
    return GIMG_ERR_CORRUPT;
  }
  if (frame) {
    GIMG_Result hr = gimg_webp_parse_anmf_header(
        st->file_bytes + anmf_payload_off, frame);
    if (hr != GIMG_OK) {
      webp_load_diag(diagnostics, anmf_payload_off, GIMG_DIAG_ERROR,
          "ANMF header rejected");
      return hr;
    }
  }
  size_t off = anmf_payload_off + GIMG_WEBP_ANMF_HEADER_SIZE;
  size_t end = anmf_payload_off + (size_t)anmf_payload_size;
  while (off + 8u <= end) {
    const unsigned char * p = st->file_bytes + off;
    uint32_t fourcc = webp_u32(p);
    uint32_t size = webp_u32(p + 4);
    size_t payload = off + 8u;
    if (size > end - payload) {
      webp_load_diag(diagnostics, off, GIMG_DIAG_ERROR,
          "nested chunk size past ANMF end");
      return GIMG_ERR_CORRUPT;
    }
    if (fourcc != GIMG_WEBP_ALPH && fourcc != GIMG_WEBP_VP8 &&
        fourcc != GIMG_WEBP_VP8L) {
      // Unknown nested fourcc: stop rather than invent inventory webpinfo
      // would not show as a top-level Chunk line.
      break;
    }
    GIMG_Result r =
        webp_push_chunk(st, fourcc, off, size, limits, diagnostics);
    if (r != GIMG_OK) {
      return r;
    }
    if (fourcc == GIMG_WEBP_ALPH) {
      st->has_alpha = 1;
      if (frame) {
        frame->alph_payload_off = payload;
        frame->alph_payload_size = size;
      }
    }
    else if (fourcc == GIMG_WEBP_VP8) {
      st->is_lossy = 1;
      if (frame) {
        frame->vp8_payload_off = payload;
        frame->vp8_payload_size = size;
      }
    }
    else if (fourcc == GIMG_WEBP_VP8L) {
      st->is_lossless = 1;
      if (frame) {
        frame->vp8l_payload_off = payload;
        frame->vp8l_payload_size = size;
      }
      int alpha = 0;
      uint32_t w = 0, h = 0;
      if (gimg_webp_peek_vp8l_dims(
              st->file_bytes + payload, size, &w, &h, &alpha) &&
          alpha) {
        st->has_alpha = 1;
      }
    }
    size_t step = 8u + (size_t)size + ((size & 1u) ? 1u : 0u);
    if (step > end - off) {
      break;
    }
    off += step;
  }
  return GIMG_OK;
}

static GIMG_Result webp_push_frame(gimg_webp_doc_state_t * st,
    const gimg_webp_frame_t * frame, size_t max_frames,
    GIMG_Diagnostics * diagnostics, size_t diag_off) {
  if (st->frame_count >= max_frames) {
    webp_load_diag(diagnostics, diag_off, GIMG_DIAG_ERROR,
        "ANMF count exceeds max_frame_count");
    return GIMG_ERR_LIMIT;
  }
  gimg_webp_frame_t * grown = (gimg_webp_frame_t *)gimg_realloc(st->allocator,
      st->frames, (st->frame_count + 1u) * sizeof(gimg_webp_frame_t));
  if (!grown) {
    return GIMG_ERR_OOM;
  }
  st->frames = grown;
  st->frames[st->frame_count] = *frame;
  st->frame_count++;
  return GIMG_OK;
}

void gimg_webp_free_doc_state(GIMG_Codec * codec, void * codec_private) {
  (void)codec;
  gimg_webp_doc_state_t * state = (gimg_webp_doc_state_t *)codec_private;
  if (!state) {
    return;
  }
  const GIMG_Allocator * alloc = state->allocator;
  gimg_free(alloc, state->chunks);
  gimg_free(alloc, state->frames);
  gimg_free(alloc, state->file_bytes);
  gimg_free(alloc, state);
}

GIMG_Result gimg_webp_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const GIMG_Limits * limits = options ? options->limits : NULL;
  size_t max_frames = GIMG_WEBP_DEFAULT_MAX_FRAMES;
  if (limits && limits->max_frame_count > 0u) {
    max_frames = limits->max_frame_count;
  }
  size_t file_size = gimg_stream_size(stream);
  if (file_size == GIMG_STREAM_SIZE_UNKNOWN) {
    webp_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR,
        "WebP load requires a sized stream");
    return GIMG_ERR_UNSUPPORTED;
  }
  if (file_size < GIMG_WEBP_SIGNATURE_LEN + 8u) {
    webp_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR, "truncated RIFF header");
    return GIMG_ERR_CORRUPT;
  }

  unsigned char * bytes = (unsigned char *)gimg_malloc(alloc, file_size);
  if (!bytes) {
    return GIMG_ERR_OOM;
  }
  GIMG_Result r = gimg_stream_seek(stream, 0u);
  if (r == GIMG_OK) {
    size_t got = 0;
    r = gimg_stream_read(stream, bytes, file_size, &got);
    if (r == GIMG_OK && got != file_size) {
      r = GIMG_ERR_IO;
    }
  }
  if (r != GIMG_OK) {
    gimg_free(alloc, bytes);
    return r;
  }

  if (memcmp(bytes, "RIFF", 4) != 0 || memcmp(bytes + 8, "WEBP", 4) != 0) {
    gimg_free(alloc, bytes);
    webp_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR, "not a WebP RIFF");
    return GIMG_ERR_CORRUPT;
  }
  uint32_t riff_size = webp_u32(bytes + 4);
  // riff_size counts bytes after the size field: form type (4) + chunks.
  // File length must be at least 8 + riff_size (RIFF + size + payload).
  if ((size_t)riff_size + 8u < 12u || (size_t)riff_size + 8u > file_size) {
    gimg_free(alloc, bytes);
    webp_load_diag(diagnostics, 4u, GIMG_DIAG_ERROR, "RIFF size past EOF");
    return GIMG_ERR_CORRUPT;
  }
  size_t riff_end = 8u + (size_t)riff_size;

  gimg_webp_doc_state_t * state =
      (gimg_webp_doc_state_t *)gimg_calloc(alloc, 1u, sizeof(*state));
  if (!state) {
    gimg_free(alloc, bytes);
    return GIMG_ERR_OOM;
  }
  state->allocator = alloc;
  state->file_bytes = bytes;
  state->file_size = file_size;
  state->chunks = (gimg_webp_chunk_t *)gimg_calloc(
      alloc, GIMG_WEBP_MAX_CHUNKS, sizeof(gimg_webp_chunk_t));
  if (!state->chunks) {
    gimg_webp_free_doc_state(codec, state);
    return GIMG_ERR_OOM;
  }

  size_t off = 12u;
  while (off + 8u <= riff_end) {
    const unsigned char * p = bytes + off;
    uint32_t fourcc = webp_u32(p);
    uint32_t size = webp_u32(p + 4);
    size_t payload = off + 8u;
    if (size > riff_end - payload) {
      webp_load_diag(diagnostics, off, GIMG_DIAG_ERROR,
          "chunk size past RIFF end");
      gimg_webp_free_doc_state(codec, state);
      return GIMG_ERR_CORRUPT;
    }
    r = webp_push_chunk(state, fourcc, off, size, limits, diagnostics);
    if (r != GIMG_OK) {
      gimg_webp_free_doc_state(codec, state);
      return r;
    }

    if (fourcc == GIMG_WEBP_VP8X) {
      if (size < 10u) {
        webp_load_diag(diagnostics, payload, GIMG_DIAG_ERROR,
            "VP8X shorter than 10 bytes");
        gimg_webp_free_doc_state(codec, state);
        return GIMG_ERR_CORRUPT;
      }
      state->has_vp8x = 1;
      state->vp8x_flags = bytes[payload];
      state->canvas_width = webp_u24(bytes + payload + 4) + 1u;
      state->canvas_height = webp_u24(bytes + payload + 7) + 1u;
      if (state->vp8x_flags & GIMG_WEBP_VP8X_ALPHA) {
        state->has_alpha = 1;
      }
      if (state->vp8x_flags & GIMG_WEBP_VP8X_ANIMATION) {
        state->is_animation = 1;
      }
    }
    else if (fourcc == GIMG_WEBP_VP8) {
      state->is_lossy = 1;
      if (!state->has_vp8x) {
        uint32_t w = 0, h = 0;
        if (gimg_webp_peek_vp8_dims(bytes + payload, size, &w, &h)) {
          state->canvas_width = w;
          state->canvas_height = h;
        }
      }
    }
    else if (fourcc == GIMG_WEBP_VP8L) {
      state->is_lossless = 1;
      int alpha = 0;
      uint32_t w = 0, h = 0;
      if (gimg_webp_peek_vp8l_dims(bytes + payload, size, &w, &h, &alpha)) {
        if (!state->has_vp8x) {
          state->canvas_width = w;
          state->canvas_height = h;
        }
        if (alpha) {
          state->has_alpha = 1;
        }
      }
    }
    else if (fourcc == GIMG_WEBP_ALPH) {
      state->has_alpha = 1;
    }
    else if (fourcc == GIMG_WEBP_ANIM) {
      state->is_animation = 1;
      if (size < 6u) {
        webp_load_diag(diagnostics, payload, GIMG_DIAG_ERROR,
            "ANIM shorter than 6 bytes");
        gimg_webp_free_doc_state(codec, state);
        return GIMG_ERR_CORRUPT;
      }
      // ANIM stores bgcolor as BGRA; the document background is RGBA.
      state->bgcolor_rgba[0] = bytes[payload + 2];
      state->bgcolor_rgba[1] = bytes[payload + 1];
      state->bgcolor_rgba[2] = bytes[payload + 0];
      state->bgcolor_rgba[3] = bytes[payload + 3];
      state->loop_count = (uint16_t)(bytes[payload + 4] |
          ((uint16_t)bytes[payload + 5] << 8));
      state->has_anim_chunk = 1;
    }
    else if (fourcc == GIMG_WEBP_ANMF) {
      state->is_animation = 1;
      gimg_webp_frame_t frame;
      memset(&frame, 0, sizeof(frame));
      r = webp_walk_anmf_payload(
          state, payload, size, &frame, limits, diagnostics);
      if (r != GIMG_OK) {
        gimg_webp_free_doc_state(codec, state);
        return r;
      }
      if ((uint64_t)frame.x + frame.width > state->canvas_width ||
          (uint64_t)frame.y + frame.height > state->canvas_height) {
        // Canvas may not be known yet if VP8X was missing; reject only when
        // we have dimensions.
        if (state->canvas_width > 0u && state->canvas_height > 0u) {
          webp_load_diag(diagnostics, payload, GIMG_DIAG_ERROR,
              "ANMF rectangle outside canvas");
          gimg_webp_free_doc_state(codec, state);
          return GIMG_ERR_CORRUPT;
        }
      }
      r = webp_push_frame(state, &frame, max_frames, diagnostics, payload);
      if (r != GIMG_OK) {
        gimg_webp_free_doc_state(codec, state);
        return r;
      }
    }
    else if (fourcc == GIMG_WEBP_ICCP || fourcc == GIMG_WEBP_EXIF ||
             fourcc == GIMG_WEBP_XMP) {
      // The one cap every other codec in this library reads, and the one this
      // codec did not until 2026-10-02. ICCP, EXIF and XMP are the chunks
      // max_metadata_size is for, and before this they were bounded only by
      // the length of the file.
      //
      // Dropped rather than refused when merely implausible, which is what
      // PNG, JPEG, BMP, GIF and TIFF all do and for the same reason: an
      // oversized profile or Exif block says nothing about whether the
      // picture decodes. A cap the caller set is different - they asked to be
      // told, so that is GIMG_ERR_LIMIT.
      const gimg_metadata_verdict_t v =
          gimg_metadata_verdict(limits, (size_t)size);
      if (v == GIMG_METADATA_REFUSED) {
        webp_load_diag(diagnostics, payload, GIMG_DIAG_ERROR,
            "metadata chunk exceeds max_metadata_size");
        gimg_webp_free_doc_state(codec, state);
        return GIMG_ERR_LIMIT;
      }
      if (v == GIMG_METADATA_KEEP) {
        if (fourcc == GIMG_WEBP_ICCP) {
          state->iccp = bytes + payload;
          state->iccp_size = size;
        }
        else if (fourcc == GIMG_WEBP_EXIF) {
          state->exif = bytes + payload;
          state->exif_size = size;
        }
        else {
          state->xmp = bytes + payload;
          state->xmp_size = size;
        }
      }
      else {
        webp_load_diag(diagnostics, payload, GIMG_DIAG_WARNING,
            "metadata chunk past the built-in guard; dropped");
      }
    }

    size_t step = 8u + (size_t)size + ((size & 1u) ? 1u : 0u);
    if (step > riff_end - off) {
      // Padding or trailing bytes that do not form another chunk: stop.
      break;
    }
    off += step;
  }

  if (state->canvas_width == 0u || state->canvas_height == 0u) {
    webp_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR,
        "could not determine canvas size");
    gimg_webp_free_doc_state(codec, state);
    return GIMG_ERR_CORRUPT;
  }
  if (options && options->limits &&
      options->limits->max_decoded_pixels > 0u) {
    uint64_t pixels = (uint64_t)state->canvas_width * state->canvas_height;
    if (pixels > options->limits->max_decoded_pixels) {
      webp_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR,
          "canvas exceeds max_decoded_pixels");
      gimg_webp_free_doc_state(codec, state);
      return GIMG_ERR_LIMIT;
    }
  }

  GIMG_Doc * doc = NULL;
  r = gimg_doc_create_with_allocator(alloc, &doc);
  const size_t item_count =
      (state->frame_count > 0u) ? state->frame_count : 1u;
  if (r == GIMG_OK) {
    r = gimg_doc_set_item_count(doc, item_count);
  }
  if (r != GIMG_OK) {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    gimg_webp_free_doc_state(codec, state);
    return r;
  }

  if (state->has_anim_chunk) {
    gimg_doc_set_loop_count(doc, state->loop_count);
    gimg_doc_set_background_color(doc, state->bgcolor_rgba);
  }
  for (size_t i = 0; i < state->frame_count; ++i) {
    const gimg_webp_frame_t * fr = &state->frames[i];
    GIMG_Item * item = gimg_doc_item(doc, i);
    if (!item) {
      gimg_doc_destroy(doc);
      gimg_webp_free_doc_state(codec, state);
      return GIMG_ERR_INTERNAL;
    }
    gimg_item_set_role(item,
        state->frame_count > 1u ? GIMG_ITEM_FRAME : GIMG_ITEM_IMAGE, i);
    {
      // Item delay is uint16/uint16; ANMF duration is 24-bit ms. Cap rather
      // than lose the denominator (1000) when a frame is longer than ~65s.
      const uint16_t num = (fr->duration_ms > 65535u)
          ? (uint16_t)65535u
          : (uint16_t)fr->duration_ms;
      gimg_item_set_frame_delay(item, num, 1000u);
    }
    gimg_item_set_dispose_op(item,
        fr->dispose_background ? GIMG_DISPOSE_BACKGROUND : GIMG_DISPOSE_NONE);
    gimg_item_set_blend_op(
        item, fr->blend_source ? GIMG_BLEND_SOURCE : GIMG_BLEND_OVER);
  }

  if (state->exif && state->exif_size > 0u) {
    GIMG_Meta_Raw * raw = NULL;
    if (gimg_doc_ensure_meta_raw(doc, &raw) == GIMG_OK && raw) {
      (void)gimg_meta_raw_attach(
          raw, "webp", GIMG_WEBP_EXIF, state->exif, state->exif_size);
    }
    // Orientation: payload may start with "Exif\0\0" or be raw TIFF.
    const unsigned char * tiff = state->exif;
    size_t tiff_size = state->exif_size;
    if (tiff_size >= 6u && memcmp(tiff, "Exif\0\0", 6) == 0) {
      tiff += 6;
      tiff_size -= 6u;
    }
    GIMG_Orientation orient = GIMG_ORIENTATION_UNKNOWN;
    if (gimg_exif_parse_orientation(tiff, tiff_size, &orient) == GIMG_OK &&
        orient != GIMG_ORIENTATION_UNKNOWN) {
      GIMG_Meta_Common * common = NULL;
      if (gimg_doc_ensure_meta_common(doc, &common) == GIMG_OK && common) {
        gimg_meta_common_set_orientation(common, orient);
      }
    }
  }
  if (state->xmp && state->xmp_size > 0u) {
    GIMG_Meta_Raw * raw = NULL;
    if (gimg_doc_ensure_meta_raw(doc, &raw) == GIMG_OK && raw) {
      (void)gimg_meta_raw_attach(
          raw, "webp", GIMG_WEBP_XMP, state->xmp, state->xmp_size);
    }
  }
  if (state->iccp && state->iccp_size > 0u) {
    GIMG_Meta_Raw * raw = NULL;
    if (gimg_doc_ensure_meta_raw(doc, &raw) == GIMG_OK && raw) {
      (void)gimg_meta_raw_attach(
          raw, "webp", GIMG_WEBP_ICCP, state->iccp, state->iccp_size);
    }
  }

  doc->loaded_by_codec = codec;
  doc->codec_private = state;
  *out_doc = doc;
  return GIMG_OK;
}

GIMG_Result gimg_webp_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  (void)options;
  if (out_raster) {
    *out_raster = NULL;
  }
  if (!codec || !item || !out_raster || !item->doc) {
    return GIMG_ERR_INTERNAL;
  }

  GIMG_Doc * doc = item->doc;
  if (!doc->codec_private) {
    return GIMG_ERR_INTERNAL;
  }
  const gimg_webp_doc_state_t * st =
      (const gimg_webp_doc_state_t *)doc->codec_private;

  if (st->frame_count > 0u) {
    return gimg_webp_decode_animation_frame(st, item->index, out_raster);
  }

  const gimg_webp_chunk_t * vp8 = NULL;
  const gimg_webp_chunk_t * vp8l = NULL;
  const gimg_webp_chunk_t * alph = NULL;
  for (size_t i = 0; i < st->chunk_count; ++i) {
    const gimg_webp_chunk_t * c = &st->chunks[i];
    if (c->fourcc == GIMG_WEBP_ALPH && !alph) {
      alph = c;
    }
    else if (c->fourcc == GIMG_WEBP_VP8 && !vp8) {
      vp8 = c;
    }
    else if (c->fourcc == GIMG_WEBP_VP8L && !vp8l) {
      vp8l = c;
    }
  }

  const unsigned char * vp8p = NULL;
  size_t vp8_size = 0;
  const unsigned char * vp8lp = NULL;
  size_t vp8l_size = 0;
  const unsigned char * alphp = NULL;
  size_t alph_size = 0;
  if (vp8) {
    const size_t payload_off = vp8->offset + 8u;
    if (payload_off + (size_t)vp8->payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    vp8p = st->file_bytes + payload_off;
    vp8_size = vp8->payload_size;
  }
  if (vp8l) {
    const size_t payload_off = vp8l->offset + 8u;
    if (payload_off + (size_t)vp8l->payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    vp8lp = st->file_bytes + payload_off;
    vp8l_size = vp8l->payload_size;
  }
  if (alph) {
    const size_t alph_off = alph->offset + 8u;
    if (alph_off + (size_t)alph->payload_size > st->file_size) {
      return GIMG_ERR_CORRUPT;
    }
    alphp = st->file_bytes + alph_off;
    alph_size = alph->payload_size;
  }

  // Still image: VP8L alone, or VP8 with optional ALPH. Not both bitstreams.
  if (vp8lp && !vp8p) {
    return gimg_webp_decode_picture(
        NULL, 0u, vp8lp, vp8l_size, NULL, 0u, st->allocator, NULL, out_raster);
  }
  if (vp8p && !vp8lp) {
    return gimg_webp_decode_picture(
        vp8p, vp8_size, NULL, 0u, alphp, alph_size, st->allocator, NULL,
        out_raster);
  }

  return GIMG_ERR_UNSUPPORTED;
}
