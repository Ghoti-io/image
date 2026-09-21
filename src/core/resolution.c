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
 * Physical resolution conversion between dots per inch and pixels per metre.
 */

#include <ghoti.io/image/macros.h>

#include "resolution_internal.h"

uint32_t gimg_dpi_to_pixels_per_meter(uint32_t dpi) {
  if (dpi == 0) {
    return 0; // "not stated" on both sides
  }
  uint64_t ppm = ((uint64_t)dpi * 5000u + 63u) / 127u;
  return (ppm > UINT32_MAX) ? UINT32_MAX : (uint32_t)ppm;
}

uint32_t gimg_pixels_per_meter_to_dpi(uint32_t ppm) {
  if (ppm == 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)ppm * 127u + 2500u) / 5000u);
}
