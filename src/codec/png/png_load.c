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
 * PNG document load: signature, IHDR/PLTE/tRNS/IDAT/IEND, build doc with
 * codec-private state for decode.
 *
 * --- Internal algorithms and design ---
 *
 * Chunk order enforcement: We require the first chunk after the signature to
 * be IHDR. For palette images (color_type 3), PLTE must appear before IDAT,
 * and tRNS (if present) after PLTE and before IDAT. IDAT chunks may appear
 * multiple times; we concatenate their payloads. Any other critical chunk
 * (unknown or duplicate IHDR) is rejected. Ancillary chunks are accepted in
 * any allowed position and stored in read order so save can round-trip them
 * in the same order when metadata policy permits.
 *
 * Ancillary storage: Each ancillary chunk (tEXt, zTXt, iTXt, iCCP, sRGB, gAMA,
 * cHRM, eXIf, or unknown 4-byte type) is appended to state->ancillary with a
 * copy of its payload. Color interpretation (sRGB/iCCP/gAMA) is not applied
 * during load; it is applied at decode time in png_decode.c so that the
 * raster gets the correct GIMG_Color_Info. eXIf is also attached to doc
 * meta_raw for round-trip (first eXIf only).
 *
 * Limits: GIMG_Load_Options.limits (e.g. max_chunk_size) is passed to
 * gimg_png_read_chunk_payload_and_crc() to reject oversized chunks (bomb
 * protection). For APNG, limits->max_frame_count caps the number of frames
 * we accept (acTL num_frames is validated against actual fcTL/fdAT count).
 *
 * APNG: When acTL is present we treat the stream as APNG. We require fcTL
 * before each frame's data (IDAT for frame 0 or fdAT for any frame). fdAT
 * payloads (4-byte sequence number + DEFLATE data) are concatenated per
 * frame. Frame count is validated and capped by max_frame_count; doc items
 * and frame timing (delay_num/den, dispose_op, blend_op) are filled from
 * fcTL for each frame.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../meta/exif_internal.h"
#include "../codec_internal.h"
#include "../../core/resolution_internal.h"
#include "png_internal.h"

/** Append diagnostic on load error (codec "png", offset, chunk type). */
static void png_load_diag(GIMG_Diagnostics * d, size_t offset,
    gimg_png_chunk_type_t type, GIMG_Result r, const char * action) {
  if (!d) {
    return;
  }
  (void)gimg_diagnostics_append(
      d, "png", offset, (uint32_t)type, GIMG_DIAG_ERROR, action);
  (void)r;
}

/** Chunk is critical if type has bit 5 of first byte = 0 (uppercase). */
static bool gimg_png_chunk_is_critical(gimg_png_chunk_type_t type) {
  return (type & 0x20000000u) == 0;
}

#define ANCILLARY_INITIAL_CAP 8

GIMG_Result gimg_png_append_ancillary(gimg_png_doc_state_t * state,
    gimg_png_chunk_type_t type, const unsigned char * payload,
    size_t payload_size) {
  if (!state) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);
  if (state->ancillary_count >= state->ancillary_capacity) {
    size_t new_cap = state->ancillary_capacity ? state->ancillary_capacity * 2
                                               : ANCILLARY_INITIAL_CAP;
    gimg_png_ancillary_t * new_arr = (gimg_png_ancillary_t *)gimg_realloc(
        alloc, state->ancillary, new_cap * sizeof(gimg_png_ancillary_t));
    if (!new_arr) {
      return GIMG_ERR_OOM;
    }
    state->ancillary = new_arr;
    state->ancillary_capacity = new_cap;
  }
  unsigned char * copy = NULL;
  if (payload_size > 0 && payload) {
    copy = (unsigned char *)gimg_malloc(alloc, payload_size);
    if (!copy) {
      return GIMG_ERR_OOM;
    }
    memcpy(copy, payload, payload_size);
  }
  state->ancillary[state->ancillary_count].type = type;
  state->ancillary[state->ancillary_count].payload = copy;
  state->ancillary[state->ancillary_count].payload_size = payload_size;
  state->ancillary_count++;
  return GIMG_OK;
}

