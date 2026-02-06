/**
 * @file
 *
 * JPEG save stub. Returns GIMG_ERR_UNSUPPORTED until baseline encode is
 * implemented.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>

#include "jpeg_internal.h"

GIMG_Result gimg_jpeg_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report) {
  (void)codec;
  (void)doc;
  (void)stream;
  (void)format_name;
  (void)options;
  (void)report;
  return GIMG_ERR_UNSUPPORTED;
}
