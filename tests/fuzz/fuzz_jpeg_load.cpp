/**
 * @file
 *
 * LibFuzzer harness for JPEG load and decode. Feed random or truncated
 * data; must not crash; expect GIMG_ERR_FORMAT, GIMG_ERR_CORRUPT, or
 * GIMG_ERR_LIMIT as appropriate.
 *
 * Build with: make fuzz-jpeg (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_jpeg_load [corpus_dir]
 * Minimal corpus in tests/fuzz/corpus/ (e.g. copy JPEG files from
 * tests/data/jpeg/).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>

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
    return 0;
  }

  if (doc == nullptr) {
    return 0;
  }

  // Cap the decoded size.  A JPEG header is a handful of bytes and can ask for
  // an image of any size the fields allow, so without a limit the fuzzer spends
  // its time rediscovering that a 580-byte file declaring 25889x27248 needs
  // gigabytes - which is not a defect, it is what GIMG_Limits is for - instead
  // of exploring the decoders.  Callers that care set this; the library does
  // not impose a default, because what counts as too large is the caller's
  // judgement.
  GIMG_Limits limits = {};
  limits.max_decoded_pixels = 4u * 1024u * 1024u;
  GIMG_Decode_Options opts = {};
  opts.limits = &limits;

  size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    GIMG_Result dr = gimg_item_decode(gimg_doc_item(doc, i), &opts, &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
    (void)dr;
  }

  gimg_doc_destroy(doc);
  return 0;
}
