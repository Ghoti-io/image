/**
 * @file
 *
 * JPEG decode: dispatch to baseline or progressive.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/raster.h>

#include "../../container/doc_internal.h"
#include "jpeg_internal.h"

GIMG_Result gimg_jpeg_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster) {
  if (!codec || !item || !out_raster) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Doc * doc = item->doc;
  if (!doc || doc->loaded_by_codec != codec || !doc->codec_private) {
    return GIMG_ERR_UNSUPPORTED;
  }
  gimg_jpeg_doc_state_t * state =
      (gimg_jpeg_doc_state_t *)doc->codec_private;
  if (state->is_progressive) {
    return gimg_jpeg_decode_progressive(state, options, out_raster);
  }
  return gimg_jpeg_decode_baseline(state, options, out_raster);
}
