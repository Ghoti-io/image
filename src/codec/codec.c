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
 * Codec registry, probe, load/save/decode stubs.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../container/doc_internal.h"
#include "../core/alloc_internal.h"
#include "codec_internal.h"

#define REGISTRY_INITIAL 8

static GIMG_Codec ** gimg_codec_registry = NULL;
static size_t gimg_codec_registry_count = 0;
static size_t gimg_codec_registry_capacity = 0;
// Allocator for the registry array; set from the first registering codec so
// tests and embedders can use a custom allocator for registry storage.
static const GIMG_Allocator * gimg_codec_registry_alloc = NULL;

GIMG_API GIMG_Result gimg_codec_create_stub(const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_Codec ** out_codec) {
  return gimg_codec_create_stub_with_allocator(
      NULL, name, magic_bytes, magic_len, out_codec);
}

GIMG_API GIMG_Result gimg_codec_create_stub_with_allocator(
    const GIMG_Allocator * allocator, const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_Codec ** out_codec) {
  if (!name || !out_codec) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Codec * c = (GIMG_Codec *)gimg_malloc(allocator, sizeof(GIMG_Codec));
  if (!c) {
    return GIMG_ERR_OOM;
  }
  c->allocator = allocator;
  c->name = (char *)gimg_malloc(allocator, strlen(name) + 1);
  if (!c->name) {
    gimg_free(allocator, c);
    return GIMG_ERR_OOM;
  }
  strcpy(c->name, name);
  c->magic_count = (magic_bytes && magic_len > 0) ? 1 : 0;
  c->magics = NULL;
  if (c->magic_count > 0) {
    c->magics = (gimg_codec_magic_t *)gimg_malloc(
        allocator, sizeof(gimg_codec_magic_t));
    if (!c->magics) {
      gimg_free(allocator, c->name);
      gimg_free(allocator, c);
      return GIMG_ERR_OOM;
    }
    unsigned char * copy = (unsigned char *)gimg_malloc(allocator, magic_len);
    if (!copy) {
      gimg_free(allocator, c->magics);
      gimg_free(allocator, c->name);
      gimg_free(allocator, c);
      return GIMG_ERR_OOM;
    }
    memcpy(copy, magic_bytes, magic_len);
    c->magics[0].bytes = copy;
    c->magics[0].length = magic_len;
    c->magics[0].offset = 0;
  }
  c->capabilities = 0;
  c->load_cb = NULL;
  c->save_cb = NULL;
  c->decode_cb = NULL;
  c->free_doc_private = NULL;
  *out_codec = c;
  return GIMG_OK;
}

void gimg_codec_set_free_doc_private(
    GIMG_Codec * codec, gimg_codec_free_doc_private_fn fn) {
  if (codec) {
    codec->free_doc_private = fn;
  }
}

void gimg_codec_set_load_cb(GIMG_Codec * codec, gimg_codec_load_fn fn) {
  if (codec) {
    codec->load_cb = fn;
  }
}

void gimg_codec_set_save_cb(GIMG_Codec * codec, gimg_codec_save_fn fn) {
  if (codec) {
    codec->save_cb = fn;
  }
}

void gimg_codec_set_decode_cb(GIMG_Codec * codec, gimg_codec_decode_fn fn) {
  if (codec) {
    codec->decode_cb = fn;
  }
}

