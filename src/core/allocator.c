/**
 * @file
 *
 * The library's default allocator, which is cutil's.
 *
 * The stdlib-backed implementation this file used to carry was the same one
 * cutil has, so it forwards rather than repeating it. One behavior came along
 * with the move and is worth noting: this allocator has always guaranteed a
 * non-NULL return for a zero-size request, because NULL has to mean failure
 * and nothing else. It used to satisfy that with a plain malloc, so a
 * zero-item calloc returned uninitialized memory; cutil's returns a zeroed
 * byte instead.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cutil/allocator.h>
#include <ghoti.io/image/allocator.h>

GIMG_API const GIMG_Allocator * gimg_allocator_default(void) {
  return gcu_allocator_default();
}
