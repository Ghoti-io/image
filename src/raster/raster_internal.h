/**
 * @file
 *
 * Internal raster implementation structures.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_RASTER_INTERNAL_H
#define GHOTI_IO_GIMG_RASTER_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/raster.h>
#include <stddef.h>
#include <stdint.h>

#define GIMG_DEFAULT_STRIDE_ALIGNMENT 16

/**
 * @brief Raster image instance.
 */
struct GIMG_RASTER {
  const GIMG_ALLOCATOR * allocator;
  uint32_t width;
  uint32_t height;
  size_t stride_bytes;
  GIMG_PIXEL_FORMAT format;
  GIMG_RASTER_OWNERSHIP ownership;
  void * pixels;  ///< Owned buffer or borrowed pointer.
};

/**
 * @brief Compute bytes per pixel for interleaved format (simplified).
 */
size_t gimg_raster_bytes_per_pixel(const GIMG_PIXEL_FORMAT * format);

#endif // GHOTI_IO_GIMG_RASTER_INTERNAL_H
