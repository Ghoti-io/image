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
 * Shared color helpers that are not part of the public API.
 */

#ifndef GHOTI_IO_GIMG_COLOR_INTERNAL_H
#define GHOTI_IO_GIMG_COLOR_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Build an ICC profile that says what @p info says.
 *
 * For a format whose only way to name a color space is an ICC profile.  The
 * profile is manufactured, not repeated: use it only when the caller stated a
 * color model and the file has nowhere else to put it, and never in place of
 * a profile the source actually carried.
 *
 * @param alloc Allocator for the returned buffer, or NULL for the default.
 * @param info The color model to state.  Both the primaries and the transfer
 *   function must be known; anything less cannot be written as a matrix/TRC
 *   profile without inventing the missing half.
 * @param out_bytes Receives the profile, owned by the caller and freed with
 *   gimg_free().  Set to NULL when @p info does not state enough.
 * @param out_size Receives the profile size, or 0.
 * @return GIMG_OK (including when nothing was built), or GIMG_ERR_OOM.
 */
GIMG_Result gimg_icc_synthesize(const GIMG_Allocator * alloc,
    const GIMG_Color_Info * info, void ** out_bytes, size_t * out_size);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_COLOR_INTERNAL_H
