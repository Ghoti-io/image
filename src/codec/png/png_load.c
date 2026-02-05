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
 * protection).
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../codec_internal.h"
#include "png_internal.h"

/** Chunk is critical if type has bit 5 of first byte = 0 (uppercase). */
static int gimg_png_chunk_is_critical(gimg_png_chunk_type_t type) {
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
  gimg_free(alloc, state);
}

/** Append bytes to idat buffer; state->idat may be reallocated. */
static GIMG_Result gimg_png_append_idat(gimg_png_doc_state_t * state,
    const unsigned char * data, size_t len) {
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

GIMG_Result gimg_png_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  (void)diagnostics;
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

  // First chunk must be IHDR.
  uint32_t length = 0;
  gimg_png_chunk_type_t type = 0;
  r = gimg_png_read_chunk_header(stream, &length, &type);
  if (r != GIMG_OK) {
    return r;
  }
  if (type != GIMG_PNG_IHDR || length != GIMG_PNG_IHDR_LEN) {
    return GIMG_ERR_FORMAT;
  }
  unsigned char ihdr_buf[GIMG_PNG_IHDR_LEN];
  r = gimg_png_read_chunk_payload_and_crc(stream, length, type, ihdr_buf,
      limits);
  if (r != GIMG_OK) {
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

  int have_plte = 0;
  int have_trns = 0;
  int seen_idat = 0;

  for (;;) {
    r = gimg_png_read_chunk_header(stream, &length, &type);
    if (r != GIMG_OK) {
      gimg_png_free_doc_state(codec, state);
      return r;
    }

    if (type == GIMG_PNG_IHDR) {
      gimg_png_free_doc_state(codec, state);
      return GIMG_ERR_FORMAT; // Duplicate IHDR.
    }

    if (type == GIMG_PNG_IEND) {
      r = gimg_png_read_chunk_payload_and_crc(stream, length, type, NULL,
          limits);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      break;
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
      if (length % 3 != 0 || length == 0 || length > 256 * 3) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      state->plte = (unsigned char *)gimg_malloc(alloc, length);
      if (!state->plte) {
        gimg_png_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      state->plte_size = length;
      r = gimg_png_read_chunk_payload_and_crc(stream, length, type,
          state->plte, limits);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      have_plte = 1;
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
      r = gimg_png_read_chunk_payload_and_crc(stream, length, type,
          state->trns, limits);
      if (r != GIMG_OK) {
        gimg_png_free_doc_state(codec, state);
        return r;
      }
      have_trns = 1;
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
        r = gimg_png_read_chunk_payload_and_crc(stream, length, type, buf,
            limits);
        if (r != GIMG_OK) {
          gimg_free(alloc, buf);
          gimg_png_free_doc_state(codec, state);
          return r;
        }
        r = gimg_png_append_idat(state, buf, length);
        gimg_free(alloc, buf);
        if (r != GIMG_OK) {
          gimg_png_free_doc_state(codec, state);
          return r;
        }
      }
      else {
        r = gimg_png_read_chunk_payload_and_crc(stream, length, type, NULL,
            limits);
        if (r != GIMG_OK) {
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
      r = gimg_png_read_chunk_payload_and_crc(stream, length, type,
          payload_buf, limits);
      if (r != GIMG_OK) {
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
      r = gimg_png_read_chunk_payload_and_crc(stream, length, type, NULL,
          limits);
      if (r != GIMG_OK) {
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

  // Build document.
  GIMG_Doc * doc = (GIMG_Doc *)gimg_malloc(alloc, sizeof(GIMG_Doc));
  if (!doc) {
    gimg_png_free_doc_state(codec, state);
    return GIMG_ERR_OOM;
  }
  doc->allocator = alloc;
  doc->item_count = 1;
  doc->items = (GIMG_Item *)gimg_malloc(alloc, sizeof(GIMG_Item));
  if (!doc->items) {
    gimg_free(alloc, doc);
    gimg_png_free_doc_state(codec, state);
    return GIMG_ERR_OOM;
  }
  doc->loaded_by_codec = codec;
  doc->codec_private = state;
  doc->meta_raw = NULL;
  doc->items[0].index = 0;
  doc->items[0].doc = doc;

  // Attach eXIf (and other raw metadata) to doc for round-trip.
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
      break; // First eXIf chunk only per spec.
    }
  }

  *out_doc = doc;
  return GIMG_OK;
}
