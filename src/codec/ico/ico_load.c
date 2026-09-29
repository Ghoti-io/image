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
 * ICO/CUR directory load: ICONDIR, entries, item roles, payload kind.
 *
 * Entry 0 is GIMG_ITEM_IMAGE; the rest are GIMG_ITEM_ALTERNATE of subject 0 —
 * the same shape as an OS/2 BA array. Payloads are not decoded here.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../codec_internal.h"
#include "ico_internal.h"

static void ico_load_diag(GIMG_Diagnostics * d, size_t offset,
    GIMG_Diag_Severity severity, const char * action) {
  if (!d) {
    return;
  }
  (void)gimg_diagnostics_append(d, "ico", offset, 0u, severity, action);
}

static uint16_t ico_u16(const unsigned char * p) {
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t ico_u32(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}

/** PNG signature: 89 50 4E 47 0D 0A 1A 0A. */
static int ico_payload_is_png(const unsigned char * bytes, size_t size) {
  static const unsigned char png[8] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  return size >= 8u && memcmp(bytes, png, 8) == 0;
}

void gimg_ico_free_doc_state(GIMG_Codec * codec, void * codec_private) {
  (void)codec;
  gimg_ico_doc_state_t * state = (gimg_ico_doc_state_t *)codec_private;
  if (!state) {
    return;
  }
  const GIMG_Allocator * alloc = state->allocator;
  gimg_free(alloc, state->entries);
  gimg_free(alloc, state->file_bytes);
  gimg_free(alloc, state);
}

GIMG_Result gimg_ico_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;
  (void)options;

  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  size_t file_size = gimg_stream_size(stream);
  if (file_size == GIMG_STREAM_SIZE_UNKNOWN) {
    ico_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR,
        "ICO load requires a sized stream");
    return GIMG_ERR_UNSUPPORTED;
  }
  if (file_size < GIMG_ICO_DIR_SIZE + GIMG_ICO_DIRENTRY_SIZE) {
    ico_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR, "truncated ICONDIR");
    return GIMG_ERR_CORRUPT;
  }

  unsigned char * bytes = (unsigned char *)gimg_malloc(alloc, file_size);
  if (!bytes) {
    return GIMG_ERR_OOM;
  }
  GIMG_Result r = gimg_stream_seek(stream, 0u);
  if (r == GIMG_OK) {
    r = gimg_stream_read_exact(stream, bytes, file_size);
  }
  if (r != GIMG_OK) {
    gimg_free(alloc, bytes);
    ico_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR, "cannot read icon file");
    return r;
  }

  uint16_t type = ico_u16(bytes + 2);
  uint16_t count = ico_u16(bytes + 4);
  if ((type != GIMG_ICO_TYPE_ICON && type != GIMG_ICO_TYPE_CURSOR) ||
      count < 1u || count > GIMG_ICO_MAX_ENTRIES) {
    gimg_free(alloc, bytes);
    ico_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR, "invalid ICONDIR");
    return GIMG_ERR_CORRUPT;
  }

  size_t dir_end =
      (size_t)GIMG_ICO_DIR_SIZE + (size_t)count * GIMG_ICO_DIRENTRY_SIZE;
  if (dir_end > file_size) {
    gimg_free(alloc, bytes);
    ico_load_diag(diagnostics, 0u, GIMG_DIAG_ERROR, "directory past end of file");
    return GIMG_ERR_CORRUPT;
  }

  gimg_ico_entry_t * entries = (gimg_ico_entry_t *)gimg_calloc(
      alloc, count, sizeof(gimg_ico_entry_t));
  if (!entries) {
    gimg_free(alloc, bytes);
    return GIMG_ERR_OOM;
  }

  for (uint16_t i = 0; i < count; i++) {
    const unsigned char * e = bytes + GIMG_ICO_DIR_SIZE + i * GIMG_ICO_DIRENTRY_SIZE;
    gimg_ico_entry_t * ent = &entries[i];
    ent->dir_width = e[0];
    ent->dir_height = e[1];
    ent->hotspot_x = ico_u16(e + 4);
    ent->hotspot_y = ico_u16(e + 6);
    // For ICO, bytes 4-7 are planes and bit count; for CUR they are the hotspot.
    // Planes is usually 1; store bpp from the bit-count field either way so a
    // diagnostic can compare it to the payload. CUR hotspots overwrite below.
    if (type == GIMG_ICO_TYPE_ICON) {
      ent->dir_bpp = ico_u16(e + 6);
      ent->hotspot_x = 0;
      ent->hotspot_y = 0;
    }
    else {
      ent->dir_bpp = 0;
    }
    ent->size = ico_u32(e + 8);
    ent->offset = ico_u32(e + 12);

    if (ent->size == 0u || (size_t)ent->offset > file_size ||
        (size_t)ent->size > file_size - (size_t)ent->offset ||
        (size_t)ent->offset < dir_end) {
      gimg_free(alloc, entries);
      gimg_free(alloc, bytes);
      ico_load_diag(diagnostics,
          (size_t)GIMG_ICO_DIR_SIZE + (size_t)i * GIMG_ICO_DIRENTRY_SIZE,
          GIMG_DIAG_ERROR, "entry offset or size outside the file");
      return GIMG_ERR_CORRUPT;
    }

    ent->kind = ico_payload_is_png(bytes + ent->offset, ent->size)
        ? GIMG_ICO_PAYLOAD_KIND_PNG
        : GIMG_ICO_PAYLOAD_KIND_DIB;

    // Trust the payload; note when the directory disagrees (§5.2).
    if (ent->kind == GIMG_ICO_PAYLOAD_KIND_DIB && ent->size >= 16u) {
      const unsigned char * dib = bytes + ent->offset;
      uint32_t bi_w = ico_u32(dib + 4);
      uint32_t bi_h_raw = ico_u32(dib + 8);
      uint32_t bi_h = (bi_h_raw & 0x80000000u)
          ? (uint32_t)(-(int32_t)bi_h_raw)
          : bi_h_raw;
      if ((bi_h % 2u) == 0u) {
        bi_h /= 2u;
      }
      uint32_t dir_w = gimg_ico_dir_dim(ent->dir_width);
      uint32_t dir_h = gimg_ico_dir_dim(ent->dir_height);
      if (bi_w != dir_w || bi_h != dir_h) {
        ico_load_diag(diagnostics,
            (size_t)ent->offset, GIMG_DIAG_WARNING,
            "directory dimensions disagree with the DIB payload");
      }
    }
  }

  gimg_ico_doc_state_t * state =
      (gimg_ico_doc_state_t *)gimg_calloc(alloc, 1u, sizeof(*state));
  if (!state) {
    gimg_free(alloc, entries);
    gimg_free(alloc, bytes);
    return GIMG_ERR_OOM;
  }
  state->allocator = alloc;
  state->type = type;
  state->entry_count = count;
  state->entries = entries;
  state->file_bytes = bytes;
  state->file_size = file_size;

  GIMG_Doc * doc = NULL;
  r = gimg_doc_create_with_allocator(alloc, &doc);
  if (r == GIMG_OK) {
    r = gimg_doc_set_item_count(doc, count);
  }
  if (r != GIMG_OK) {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    gimg_ico_free_doc_state(codec, state);
    return r;
  }

  for (size_t i = 1; i < count; i++) {
    gimg_item_set_role(gimg_doc_item(doc, i), GIMG_ITEM_ALTERNATE, 0u);
  }
  for (size_t i = 0; i < count; i++) {
    gimg_item_set_hotspot(gimg_doc_item(doc, i), entries[i].hotspot_x,
        entries[i].hotspot_y);
  }

  doc->loaded_by_codec = codec;
  doc->codec_private = state;
  *out_doc = doc;
  return GIMG_OK;
}
