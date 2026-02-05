/**
 * @file
 *
 * Internal metadata structures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_META_INTERNAL_H
#define GHOTI_IO_GIMG_META_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/meta.h>
#include <stddef.h>
#include <stdint.h>

struct GIMG_Meta_Common {
  const GIMG_Allocator * allocator;
  GIMG_Orientation orientation;
  uint32_t x_dpi;
  uint32_t y_dpi;
};

typedef struct gimg_meta_raw_block {
  char * format_id;
  uint32_t tag_or_chunk_id;
  void * data;
  size_t size;
} gimg_meta_raw_block_t;

struct GIMG_Meta_Raw {
  const GIMG_Allocator * allocator;
  gimg_meta_raw_block_t * blocks;
  size_t count;
  size_t capacity;
};

#endif // GHOTI_IO_GIMG_META_INTERNAL_H
