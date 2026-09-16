/**
 * @file
 *
 * PNG document load: signature, IHDR/PLTE/tRNS/IDAT/IEND, build doc with
 * codec-private state for decode.
 *
 * Copyright 2026 by Corey Pennycuff
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
  r = gimg_png_parse_ihdr(ihdr_buf, &state->ihdr);
  if (r != GIMG_OK) {
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
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // acTL must appear before first IDAT.
      }
      if (actl_seen) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // Duplicate acTL.
      }
      if (length != GIMG_PNG_acTL_LEN) {
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
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // Out-of-order sequence.
      }
      next_sequence++;
      if (num_fcTL_seen == 0 && !seen_idat) {
        // fcTL(0) before IDAT: default image is first frame.
        if (fctl.width != state->ihdr.width ||
            fctl.height != state->ihdr.height || fctl.x_offset != 0 ||
            fctl.y_offset != 0) {
          gimg_png_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        fcTL_before_first_idat = 1;
      }
      if (num_fcTL_seen >= state->frame_count) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // More fcTL than acTL num_frames.
      }
      state->frames[num_fcTL_seen].fctl = fctl;
      num_fcTL_seen++;
      continue;
    }

    if (type == GIMG_PNG_fdAT) {
      if (!state->is_apng) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // fdAT only in APNG.
      }
      if (num_fcTL_seen == 0) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;  // fdAT must follow an fcTL.
      }
      if (length < GIMG_PNG_fdAT_SEQ_LEN) {
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
      if (state->ihdr.color_type != 3) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // PLTE only for palette.
      }
      if (seen_idat || have_plte) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // PLTE before IDAT, single PLTE.
      }
      if (length % 3 != 0 || length == 0 ||
          length > (uint32_t)(GIMG_PNG_PLTE_MAX_ENTRIES * 3u)) {
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
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // tRNS before IDAT, single tRNS.
      }
      if (state->ihdr.color_type == 3 && !have_plte) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT; // For palette, PLTE before tRNS.
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
      gimg_png_free_doc_state(codec, state);
      return GIMG_ERR_FORMAT;
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
      gimg_png_free_doc_state(codec, state);
      return GIMG_ERR_FORMAT;
    }
    for (size_t i = 0; i < state->frame_count; i++) {
      if (!state->frames[i].data || state->frames[i].data_size == 0) {
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

  // Populate meta_common description from first tEXt/zTXt/iTXt with keyword
  // "Description" or "Comment".
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
