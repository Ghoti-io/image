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
 * Register the ICO/CUR codec. One name ("ico") covers both file types.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
#include "ico_internal.h"

#define ICO_CAPABILITIES (GIMG_CAP_READ | GIMG_CAP_WRITE)

#if defined(__GNUC__) || defined(__clang__)
#define GIMG_CONSTRUCTOR __attribute__((constructor))
#else
#define GIMG_CONSTRUCTOR
#endif

static void gimg_ico_register(void) GIMG_CONSTRUCTOR;

static void gimg_ico_register(void) {
  GIMG_Codec * codec = NULL;
  GIMG_Result r = gimg_codec_create_stub_with_allocator(
      NULL, "ico", gimg_ico_signature, GIMG_ICO_SIGNATURE_LEN, &codec);
  if (r != GIMG_OK || !codec) {
    return;
  }
  r = gimg_codec_add_magic(
      codec, gimg_cur_signature, GIMG_ICO_SIGNATURE_LEN, 0u);
  if (r != GIMG_OK) {
    return;
  }
  codec->capabilities = ICO_CAPABILITIES;
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_ico_load);
  gimg_codec_set_save_cb(codec, (gimg_codec_save_fn)gimg_ico_save);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_ico_decode);
  gimg_codec_set_free_doc_private(codec, gimg_ico_free_doc_state);
  gimg_codec_set_probe_cb(codec, gimg_ico_probe);
  (void)gimg_codec_register(codec);
}
