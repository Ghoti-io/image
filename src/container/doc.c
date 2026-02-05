/**
 * @file
 *
 * Document and item create/destroy and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>

#include "../core/alloc_internal.h"
#include "../codec/codec_internal.h"
#include "doc_internal.h"

GIMG_API GIMG_Result gimg_doc_create(GIMG_Doc ** out_doc) {
  return gimg_doc_create_with_allocator(NULL, out_doc);
}

GIMG_API GIMG_Result gimg_doc_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Doc ** out_doc) {
  if (!out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Doc * doc = (GIMG_Doc *)gimg_malloc(allocator, sizeof(GIMG_Doc));
  if (!doc) {
    return GIMG_ERR_OOM;
  }
  doc->allocator = allocator;
  doc->item_count = 1;
  doc->items = (GIMG_Item *)gimg_malloc(allocator, sizeof(GIMG_Item));
  if (!doc->items) {
    gimg_free(allocator, doc);
    return GIMG_ERR_OOM;
  }
  doc->loaded_by_codec = NULL;
  doc->codec_private = NULL;
  doc->meta_raw = NULL;
  doc->items[0].index = 0;
  doc->items[0].doc = doc;
  *out_doc = doc;
  return GIMG_OK;
}

GIMG_API void gimg_doc_destroy(GIMG_Doc * doc) {
  if (!doc) {
    return;
  }
  if (doc->meta_raw) {
    gimg_meta_raw_destroy(doc->meta_raw);
    doc->meta_raw = NULL;
  }
  if (doc->loaded_by_codec && doc->codec_private) {
    GIMG_Codec * c = (GIMG_Codec *)doc->loaded_by_codec;
    if (c->free_doc_private) {
      c->free_doc_private(c, doc->codec_private);
    }
  }
  const GIMG_Allocator * alloc = doc->allocator;
  gimg_free(alloc, doc->items);
  gimg_free(alloc, doc);
}

GIMG_API size_t gimg_doc_item_count(const GIMG_Doc * doc) {
  return doc ? doc->item_count : 0;
}

GIMG_API GIMG_Item * gimg_doc_item(const GIMG_Doc * doc, size_t index) {
  if (!doc || !doc->items || index >= doc->item_count) {
    return NULL;
  }
  return &doc->items[index];
}

GIMG_API GIMG_Meta_Raw * gimg_doc_meta_raw(const GIMG_Doc * doc) {
  return doc ? doc->meta_raw : NULL;
}

GIMG_API GIMG_Result gimg_doc_ensure_meta_raw(GIMG_Doc * doc,
    GIMG_Meta_Raw ** out_raw) {
  if (!doc || !out_raw) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raw = NULL;
  if (doc->meta_raw) {
    *out_raw = doc->meta_raw;
    return GIMG_OK;
  }
  GIMG_Result r = gimg_meta_raw_create_with_allocator(doc->allocator,
      &doc->meta_raw);
  if (r != GIMG_OK) {
    return r;
  }
  *out_raw = doc->meta_raw;
  return GIMG_OK;
}
