/**
 * @file
 *
 * Shared JPEG test helpers implementation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <ghoti.io/image/raster.h>

#include "jpeg_test_utils.h"

namespace jpeg_test {

static constexpr uint64_t kFnv1aOffsetBasis = 0xcbf29ce484222325ULL;
static constexpr uint64_t kFnv1aPrime = 0x100000001b3ULL;

bool load_jpeg_file(const char * filename, std::vector<uint8_t> & out) {
  std::string path = std::string(GIMG_TEST_DATA_JPEG) + "/" + filename;
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

std::string jpeg_output_dir(void) {
  std::string s(GIMG_TEST_DATA_JPEG);
  std::string const needle("data/jpeg");
  auto const pos = s.rfind(needle);
  if (pos != std::string::npos) {
    s.replace(pos, needle.size(), "out/jpeg");
  }
  else {
    s += "/../out/jpeg";
  }
  return s;
}

void write_jpeg_output(
    const char * filename, const uint8_t * data, size_t size) {
  std::string path = jpeg_output_dir() + "/" + filename;
  std::ofstream f(path, std::ios::binary);
  if (f && data && size > 0) {
    f.write(reinterpret_cast<const char *>(data),
        static_cast<std::streamsize>(size));
  }
}

void write_jpeg_expected_pixels(
    const char * filename_base, const uint8_t * rgb_data, size_t size) {
  std::string path = jpeg_output_dir() + "/" + filename_base + ".expected";
  std::ofstream f(path, std::ios::binary);
  if (f && rgb_data && size > 0) {
    f.write(reinterpret_cast<const char *>(rgb_data),
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

std::string raster_first_diff(const GIMG_Raster * a, const GIMG_Raster * b) {
  if (!a || !b) {
    return "null raster";
  }
  if (gimg_raster_width(a) != gimg_raster_width(b) ||
      gimg_raster_height(a) != gimg_raster_height(b)) {
    return "dimension mismatch";
  }
  const GIMG_Pixel_Format * fa = gimg_raster_format(a);
  const GIMG_Pixel_Format * fb = gimg_raster_format(b);
  if (!fa || !fb || fa->channel_model != fb->channel_model ||
      fa->channel_count != fb->channel_count) {
    return "format mismatch";
  }
  size_t bpp = gimg_raster_bytes_per_pixel(fa);
  if (bpp == 0) {
    return "bpp 0";
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
    return "null pixels";
  }
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      size_t off_a = (size_t)y * stride_a + (size_t)x * bpp;
      size_t off_b = (size_t)y * stride_b + (size_t)x * bpp;
      if (std::memcmp(pa + off_a, pb + off_b, bpp) != 0) {
        char buf[256];
        int n = std::snprintf(buf, sizeof(buf),
            "First diff at (%u,%u): ", (unsigned)x, (unsigned)y);
        if (n < 0 || n >= (int)sizeof(buf)) {
          return "First diff (snprintf failed)";
        }
        std::string s(buf);
        for (size_t c = 0; c < bpp && c < 4; c++) {
          char ch[24];
          std::snprintf(ch, sizeof(ch), "%s%u", c ? "," : " (", (unsigned)pa[off_a + c]);
          s += ch;
        }
        s += ") vs (";
        for (size_t c = 0; c < bpp && c < 4; c++) {
          char ch[24];
          std::snprintf(ch, sizeof(ch), "%s%u", c ? "," : "", (unsigned)pb[off_b + c]);
          s += ch;
        }
        s += ")";
        return s;
      }
    }
  }
  return "";
}

bool rasters_equal_with_tolerance(
    const GIMG_Raster * a, const GIMG_Raster * b, int max_diff) {
  if (!a || !b || max_diff < 0) {
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
    const unsigned char * ra = pa + y * stride_a;
    const unsigned char * rb = pb + y * stride_b;
    for (uint32_t x = 0; x < w; x++) {
      for (size_t c = 0; c < bpp; c++) {
        int d = static_cast<int>(ra[x * bpp + c]) -
            static_cast<int>(rb[x * bpp + c]);
        if (d < 0) {
          d = -d;
        }
        if (d > max_diff) {
          return false;
        }
      }
    }
  }
  return true;
}

/** Run dump_jpeg_pixels_ref (libjpeg-based oracle) and parse one line.
 * Prefers dump_jpeg_pixels_ref_debug when present (built with third_party
 * libjpeg) for bit-exact match with our decoder's fancy upsampling. */
