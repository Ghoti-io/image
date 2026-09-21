/**
 * @file
 *
 * LibFuzzer harness for the GIF writer: load -> decode every frame -> save ->
 * load -> decode again.  Must not crash.  Input that fails load or save is
 * ignored (return 0); the writer refusing an image is an answer, not a fault.
 *
 * The GIF writer is the one in this library that reasons about more than the
 * frame in front of it.  It carries the logical screen from frame to frame,
 * crops each frame to where it differs from that screen, marks the pixels
 * inside that rectangle which match the screen as transparent, and grows a
 * frame's rectangle and flips its disposal when the frame after it needs
 * something erased - all of it decided one frame behind, with the wrap from
 * the last frame back to the first treated as a transition like any other.
 * Every one of those is an index or a rectangle derived from two frames at
 * once, which a single-frame harness cannot reach.  So the input worth giving
 * it is a whole animation, which is what a GIF in the corpus already is.
 *
 * The two save options that change the shape of the output rather than its
 * content - interlacing, and the alpha threshold that decides which way a
 * partly transparent pixel rounds - are driven from the input so that one
 * corpus entry can explore both.
 *
 * Build with: make fuzz-gif-encode (uses clang -fsanitize=fuzzer).
 * Run: ./build/linux/release/apps/fuzz_gif_encode [corpus_dir]
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

namespace {

/** Decode every frame and throw the pixels away; the point is the walking. */
void decode_all(GIMG_Doc * doc) {
  const size_t n = gimg_doc_item_count(doc);
  for (size_t i = 0; i < n; i++) {
    GIMG_Raster * raster = nullptr;
    (void)gimg_item_decode(gimg_doc_item(doc, i), nullptr, &raster);
    if (raster != nullptr) {
      gimg_raster_destroy(raster);
    }
  }
}

} // namespace

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

  decode_all(doc);

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  if (r != GIMG_OK || out_s == nullptr) {
    gimg_doc_destroy(doc);
    return 0;
  }

  GIMG_Save_Options opts = {};
  opts.metadata_policy = fuzz_save_policy(data, size);
  // The last byte steers the two options that change the shape of the output:
  // whether rows are woven into the four-pass order, and where a partly
  // transparent pixel rounds.  A threshold of 0 means "refuse rather than
  // guess", which is a path worth reaching as often as the others.
  opts.gif_interlace = static_cast<uint8_t>(data[size - 1u] & 0x01u);
  opts.gif_alpha_threshold =
      static_cast<uint16_t>((data[size - 1u] >> 1) & 0x7Fu);

  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "gif", &opts, &report);
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

  // Re-load what was written.  The memory stream does not copy, so out_s has
  // to outlive the stream reading from its buffer.
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
    decode_all(doc2);
    gimg_doc_destroy(doc2);
  }
  return 0;
}
