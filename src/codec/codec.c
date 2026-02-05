/**
 * @file
 *
 * Codec registry, probe, load/save/decode stubs.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "codec_internal.h"

#define REGISTRY_INITIAL 8

static GIMG_CODEC ** gimg_codec_registry = NULL;
static size_t gimg_codec_registry_count = 0;
static size_t gimg_codec_registry_capacity = 0;

static const GIMG_ALLOCATOR * gimg_codec_registry_allocator(void) {
  return gimg_allocator_default();
}

GIMG_API GIMG_RESULT gimg_codec_create_stub(const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_CODEC ** out_codec) {
  return gimg_codec_create_stub_with_allocator(NULL, name, magic_bytes,
      magic_len, out_codec);
}

GIMG_API GIMG_RESULT gimg_codec_create_stub_with_allocator(
    const GIMG_ALLOCATOR * allocator, const char * name,
    const void * magic_bytes, size_t magic_len, GIMG_CODEC ** out_codec) {
  if (!name || !out_codec) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_CODEC * c = (GIMG_CODEC *)gimg_malloc(allocator, sizeof(GIMG_CODEC));
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
    c->magics = (gimg_codec_magic_t *)gimg_malloc(allocator,
        sizeof(gimg_codec_magic_t));
    if (!c->magics) {
      gimg_free(allocator, c->name);
      gimg_free(allocator, c);
      return GIMG_ERR_OOM;
    }
    unsigned char * copy =
        (unsigned char *)gimg_malloc(allocator, magic_len);
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
  *out_codec = c;
  return GIMG_OK;
}

GIMG_API GIMG_RESULT gimg_codec_register(GIMG_CODEC * codec) {
  if (!codec || !codec->name) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_ALLOCATOR * alloc = gimg_codec_registry_allocator();
  for (size_t i = 0; i < gimg_codec_registry_count; i++) {
    if (strcmp(gimg_codec_registry[i]->name, codec->name) == 0) {
      return GIMG_ERR_INTERNAL;  // Duplicate
    }
  }
  if (gimg_codec_registry_count >= gimg_codec_registry_capacity) {
    size_t new_cap = gimg_codec_registry_capacity
        ? gimg_codec_registry_capacity * 2
        : REGISTRY_INITIAL;
    GIMG_CODEC ** new_reg = (GIMG_CODEC **)gimg_realloc(alloc,
        gimg_codec_registry, new_cap * sizeof(GIMG_CODEC *));
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

GIMG_API GIMG_CODEC * gimg_codec_by_index(size_t index) {
  if (index >= gimg_codec_registry_count) {
    return NULL;
  }
  return gimg_codec_registry[index];
}

GIMG_API GIMG_CODEC * gimg_codec_by_name(const char * name) {
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

GIMG_API const char * gimg_codec_name(const GIMG_CODEC * codec) {
  return codec ? codec->name : NULL;
}

GIMG_API GIMG_RESULT gimg_probe(
    GIMG_STREAM * stream, GIMG_PROBE_RESULT * result) {
  if (!stream || !result) {
    return GIMG_ERR_INTERNAL;
  }
  result->format_name = NULL;
  result->confidence = 0;

  unsigned char peek_buf[32];
  size_t peeked = 0;
  GIMG_RESULT r = gimg_stream_peek(stream, peek_buf, sizeof(peek_buf), &peeked);
  if (r != GIMG_OK || peeked == 0) {
    return GIMG_OK; // No match
  }

  for (size_t c = 0; c < gimg_codec_registry_count; c++) {
    GIMG_CODEC * codec = gimg_codec_registry[c];
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

GIMG_API GIMG_RESULT gimg_doc_load(GIMG_STREAM * stream,
    const GIMG_LOAD_OPTIONS * options, GIMG_DIAGNOSTICS * diagnostics,
    GIMG_DOC ** out_doc) {
  (void)stream;
  (void)options;
  (void)diagnostics;
  if (!out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;
  return GIMG_ERR_UNSUPPORTED;
}

GIMG_API GIMG_RESULT gimg_doc_save(const GIMG_DOC * doc, GIMG_STREAM * stream,
    const char * format_name, const GIMG_SAVE_OPTIONS * options,
    GIMG_SAVE_REPORT * report) {
  (void)doc;
  (void)stream;
  (void)format_name;
  (void)options;
  (void)report;
  return GIMG_ERR_UNSUPPORTED;
}

GIMG_API GIMG_RESULT gimg_item_decode(const GIMG_ITEM * item,
    const GIMG_DECODE_OPTIONS * options, GIMG_RASTER ** out_raster) {
  (void)item;
  (void)options;
  if (!out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raster = NULL;
  return GIMG_ERR_UNSUPPORTED;
}
