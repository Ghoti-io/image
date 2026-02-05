/**
 * @file
 *
 * Normalized common metadata implementation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/meta.h>

#include "../core/alloc_internal.h"
#include "meta_internal.h"

GIMG_API GIMG_Result gimg_meta_common_create(GIMG_Meta_Common ** out_meta) {
  return gimg_meta_common_create_with_allocator(NULL, out_meta);
}

GIMG_API GIMG_Result gimg_meta_common_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Meta_Common ** out_meta) {
  if (!out_meta) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Meta_Common * m =
      (GIMG_Meta_Common *)gimg_malloc(allocator, sizeof(GIMG_Meta_Common));
  if (!m) {
    return GIMG_ERR_OOM;
  }
  m->allocator = allocator;
  m->orientation = GIMG_ORIENTATION_UNKNOWN;
  m->x_dpi = 0;
  m->y_dpi = 0;
  *out_meta = m;
  return GIMG_OK;
}

GIMG_API void gimg_meta_common_destroy(GIMG_Meta_Common * meta) {
  if (!meta) {
    return;
  }
  gimg_free(meta->allocator, meta);
}

GIMG_API void gimg_meta_common_set_orientation(
    GIMG_Meta_Common * meta, GIMG_Orientation value) {
  if (meta) {
    meta->orientation = value;
  }
}

GIMG_API GIMG_Orientation gimg_meta_common_orientation(
    const GIMG_Meta_Common * meta) {
  return meta ? meta->orientation : GIMG_ORIENTATION_UNKNOWN;
}

GIMG_API void gimg_meta_common_set_dpi(
    GIMG_Meta_Common * meta, uint32_t x_dpi, uint32_t y_dpi) {
  if (meta) {
    meta->x_dpi = x_dpi;
    meta->y_dpi = y_dpi;
  }
}

GIMG_API void gimg_meta_common_dpi(
    const GIMG_Meta_Common * meta, uint32_t * out_x, uint32_t * out_y) {
  if (meta && out_x && out_y) {
    *out_x = meta->x_dpi;
    *out_y = meta->y_dpi;
  }
}