static bool run_libjpeg_oracle(const std::string & data_dir,
    const char * file_path, uint64_t * out_hash, uint32_t * out_width,
    uint32_t * out_height) {
  std::string ref_debug = data_dir + "/dump_jpeg_pixels_ref_debug";
  std::string ref_std = data_dir + "/dump_jpeg_pixels_ref";
#ifdef _WIN32
  ref_debug += ".exe";
  ref_std += ".exe";
#endif
  std::string ref = ref_std;
  if (std::ifstream(ref_debug).good())
    ref = ref_debug;
  std::string cmd = "\"" + ref + "\" \"" + file_path + "\" 2>";
#ifdef _WIN32
  cmd += "NUL";
#else
  cmd += "/dev/null";
#endif
  FILE * pipe = popen(cmd.c_str(), "r");
  if (!pipe) {
    return false;
  }
  char line[256];
  if (!fgets(line, static_cast<int>(sizeof(line)), pipe)) {
    pclose(pipe);
    return false;
  }
  pclose(pipe);
  unsigned long long h = 0;
  unsigned int w = 0, ht = 0;
  char mode[8];
  if (sscanf(line, "HASH %16llx WIDTH %u HEIGHT %u MODE %7s", &h, &w, &ht,
             mode) != 4) {
    return false;
  }
  *out_hash = static_cast<uint64_t>(h);
  *out_width = w;
  *out_height = ht;
  return true;
}

/** Run dump_jpeg_pixels_ref -o raw_path jpeg_path; return true on success. */
static bool run_libjpeg_oracle_to_raw(const std::string & data_dir,
    const char * jpeg_path, const char * raw_path) {
  std::string ref = data_dir + "/dump_jpeg_pixels_ref";
#ifdef _WIN32
  ref += ".exe";
#endif
  std::string cmd = "\"" + ref + "\" -o \"" + raw_path + "\" \"" + jpeg_path + "\" 2>";
#ifdef _WIN32
  cmd += "NUL";
#else
  cmd += "/dev/null";
#endif
  int ret = std::system(cmd.c_str());
  return (ret == 0);
}

bool pillow_oracle_hash(const char * fixture_filename, uint64_t * out_hash,
    uint32_t * out_width, uint32_t * out_height) {
  if (!fixture_filename || !out_hash || !out_width || !out_height) {
    return false;
  }
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string path = data_dir + "/" + fixture_filename;
  return run_libjpeg_oracle(data_dir, path.c_str(), out_hash, out_width,
                           out_height);
}

bool pillow_oracle_hash_from_path(const char * file_path, uint64_t * out_hash,
    uint32_t * out_width, uint32_t * out_height) {
  if (!file_path || !out_hash || !out_width || !out_height) {
    return false;
  }
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  return run_libjpeg_oracle(data_dir, file_path, out_hash, out_width,
                            out_height);
}

bool load_jpeg_from_path(const char * file_path, std::vector<uint8_t> & out) {
  if (!file_path) {
    return false;
  }
  std::ifstream f(file_path, std::ios::binary | std::ios::ate);
  if (!f) {
    return false;
  }
  std::ifstream::pos_type size = f.tellg();
  if (size <= 0) {
    return false;
  }
  out.resize(static_cast<size_t>(size));
  f.seekg(0);
  return static_cast<bool>(f.read(
      reinterpret_cast<char *>(out.data()), out.size()));
}

bool load_jpeg_oracle_raw(const char * fixture_base,
    std::vector<uint8_t> & out_pixels, uint32_t * out_width,
    uint32_t * out_height, int * out_mode) {
  if (!fixture_base || !out_width || !out_height || !out_mode) {
    return false;
  }
  std::string path = std::string(GIMG_TEST_DATA_JPEG) + "/" + fixture_base + ".raw";
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  unsigned char header[9];
  if (!f.read(reinterpret_cast<char *>(header), 9) ||
      f.gcount() != 9) {
    return false;
  }
  int mode = header[0];
  if (mode != 0 && mode != 1 && mode != 2) {
    return false;
  }
  uint32_t w = static_cast<uint32_t>(header[1]) |
      (static_cast<uint32_t>(header[2]) << 8) |
      (static_cast<uint32_t>(header[3]) << 16) |
      (static_cast<uint32_t>(header[4]) << 24);
  uint32_t h = static_cast<uint32_t>(header[5]) |
      (static_cast<uint32_t>(header[6]) << 8) |
      (static_cast<uint32_t>(header[7]) << 16) |
      (static_cast<uint32_t>(header[8]) << 24);
  size_t pixel_bytes = (mode == 0) ? (size_t)w * h
      : (mode == 1) ? (size_t)w * h * 3u
      : (size_t)w * h * 4u;
  out_pixels.resize(pixel_bytes);
  if (!f.read(reinterpret_cast<char *>(out_pixels.data()),
              static_cast<std::streamsize>(pixel_bytes)) ||
      f.gcount() != static_cast<std::streamsize>(pixel_bytes)) {
    return false;
  }
  *out_width = w;
  *out_height = h;
  *out_mode = mode;
  return true;
}

