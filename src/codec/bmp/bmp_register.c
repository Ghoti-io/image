/**
 * @file
 *
 * Register the BMP codec with its magic probe ('B' 'M') and load/save/decode
 * callbacks.  Capabilities: read, write, palette (1/2/4/8-bit indexed bitmaps
 * are decoded through their palette), and ICC (a V5 header's PROFILE_EMBEDDED
 * is read, and one is written).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
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
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_bmp_load);
  gimg_codec_set_save_cb(codec, (gimg_codec_save_fn)gimg_bmp_save);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_bmp_decode);
  gimg_codec_set_free_doc_private(codec, gimg_bmp_free_doc_state);
  (void)gimg_codec_register(codec);
}
