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
 * Internal raster implementation structures.
 */

#ifndef GHOTI_IO_GIMG_SRC_RASTER_RASTER_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_RASTER_RASTER_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/raster.h>
#include <stddef.h>
#include <stdint.h>

#define GIMG_DEFAULT_STRIDE_ALIGNMENT 16

/**
 * @brief Raster image instance.
 */
struct GIMG_Raster {
  const GIMG_Allocator * allocator;
  uint32_t width;
  uint32_t height;
  size_t stride_bytes;
  GIMG_Pixel_Format format;
  GIMG_Raster_Ownership ownership;
  void * pixels;       ///< Owned buffer or borrowed pointer.
  GCOL_Color_Info color_info;
  void * color_icc_owned; ///< If non-NULL, raster owns ICC bytes; color_info.icc_bytes points here.
  char * color_icc_linked_path_owned; ///< If non-NULL, raster owns the linked
                                      ///< profile path; color_info.icc_linked_path points here.
};

/**
 * @brief Compute bytes per pixel for interleaved format (simplified).
 */
size_t gimg_raster_bytes_per_pixel(const GIMG_Pixel_Format * format);

/**
 * @brief Replace an owned raster's pixel buffer, dimensions and stride.
 *
 * Frees the buffer the raster held and takes ownership of @a pixels, which
 * must come from the raster's own allocator. The raster itself keeps its
 * address, so pointers held elsewhere (a document's item, say) stay valid.
 * Only for rasters that own their pixels; a borrowed buffer is not ours to
 * free or to replace.
 */
void gimg_raster_replace_owned_buffer(GIMG_Raster * raster, void * pixels,
    uint32_t width, uint32_t height, size_t stride_bytes);

#endif // GHOTI_IO_GIMG_SRC_RASTER_RASTER_INTERNAL_H
