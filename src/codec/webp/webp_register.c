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
 * Register the WebP codec. Phases A–E: container, VP8L, ALPH, VP8 keyframes,
 * ANIM/ANMF. Encode remains Phase F.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
#include "webp_internal.h"

#define WEBP_CAPABILITIES (GIMG_CAP_READ | GIMG_CAP_ICC)
#define GIMG_WEBP_MAGIC_LEN 4

#if defined(__GNUC__) || defined(__clang__)
#define GIMG_CONSTRUCTOR __attribute__((constructor))
#else
#define GIMG_CONSTRUCTOR
#endif

int gimg_webp_probe(
    GIMG_Stream * stream, const unsigned char * peek, size_t peeked);

static void gimg_webp_register(void) GIMG_CONSTRUCTOR;

static void gimg_webp_register(void) {
  GIMG_Codec * codec = NULL;
  GIMG_Result r = gimg_codec_create_stub_with_allocator(
      NULL, "webp", gimg_webp_signature, GIMG_WEBP_MAGIC_LEN, &codec);
  if (r != GIMG_OK || !codec) {
    return;
  }
  codec->capabilities = WEBP_CAPABILITIES;
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_webp_load);
  gimg_codec_set_save_cb(codec, (gimg_codec_save_fn)gimg_webp_save);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_webp_decode);
  gimg_codec_set_free_doc_private(codec, gimg_webp_free_doc_state);
  gimg_codec_set_probe_cb(codec, gimg_webp_probe);
  (void)gimg_codec_register(codec);
}
