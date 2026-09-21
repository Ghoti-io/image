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
 * Image-specific safe arithmetic (internal).
 *
 * The general overflow-checked helpers this file used to carry are cutil's
 * now (`<cutil/safemath.h>`); they were duplicates of the same functions in
 * compress. What remains is the one calculation that is specific to this
 * library, because it works in image dimensions and reports a GIMG_Result.
 */

#ifndef GHOTI_IO_GIMG_SRC_CORE_SAFE_MATH_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CORE_SAFE_MATH_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/cutil/safemath.h>
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

#endif  // GHOTI_IO_GIMG_SRC_CORE_SAFE_MATH_INTERNAL_H
