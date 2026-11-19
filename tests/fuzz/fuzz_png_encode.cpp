/**
 * @file
 *
 * LibFuzzer harness for PNG/APNG round-trip: load -> decode all items ->
 * save -> load -> decode.  The save options (row filter, palette policy,
 * Adam7, metadata policy, EXIF thumbnail) are derived from the input, and the
 * picture is compared after the save: PNG is lossless.
 *
 * The option derivation and the checks live in fuzz_encode.h, shared with the
 * corpus replay in tests/unit/test_fuzz_corpus.cpp.
 *
 * Build with: make fuzz-png-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_png_encode [corpus_dir]
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
  fuzz_encode_unit("png", data, size, fuzz_option_selector(data, size));
  return 0;
}
