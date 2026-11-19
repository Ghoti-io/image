/**
 * @file
 *
 * LibFuzzer harness for TIFF round-trip: load -> decode -> save as TIFF ->
 * load again.  Stresses the encoder with whatever the decoder was willing to
 * produce, including rasters from other formats, since gimg_doc_load
 * dispatches on the bytes rather than on this harness's name.
 *
 * The writer lays the whole file out on paper before writing a byte - every
 * strip offset, value pool and directory - so a raster that makes one of those
 * numbers unusual makes all of them unusual.  Compression, predictor, byte
 * order, BigTIFF and rows per strip are derived from the input; TIFF is
 * lossless, so the picture is compared.
 *
 * The option derivation and the checks live in fuzz_encode.h, shared with the
 * corpus replay in tests/unit/test_fuzz_corpus.cpp.
 *
 * Build with: make fuzz-tiff-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_tiff_encode [corpus_dir]
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>

#include "fuzz_encode.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  fuzz_select_arm(data, size);
  fuzz_encode_unit("tiff", data, size, fuzz_option_selector(data, size));
  return 0;
}
