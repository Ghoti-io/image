/**
 * @file
 *
 * LibFuzzer harness for TIFF round-trip: load -> decode -> save as TIFF ->
 * load again. Stresses the encoder with whatever the decoder was willing to
 * produce, including rasters from other formats, since gimg_doc_load
 * dispatches on the bytes rather than on this harness's name.
 *
 * The writer has its own reason to be fuzzed and it is not the usual one.
 * It lays the whole file out on paper before writing a byte - every strip
 * offset, every value pool, every directory - so a raster that makes one of
 * those numbers unusual makes *all* of them unusual at once. A document of
 * many items, a page one pixel wide, a 16-bit CMYK raster: each moves the
 * arithmetic that every later offset is computed from.
 *
 * The save options are varied with the input so that the four TIFF ones -
 * compression, predictor, byte order, rows per strip - are all reached,
 * rather than every run exercising the default file.
 *
 * Build with: make fuzz-tiff-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_tiff_encode [corpus_dir]
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
    return 0;
  }

  // Attach a raster so the save path takes a decoded item rather than
  // re-decoding one; both routes are exercised across a corpus, since an item
  // whose decode fails stays bare.
  size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    (void)gimg_item_ensure_decoded(gimg_doc_item(doc, i), fuzz_decode_options());
  }

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  if (r != GIMG_OK || out_s == nullptr) {
    gimg_doc_destroy(doc);
    return 0;
  }

  GIMG_Save_Options opts = {};
  opts.metadata_policy = fuzz_save_policy(data, size);
  // Every option, chosen from the input so that one corpus reaches all of
  // them. A harness that only ever wrote the default file would leave the
  // compressors, the predictor and the big-endian layout unfuzzed while
  // reporting a clean run over the writer.
  opts.tiff_compression = (uint8_t)(data[0] & 3u);
  opts.tiff_predictor = (uint8_t)((data[0] >> 2) & 1u) ? 2u : 0u;
  opts.tiff_big_endian = (uint8_t)((data[0] >> 3) & 1u);
  opts.tiff_rows_per_strip = (uint32_t)(data[size - 1u] & 7u);
  if (opts.tiff_compression == 0u) {
    // The writer refuses a predictor with nothing behind it, on purpose.
    opts.tiff_predictor = 0u;
  }
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "tiff", &opts, &report);
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

  // Keep out_s alive until s2 is destroyed: the memory stream reads the
  // output buffer in place rather than copying it.
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
  gimg_stream_destroy(out_s);

  if (r == GIMG_OK && doc2 != nullptr) {
    size_t n2 = gimg_doc_item_count(doc2);
    for (size_t i = 0; i < n2; i++) {
      GIMG_Raster * raster = nullptr;
      (void)gimg_item_decode(
          gimg_doc_item(doc2, i), fuzz_decode_options(), &raster);
      if (raster != nullptr) {
        gimg_raster_destroy(raster);
      }
    }
    gimg_doc_destroy(doc2);
  }

  return 0;
}
