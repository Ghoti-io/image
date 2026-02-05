/**
 * @file
 *
 * Register PNG codec with magic probe and load callback.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
#include "png_internal.h"

#if defined(__GNUC__) || defined(__clang__)
#define GIMG_CONSTRUCTOR __attribute__((constructor))
#else
#define GIMG_CONSTRUCTOR
#endif

static void gimg_png_register(void) GIMG_CONSTRUCTOR;

static void gimg_png_register(void) {
  GIMG_Codec * codec = NULL;
  GIMG_Result r = gimg_codec_create_stub_with_allocator(
      NULL, "png", gimg_png_signature, GIMG_PNG_SIGNATURE_LEN, &codec);
  if (r != GIMG_OK || !codec) {
    return;
  }
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_png_load);
  gimg_codec_set_save_cb(codec, NULL);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_png_decode);
  gimg_codec_set_free_doc_private(codec, gimg_png_free_doc_state);
  (void)gimg_codec_register(codec);
}
