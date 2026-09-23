/**
 * @file
 *
 * The writer configurations worth sweeping, and a raster to feed them.
 *
 * Two sweeps walk this same list - one injecting an allocation failure, one a
 * sink that runs out of room - and they ask different questions of the same
 * code.  Keeping the list in one place is what makes both of them widen the
 * day somebody adds a writer option: a case added here is a case both sweeps
 * start running, rather than one sweep learning about it and the other not.
 *
 * The cases are not one writer with a size swept over it.  Each reaches a
 * different half of the JPEG writer - a progressive scan script, a lossless
 * frame, a hierarchical pyramid, a twelve-bit frame, a CMYK frame with its
 * Adobe marker, a non-interleaved scan order - and a sweep is only ever as
 * wide as the paths its fixtures walk.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_TESTS_SAVE_CASES_H
#define GHOTI_IO_GIMG_TESTS_SAVE_CASES_H

#include <cstdint>
#include <vector>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/raster.h>

namespace gimg_test {

/**
 * A raster with something in it worth coding.
 *
 * A flat fill would quantize to a DC term and nothing else, and several of
 * the writer's arms only run when a block has AC coefficients to emit, so the
 * pattern is a gradient with a per-pixel perturbation: deterministic, but not
 * constant along either axis or within a block.
 */
inline GIMG_Raster * make_raster(
    const GIMG_Pixel_Format & fmt, uint32_t w, uint32_t h, unsigned levels) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, &fmt, GIMG_RASTER_OWNED, nullptr, 0, &r)
      != GIMG_OK) {
    return nullptr;
  }
  unsigned char * px = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  const unsigned ch = fmt.channel_count;
  const unsigned bits = fmt.bits_per_channel[0];
  const unsigned max = (1u << (bits > 12u ? 12u : bits)) - 1u;
  // GIF refuses an image with more colours than a palette holds, and refuses
  // a partly transparent pixel outright, so a case can ask for a coarser
  // pattern rather than for a generator of its own.  Asking for one also
  // makes the alpha channel opaque, since a varying alpha is the other half
  // of what GIF will not take.
  const unsigned span = (levels && levels <= max + 1u) ? levels : max + 1u;
  const bool opaque_alpha = levels != 0u && fmt.channel_model == GIMG_CHANNEL_RGBA;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      for (unsigned c = 0; c < ch; c++) {
        const unsigned v = (opaque_alpha && c == 3u)
            ? max
            : ((x * 7u + y * 13u + c * 29u) * 37u) % span;
        if (bits <= 8u) {
          px[y * stride + (x * ch + c)] = (unsigned char)v;
        }
        else {
          uint16_t * row = (uint16_t *)(px + y * stride);
          row[x * ch + c] = (uint16_t)v;
        }
      }
    }
  }
  return r;
}

/** One save configuration, named so a failure says which. */
struct SaveCase {
  const char * name;
  const char * codec;
  const GIMG_Pixel_Format * format;
  GIMG_Save_Options options;
  unsigned levels; ///< Distinct values per channel; 0 = the format's full range.
};

/** Every writer configuration the sweeps walk. */
inline std::vector<SaveCase> save_cases(void) {
  auto opt = [](void) {
    GIMG_Save_Options o = {};
    o.quality = 80;
    return o;
  };

  std::vector<SaveCase> cases;
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg baseline gray", "jpeg", &GIMG_PIXEL_GRAY8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg baseline rgb 4:2:0", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444;
    cases.push_back({"jpeg rgb 4:4:4", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_progressive = 1;
    cases.push_back({"jpeg progressive rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_arithmetic = 1;
    cases.push_back({"jpeg arithmetic rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_restart_interval = 2;
    cases.push_back({"jpeg restarts rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_non_interleaved = 1;
    cases.push_back({"jpeg non-interleaved rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_lossless_predictor = 1;
    cases.push_back({"jpeg lossless rgb", "jpeg", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_hierarchical_levels = 1;
    cases.push_back({"jpeg hierarchical gray", "jpeg", &GIMG_PIXEL_GRAY8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_precision = 12;
    cases.push_back({"jpeg 12-bit gray", "jpeg", &GIMG_PIXEL_GRAY12, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_precision = 12;
    cases.push_back({"jpeg 12-bit rgb", "jpeg", &GIMG_PIXEL_RGBA12, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"jpeg cmyk", "jpeg", &GIMG_PIXEL_CMYK8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.jpeg_cmyk_transform = 2;
    cases.push_back({"jpeg ycck", "jpeg", &GIMG_PIXEL_CMYK8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"png rgba", "png", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    o.interlaced = 1;
    cases.push_back({"png interlaced rgba", "png", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    cases.push_back({"bmp rgba", "bmp", &GIMG_PIXEL_RGBA8, o, 0u});
  }
  {
    GIMG_Save_Options o = opt();
    // GIF takes an RGBA8 raster and nothing else, and only one whose
    // colours already fit a table: 6 levels per channel is 216 of them.
    cases.push_back({"gif rgba 216 colours", "gif", &GIMG_PIXEL_RGBA8, o, 6u});
  }

  return cases;
}

} // namespace gimg_test

#endif
