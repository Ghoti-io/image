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
 * JPEG decode: dispatch to baseline or progressive.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>

#include "../../container/doc_internal.h"
#include "jpeg_internal.h"

/**
 * Restate a decoded raster at the precision the caller asked for.
 *
 * GIMG_Decode_Options::jpeg_precision names the depth the caller wants back,
 * and 0 means the file's own - which is what every decoder here produces, so
 * with 0 this does nothing.  It is the reading counterpart of
 * GIMG_Save_Options::jpeg_precision, and is applied in one place for the same
 * reason the writer applies its own in one place: four frame processes decode
 * here and a per-process conversion would be four chances to disagree.
 *
 * Unlike the writer's, this one is not bounded by T.81 Table B.2.  That table
 * constrains what a *frame* may state, and 16 bits is not a DCT precision -
 * but nothing stops a caller wanting a 16-bit raster out of an 8-bit file, and
 * widening is exact.  So 8, 12 and 16 are all accepted.
 *
 * A value that is none of those is refused rather than ignored.  Ignoring it
 * is what this field did for its whole life before it was implemented, and a
 * caller who writes 10 has misunderstood something that silence will not fix.
 */
static GIMG_Result jpeg_apply_decode_precision(
    const GIMG_Decode_Options * options, GIMG_Raster ** raster) {
  if (!options || options->jpeg_precision == 0) {
    return GIMG_OK;
  }
  const uint8_t want = options->jpeg_precision;
  if (want != 8 && want != 12 && want != 16) {
    gimg_raster_destroy(*raster);
    *raster = NULL;
    return GIMG_ERR_UNSUPPORTED;
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(*raster);
  if (fmt && fmt->channel_count > 0 && fmt->bits_per_channel[0] == want) {
    return GIMG_OK; // already there; a conversion would only copy
  }
  GIMG_Raster * converted = NULL;
  GIMG_Result r = gimg_ops_convert_bit_depth(*raster, want, &converted);
  if (r != GIMG_OK || !converted) {
    gimg_raster_destroy(*raster);
    *raster = NULL;
    return r != GIMG_OK ? r : GIMG_ERR_OOM;
  }
  gimg_raster_destroy(*raster);
  *raster = converted;
  return GIMG_OK;
}

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
  GIMG_Result r;
  // Non-primary items (e.g. EXIF thumbnail at index 1) have their raster
  // attached at load; return a copy so decode yields the thumbnail.
  if (item->index != 0 && item->raster) {
    r = gimg_raster_copy_with_allocator(
        doc->allocator, item->raster, out_raster);
  }
  else {
    gimg_jpeg_doc_state_t * state =
        (gimg_jpeg_doc_state_t *)doc->codec_private;
    if (state->is_hierarchical) {
      r = gimg_jpeg_decode_hierarchical(state, options, out_raster);
    }
    else if (state->is_lossless) {
      r = gimg_jpeg_decode_lossless(state, options, out_raster);
    }
    else if (state->is_progressive) {
      r = gimg_jpeg_decode_progressive(state, options, out_raster);
    }
    else {
      r = gimg_jpeg_decode_baseline(state, options, out_raster);
    }
  }
  if (r != GIMG_OK || !*out_raster) {
    return r;
  }
  // The thumbnail gets this too. It is always eight bits, so asking for
  // sixteen widens it - but a caller who asked for one precision and got two
  // different ones out of one document would have a harder bug than the
  // conversion costs.
  return jpeg_apply_decode_precision(options, out_raster);
}