GIMG_API GIMG_Result gimg_codec_register(GIMG_Codec * codec) {
  if (!codec || !codec->name) {
    return GIMG_ERR_INTERNAL;
  }
  for (size_t i = 0; i < gimg_codec_registry_count; i++) {
    if (strcmp(gimg_codec_registry[i]->name, codec->name) == 0) {
      return GIMG_ERR_INTERNAL; // Duplicate
    }
  }
  if (gimg_codec_registry_count >= gimg_codec_registry_capacity) {
    const GIMG_Allocator * alloc = (gimg_codec_registry_alloc != NULL)
        ? gimg_codec_registry_alloc
        : gimg_alloc_or_default(codec->allocator);
    if (gimg_codec_registry_alloc == NULL) {
      gimg_codec_registry_alloc = alloc;
    }
    size_t new_cap = gimg_codec_registry_capacity
        ? gimg_codec_registry_capacity * 2
        : REGISTRY_INITIAL;
    GIMG_Codec ** new_reg = (GIMG_Codec **)gimg_realloc(
        alloc, gimg_codec_registry, new_cap * sizeof(GIMG_Codec *));
    if (!new_reg) {
      return GIMG_ERR_OOM;
    }
    gimg_codec_registry = new_reg;
    gimg_codec_registry_capacity = new_cap;
  }
  gimg_codec_registry[gimg_codec_registry_count++] = codec;
  return GIMG_OK;
}

GIMG_API size_t gimg_codec_count(void) {
  return gimg_codec_registry_count;
}

GIMG_API GIMG_Codec * gimg_codec_by_index(size_t index) {
  if (index >= gimg_codec_registry_count) {
    return NULL;
  }
  return gimg_codec_registry[index];
}

GIMG_API GIMG_Codec * gimg_codec_by_name(const char * name) {
  if (!name) {
    return NULL;
  }
  for (size_t i = 0; i < gimg_codec_registry_count; i++) {
    if (strcmp(gimg_codec_registry[i]->name, name) == 0) {
      return gimg_codec_registry[i];
    }
  }
  return NULL;
}

GIMG_API const char * gimg_codec_name(const GIMG_Codec * codec) {
  return codec ? codec->name : NULL;
}

GIMG_API unsigned int gimg_codec_capabilities(const GIMG_Codec * codec) {
  return codec ? codec->capabilities : 0;
}

