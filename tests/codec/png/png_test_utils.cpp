/**
 * @file
 *
 * Shared PNG test helpers implementation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <fstream>
#include <string>
#include <ghoti.io/image/raster.h>

#include "png_test_utils.h"

namespace png_test {

static constexpr uint64_t kFnv1aOffsetBasis = 0xcbf29ce484222325ULL;
static constexpr uint64_t kFnv1aPrime = 0x100000001b3ULL;

bool load_png_file(const char * filename, std::vector<uint8_t> & out) {
  std::string path = std::string(GIMG_TEST_DATA_PNG) + "/" + filename;
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) {
    return false;
  }
  std::ifstream::pos_type size = f.tellg();
  if (size <= 0) {
    return false;
  }
  out.resize(static_cast<size_t>(size));
  f.seekg(0);
  if (!f.read(reinterpret_cast<char *>(out.data()), out.size())) {
    return false;
  }
  return true;
}

std::string png_output_dir(void) {
  std::string s(GIMG_TEST_DATA_PNG);
  std::string const needle("data/png");
  auto const pos = s.rfind(needle);
  if (pos != std::string::npos) {
    s.replace(pos, needle.size(), "out/png");
  }
  else {
    s += "/../out/png";
  }
  return s;
}

void write_png_output(
    const char * filename, const uint8_t * data, size_t size) {
  std::string path = png_output_dir() + "/" + filename;
  std::ofstream f(path, std::ios::binary);
  if (f && data && size > 0) {
    f.write(reinterpret_cast<const char *>(data),
        static_cast<std::streamsize>(size));
  }
}

uint64_t raster_pixel_hash(const GIMG_Raster * raster) {
  if (!raster) {
    return 0;
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  if (bpp == 0) {
    return 0;
  }
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t row_bytes = w * bpp;
  const unsigned char * pixels = static_cast<const unsigned char *>(
      gimg_raster_pixels_const(const_cast<GIMG_Raster *>(raster)));
  size_t stride = gimg_raster_stride_bytes(raster);
  if (!pixels || stride < row_bytes) {
    return 0;
  }
  uint64_t hval = kFnv1aOffsetBasis;
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * row = pixels + y * stride;
    for (size_t i = 0; i < row_bytes; i++) {
      hval ^= static_cast<uint64_t>(row[i]);
      hval *= kFnv1aPrime;
    }
  }
  return hval;
}

bool rasters_equal(const GIMG_Raster * a, const GIMG_Raster * b) {
  if (!a || !b) {
    return false;
  }
  if (gimg_raster_width(a) != gimg_raster_width(b) ||
      gimg_raster_height(a) != gimg_raster_height(b)) {
    return false;
  }
  const GIMG_Pixel_Format * fa = gimg_raster_format(a);
  const GIMG_Pixel_Format * fb = gimg_raster_format(b);
  if (!fa || !fb || fa->channel_model != fb->channel_model ||
      fa->channel_count != fb->channel_count) {
    return false;
  }
  size_t bpp = gimg_raster_bytes_per_pixel(fa);
  if (bpp == 0) {
    return false;
  }
  uint32_t w = gimg_raster_width(a);
  uint32_t h = gimg_raster_height(a);
  size_t stride_a = gimg_raster_stride_bytes(a);
  size_t stride_b = gimg_raster_stride_bytes(b);
  const unsigned char * pa = static_cast<const unsigned char *>(
      gimg_raster_pixels_const(const_cast<GIMG_Raster *>(a)));
  const unsigned char * pb = static_cast<const unsigned char *>(
      gimg_raster_pixels_const(const_cast<GIMG_Raster *>(b)));
  if (!pa || !pb) {
    return false;
  }
  for (uint32_t y = 0; y < h; y++) {
    if (std::memcmp(pa + y * stride_a, pb + y * stride_b, w * bpp) != 0) {
      return false;
    }
  }
  return true;
}

} // namespace png_test
