/**
 * @file
 *
 * Resize a headerless raster and write the result back out headerless, so
 * that tests/data/verify_resample.py can put the same pixels through Pillow
 * and compare byte for byte.  Raw in and raw out on purpose: a PNG in the
 * middle would put this library's own encoder between the resampler and the
 * comparison, and a disagreement could then be either one.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

void usage() {
  std::fprintf(stderr,
      "usage: resample_tool <in.raw> <w> <h> <channels> <bits> <dst_w> "
      "<dst_h> <filter> <out.raw>\n"
      "  channels: 1 (gray) or 4 (rgba); bits: 8 or 16\n"
      "  filter:   the GIMG_Resample_Filter value\n");
}

} // namespace

int main(int argc, char ** argv) {
  if (argc != 10) {
    usage();
    return 2;
  }
  const char * in_path = argv[1];
  const uint32_t w = (uint32_t)std::strtoul(argv[2], nullptr, 10);
  const uint32_t h = (uint32_t)std::strtoul(argv[3], nullptr, 10);
  const int channels = std::atoi(argv[4]);
  const int bits = std::atoi(argv[5]);
  const uint32_t dst_w = (uint32_t)std::strtoul(argv[6], nullptr, 10);
  const uint32_t dst_h = (uint32_t)std::strtoul(argv[7], nullptr, 10);
  const int filter = std::atoi(argv[8]);
  const char * out_path = argv[9];

  const GIMG_Pixel_Format * fmt = nullptr;
  if (channels == 1) {
    fmt = (bits == 16) ? &GIMG_PIXEL_GRAY16 : &GIMG_PIXEL_GRAY8;
  }
  else if (channels == 4) {
    fmt = (bits == 16) ? &GIMG_PIXEL_RGBA16 : &GIMG_PIXEL_RGBA8;
  }
  else {
    usage();
    return 2;
  }
  const size_t bpp = (size_t)channels * (size_t)(bits / 8);

  GIMG_Raster * src = nullptr;
  if (gimg_raster_create(w, h, fmt, GIMG_RASTER_OWNED, nullptr, 0, &src) !=
      GIMG_OK) {
    std::fprintf(stderr, "could not create a %ux%u raster\n", w, h);
    return 3;
  }
  std::FILE * f = std::fopen(in_path, "rb");
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", in_path);
    gimg_raster_destroy(src);
    return 3;
  }
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  const size_t stride = gimg_raster_stride_bytes(src);
  for (uint32_t y = 0; y < h; y++) {
    if (std::fread(p + (size_t)y * stride, 1, (size_t)w * bpp, f) !=
        (size_t)w * bpp) {
      std::fprintf(stderr, "%s is shorter than %ux%u\n", in_path, w, h);
      std::fclose(f);
      gimg_raster_destroy(src);
      return 3;
    }
  }
  std::fclose(f);

  GIMG_Resize_Options options;
  gimg_resize_options_default(&options);
  options.filter = (GIMG_Resample_Filter)filter;

  GIMG_Raster * dst = nullptr;
  const GIMG_Result r = gimg_ops_resize(src, dst_w, dst_h, &options, &dst);
  if (r != GIMG_OK) {
    std::fprintf(stderr, "resize returned %d\n", (int)r);
    gimg_raster_destroy(src);
    return 4;
  }

  f = std::fopen(out_path, "wb");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", out_path);
    gimg_raster_destroy(dst);
    gimg_raster_destroy(src);
    return 3;
  }
  const unsigned char * dp =
      (const unsigned char *)gimg_raster_pixels_const(dst);
  const size_t dst_stride = gimg_raster_stride_bytes(dst);
  for (uint32_t y = 0; y < dst_h; y++) {
    std::fwrite(dp + (size_t)y * dst_stride, 1, (size_t)dst_w * bpp, f);
  }
  std::fclose(f);
  gimg_raster_destroy(dst);
  gimg_raster_destroy(src);
  return 0;
}
