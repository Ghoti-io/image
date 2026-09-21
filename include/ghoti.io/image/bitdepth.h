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
 * Bit-depth conversion for image samples: 8-, 12-, and 16-bit (spec §4 / library
 * first-class). Used by codecs (e.g. JPEG) when converting between raster depth
 * and codec precision. All conversions use bitshift and clamp; 12-bit range is
 * 0..4095, 16-bit is 0..65535.
 */

#ifndef GHOTI_IO_GIMG_BITDEPTH_H
#define GHOTI_IO_GIMG_BITDEPTH_H

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Convert 8-bit sample to 12-bit (0..255 → 0..4095). Replicate bits. */
GIMG_API uint16_t gimg_bitdepth_8_to_12(uint8_t v);

/** @brief Convert 8-bit sample to 16-bit (0..255 → 0..65535). Replicate bits. */
GIMG_API uint16_t gimg_bitdepth_8_to_16(uint8_t v);

/** @brief Convert 12-bit sample to 8-bit (0..4095 → 0..255). Round and clamp. */
GIMG_API uint8_t gimg_bitdepth_12_to_8(uint16_t v);

/** @brief Convert 12-bit to 16-bit, replicating high bits (0..4095 → 0..65535). */
GIMG_API uint16_t gimg_bitdepth_12_to_16(uint16_t v);

/** @brief Convert 16-bit sample to 8-bit (0..65535 → 0..255). Round and clamp. */
GIMG_API uint8_t gimg_bitdepth_16_to_8(uint16_t v);

/** @brief Convert 16-bit sample to 12-bit (0..65535 → 0..4095). Round and clamp. */
GIMG_API uint16_t gimg_bitdepth_16_to_12(uint16_t v);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_BITDEPTH_H
