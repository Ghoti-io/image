/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Internal allocator helpers for the Ghoti.io Image library.
 */

#ifndef GHOTI_IO_GIMG_SRC_CORE_ALLOC_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CORE_ALLOC_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <stddef.h>

static inline const GIMG_Allocator * gimg_alloc_or_default(
    const GIMG_Allocator * allocator) {
  return allocator ? allocator : gimg_allocator_default();
}

static inline void * gimg_malloc(
    const GIMG_Allocator * allocator, size_t size) {
  allocator = gimg_alloc_or_default(allocator);
  return allocator->malloc_fn(allocator->ctx, size);
}

static inline void * gimg_calloc(
    const GIMG_Allocator * allocator, size_t nitems, size_t size) {
  allocator = gimg_alloc_or_default(allocator);
  return allocator->calloc_fn(allocator->ctx, nitems, size);
}

static inline void * gimg_realloc(
    const GIMG_Allocator * allocator, void * ptr, size_t size) {
  allocator = gimg_alloc_or_default(allocator);
  return allocator->realloc_fn(allocator->ctx, ptr, size);
}

static inline void gimg_free(const GIMG_Allocator * allocator, void * ptr) {
  if (!ptr) {
    return;
  }
  allocator = gimg_alloc_or_default(allocator);
  allocator->free_fn(allocator->ctx, ptr);
}

#endif // GHOTI_IO_GIMG_SRC_CORE_ALLOC_INTERNAL_H
