/**
 * @file
 *
 * LibFuzzer harness for ICO encode: load whatever probes as an image, save as
 * ICO, load the result back.
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

  // Decode first item so save has pixels (or attached raster).
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), fuzz_decode_options(), &raster) ==
          GIMG_OK &&
      raster) {
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  }

  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return 0;
  }
  GIMG_Save_Report report = {};
  GIMG_Save_Options opts = {};
  opts.ico_payload = GIMG_ICO_PAYLOAD_AUTO;
  (void)gimg_doc_save(doc, out, "ico", &opts, &report);
  gimg_doc_destroy(doc);

  const void * buf = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &buf, &n);
  if (buf && n) {
    GIMG_Stream * back = nullptr;
    if (gimg_stream_create_memory(buf, n, &back) == GIMG_OK) {
      GIMG_Doc * round = nullptr;
      (void)gimg_doc_load(back, fuzz_load_options(), nullptr, &round);
      if (round) {
        gimg_doc_destroy(round);
      }
      gimg_stream_destroy(back);
    }
  }
  gimg_stream_destroy(out);
  return 0;
}
