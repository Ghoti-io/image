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
 * Standard JPEG quantization tables (T.81 Annex K.1). Luminance and
 * chrominance 64-entry tables in natural/row-major order. Encode and save use
 * these; scaling by quality remains in jpeg_save.c.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_JPEG_JPEG_QUANT_TABLES_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_JPEG_JPEG_QUANT_TABLES_INTERNAL_H

#include <ghoti.io/image/macros.h>

/** Standard luminance quant table (T.81 Annex K.1). 64 entries, row-major. */
extern const unsigned int gimg_jpeg_std_luminance_quant[64];
/** Standard chrominance quant table (T.81 Annex K.1). 64 entries, row-major. */
extern const unsigned int gimg_jpeg_std_chrominance_quant[64];

#ifdef GIMG_JPEG_QUANT_TABLES_DEFINE
// clang-format off
const unsigned int gimg_jpeg_std_luminance_quant[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};
const unsigned int gimg_jpeg_std_chrominance_quant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};
// clang-format on
#endif

#endif // GHOTI_IO_GIMG_SRC_CODEC_JPEG_JPEG_QUANT_TABLES_INTERNAL_H
