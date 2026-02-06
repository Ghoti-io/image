/**
 * @file
 *
 * Overflow-safe integer helpers for the Ghoti.io Image library (internal).
 * Used for pixel count and buffer size calculations to avoid undefined
 * behavior and reject corrupt/oversized inputs.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SAFE_MATH_INTERNAL_H
#define GHOTI_IO_GIMG_SAFE_MATH_INTERNAL_H

#include <ghoti.io/image/core.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Safely multiply two size_t values.
 * @return true if no overflow, false otherwise. On overflow *result is
 * unchanged.
 */
static inline bool gimg_safe_mul_size(size_t a, size_t b, size_t * result) {
  if (a == 0 || b == 0) {
    *result = 0;
    return true;
  }
  if (a > SIZE_MAX / b) {
    return false;
  }
  *result = a * b;
  return true;
}

/**
 * @brief Safely add two size_t values.
 * @return true if no overflow, false otherwise.
 */
static inline bool gimg_safe_add_size(size_t a, size_t b, size_t * result) {
  if (a > SIZE_MAX - b) {
    return false;
  }
  *result = a + b;
  return true;
}

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
  size_t w = (size_t)width;
  size_t h = (size_t)height;
  if (w == 0 || h == 0) {
    *out_count = 0;
    return GIMG_OK;
  }
  if (w > SIZE_MAX / h) {
    return GIMG_ERR_LIMIT;
  }
  *out_count = w * h;
  return GIMG_OK;
}

#ifdef __cplusplus
}
#endif

#endif  // GHOTI_IO_GIMG_SAFE_MATH_INTERNAL_H
