/**
 * @file
 *
 * GIF file signature for probe ('G' 'I' 'F').
 *
 * The version that follows - "87a" or "89a" - is deliberately not part of the
 * magic.  Files in the wild carry either one regardless of which blocks they
 * use, so matching on it would refuse readable files and admit nothing extra.
 * Load records the version; nothing branches on it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include "gif_internal.h"

const unsigned char gimg_gif_signature[GIMG_GIF_SIGNATURE_LEN] = {
    0x47, 0x49, 0x46};

GIMG_Result gimg_gif_verify_signature(GIMG_Stream * stream) {
  unsigned char magic[GIMG_GIF_SIGNATURE_LEN];
  GIMG_Result r = gimg_stream_read_exact(stream, magic, sizeof(magic));
  if (r != GIMG_OK) {
    return r;
  }
  for (size_t i = 0; i < sizeof(magic); i++) {
    if (magic[i] != gimg_gif_signature[i]) {
      return GIMG_ERR_FORMAT;
    }
  }
  return GIMG_OK;
}
