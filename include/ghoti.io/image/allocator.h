/**
 * @file
 *
 * Allocator abstraction for the Ghoti.io Image library.
 *
 * This is cutil's @ref GCU_Allocator under a local name. The two were
 * identical - same four function pointers, same context argument, same
 * semantics - and having one definition means an allocator written for any
 * library in the suite works with all of them, rather than needing a
 * near-identical copy per library.
 *
 * Existing code needs no change: `GIMG_Allocator` still names the type and
 * gimg_allocator_default() still returns the stdlib-backed instance.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_ALLOCATOR_H
#define GHOTI_IO_IMAGE_ALLOCATOR_H

#include <cutil/allocator.h>
#include <ghoti.io/image/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL. Each receives the `ctx` pointer
 * from the struct as its first argument.
 *
 * Two requirements beyond the C library equivalents: `calloc_fn` must treat
 * overflow of `nitems * size` as an allocation failure and return NULL rather
 * than allocating a truncated block, and a zero-size request must return a
 * usable non-NULL pointer, so that NULL always means failure.
 */
typedef GCU_Allocator GIMG_Allocator;

/**
 * @brief Get the default allocator (stdlib-backed).
 *
 * The default allocator treats overflow in calloc(nitems, size) as allocation
 * failure: if nitems * size would overflow size_t, it returns NULL. It never
 * returns NULL for a zero-size request, and a zero-size calloc is still
 * zeroed.
 *
 * @return Pointer to a process-global allocator instance.
 */
GIMG_API const GIMG_Allocator * gimg_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_ALLOCATOR_H
