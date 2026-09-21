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
 * Low-level JPEG segment I/O: read next marker (0xFF + byte), read segment
 * length (big-endian), read payload with optional limit check. No parsing of
 * SOF/DQT/DHT/SOS/APP/COM contents; used by jpeg_load.c in its segment loop.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>

#include "jpeg_internal.h"

GIMG_Result gimg_jpeg_verify_soi(GIMG_Stream * stream) {
  unsigned char buf[GIMG_JPEG_SIGNATURE_LEN];
  GIMG_Result r = gimg_stream_read_exact(stream, buf, GIMG_JPEG_SIGNATURE_LEN);
  if (r != GIMG_OK) {
    return GIMG_ERR_FORMAT;
  }
  if (buf[0] != 0xFF || buf[1] != GIMG_JPEG_MARKER_SOI) {
    return GIMG_ERR_FORMAT;
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_read_marker(GIMG_Stream * stream, uint8_t * out_marker) {
  for (;;) {
    unsigned char b = 0;
    size_t n = 0;
    GIMG_Result r = gimg_stream_read(stream, &b, 1, &n);
    if (r != GIMG_OK || n == 0) {
      return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
    }
    if (b != 0xFF) {
      continue; // Skip until 0xFF.
    }
    // T.81 B.1.1.2: "any marker may optionally be preceded by any number of
    // fill bytes, which are bytes assigned code X'FF'".  So a run of 0xFF is
    // not a marker of its own; the marker is the first byte after it that is
    // neither 0xFF nor the 0x00 of byte stuffing.  This used to read exactly
    // one byte and take whatever it found, so 0xFF 0xFF 0xC0 returned a marker
    // of 0xFF and the file was refused - a single pad byte anywhere was enough,
    // and libjpeg accepts all of them.
    do {
      r = gimg_stream_read(stream, &b, 1, &n);
      if (r != GIMG_OK || n == 0) {
        return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
      }
    } while (b == 0xFF);
    if (b == 0x00) {
      continue; // Byte stuffing: 0xFF 0x00 is data.
    }
    *out_marker = b;
    return GIMG_OK;
  }
}

GIMG_Result gimg_jpeg_read_segment_length(
    GIMG_Stream * stream, uint16_t * out_length) {
  unsigned char buf[2];
  GIMG_Result r = gimg_stream_read_exact(stream, buf, 2);
  if (r != GIMG_OK) {
    return GIMG_ERR_FORMAT;
  }
  *out_length = (uint16_t)((buf[0] << 8) | buf[1]);
  return GIMG_OK;
}
