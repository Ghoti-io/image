/**
 * @file
 *
 * LibFuzzer harness for ICO encode: load whatever probes as an image, save as
 * ICO with a payload kind (automatic, DIB or PNG) derived from the input, load
 * the result back and decode every entry.
 *
 * The option derivation and the checks live in fuzz_encode.h, shared with the
 * corpus replay in tests/unit/test_fuzz_corpus.cpp.
 *
 * Build with: make fuzz-ico-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_ico_encode [corpus_dir]
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
  fuzz_encode_unit("ico", data, size, fuzz_option_selector(data, size));
  return 0;
}
