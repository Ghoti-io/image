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
 * WebP magic and post-magic probe.
 *
 * The on-disk head is twelve bytes (`RIFF` + size + `WEBP`), but the size
 * field varies, so the registered magic is only `RIFF`. The probe confirms
 * `WEBP` at offset 8 before claiming the file — otherwise every RIFF
 * container (WAV, AVI, …) would match.
 */

#include <ghoti.io/image/macros.h>
#include <string.h>

#include "../codec_internal.h"
#include "webp_internal.h"

const unsigned char gimg_webp_signature[GIMG_WEBP_SIGNATURE_LEN] = {
    'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};

/** Registered magic length: only the constant `RIFF` prefix. */
#define GIMG_WEBP_MAGIC_LEN 4

int gimg_webp_probe(
    GIMG_Stream * stream, const unsigned char * peek, size_t peeked) {
  (void)stream;
  if (!peek || peeked < GIMG_WEBP_SIGNATURE_LEN) {
    return 0;
  }
  if (memcmp(peek, "RIFF", 4) != 0) {
    return 0;
  }
  if (memcmp(peek + 8, "WEBP", 4) != 0) {
    return 0;
  }
  return 1;
}
