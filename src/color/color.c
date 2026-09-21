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
 * Color info default (stub; sRGB/linear conversion in ops).
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/color.h>

GIMG_API void gimg_color_info_default(GIMG_Color_Info * info) {
  if (!info) {
    return;
  }
  *info = (GIMG_Color_Info){
      .primaries = GIMG_PRIMARIES_UNKNOWN,
      .white_point = GIMG_PRIMARIES_UNKNOWN,
      .transfer = GIMG_TRANSFER_UNKNOWN,
      .gamma_value = 0.0,
      .intent = GIMG_INTENT_PERCEPTUAL,
      .icc_bytes = NULL,
      .icc_size = 0,
      .cmyk_polarity = GIMG_CMYK_POLARITY_UNKNOWN,
  };
}
