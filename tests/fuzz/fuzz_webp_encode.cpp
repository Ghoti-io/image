/**
 * @file
 *
 * LibFuzzer harness for WebP encode: load whatever probes as an image, save it
 * as WebP lossless *and* lossy, and load and decode each result.
 *
 * Both compressors, on every unit, on purpose: VP8L and VP8 share a container
 * and nothing else.  The effort (0-9) and the exact flag are derived from the
 * input, each compressor with its own draw, so the search levels are reached.
 * A lossless file is compared with the source.
 *
 * The option derivation and the checks live in fuzz_encode.h, shared with the
 * corpus replay in tests/unit/test_fuzz_corpus.cpp.
 *
 * Build with: make fuzz-webp-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_webp_encode [corpus_dir]
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
  fuzz_encode_unit("webp", data, size, fuzz_option_selector(data, size));
  return 0;
}
