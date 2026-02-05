/**
 * @file
 *
 * Internal document and item structures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_DOC_INTERNAL_H
#define GHOTI_IO_GIMG_DOC_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/doc.h>
#include <stddef.h>

/**
 * @brief Single item (page/frame/level/thumbnail).
 */
struct GIMG_ITEM {
  size_t index; ///< Index in parent doc.
  /* Per-item metadata and timing stubs added in meta milestone. */
};

/**
 * @brief Document container.
 */
struct GIMG_DOC {
  const GIMG_ALLOCATOR * allocator;
  GIMG_ITEM * items;
  size_t item_count;
};

#endif // GHOTI_IO_GIMG_DOC_INTERNAL_H
