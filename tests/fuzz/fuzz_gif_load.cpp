/**
 * @file
 *
 * LibFuzzer harness for GIF load and decode. Feed random, truncated or
 * hostile data; the codec must return a GIMG_ERR_* code rather than crash.
 *
 * GIF's parser has three shapes worth pointing sanitizers at. A sub-block
 * chain declares no total, so its length is discovered 255 bytes at a time and
 * bounded only by the caller's limit. An image descriptor's position and size
 * are independent of the logical screen, so a frame can claim to sit outside
 * the canvas it is composited onto. And the LZW code size is set
 * independently of the colour table, so a code stream can name an index the
 * table does not have.
 *
 * Decoding every item matters here more than in a still format: a GIF frame
 * is composited over the frames before it, so item N exercises a replay of
 * 0..N and the disposal handling between them.
 *
 * They are decoded twice, forward and then backward, because the decoder
 * keeps the composited canvas between calls.  Forward is the path where that
 * cache is read back and built on; backward is the path where it is always
 * for a later frame and so has to be recognized as unusable.  Decoding a
 * document whose later frames fail also leaves a cache behind for a prefix
 * that succeeded, which only a second pass reaches.
 *
 * Build with: make fuzz-gif (uses clang -fsanitize=fuzzer,address,undefined).
 * Run: ./build/linux/release/apps/fuzz_gif_load [corpus_dir]
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>

#include "fuzz_limits.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(data, size, &s);
  if (r != GIMG_OK || s == nullptr) {
    return 0;
  }

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, fuzz_load_options(), nullptr, &doc);
  gimg_stream_destroy(s);
  s = nullptr;

  if (r != GIMG_OK || doc == nullptr) {
    // Expected for invalid or truncated input.
    return 0;
  }

  size_t n = gimg_doc_item_count(doc);
  for (size_t pass = 0; pass < 2u; pass++) {
    for (size_t k = 0; k < n; k++) {
      const size_t i = pass ? n - 1u - k : k;
      GIMG_Raster * raster = nullptr;
      GIMG_Result dr = gimg_item_decode(
          gimg_doc_item(doc, i), fuzz_decode_options(), &raster);
      if (raster != nullptr) {
        gimg_raster_destroy(raster);
      }
      (void)dr;  // GIMG_ERR_* expected for hostile pixel data.
    }
  }

  gimg_doc_destroy(doc);
  return 0;
}
