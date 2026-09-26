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
 * Register the TIFF codec with its four magics and its load/decode callbacks.
 *
 * Capabilities are what this codec can do today, not what the format can
 * hold.  GIMG_CAP_READ and GIMG_CAP_PALETTE are set; GIMG_CAP_WRITE is not,
 * because there is no writer yet, and a capability that says otherwise would
 * put TIFF in every sweep that looks for a save target and report the
 * absence as a stream of failures rather than as the gap it is.
 *
 * GIMG_CAP_16BPC and GIMG_CAP_CMYK will follow the code that earns them.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
#include "tiff_internal.h"

#define TIFF_CAPABILITIES (GIMG_CAP_READ | GIMG_CAP_PALETTE)

#if defined(__GNUC__) || defined(__clang__)
#define GIMG_CONSTRUCTOR __attribute__((constructor))
#else
#define GIMG_CONSTRUCTOR
#endif

static void gimg_tiff_register(void) GIMG_CONSTRUCTOR;

static void gimg_tiff_register(void) {
  GIMG_Codec * codec = NULL;
  GIMG_Result r = gimg_codec_create_stub_with_allocator(
      NULL, "tiff", gimg_tiff_magic_le, GIMG_TIFF_SIGNATURE_LEN, &codec);
  if (r != GIMG_OK || !codec) {
    return;
  }
  // A codec that recognised only half the files of its own format would be
  // worse than no codec, so a magic that cannot be installed takes the whole
  // registration with it rather than registering a partial one.
  const unsigned char * rest[] = {
      gimg_tiff_magic_be, gimg_tiff_magic_le_big, gimg_tiff_magic_be_big};
  for (size_t i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
    if (gimg_codec_add_magic(codec, rest[i], GIMG_TIFF_SIGNATURE_LEN, 0u) !=
        GIMG_OK) {
      return;
    }
  }
  codec->capabilities = TIFF_CAPABILITIES;
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_tiff_load);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_tiff_decode);
  gimg_codec_set_free_doc_private(codec, gimg_tiff_free_doc_state);
  (void)gimg_codec_register(codec);
}
