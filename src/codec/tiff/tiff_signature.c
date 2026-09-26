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
 * The four spellings of a TIFF header (TIFF 6.0 section 2).
 *
 * Bytes 0 and 1 are the byte order - "II" for little-endian, "MM" for big -
 * and bytes 2 and 3 are the number 42 written in that order.  All four bytes
 * are the magic, together, on purpose: a probe that matched "II" or "MM"
 * alone would claim every file that happens to start with two capital letters,
 * and one that matched 42 without reading the order first cannot know which
 * end of the field to look at.  That second mistake has a name in this
 * codebase - it is the defect that rejected one whole endianness - so the
 * order and the version are one indivisible test.
 *
 * BigTIFF writes 43 where classic TIFF writes 42.  Its two spellings are
 * registered here as well, not because this codec reads one, but so that a
 * BigTIFF is refused by the TIFF codec with a reason rather than falling
 * through every codec and coming back as "no format recognised these bytes".
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "tiff_internal.h"

const unsigned char gimg_tiff_magic_le[GIMG_TIFF_SIGNATURE_LEN] = {
    'I', 'I', 42, 0};
const unsigned char gimg_tiff_magic_be[GIMG_TIFF_SIGNATURE_LEN] = {
    'M', 'M', 0, 42};
const unsigned char gimg_tiff_magic_le_big[GIMG_TIFF_SIGNATURE_LEN] = {
    'I', 'I', 43, 0};
const unsigned char gimg_tiff_magic_be_big[GIMG_TIFF_SIGNATURE_LEN] = {
    'M', 'M', 0, 43};

GIMG_Result gimg_tiff_read_header(
    const unsigned char * bytes, size_t size, bool * out_big_endian) {
  if (!bytes || !out_big_endian) {
    return GIMG_ERR_INTERNAL;
  }
  if (size < GIMG_TIFF_SIGNATURE_LEN) {
    return GIMG_ERR_FORMAT;
  }
  if (memcmp(bytes, gimg_tiff_magic_le, GIMG_TIFF_SIGNATURE_LEN) == 0) {
    *out_big_endian = false;
    return GIMG_OK;
  }
  if (memcmp(bytes, gimg_tiff_magic_be, GIMG_TIFF_SIGNATURE_LEN) == 0) {
    *out_big_endian = true;
    return GIMG_OK;
  }
  if (memcmp(bytes, gimg_tiff_magic_le_big, GIMG_TIFF_SIGNATURE_LEN) == 0 ||
      memcmp(bytes, gimg_tiff_magic_be_big, GIMG_TIFF_SIGNATURE_LEN) == 0) {
    return GIMG_ERR_UNSUPPORTED;
  }
  return GIMG_ERR_FORMAT;
}