GIMG_API GIMG_Result gimg_probe(
    GIMG_Stream * stream, GIMG_Probe_Result * result) {
  if (!stream || !result) {
    return GIMG_ERR_INTERNAL;
  }
  result->format_name = NULL;
  result->confidence = 0;

  unsigned char peek_buf[32];
  size_t peeked = 0;
  GIMG_Result r = gimg_stream_peek(stream, peek_buf, sizeof(peek_buf), &peeked);
  if (r != GIMG_OK || peeked == 0) {
    return GIMG_OK; // No match
  }

  for (size_t c = 0; c < gimg_codec_registry_count; c++) {
    GIMG_Codec * codec = gimg_codec_registry[c];
    for (size_t m = 0; m < codec->magic_count; m++) {
      gimg_codec_magic_t * mag = &codec->magics[m];
      if (mag->length == 0 || mag->bytes == NULL) {
        continue;
      }
      size_t need = mag->offset + mag->length;
      if (peeked < need) {
        continue;
      }
      if (memcmp(peek_buf + mag->offset, mag->bytes, mag->length) == 0) {
        result->format_name = codec->name;
        result->confidence = 100;
        return GIMG_OK;
      }
    }
  }
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_doc_load(GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;

  GIMG_Probe_Result probe = {0};
  GIMG_Result r = gimg_probe(stream, &probe);
  if (r != GIMG_OK || !probe.format_name) {
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Codec * codec = gimg_codec_by_name(probe.format_name);
  if (!codec || !codec->load_cb) {
    return GIMG_ERR_UNSUPPORTED;
  }

  // Probe uses peek and does not advance the stream; rewind only if seekable
  // so that codecs that assume position 0 see the start. Non-seekable streams
  // are already at 0 after peek.
  r = gimg_stream_seek(stream, 0);
  if (r == GIMG_ERR_UNSUPPORTED) {
    r = GIMG_OK;  // Non-seekable; probe used peek, position unchanged.
  }
  if (r != GIMG_OK) {
    return r;
  }

  r = codec->load_cb(codec, stream, options, diagnostics, out_doc);
  // Remember the caller's limits on the document.  A load only parses headers;
  // the pixels are decoded later, so without this the limits would apply to
  // nothing that actually allocates.  Copied by value: the caller owns the
  // GIMG_Limits it passed and may free it as soon as the load returns.
  if (r == GIMG_OK && *out_doc && options && options->limits) {
    (*out_doc)->load_limits = *options->limits;
    (*out_doc)->has_load_limits = 1;
  }
  return r;
}

GIMG_API GIMG_Result gimg_doc_save(const GIMG_Doc * doc, GIMG_Stream * stream,
    const char * format_name, const GIMG_Save_Options * options,
    GIMG_Save_Report * report) {
  if (!doc || !stream || !format_name) {
    return GIMG_ERR_INTERNAL;
  }

  GIMG_Codec * codec = gimg_codec_by_name(format_name);
  if (!codec || !codec->save_cb) {
    return GIMG_ERR_UNSUPPORTED;
  }

  return codec->save_cb(
      (GIMG_Codec *)codec, doc, stream, format_name, options, report);
}

GIMG_API GIMG_Result gimg_item_decode(const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!item || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;

  const GIMG_Doc * doc = item->doc;
  if (!doc || !doc->loaded_by_codec) {
    return GIMG_ERR_UNSUPPORTED;
  }

  GIMG_Codec * codec = doc->loaded_by_codec;
  if (!codec->decode_cb) {
    return GIMG_ERR_UNSUPPORTED;
  }

  // Fall back to the limits the document was loaded with whenever this call
  // does not carry its own.  Two callers rely on it: an application that set
  // limits at load time and then decodes with NULL options, and the library
  // itself - gimg_*_save re-decodes its source item when the document was
  // loaded by that codec and holds no raster, and has no options to pass on.
  // Without this a limit an application deliberately set is silently dropped
  // by the one path it never sees, and a save can be made to allocate without
  // bound by a header that names an enormous frame.
  GIMG_Decode_Options inherited;
  if (doc->has_load_limits && (!options || !options->limits)) {
    if (options) {
      inherited = *options;
    }
    else {
      // Safe to substitute a zeroed struct for NULL: every field of
      // GIMG_Decode_Options takes its default at zero, so the two are
      // equivalent.  Keep it that way when adding fields - a field whose
      // default is not zero makes this silently decode differently.
      memset(&inherited, 0, sizeof(inherited));
    }
    inherited.limits = &doc->load_limits;
    options = &inherited;
  }

  GIMG_Result r = codec->decode_cb(codec, item, options, out_raster);
  if (r != GIMG_OK || !*out_raster) {
    if (*out_raster) {
      gimg_raster_destroy(*out_raster);
      *out_raster = NULL;
    }
    return r;
  }

  // Apply EXIF (or other) orientation so decoded pixels match display image
  // (CIPA DC-008 / EXIF 2.32 orientation convention).
  GIMG_Meta_Common * meta = gimg_doc_meta_common(doc);
  if (meta) {
    GIMG_Orientation orient = gimg_meta_common_orientation(meta);
    if (orient != GIMG_ORIENTATION_UNKNOWN && orient != GIMG_ORIENTATION_NORMAL) {
      r = gimg_ops_apply_orientation(*out_raster, orient);
      if (r != GIMG_OK) {
        gimg_raster_destroy(*out_raster);
        *out_raster = NULL;
        return r;
      }
    }
  }
  return GIMG_OK;
}

// Ensure the item has a decoded raster: if one is already attached, no-op;
// otherwise decode via the document's codec and attach the raster to the
// item (document owns it). Simplifies load -> modify -> save without
// managing decode ownership. Options (e.g. limits) passed to decode.
GIMG_API GIMG_Result gimg_item_ensure_decoded(
    GIMG_Item * item, const GIMG_Decode_Options * options) {
  if (!item) {
    return GIMG_ERR_INTERNAL;
  }
  if (item->raster) {
    return GIMG_OK; // Already decoded.
  }
  GIMG_Raster * decoded = NULL;
  GIMG_Result r = gimg_item_decode(item, options, &decoded);
  if (r != GIMG_OK) {
    return r;
  }
  gimg_item_set_raster(item, decoded); // Item (doc) takes ownership.
  return GIMG_OK;
}
