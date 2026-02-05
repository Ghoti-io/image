/**
 * @file
 *
 * Configurable allocator abstraction for the Ghoti.io Image library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_ALLOCATOR_H
#define GHOTI_IO_IMAGE_ALLOCATOR_H

#include <ghoti.io/image/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL. When an allocator is passed as
 * optional (e.g. NULL), the library uses the default allocator.
 */
typedef struct GIMG_ALLOCATOR {
  void * ctx;
  void * (*malloc_fn)(void * ctx, size_t size);
  void * (*calloc_fn)(void * ctx, size_t nitems, size_t size);
  void * (*realloc_fn)(void * ctx, void * ptr, size_t size);
  void (*free_fn)(void * ctx, void * ptr);
} GIMG_ALLOCATOR;

/**
 * @brief Get the default allocator (stdlib-backed).
 *
 * The default allocator treats overflow in calloc(nitems, size) as allocation
 * failure: if nitems * size would overflow size_t, it returns NULL.
 *
 * @return Pointer to a process-global allocator instance.
 */
GIMG_API const GIMG_ALLOCATOR * gimg_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_ALLOCATOR_H
