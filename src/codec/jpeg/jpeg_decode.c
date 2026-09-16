/**
 * @file
 *
 * JPEG decode: dispatch to baseline or progressive.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
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
  *out_raster = NULL;
  GIMG_Doc * doc = item->doc;
  if (!doc || doc->loaded_by_codec != codec || !doc->codec_private) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // Non-primary items (e.g. EXIF thumbnail at index 1) have their raster
  // attached at load; return a copy so decode yields the thumbnail.
  if (item->index != 0 && item->raster) {
    return gimg_raster_copy_with_allocator(
        doc->allocator, item->raster, out_raster);
  }
  gimg_jpeg_doc_state_t * state =
      (gimg_jpeg_doc_state_t *)doc->codec_private;
  if (state->is_lossless) {
    return gimg_jpeg_decode_lossless(state, options, out_raster);
  }
  if (state->is_progressive) {
    return gimg_jpeg_decode_progressive(state, options, out_raster);
  }
  return gimg_jpeg_decode_baseline(state, options, out_raster);
}
