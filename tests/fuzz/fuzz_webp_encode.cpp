/**
 * @file
 *
 * LibFuzzer harness for WebP encode: load whatever probes as an image, save it
 * as WebP lossless *and* lossy, and load each result back.
 *
 * Both, on every unit, on purpose. VP8L and VP8 share a container and nothing
 * else - two entropy coders, two residual models, two sets of allocations - so
 * a harness that exercised only the default compressor would leave the whole
 * lossy encoder unreached, which is the larger half of the two.
 *
 * The input is the picture and nothing else, as in every other encode harness
 * here. An earlier draft took the first byte as a compressor selector, which
 * would have made every seed in tests/fuzz/corpus/webp_load useless as a seed
 * here: the byte it ate is the `R` of `RIFF`.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>

#include "fuzz_limits.h"

namespace {

/** Save @p doc as WebP with this compressor, then load and decode it back. */
void round_trip(GIMG_Doc * doc, uint8_t compressor) {
  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    return;
  }
  GIMG_Save_Report report = {};
  GIMG_Save_Options opts = {};
  opts.webp_lossless = compressor;
  (void)gimg_doc_save(doc, out, "webp", &opts, &report);

  const void * buf = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &buf, &n);
  if (buf && n) {
    GIMG_Stream * back = nullptr;
    if (gimg_stream_create_memory(buf, n, &back) == GIMG_OK) {
      GIMG_Doc * round = nullptr;
      (void)gimg_doc_load(back, fuzz_load_options(), nullptr, &round);
      if (round) {
        // Decode as well as load. A writer that emits a header the reader
        // accepts over a payload it cannot finish is the interesting failure,
        // and stopping at load would not reach it.
        const size_t items = gimg_doc_item_count(round);
        for (size_t i = 0; i < items && i < 8u; ++i) {
          GIMG_Raster * r = nullptr;
          if (gimg_item_decode(gimg_doc_item(round, i), fuzz_decode_options(),
                  &r) == GIMG_OK &&
              r) {
            gimg_raster_destroy(r);
          }
        }
        gimg_doc_destroy(round);
      }
      gimg_stream_destroy(back);
    }
  }
  gimg_stream_destroy(out);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  if (data == nullptr || size == 0) {
    return 0;
  }

  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(data, size, &s) != GIMG_OK || !s) {
    return 0;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, fuzz_load_options(), nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK || !doc) {
    return 0;
  }

  // Attach a decoded raster so save has pixels rather than a lazy item.
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), fuzz_decode_options(), &raster) ==
          GIMG_OK &&
      raster) {
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  }

  round_trip(doc, GIMG_WEBP_COMPRESS_LOSSLESS);
  round_trip(doc, GIMG_WEBP_COMPRESS_LOSSY);
  gimg_doc_destroy(doc);
  return 0;
}
