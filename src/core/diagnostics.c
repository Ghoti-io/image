/**
 * @file
 *
 * Diagnostics list (append, grow).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/core.h>
#include <stddef.h>

#include "alloc_internal.h"

#define DIAG_GROW 4

static const GIMG_Allocator * diag_alloc(const GIMG_Diagnostics * d) {
  return d->allocator ? d->allocator : gimg_allocator_default();
}

GIMG_API void gimg_diagnostics_init(GIMG_Diagnostics * d,
    const GIMG_Allocator * allocator) {
  if (!d) {
    return;
  }
  d->allocator = allocator;
}

GIMG_API GIMG_Result gimg_diagnostics_append(GIMG_Diagnostics * diagnostics,
    const char * codec_name, size_t offset, uint32_t chunk_or_tag_id,
    GIMG_Diag_Severity severity, const char * recommended_action) {
  if (!diagnostics) {
    return GIMG_ERR_INTERNAL;
  }
  if (diagnostics->count >= diagnostics->capacity) {
    size_t new_cap =
        diagnostics->capacity ? diagnostics->capacity + DIAG_GROW : DIAG_GROW;
    const GIMG_Allocator * alloc = diag_alloc(diagnostics);
    GIMG_Diagnostic * new_items = (GIMG_Diagnostic *)gimg_realloc(
        alloc, diagnostics->items, new_cap * sizeof(GIMG_Diagnostic));
    if (!new_items) {
      return GIMG_ERR_OOM;
    }
    diagnostics->items = new_items;
    diagnostics->capacity = new_cap;
  }
  GIMG_Diagnostic * d = &diagnostics->items[diagnostics->count];
  d->codec_name = codec_name;
  d->offset = offset;
  d->chunk_or_tag_id = chunk_or_tag_id;
  d->severity = severity;
  d->recommended_action = recommended_action;
  diagnostics->count++;
  return GIMG_OK;
}

GIMG_API void gimg_diagnostics_clear(GIMG_Diagnostics * d) {
  if (!d) {
    return;
  }
  if (d->items) {
    gimg_free(diag_alloc(d), d->items);
    d->items = NULL;
  }
  d->count = 0;
  d->capacity = 0;
}

GIMG_API void gimg_diagnostics_destroy(GIMG_Diagnostics * d) {
  if (!d) {
    return;
  }
  gimg_diagnostics_clear(d);
  d->allocator = NULL;
}
