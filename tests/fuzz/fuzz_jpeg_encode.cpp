/**
 * @file
 *
 * LibFuzzer harness for JPEG round-trip: load -> decode all items ->
 * save -> load -> decode.  The writer's modes are derived from the input:
 * baseline, progressive (default and caller scan scripts), arithmetic,
 * lossless predictors, hierarchical, non-interleaved and both abbreviated
 * forms, with quality, subsampling, precision and restart intervals.  A
 * lossless frame is compared; a lossy one must decode.
 *
 * The option derivation and the checks live in fuzz_encode.h, shared with the
 * corpus replay in tests/unit/test_fuzz_corpus.cpp.
 *
 * Build with: make fuzz-jpeg-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_jpeg_encode [corpus_dir]
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
  fuzz_encode_unit("jpeg", data, size, fuzz_option_selector(data, size));
  return 0;
}
