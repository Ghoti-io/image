/**
 * @file
 *
 * Low-level JPEG segment I/O: read next marker (0xFF + byte), read segment
 * length (big-endian), read payload with optional limit check. No parsing of
 * SOF/DQT/DHT/SOS/APP/COM contents; used by jpeg_load.c in its segment loop.
 *
 * Copyright 2026 by Corey Pennycuff
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
    r = gimg_stream_read(stream, &b, 1, &n);
    if (r != GIMG_OK || n == 0) {
      return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
    }
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
