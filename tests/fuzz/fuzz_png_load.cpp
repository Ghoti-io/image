/**
 * @file
 *
 * LibFuzzer harness for PNG/APNG load and decode. Feed random or truncated
 * data; must not crash; expect GIMG_ERR_FORMAT, GIMG_ERR_CORRUPT, or
 * GIMG_ERR_LIMIT as appropriate.
 *
 * Build with: make fuzz-png (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_png_load [corpus_dir]
 * Minimal corpus in tests/fuzz/corpus (e.g. copies of the PNG files under
 * tests/data/png).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <cstddef>
#include <cstdint>

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

  if (r != GIMG_OK) {
    // Expected for invalid/truncated input: FORMAT, CORRUPT, LIMIT, etc.
    return 0;
  }

  if (doc == nullptr) {
    return 0;
  }

  // Decode every item/frame to exercise full decode path and avoid crashes on
  // invalid or truncated multi-frame input.
  size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    GIMG_Result dr =
        gimg_item_decode(gimg_doc_item(doc, i), nullptr, &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
    (void)dr;  // GIMG_ERR_* expected for invalid/truncated frames
  }

  gimg_doc_destroy(doc);
  return 0;
}
