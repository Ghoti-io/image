/**
 * @file
 *
 * BMP file signature for probe ('B' 'M').
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include "bmp_internal.h"

const unsigned char gimg_bmp_signature[GIMG_BMP_SIGNATURE_LEN] = {0x42, 0x4D};

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
