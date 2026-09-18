/**
 * @file
 *
 * LibFuzzer harness for PNG/APNG round-trip: load -> decode all items ->
 * save -> load. Stress the encoder with random or truncated input; must not
 * crash. Input that fails load or save is ignored (return 0).
 *
 * Build with: make fuzz-png-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_png_encode [corpus_dir]
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
    return 0;
  }

  // Decode every item to exercise decode path before save.
  size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    (void)gimg_item_decode(gimg_doc_item(doc, i), nullptr, &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
  }

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  if (r != GIMG_OK || out_s == nullptr) {
    gimg_doc_destroy(doc);
    return 0;
  }

  // Value-initialize first: a designated initializer that names one member
  // leaves the rest zeroed, but clang warns about it under -Wextra, and the
  // fuzz harnesses build with -Werror.  Zeroing and then assigning says the
  // same thing without the warning, and keeps building when the struct grows.
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  if (r != GIMG_OK) {
    gimg_stream_destroy(out_s);
    gimg_doc_destroy(doc);
    return 0;
  }

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  if (out_data == nullptr || out_size == 0) {
    gimg_stream_destroy(out_s);
    gimg_doc_destroy(doc);
    return 0;
  }

  // Re-load from encoded output. Keep out_s alive until s2 is destroyed
  // (memory stream does not copy; s2 points into out_s buffer).
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(out_data, out_size, &s2);
  if (r != GIMG_OK || s2 == nullptr) {
    gimg_stream_destroy(out_s);
    gimg_doc_destroy(doc);
    return 0;
  }
  gimg_doc_destroy(doc);
  doc = nullptr;

  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, fuzz_load_options(), nullptr, &doc2);
  gimg_stream_destroy(s2);
  s2 = nullptr;
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  if (r == GIMG_OK && doc2 != nullptr) {
    size_t n2 = gimg_doc_item_count(doc2);
    for (size_t i = 0; i < n2; i++) {
      GIMG_Raster * raster = nullptr;
      (void)gimg_item_decode(gimg_doc_item(doc2, i), nullptr, &raster);
      if (raster != nullptr) {
        gimg_raster_destroy(raster);
      }
    }
    gimg_doc_destroy(doc2);
  }

  return 0;
}
