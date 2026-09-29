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
 * ICO/CUR magic and post-magic probe.
 *
 * The signature is four sparse bytes (`00 00 01 00` or `00 00 02 00`). Without
 * the probe callback validating the count and first entry, that pattern would
 * claim unrelated files. Strong magics of other codecs already win first;
 * this only claims after the directory looks like an icon.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../codec_internal.h"
#include "ico_internal.h"

const unsigned char gimg_ico_signature[GIMG_ICO_SIGNATURE_LEN] = {
    0x00, 0x00, 0x01, 0x00};

const unsigned char gimg_cur_signature[GIMG_ICO_SIGNATURE_LEN] = {
    0x00, 0x00, 0x02, 0x00};

static uint16_t ico_u16(const unsigned char * p) {
  return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static uint32_t ico_u32(const unsigned char * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}

int gimg_ico_probe(
    GIMG_Stream * stream, const unsigned char * peek, size_t peeked) {
  // Need ICONDIR (6) plus the first ICONDIRENTRY (16) to validate the offset.
  if (!stream || !peek || peeked < GIMG_ICO_DIR_SIZE + GIMG_ICO_DIRENTRY_SIZE) {
    return 0;
  }
  if (peek[0] != 0x00 || peek[1] != 0x00) {
    return 0;
  }
  uint16_t type = ico_u16(peek + 2);
  if (type != GIMG_ICO_TYPE_ICON && type != GIMG_ICO_TYPE_CURSOR) {
    return 0;
  }
  uint16_t count = ico_u16(peek + 4);
  if (count < 1u || count > GIMG_ICO_MAX_ENTRIES) {
    return 0;
  }

  const unsigned char * entry = peek + GIMG_ICO_DIR_SIZE;
  uint32_t bytes = ico_u32(entry + 8);
  uint32_t offset = ico_u32(entry + 12);
  if (bytes == 0u) {
    return 0;
  }

  // The first payload must sit after the directory table. Extent checks that
  // need the file length (offset past EOF, size running off the end) are
  // load's job: declining them here left corrupt fixtures with no codec to
  // attach a diagnostic to, which the refusal gate counts as silence.
  size_t dir_end =
      (size_t)GIMG_ICO_DIR_SIZE + (size_t)count * GIMG_ICO_DIRENTRY_SIZE;
  if ((size_t)offset < dir_end) {
    return 0;
  }
  (void)stream;
  return 1;
}