bool load_jpeg_oracle_raw_from_path(const char * raw_path,
    std::vector<uint8_t> & out_pixels, uint32_t * out_width,
    uint32_t * out_height, int * out_mode) {
  if (!raw_path || !out_width || !out_height || !out_mode) {
    return false;
  }
  std::ifstream f(raw_path, std::ios::binary);
  if (!f) {
    return false;
  }
  unsigned char header[9];
  if (!f.read(reinterpret_cast<char *>(header), 9) ||
      f.gcount() != 9) {
    return false;
  }
  int mode = header[0];
  if (mode != 0 && mode != 1 && mode != 2) {
    return false;
  }
  uint32_t w = static_cast<uint32_t>(header[1]) |
      (static_cast<uint32_t>(header[2]) << 8) |
      (static_cast<uint32_t>(header[3]) << 16) |
      (static_cast<uint32_t>(header[4]) << 24);
  uint32_t h = static_cast<uint32_t>(header[5]) |
      (static_cast<uint32_t>(header[6]) << 8) |
      (static_cast<uint32_t>(header[7]) << 16) |
      (static_cast<uint32_t>(header[8]) << 24);
  size_t pixel_bytes = (mode == 0) ? (size_t)w * h
      : (mode == 1) ? (size_t)w * h * 3u
      : (size_t)w * h * 4u;
  out_pixels.resize(pixel_bytes);
  if (!f.read(reinterpret_cast<char *>(out_pixels.data()),
              static_cast<std::streamsize>(pixel_bytes)) ||
      f.gcount() != static_cast<std::streamsize>(pixel_bytes)) {
    return false;
  }
  *out_width = w;
  *out_height = h;
  *out_mode = mode;
  return true;
}

/** Run libjpeg oracle to decode jpeg_path into a .raw file, then load it.
 * raw_path is the path where the .raw will be written (e.g. jpeg_output_dir() + "/libjpeg_compare.raw").
 * Returns true and fills out_* if the ref ran successfully and the .raw was loaded. */
bool libjpeg_decode_to_oracle_raw(const char * jpeg_path, const char * raw_path,
    std::vector<uint8_t> & out_pixels, uint32_t * out_width,
    uint32_t * out_height, int * out_mode) {
  if (!jpeg_path || !raw_path || !out_width || !out_height || !out_mode) {
    return false;
  }
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  if (!run_libjpeg_oracle_to_raw(data_dir, jpeg_path, raw_path)) {
    return false;
  }
  return load_jpeg_oracle_raw_from_path(raw_path, out_pixels, out_width,
      out_height, out_mode);
}

bool raster_matches_oracle_raw(const GIMG_Raster * raster,
    const uint8_t * raw_pixels, uint32_t raw_w, uint32_t raw_h, int raw_mode,
    int tolerance) {
  if (!raster || !raw_pixels) {
    return false;
  }
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  if (w != raw_w || h != raw_h) {
    return false;
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  size_t bpp = fmt ? gimg_raster_bytes_per_pixel(fmt) : 0;
  const unsigned char * pix =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(
          const_cast<GIMG_Raster *>(raster)));
  size_t stride = gimg_raster_stride_bytes(raster);
  if (!pix || stride < w * bpp) {
    return false;
  }
  for (uint32_t y = 0; y < h; y++) {
    const unsigned char * row = pix + y * stride;
    for (uint32_t x = 0; x < w; x++) {
      if (raw_mode == 0) {
        unsigned char ours = (bpp >= 1) ? row[x * bpp] : 0;
        int d = static_cast<int>(ours) - static_cast<int>(raw_pixels[y * w + x]);
        if (d < 0) d = -d;
        if (d > tolerance) return false;
      }
      else if (raw_mode == 1) {
        unsigned char r = (bpp >= 1) ? row[x * bpp + 0] : 0;
        unsigned char g = (bpp >= 2) ? row[x * bpp + 1] : r;
        unsigned char b = (bpp >= 3) ? row[x * bpp + 2] : r;
        size_t raw_off = (y * w + x) * 3;
        int dr = static_cast<int>(r) - static_cast<int>(raw_pixels[raw_off + 0]);
        int dg = static_cast<int>(g) - static_cast<int>(raw_pixels[raw_off + 1]);
        int db = static_cast<int>(b) - static_cast<int>(raw_pixels[raw_off + 2]);
        if (dr < 0) dr = -dr;
        if (dg < 0) dg = -dg;
        if (db < 0) db = -db;
        if (dr > tolerance || dg > tolerance || db > tolerance) return false;
      }
      else {
        /* raw_mode == 2: CMYK, 4 bytes per pixel */
        unsigned char c = (bpp >= 1) ? row[x * bpp + 0] : 0;
        unsigned char m = (bpp >= 2) ? row[x * bpp + 1] : c;
        unsigned char y_ = (bpp >= 3) ? row[x * bpp + 2] : c;
        unsigned char k = (bpp >= 4) ? row[x * bpp + 3] : c;
        size_t raw_off = (y * w + x) * 4;
        int dc = static_cast<int>(c) - static_cast<int>(raw_pixels[raw_off + 0]);
        int dm = static_cast<int>(m) - static_cast<int>(raw_pixels[raw_off + 1]);
        int dy = static_cast<int>(y_) - static_cast<int>(raw_pixels[raw_off + 2]);
        int dk = static_cast<int>(k) - static_cast<int>(raw_pixels[raw_off + 3]);
        if (dc < 0) dc = -dc;
        if (dm < 0) dm = -dm;
        if (dy < 0) dy = -dy;
        if (dk < 0) dk = -dk;
        if (dc > tolerance || dm > tolerance || dy > tolerance || dk > tolerance)
          return false;
      }
    }
  }
  return true;
}

} // namespace jpeg_test
