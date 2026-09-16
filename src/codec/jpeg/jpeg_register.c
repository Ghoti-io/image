/**
 * @file
 *
 * Register JPEG codec with magic probe (SOI 0xFF 0xD8) and load/save/decode
 * callbacks. Capabilities: read, write, EXIF/XMP/ICC (via APP segments), CMYK.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
#include "jpeg_internal.h"

// GIMG_CAP_16BPC says the decoded raster can be 16 bits per channel, which it
// is for a 12-bit frame (left-justified).  It does not mean 16-bit JPEG: T.81
// Table B.2 has no such frame.  See documentation/format-references.md.
#define JPEG_CAPABILITIES                                                      \
  (GIMG_CAP_READ | GIMG_CAP_WRITE | GIMG_CAP_ICC | GIMG_CAP_CMYK | GIMG_CAP_16BPC)

#if defined(__GNUC__) || defined(__clang__)
#define GIMG_CONSTRUCTOR __attribute__((constructor))
#else
#define GIMG_CONSTRUCTOR
#endif

static void gimg_jpeg_register(void) GIMG_CONSTRUCTOR;

static void gimg_jpeg_register(void) {
  GIMG_Codec * codec = NULL;
  GIMG_Result r = gimg_codec_create_stub_with_allocator(
      NULL, "jpeg", gimg_jpeg_signature, GIMG_JPEG_SIGNATURE_LEN, &codec);
  if (r != GIMG_OK || !codec) {
    return;
  }
  codec->capabilities = JPEG_CAPABILITIES;
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_jpeg_load);
  gimg_codec_set_save_cb(codec, (gimg_codec_save_fn)gimg_jpeg_save);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_jpeg_decode);
  gimg_codec_set_free_doc_private(codec, gimg_jpeg_free_doc_state);
  (void)gimg_codec_register(codec);
}
