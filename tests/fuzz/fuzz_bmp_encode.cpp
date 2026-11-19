/**
 * @file
 *
 * LibFuzzer harness for BMP round-trip: load -> decode -> save as BMP ->
 * load again.  Palette policy, RLE4/RLE8/RLE24, OS/2 Huffman, 2-bit indices,
 * top-down rows and the JPEG and PNG wrappers are derived from the input;
 * everything but the JPEG wrapper is lossless and is compared.
 *
 * The option derivation and the checks live in fuzz_encode.h, shared with the
 * corpus replay in tests/unit/test_fuzz_corpus.cpp.
 *
 * Build with: make fuzz-bmp-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_bmp_encode [corpus_dir]
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
  fuzz_encode_unit("bmp", data, size, fuzz_option_selector(data, size));
  return 0;
}