void gimg_png_free_doc_state(GIMG_Codec * codec, void * codec_private) {
  (void)codec;
  gimg_png_doc_state_t * state = (gimg_png_doc_state_t *)codec_private;
  if (!state) {
    return;
  }
  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);
  for (size_t i = 0; i < state->ancillary_count; i++) {
    gimg_free(alloc, state->ancillary[i].payload);
  }
  gimg_free(alloc, state->ancillary);
  gimg_free(alloc, state->plte);
  gimg_free(alloc, state->trns);
  gimg_free(alloc, state->idat);
  if (state->frames) {
    for (size_t i = 0; i < state->frame_count; i++) {
      gimg_free(alloc, state->frames[i].data);
    }
    gimg_free(alloc, state->frames);
  }
  gimg_free(alloc, state);
}

/** Append bytes to idat buffer; state->idat may be reallocated. */
static GIMG_Result gimg_png_append_idat(
    gimg_png_doc_state_t * state, const unsigned char * data, size_t len) {
  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);
  size_t new_size = state->idat_size + len;
  unsigned char * new_buf =
      (unsigned char *)gimg_realloc(alloc, state->idat, new_size);
  if (!new_buf) {
    return GIMG_ERR_OOM;
  }
  state->idat = new_buf;
  memcpy(state->idat + state->idat_size, data, len);
  state->idat_size = new_size;
  return GIMG_OK;
}

GIMG_Result gimg_png_append_frame_data(gimg_png_doc_state_t * state,
    size_t frame_index, const unsigned char * data, size_t len) {
  if (!state || !state->frames || frame_index >= state->frame_count) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);
  gimg_png_frame_t * f = &state->frames[frame_index];
  size_t new_size = f->data_size + len;
  unsigned char * new_buf =
      (unsigned char *)gimg_realloc(alloc, f->data, new_size);
  if (!new_buf) {
    return GIMG_ERR_OOM;
  }
  f->data = new_buf;
  if (len > 0 && data) {
    memcpy(f->data + f->data_size, data, len);
  }
  f->data_size = new_size;
  return GIMG_OK;
}

