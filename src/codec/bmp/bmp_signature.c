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
 * BMP file signature for probe ('B' 'M').
 */

#include <ghoti.io/image/macros.h>
#include "bmp_internal.h"

const unsigned char gimg_bmp_signature[GIMG_BMP_SIGNATURE_LEN] = {0x42, 0x4D};

const unsigned char gimg_bmp_array_signature[GIMG_BMP_SIGNATURE_LEN] = {
    0x42, 0x41};

GIMG_Result gimg_bmp_verify_signature(GIMG_Stream * stream) {
  unsigned char magic[GIMG_BMP_SIGNATURE_LEN];
  GIMG_Result r = gimg_stream_read_exact(stream, magic, sizeof(magic));
  if (r != GIMG_OK) {
    return r;
  }
  if (magic[0] != gimg_bmp_signature[0] || magic[1] != gimg_bmp_signature[1]) {
    return GIMG_ERR_FORMAT;
  }
  return GIMG_OK;
}
