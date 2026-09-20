/**
 * @file
 *
 * LibFuzzer harness for BMP load and decode. Feed random, truncated or
 * hostile data; the codec must return a GIMG_ERR_* code rather than crash.
 *
 * BMP is the parser in this library that most directly indexes a buffer from
 * sizes the header supplied - stride from biWidth and biBitCount, palette
 * indices against an entry count, RLE runs against a row - so it is the one
 * that most needs the sanitizers pointed at it.
 *
 * Build with: make fuzz-bmp (uses clang -fsanitize=fuzzer,address,undefined).
 * Run: ./build/linux/release/apps/fuzz_bmp_load [corpus_dir]
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
