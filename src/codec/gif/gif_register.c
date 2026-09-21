/**
 * @file
 *
 * Register the GIF codec with its magic probe ('G' 'I' 'F') and load/decode
 * callbacks.  Capabilities: read, animation (a GIF holds a sequence of images
 * with delays and disposal methods), and palette (every GIF pixel is an index
 * into a colour table - there is no direct-colour GIF).
 *
 * GIMG_CAP_ICC is absent because the format has nowhere to put a profile, and
 * GIMG_CAP_16BPC because a colour table entry is three bytes.  GIMG_CAP_WRITE
 * is absent until the encoder lands.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>

#include "../codec_internal.h"
#include "gif_internal.h"

#define GIF_CAPABILITIES                                                       \
  (GIMG_CAP_READ | GIMG_CAP_ANIMATION | GIMG_CAP_PALETTE)

#if defined(__GNUC__) || defined(__clang__)
#define GIMG_CONSTRUCTOR __attribute__((constructor))
#else
#define GIMG_CONSTRUCTOR
#endif

static void gimg_gif_register(void) GIMG_CONSTRUCTOR;

static void gimg_gif_register(void) {
  GIMG_Codec * codec = NULL;
  GIMG_Result r = gimg_codec_create_stub_with_allocator(
      NULL, "gif", gimg_gif_signature, GIMG_GIF_SIGNATURE_LEN, &codec);
  if (r != GIMG_OK || !codec) {
    return;
  }
  codec->capabilities = GIF_CAPABILITIES;
  gimg_codec_set_load_cb(codec, (gimg_codec_load_fn)gimg_gif_load);
  gimg_codec_set_decode_cb(codec, (gimg_codec_decode_fn)gimg_gif_decode);
  gimg_codec_set_free_doc_private(codec, gimg_gif_free_doc_state);
  (void)gimg_codec_register(codec);
}
