/**
 * @file
 *
 * LibFuzzer harness for PNG/APNG load and decode. Feed random or truncated
 * data; must not crash; expect GIMG_ERR_FORMAT, GIMG_ERR_CORRUPT, or
 * GIMG_ERR_LIMIT as appropriate.
 *
 * Build with: make fuzz-png (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_png_load [corpus_dir]
 * Minimal corpus in tests/fuzz/corpus/ (e.g. copy of tests/data/png/*.png).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <cstddef>
#include <cstdint>

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
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  s = nullptr;

  if (r != GIMG_OK) {
    // Expected for invalid/truncated input: FORMAT, CORRUPT, LIMIT, etc.
    return 0;
  }

  if (doc == nullptr) {
    return 0;
  }

  // Decode first item to exercise full decode path.
  size_t n = gimg_doc_item_count(doc);
  if (n > 0) {
    GIMG_Raster * raster = nullptr;
    GIMG_Result dr = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
    (void)dr;
  }

  gimg_doc_destroy(doc);
  return 0;
}
