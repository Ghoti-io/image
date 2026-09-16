/**
 * @file
 *
 * Image-specific safe arithmetic (internal).
 *
 * The general overflow-checked helpers this file used to carry are cutil's
 * now (`<cutil/safemath.h>`); they were duplicates of the same functions in
 * compress. What remains is the one calculation that is specific to this
 * library, because it works in image dimensions and reports a GIMG_Result.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SAFE_MATH_INTERNAL_H
#define GHOTI_IO_GIMG_SAFE_MATH_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <cutil/safemath.h>
#include <ghoti.io/image/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute pixel count width * height in size_t without overflow.
 * Used for allocation and limit checks. Rejects dimensions that would
 * overflow (e.g. 0x10000 * 0x10000 on 32-bit).
 * @param width Image width.
 * @param height Image height.
 * @param out_count Output pixel count (only set on success).
 * @return GIMG_OK on success, GIMG_ERR_LIMIT if width*height would overflow.
 */
static inline GIMG_Result gimg_safe_pixel_count(
    uint32_t width, uint32_t height, size_t * out_count) {
  if (!gcu_safe_mul_size((size_t)width, (size_t)height, out_count)) {
    return GIMG_ERR_LIMIT;
  }
  return GIMG_OK;
}

#ifdef __cplusplus
}
#endif

#endif  // GHOTI_IO_GIMG_SAFE_MATH_INTERNAL_H
