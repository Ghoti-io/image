/**
 * @file
 *
 * LibFuzzer harness for PNG/APNG load and decode. Feed random or truncated
 * data; must not crash; expect GIMG_ERR_FORMAT, GIMG_ERR_CORRUPT, or
 * GIMG_ERR_LIMIT as appropriate.
 *
 * Build with: make fuzz-png (uses clang -fsanitize=fuzzer).
 * Run:        make fuzz-run-png_load FUZZ_TIME=300
 * Seeds live in tests/fuzz/corpus/png_load/*.seed; campaign units are ignored.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <cstddef>
#include <cstdint>

#include "fuzz_limits.h"

#include <vector>

/**
 * Put right the CRC of every chunk a copy of the input names.
 *
 * Every chunk is checked against its CRC before anything in it is read, and a
 * mutation of a byte the CRC covers breaks it, so most of what the fuzzer
 * produces dies at the first chunk it touches and the parsers behind that
 * check are reached only by inputs that happen to keep it.  Fixing the CRCs
 * up lets the mutated chunk be read; half the inputs are left as they came, so
 * the check itself is still fuzzed.
 */
static void fix_png_crcs(std::vector<uint8_t> & b) {
  size_t at = 8; // past the signature
  while (at + 12 <= b.size()) {
    const size_t len = ((size_t)b[at] << 24) | ((size_t)b[at + 1] << 16) |
        ((size_t)b[at + 2] << 8) | (size_t)b[at + 3];
    if (len > b.size() || at + 12 + len > b.size()) {
      return;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = at + 4; i < at + 8 + len; i++) {
      c ^= b[i];
      for (int k = 0; k < 8; k++) {
        c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
      }
    }
    c = ~c;
    const size_t crc_at = at + 8 + len;
    b[crc_at] = (uint8_t)(c >> 24);
    b[crc_at + 1] = (uint8_t)(c >> 16);
    b[crc_at + 2] = (uint8_t)(c >> 8);
    b[crc_at + 3] = (uint8_t)c;
    at = crc_at + 4;
  }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }
  fuzz_select_arm(data, size);

  std::vector<uint8_t> fixed;
  if (size > 8 && (data[size - 1] & 1u) == 0u) {
    fixed.assign(data, data + size);
    fix_png_crcs(fixed);
    data = fixed.data();
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

  if (r != GIMG_OK) {
    // Expected for invalid/truncated input: FORMAT, CORRUPT, LIMIT, etc.
    return 0;
  }

  if (doc == nullptr) {
    return 0;
  }

  // Decode every item/frame to exercise full decode path and avoid crashes on
  // invalid or truncated multi-frame input.
  size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    GIMG_Result dr =
        gimg_item_decode(gimg_doc_item(doc, i), fuzz_decode_options(), &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
    (void)dr;  // GIMG_ERR_* expected for invalid/truncated frames
  }

  gimg_doc_destroy(doc);
  return 0;
}