GIMG_Result gimg_png_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;

  GIMG_Result r = gimg_png_verify_signature(stream);
  if (r != GIMG_OK) {
    return r;
  }

  const GIMG_Allocator * alloc = codec->allocator;
  alloc = gimg_alloc_or_default(alloc);
  const GIMG_Limits * limits = options ? options->limits : NULL;

  // First chunk must be IHDR (chunk starts at offset 8 after signature).
  uint32_t length = 0;
  gimg_png_chunk_type_t type = 0;
  r = gimg_png_read_chunk_header(stream, &length, &type);
  if (r != GIMG_OK) {
    if (diagnostics) {
      (void)gimg_diagnostics_append(diagnostics, "png", 8u, 0u, GIMG_DIAG_ERROR,
          "truncated or invalid chunk header");
    }
    return r;
  }
  if (type != GIMG_PNG_IHDR || length != GIMG_PNG_IHDR_LEN) {
    png_load_diag(
        diagnostics, 8u, type, GIMG_ERR_FORMAT, "first chunk must be IHDR");
    return GIMG_ERR_FORMAT;
  }
  unsigned char ihdr_buf[GIMG_PNG_IHDR_LEN];
  r = gimg_png_read_chunk_payload_and_crc(
      stream, length, type, ihdr_buf, limits, alloc);
  if (r != GIMG_OK) {
    png_load_diag(diagnostics, 8u, type, r,
        r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                            : "bad CRC or truncated");
    return r;
  }
  gimg_png_doc_state_t * state =
      (gimg_png_doc_state_t *)gimg_malloc(alloc, sizeof(gimg_png_doc_state_t));
  if (!state) {
    return GIMG_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));
  state->allocator = alloc;
  const char * ihdr_why = NULL;
  r = gimg_png_parse_ihdr(ihdr_buf, &state->ihdr, &ihdr_why);
  if (r != GIMG_OK) {
    png_load_diag(diagnostics, 8u, GIMG_PNG_IHDR, r,
        ihdr_why ? ihdr_why : "IHDR could not be read");
    gimg_free(alloc, state);
    return r;
  }

  bool have_plte = false;
  bool have_trns = false;
  int seen_idat = 0;
  // APNG state (only meaningful when state->is_apng).
  int actl_seen = 0;
  int fcTL_before_first_idat =
      0;  // 1 if frame 0 uses IDAT (fcTL(0) before IDAT).
  uint32_t next_sequence = 0;
  size_t num_fcTL_seen = 0;

  for (;;) {
    r = gimg_png_read_chunk_header(stream, &length, &type);
    if (r != GIMG_OK) {
      if (diagnostics) {
        size_t pos = gimg_stream_tell(stream);
        (void)gimg_diagnostics_append(diagnostics, "png",
            pos >= 8 ? pos - 8 : 0, 0u, GIMG_DIAG_ERROR,
            "truncated or invalid chunk header");
      }
      gimg_png_free_doc_state(codec, state);
      return r;
    }
    size_t chunk_start = gimg_stream_tell(stream) - 8;

    // A chunk header is a claim, not a fact, and every branch below buffers
    // the payload whole before it has read a byte of it.  PNG's length field
    // is four bytes, so twelve bytes of input can ask for a two-gigabyte
    // allocation - which this loader used to make, and only then discover the
    // file was 97 bytes long.  The fuzzer found it in both the load and the
    // encode harness.
    //
    // The bound is the caller's max_chunk_size when they set one; otherwise it
    // is what is left of the stream, because a chunk cannot be longer than the
    // file that contains it, and no file that used to load is refused by that.
    // Only a stream that does not know its own length falls back to a fixed
    // figure, and there the point is simply that it be finite.
    {
      size_t max_payload;
      if (limits && limits->max_chunk_size != 0) {
        max_payload = (size_t)limits->max_chunk_size;
      }
      else {
        size_t total = gimg_stream_size(stream);
        size_t pos = gimg_stream_tell(stream);
        max_payload = (total != (size_t)-1 && pos != (size_t)-1 && total >= pos)
            ? (total - pos)
            : (size_t)GIMG_PNG_DEFAULT_MAX_CHUNK_PAYLOAD;
      }
      if ((size_t)length > max_payload) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_LIMIT,
            "chunk declares more data than there is; increase max_chunk_size "
            "if this is deliberate");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_LIMIT;
      }
    }

    if (type == GIMG_PNG_IHDR) {
      gimg_png_free_doc_state(codec, state);
      return GIMG_ERR_FORMAT; // Duplicate IHDR.
    }

    if (type == GIMG_PNG_IEND) {
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, NULL, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      break;
    }

    if (type == GIMG_PNG_acTL) {
      if (seen_idat) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "acTL must come before the first IDAT (APNG 4.1)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // acTL must appear before first IDAT.
      }
      if (actl_seen) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "a second acTL; an APNG declares its animation once (APNG 4.1)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // Duplicate acTL.
      }
      if (length != GIMG_PNG_acTL_LEN) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "acTL is eight bytes (APNG 4.1)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      unsigned char actl_buf[GIMG_PNG_acTL_LEN];
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, actl_buf, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      uint32_t num_frames = 0;
      uint32_t num_plays = 0;
      r = gimg_png_parse_actl(actl_buf, &num_frames, &num_plays);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      if (limits && limits->max_frame_count != 0 &&
          num_frames > limits->max_frame_count) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_LIMIT,
            "increase max_frame_count");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_LIMIT;
      }
      state->is_apng = 1;
      state->frame_count = (size_t)num_frames;
      state->num_plays = num_plays;
      state->frames = (gimg_png_frame_t *)gimg_calloc(
          alloc, num_frames, sizeof(gimg_png_frame_t));
      if (!state->frames) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      actl_seen = 1;
      continue;
    }

    if (type == GIMG_PNG_fcTL) {
      if (!state->is_apng || length != GIMG_PNG_fcTL_LEN) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "fcTL is twenty-six bytes and appears only in an APNG (APNG 4.2)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      unsigned char fctl_buf[GIMG_PNG_fcTL_LEN];
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, fctl_buf, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      gimg_png_fctl_t fctl;
      r = gimg_png_parse_fctl(fctl_buf, &fctl);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      if (fctl.sequence_number != next_sequence) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "fcTL sequence number out of order (APNG 4.2)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // Out-of-order sequence.
      }
      next_sequence++;
      // The APNG specification requires a frame to lie inside the canvas the
      // IHDR describes: width and height are greater than zero, x_offset +
      // width is at most the image width, and likewise for the height. It is
      // not an advisory constraint - compositing writes the frame into the
      // canvas at that offset, so a frame declared past the edge is a write
      // past the end of the canvas buffer. Pillow refuses such a file
      // ("APNG contains invalid frames").
      //
      // The sums are done in 64 bits: both terms are 32-bit and either can be
      // near the top of the range, so checking them after the addition would
      // be checking a value that had already wrapped.
      if (fctl.width == 0 || fctl.height == 0 ||
          (uint64_t)fctl.x_offset + (uint64_t)fctl.width >
              (uint64_t)state->ihdr.width ||
          (uint64_t)fctl.y_offset + (uint64_t)fctl.height >
              (uint64_t)state->ihdr.height) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "the frame rectangle has no area or lies outside the canvas (APNG"
            "4.2)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      if (num_fcTL_seen == 0 && !seen_idat) {
        // fcTL(0) before IDAT: default image is first frame.
        if (fctl.width != state->ihdr.width ||
            fctl.height != state->ihdr.height || fctl.x_offset != 0 ||
            fctl.y_offset != 0) {
          png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
              "an fcTL before the first IDAT describes the default image, so it"
              "must cover the whole canvas (APNG 4.2)");
          gimg_png_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        fcTL_before_first_idat = 1;
      }
      if (num_fcTL_seen >= state->frame_count) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "more fcTL chunks than the acTL declared frames (APNG 4.1)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // More fcTL than acTL num_frames.
      }
      state->frames[num_fcTL_seen].fctl = fctl;
      num_fcTL_seen++;
      continue;
    }

    if (type == GIMG_PNG_fdAT) {
      if (!state->is_apng) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "fdAT appears only in an APNG (APNG 4.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // fdAT only in APNG.
      }
      if (num_fcTL_seen == 0) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "fdAT before any fcTL (APNG 4.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // fdAT must follow an fcTL.
      }
      if (length < GIMG_PNG_fdAT_SEQ_LEN) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "fdAT is shorter than the four-byte sequence number it must carry"
            "(APNG 4.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      unsigned char * fdat_buf = (unsigned char *)gimg_malloc(alloc, length);
      if (!fdat_buf) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, fdat_buf, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_free(alloc, fdat_buf);
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      uint32_t seq = (uint32_t)fdat_buf[0] << 24 | (uint32_t)fdat_buf[1] << 16 |
          (uint32_t)fdat_buf[2] << 8 | (uint32_t)fdat_buf[3];
      if (seq != next_sequence) {
        gimg_free(alloc, fdat_buf);
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "fdAT sequence number out of order (APNG 4.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      next_sequence++;
      r = gimg_png_append_frame_data(state, num_fcTL_seen - 1,
          fdat_buf + GIMG_PNG_fdAT_SEQ_LEN, length - GIMG_PNG_fdAT_SEQ_LEN);
      gimg_free(alloc, fdat_buf);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      continue;
    }

    if (type == GIMG_PNG_PLTE) {
      // PNG 11.2.2: PLTE is required for color type 3 and forbidden for 0 and
      // 4, but it *may* appear for 2 and 6, where it is a suggested palette for
      // a viewer that cannot show truecolor. A decoder that can show
      // truecolor ignores it; rejecting the file is not one of the choices the
      // spec offers.
      if (state->ihdr.color_type == 0 || state->ihdr.color_type == 4) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "PLTE shall not appear for a grayscale image (PNG 11.2.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // PLTE shall not appear for 0 or 4.
      }
      if (state->ihdr.color_type != 3) {
        state->plte_is_suggested = 1;
      }
      if (seen_idat || have_plte) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "PLTE must appear once, before the first IDAT (PNG 5.6)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // PLTE before IDAT, single PLTE.
      }
      if (length % 3 != 0 || length == 0 ||
          length > (uint32_t)(GIMG_PNG_PLTE_MAX_ENTRIES * 3u)) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "PLTE length must be a non-zero multiple of three, at most 256"
            "entries (PNG 11.2.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      state->plte = (unsigned char *)gimg_malloc(alloc, length);
      if (!state->plte) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      state->plte_size = length;
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, state->plte, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      have_plte = true;
      continue;
    }

    if (type == GIMG_PNG_tRNS) {
      if (seen_idat || have_trns) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "tRNS must appear once, before the first IDAT (PNG 5.6)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // tRNS before IDAT, single tRNS.
      }
      if (state->ihdr.color_type == 3 && !have_plte) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "tRNS for a palette image must follow its PLTE (PNG 11.3.2.1)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // For palette, PLTE before tRNS.
      }
      // PNG 11.3.2.1 fixes the length for every color type it allows, and
      // forbids the chunk outright for the two that carry an alpha channel of
      // their own. A tRNS of the wrong length is not a tRNS whose meaning can
      // be guessed at: for color type 2 it is three 16-bit samples or it is
      // nothing, and reading a shorter one as a color means reading past it.
      switch (state->ihdr.color_type) {
      case 0:
        if (length != 2u) {
          png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
              "tRNS for a grayscale image is one 16-bit sample (PNG 11.3.2.1)");
          gimg_png_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT; // one gray level
        }
        break;
      case 2:
        if (length != 6u) {
          png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
              "tRNS for a truecolour image is three 16-bit samples (PNG 11.3.2.1)");
          gimg_png_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT; // three 16-bit samples
        }
        break;
      case 3:
        // "shall not contain more values than there are palette entries"
        if (length == 0 || length > state->plte_size / 3u) {
          png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
              "tRNS shall not name more entries than the palette has (PNG 11.3.2.1)");
          gimg_png_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        break;
      default:
        // Color types 4 and 6 already have alpha; 11.3.2.1 says tRNS "shall
        // not appear" for them, and a decoder that kept it would have two
        // sources of transparency and no rule for which wins.
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "tRNS shall not appear for a colour type that already carries alpha"
            "(PNG 11.3.2.1)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      state->trns = (unsigned char *)gimg_malloc(alloc, length);
      if (!state->trns) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      state->trns_size = length;
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, state->trns, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      have_trns = true;
      continue;
    }

    if (type == GIMG_PNG_IDAT) {
      if (state->ihdr.color_type == 3 && !have_plte) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "a palette image needs its PLTE before the first IDAT (PNG 11.2.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // Palette requires PLTE before IDAT.
      }
      if (length > 0) {
        unsigned char * buf = (unsigned char *)gimg_malloc(alloc, length);
        if (!buf) {
          gimg_png_free_doc_state(codec, state);
          return GIMG_ERR_OOM;
        }
        r = gimg_png_read_chunk_payload_and_crc(
            stream, length, type, buf, limits, alloc);
        if (r != GIMG_OK) {
          png_load_diag(diagnostics, chunk_start, type, r,
              r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                  : "bad CRC or truncated");
          gimg_free(alloc, buf);
          gimg_png_free_doc_state(codec, state);
          return r;
        }
        if (state->is_apng && fcTL_before_first_idat &&
            state->frame_count > 0) {
          r = gimg_png_append_frame_data(state, 0, buf, length);
        }
        else {
          r = gimg_png_append_idat(state, buf, length);
        }
        gimg_free(alloc, buf);
        if (r != GIMG_OK) {
          gimg_png_free_doc_state(codec, state);
          return r;
        }
      }
      else {
        r = gimg_png_read_chunk_payload_and_crc(
            stream, length, type, NULL, limits, alloc);
        if (r != GIMG_OK) {
          png_load_diag(diagnostics, chunk_start, type, r,
              r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                  : "bad CRC or truncated");
          gimg_png_free_doc_state(codec, state);
          return r;
        }
      }
      seen_idat = 1;
      continue;
    }

    // Other critical chunk: invalid.
    if (gimg_png_chunk_is_critical(type)) {
      png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
          "unknown critical chunk; a decoder may not skip one (PNG 5.4)");
      gimg_png_free_doc_state(codec, state);
      return GIMG_ERR_FORMAT;
    }

    // Several ancillary chunks have a fixed length, and one has a fixed set of
    // values. A chunk of the wrong length is not a chunk whose meaning can be
    // recovered - there is nothing to interpret it as - and keeping it would
    // mean writing it back out into a file that is then malformed in the same
    // way. Only the types whose length the spec actually fixes are checked
    // here; the text chunks, eXIf, sPLT and anything unknown are variable by
    // design and pass through as before.
    {
      size_t want = 0;
      switch (type) {
      case GIMG_PNG_gAMA:
        want = 4u; // 11.3.2.2: one 4-byte gamma
        break;
      case GIMG_PNG_cHRM:
        want = 32u; // 11.3.2.1: eight 4-byte values
        break;
      case GIMG_PNG_sRGB:
        want = 1u; // 11.3.2.5: one rendering intent
        break;
      case GIMG_PNG_pHYs:
        want = 9u; // 11.3.4.3: two 4-byte units and a unit specifier
        break;
      case GIMG_PNG_tIME:
        want = 7u; // 11.3.5: year, month, day, hour, minute, second
        break;
      case GIMG_PNG_cICP:
        want = GIMG_PNG_cICP_LEN; // four code points
        break;
      default:
        break;
      }
      if (want != 0 && (size_t)length != want) {
        png_load_diag(diagnostics, chunk_start, type, GIMG_ERR_FORMAT,
            "chunk length is not the length this chunk type is defined to have");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
    }

    // Ancillary: parse and store (tEXt, zTXt, iTXt, iCCP, sRGB, gAMA, cHRM,
    // eXIf, and any unknown ancillary) in read order for round-trip.
    if (length > 0) {
      unsigned char * payload_buf =
          (unsigned char *)gimg_malloc(alloc, (size_t)length);
      if (!payload_buf) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, payload_buf, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_free(alloc, payload_buf);
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      r = gimg_png_append_ancillary(state, type, payload_buf, (size_t)length);
      gimg_free(alloc, payload_buf);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
    }
    else {
      r = gimg_png_read_chunk_payload_and_crc(
          stream, length, type, NULL, limits, alloc);
      if (r != GIMG_OK) {
        png_load_diag(diagnostics, chunk_start, type, r,
            r == GIMG_ERR_LIMIT ? "increase max_chunk_size"
                                : "bad CRC or truncated");
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      r = gimg_png_append_ancillary(state, type, NULL, 0);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
    }
  }

  // APNG: validate we saw the right number of fcTL and each frame has data.
  if (state->is_apng) {
    if (num_fcTL_seen != state->frame_count) {
      png_load_diag(diagnostics, gimg_stream_tell(stream), GIMG_PNG_acTL,
          GIMG_ERR_FORMAT,
          "fewer fcTL chunks than the acTL declared frames (APNG 4.1)");
      gimg_png_free_doc_state(codec, state);
      return GIMG_ERR_FORMAT;
    }
    for (size_t i = 0; i < state->frame_count; i++) {
      if (!state->frames[i].data || state->frames[i].data_size == 0) {
        png_load_diag(diagnostics, gimg_stream_tell(stream), GIMG_PNG_fcTL,
            GIMG_ERR_FORMAT,
            "an APNG frame carries no data (APNG 4.3)");
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
    }
  }

  // Build document.
  size_t item_count = state->is_apng ? state->frame_count : 1;
  GIMG_Doc * doc = (GIMG_Doc *)gimg_malloc(alloc, sizeof(GIMG_Doc));
  if (!doc) {
    gimg_png_free_doc_state(codec, state);
    return GIMG_ERR_OOM;
  }
  // Zero the whole structure: the fields below are assigned individually,
  // so anything added to GIMG_Doc later would otherwise start as whatever
  // malloc returned.
  memset(doc, 0, sizeof(*doc));
  doc->allocator = alloc;
  doc->item_count = item_count;
  doc->items = (GIMG_Item *)gimg_malloc(alloc, item_count * sizeof(GIMG_Item));
  if (!doc->items) {
    gimg_free(alloc, doc);
    gimg_png_free_doc_state(codec, state);
    return GIMG_ERR_OOM;
  }
  doc->loaded_by_codec = codec;
  doc->codec_private = state;
  doc->meta_raw = NULL;
  doc->meta_common = NULL;
  for (size_t i = 0; i < item_count; i++) {
    doc->items[i].index = i;
    doc->items[i].doc = doc;
    doc->items[i].frame_delay_num = 0;
    doc->items[i].frame_delay_den = 0;
    doc->items[i].dispose_op = GIMG_DISPOSE_NONE;
    doc->items[i].blend_op = GIMG_BLEND_SOURCE;
    doc->items[i].raster = NULL;
  }
  if (state->is_apng) {
    // acTL's num_plays, handed to the caller rather than left in the codec's
    // private state.  A still PNG has no acTL and so declares nothing, which
    // is not the same as declaring zero: zero means forever.
    gimg_doc_set_loop_count(doc, state->num_plays);
    for (size_t i = 0; i < state->frame_count; i++) {
      const gimg_png_fctl_t * f = &state->frames[i].fctl;
      doc->items[i].frame_delay_num = f->delay_num;
      doc->items[i].frame_delay_den = f->delay_den;
      doc->items[i].dispose_op = (f->dispose_op == 0) ? GIMG_DISPOSE_NONE
          : (f->dispose_op == 1)                      ? GIMG_DISPOSE_BACKGROUND
                                                      : GIMG_DISPOSE_PREVIOUS;
      doc->items[i].blend_op =
          (f->blend_op == 0) ? GIMG_BLEND_SOURCE : GIMG_BLEND_OVER;
    }
  }

  // Attach eXIf (and other raw metadata) to doc for round-trip; populate meta_common from eXIf.
  for (size_t i = 0; i < state->ancillary_count; i++) {
    if (state->ancillary[i].type == GIMG_PNG_eXIf &&
        state->ancillary[i].payload && state->ancillary[i].payload_size > 0) {
      GIMG_Meta_Raw * raw = NULL;
      r = gimg_doc_ensure_meta_raw(doc, &raw);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
      r = gimg_meta_raw_attach(raw, "png", (uint32_t)GIMG_PNG_eXIf,
          state->ancillary[i].payload, state->ancillary[i].payload_size);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
      GIMG_Orientation orient = GIMG_ORIENTATION_UNKNOWN;
      if (gimg_exif_parse_orientation(state->ancillary[i].payload,
              state->ancillary[i].payload_size, &orient) == GIMG_OK &&
          orient != GIMG_ORIENTATION_UNKNOWN) {
        GIMG_Meta_Common * meta_common = NULL;
        if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
          gimg_meta_common_set_orientation(meta_common, orient);
        }
      }
      break; // First eXIf chunk only per spec.
    }
  }

  // bKGD (11.3.4.1) names a colour to present the image against.  It is
  // reported, not painted: the spec itself says viewers need not use it, and
  // of the decoders real files are authored against only ImageMagick does -
  // Chromium, GdkPixbuf and Pillow ignore it, and Pillow does not so much as
  // read it.  Compositing over a reported colour is one line in a caller;
  // un-compositing a background this library painted is not possible at all.
  //
  // GIF says the same thing as an index into its Global Color Table, and both
  // arrive as RGBA through gimg_doc_background_color(), because by the time a
  // caller has a decoded raster the palette an index referred to is gone.
  for (size_t i = 0; i < state->ancillary_count; i++) {
    if (state->ancillary[i].type != GIMG_PNG_bKGD) {
      continue;
    }
    uint8_t rgba[4];
    if (gimg_png_bkgd_to_rgba(
            (const unsigned char *)state->ancillary[i].payload,
            state->ancillary[i].payload_size, state->ihdr.color_type,
            state->ihdr.bit_depth, state->plte, state->plte_size, rgba)) {
      gimg_doc_set_background_color(doc, rgba);
    }
    break; // 5.6: one bKGD per datastream.
  }

  // Populate meta_common description from first tEXt/zTXt/iTXt with keyword
  // "Description" or "Comment".
  // pHYs (11.3.4.3) states the physical size of a pixel, which is the same
  // thing the common metadata calls dpi and which the JPEG codec already reads
  // out of JFIF. Without this a resolution survived a JPEG round trip and was
  // lost the moment the image became a PNG.
  //
  // Only unit specifier 1 says anything physical. Unit 0 gives an aspect
  // ratio, which is a statement about the shape of a pixel and not its size,
  // and has no dpi to offer.
  for (size_t i = 0; i < state->ancillary_count; i++) {
    if (state->ancillary[i].type != GIMG_PNG_pHYs ||
        state->ancillary[i].payload_size != 9u) {
      continue;
    }
    const unsigned char * p = state->ancillary[i].payload;
    if (p[8] != GIMG_PNG_PHYS_UNIT_METER) {
      // Unit 0 means the two numbers are a pixel aspect ratio and nothing
      // more (11.3.4.3).  That is not a density, so it cannot become a DPI;
      // it used to be dropped here, which lost the only thing such a chunk
      // says.  GIF states the same thing in its Pixel Aspect Ratio byte, and
      // both reach a caller through gimg_doc_pixel_aspect_ratio().
      gimg_doc_set_pixel_aspect_ratio(doc,
          ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
              ((uint32_t)p[2] << 8) | (uint32_t)p[3],
          ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
              ((uint32_t)p[6] << 8) | (uint32_t)p[7]);
      break;
    }
    uint32_t x_ppm = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
        ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    uint32_t y_ppm = ((uint32_t)p[4] << 24) | ((uint32_t)p[5] << 16) |
        ((uint32_t)p[6] << 8) | (uint32_t)p[7];
    GIMG_Meta_Common * meta_common = NULL;
    if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
      gimg_meta_common_set_dpi(meta_common,
          gimg_pixels_per_meter_to_dpi(x_ppm),
          gimg_pixels_per_meter_to_dpi(y_ppm));
    }
    break;
  }

  for (size_t i = 0; i < state->ancillary_count; i++) {
    gimg_png_chunk_type_t t = state->ancillary[i].type;
    if (t != GIMG_PNG_tEXt && t != GIMG_PNG_zTXt && t != GIMG_PNG_iTXt) {
      continue;
    }
    if (!state->ancillary[i].payload || state->ancillary[i].payload_size == 0) {
      continue;
    }
    size_t kw_len = 0;
    char * text = NULL;
    r = gimg_png_text_chunk_decode(t, state->ancillary[i].payload,
        state->ancillary[i].payload_size, alloc, &kw_len, &text);
    if (r != GIMG_OK || !text) {
      continue;
    }
    const unsigned char * kw = state->ancillary[i].payload;
    bool match = false;
    if (kw_len == 11 && memcmp(kw, "Description", 11) == 0) {
      match = true;
    }
    else if (kw_len == 7 && memcmp(kw, "Comment", 7) == 0) {
      match = true;
    }
    if (match) {
      GIMG_Meta_Common * meta_common = NULL;
      if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
        (void)gimg_meta_common_set_description(meta_common, text);
      }
      gimg_free(alloc, text);
      break;
    }
    gimg_free(alloc, text);
  }

  *out_doc = doc;
  return GIMG_OK;
}
