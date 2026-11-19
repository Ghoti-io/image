/**
 * @file
 *
 * LibFuzzer harness for the GIF writer: load -> decode every frame -> save ->
 * load -> decode again.  The writer reasons about more than the frame in
 * front of it (it carries the logical screen from frame to frame, crops,
 * marks unchanged pixels transparent and flips disposal), so the input worth
 * giving it is a whole animation.  Interlacing, the alpha threshold and the
 * loop count are derived from the input.
 *
 * The option derivation and the checks live in fuzz_encode.h, shared with the
 * corpus replay in tests/unit/test_fuzz_corpus.cpp.
 *
 * Build with: make fuzz-gif-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_gif_encode [corpus_dir]
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
  fuzz_encode_unit("gif", data, size, fuzz_option_selector(data, size));
  return 0;
}
