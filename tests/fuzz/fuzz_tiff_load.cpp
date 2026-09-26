/**
 * @file
 *
 * LibFuzzer harness for TIFF load and decode. Feed random, truncated or
 * hostile data; the codec must return a GIMG_ERR_* code rather than crash.
 *
 * TIFF is the format here with the largest attack surface, and it is not
 * close. Every value in it is reached by an absolute file offset that the
 * file itself supplies - a directory chains to the next by offset, a tag
 * longer than four bytes names one, and every strip and tile is one - so a
 * hostile file is a list of pointers written by the attacker. On top of that
 * the geometry comes from four independent tags that a well-formed file
 * keeps consistent and a hostile one does not: width, depth, samples per
 * pixel, and the strip or tile grid, each of which sizes a buffer the others
 * index into.
 *
 * Build with: make fuzz-tiff (uses clang -fsanitize=fuzzer,address,undefined).
 * Run: ./build/linux/release/apps/fuzz_tiff_load [corpus_dir]
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
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    GIMG_Result dr =
        gimg_item_decode(gimg_doc_item(doc, i), fuzz_decode_options(), &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
    (void)dr;  // GIMG_ERR_* expected for hostile pixel data.
  }

  gimg_doc_destroy(doc);
  return 0;
}
