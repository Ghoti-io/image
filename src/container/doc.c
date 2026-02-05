/**
 * @file
 *
 * Document and item create/destroy and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/doc.h>

#include "../core/alloc_internal.h"
#include "doc_internal.h"

GIMG_API GIMG_RESULT gimg_doc_create(GIMG_DOC ** out_doc) {
  return gimg_doc_create_with_allocator(NULL, out_doc);
}

GIMG_API GIMG_RESULT gimg_doc_create_with_allocator(
    const GIMG_ALLOCATOR * allocator, GIMG_DOC ** out_doc) {
  if (!out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_DOC * doc = (GIMG_DOC *)gimg_malloc(allocator, sizeof(GIMG_DOC));
  if (!doc) {
    return GIMG_ERR_OOM;
  }
  doc->allocator = allocator;
  doc->item_count = 1;
  doc->items = (GIMG_ITEM *)gimg_malloc(allocator, sizeof(GIMG_ITEM));
  if (!doc->items) {
    gimg_free(allocator, doc);
    return GIMG_ERR_OOM;
  }
  doc->items[0].index = 0;
  *out_doc = doc;
  return GIMG_OK;
}

GIMG_API void gimg_doc_destroy(GIMG_DOC * doc) {
  if (!doc) {
    return;
  }
  const GIMG_ALLOCATOR * alloc = doc->allocator;
  gimg_free(alloc, doc->items);
  gimg_free(alloc, doc);
}

GIMG_API size_t gimg_doc_item_count(const GIMG_DOC * doc) {
  return doc ? doc->item_count : 0;
}

GIMG_API GIMG_ITEM * gimg_doc_item(const GIMG_DOC * doc, size_t index) {
  if (!doc || !doc->items || index >= doc->item_count) {
    return NULL;
  }
  return &doc->items[index];
}
