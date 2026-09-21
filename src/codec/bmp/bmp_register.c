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
 * Register the BMP codec with its two magic probes ('B' 'M', and the OS/2
 * bitmap array's 'B' 'A') and load/save/decode callbacks.  Capabilities:
 * read, write, palette (1/2/4/8-bit indexed bitmaps are decoded through their
 * palette), and ICC (a V5 header's PROFILE_EMBEDDED is read, and one is
 * written; PROFILE_LINKED is reported as a path, never followed).
 *
 * GIMG_CAP_ANIMATION stays off even though a 'BA' file decodes to several
 * items: the entries are alternative renditions of one picture, not frames.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
#include "../../core/alloc_internal.h"
#include "bmp_internal.h"

// GIMG_CAP_16BPC is deliberately absent: a BMP sample is a byte at most, so a
// deeper raster is restated at 8 bits on the way in rather than written as it
// stands.  GIMG_CAP_CMYK likewise - the writer refuses a CMYK raster rather
// than reinterpreting its four channels.  See documentation/formats/bmp.md.
#define BMP_CAPABILITIES                                                       \
  (GIMG_CAP_READ | GIMG_CAP_WRITE | GIMG_CAP_PALETTE | GIMG_CAP_ICC)

#if defined(__GNUC__) || defined(__clang__)
#define GIMG_CONSTRUCTOR __attribute__((constructor))
#else
#define GIMG_CONSTRUCTOR
#endif

static void gimg_bmp_register(void) GIMG_CONSTRUCTOR;

static void gimg_bmp_register(void) {
  GIMG_Codec * codec = NULL;
  GIMG_Result r = gimg_codec_create_stub_with_allocator(
      NULL, "bmp", gimg_bmp_signature, GIMG_BMP_SIGNATURE_LEN, &codec);
  if (r != GIMG_OK || !codec) {
    return;
  }
  codec->capabilities = BMP_CAPABILITIES;

  // A second magic for the OS/2 'BA' bitmap array, so that such a file is
  // claimed by this codec rather than falling through as an unrecognized
  // format.  The stub creator takes one magic; the registry has always
  // iterated however many a codec carries.
  gimg_codec_magic_t * magics = (gimg_codec_magic_t *)gimg_malloc(
      gimg_alloc_or_default(codec->allocator),
      2u * sizeof(gimg_codec_magic_t));
  if (magics) {
    magics[0] = codec->magics[0];
    magics[1].bytes = gimg_bmp_array_signature;
    magics[1].length = GIMG_BMP_SIGNATURE_LEN;
    magics[1].offset = 0;
    // magics[0] still owns the copied 'BM' bytes; only the array itself is
    // replaced, and the old one is freed here rather than leaked.
    gimg_free(gimg_alloc_or_default(codec->allocator), codec->magics);
    codec->magics = magics;
    codec->magic_count = 2;
  }
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_bmp_load);
  gimg_codec_set_save_cb(codec, (gimg_codec_save_fn)gimg_bmp_save);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_bmp_decode);
  gimg_codec_set_free_doc_private(codec, gimg_bmp_free_doc_state);
  (void)gimg_codec_register(codec);
}
