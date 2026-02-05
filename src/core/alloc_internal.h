/**
 * @file
 *
 * Internal allocator helpers for the Ghoti.io Image library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_ALLOC_INTERNAL_H
#define GHOTI_IO_GIMG_ALLOC_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <stddef.h>

static inline const GIMG_ALLOCATOR * gimg_alloc_or_default(
    const GIMG_ALLOCATOR * allocator) {
  return allocator ? allocator : gimg_allocator_default();
}

static inline void * gimg_malloc(
    const GIMG_ALLOCATOR * allocator, size_t size) {
  allocator = gimg_alloc_or_default(allocator);
  return allocator->malloc_fn(allocator->ctx, size);
}

static inline void * gimg_calloc(
    const GIMG_ALLOCATOR * allocator, size_t nitems, size_t size) {
  allocator = gimg_alloc_or_default(allocator);
  return allocator->calloc_fn(allocator->ctx, nitems, size);
}

static inline void * gimg_realloc(
    const GIMG_ALLOCATOR * allocator, void * ptr, size_t size) {
  allocator = gimg_alloc_or_default(allocator);
  return allocator->realloc_fn(allocator->ctx, ptr, size);
}

static inline void gimg_free(const GIMG_ALLOCATOR * allocator, void * ptr) {
  if (!ptr) {
    return;
  }
  allocator = gimg_alloc_or_default(allocator);
  allocator->free_fn(allocator->ctx, ptr);
}

#endif // GHOTI_IO_GIMG_ALLOC_INTERNAL_H
