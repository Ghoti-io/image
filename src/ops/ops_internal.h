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
 * Helpers shared between the files under src/ops.  Not installed.
 */

#ifndef GHOTI_IO_GIMG_SRC_OPS_OPS_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_OPS_OPS_INTERNAL_H

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/raster.h>

/**
 * Copy the source raster's GIMG_Color_Info, embedded profile included, onto
 * the destination.  An operation that rearranges samples without
 * reinterpreting them carries the source's colour description; one that
 * changes what a sample means, such as the CMYK to RGBA conversion,
 * deliberately does not call this.  See ops.c for why it exists.
 */
GIMG_Result gimg_ops_carry_color(const GIMG_Raster * src, GIMG_Raster * dst);

#endif // GHOTI_IO_GIMG_SRC_OPS_OPS_INTERNAL_H
