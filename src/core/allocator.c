/**
 * @file
 *
 * Default allocator implementation for the Ghoti.io Image library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/allocator.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void * gimg_stdlib_malloc(GIMG_MAYBE_UNUSED(void * ctx), size_t size) {
  return malloc(size);
}

static void * gimg_stdlib_calloc(
    GIMG_MAYBE_UNUSED(void * ctx), size_t nitems, size_t size) {
  if (nitems == 0 || size == 0) {
    return malloc(1); // Allow 0-size for compatibility; avoid 0-size alloc.
  }
  if (size > SIZE_MAX / nitems) {
    return NULL; // Overflow
  }
  return calloc(1, nitems * size);
}

static void * gimg_stdlib_realloc(
    GIMG_MAYBE_UNUSED(void * ctx), void * ptr, size_t size) {
  return realloc(ptr, size);
}

static void gimg_stdlib_free(GIMG_MAYBE_UNUSED(void * ctx), void * ptr) {
  free(ptr);
}

GIMG_API const GIMG_Allocator * gimg_allocator_default(void) {
  static const GIMG_Allocator allocator = {
      .ctx = NULL,
      .malloc_fn = gimg_stdlib_malloc,
      .calloc_fn = gimg_stdlib_calloc,
      .realloc_fn = gimg_stdlib_realloc,
      .free_fn = gimg_stdlib_free,
  };
  return &allocator;
}
