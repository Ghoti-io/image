/**
 * @file
 *
 * LibFuzzer harness for JPEG load and decode. Feed random or truncated
 * data; must not crash; expect GIMG_ERR_FORMAT, GIMG_ERR_CORRUPT, or
 * GIMG_ERR_LIMIT as appropriate.
 *
 * Build with: make fuzz-jpeg (uses clang -fsanitize=fuzzer).
 * Run:        make fuzz-run-jpeg_load FUZZ_TIME=300
 * Seeds live in tests/fuzz/corpus/jpeg_load/*.seed; campaign units are ignored.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>

#include "fuzz_limits.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  fuzz_select_arm(data, size);

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
    return 0;
  }

  if (doc == nullptr) {
    return 0;
  }

  // The limits are the harness-wide ones (fuzz_limits.h), which also run the
  // library's defaults and a small memory cap.
  const GIMG_Decode_Options * opts = fuzz_decode_options();

  size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    GIMG_Result dr = gimg_item_decode(gimg_doc_item(doc, i), opts, &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
    (void)dr;
  }

  gimg_doc_destroy(doc);
  return 0;
}
