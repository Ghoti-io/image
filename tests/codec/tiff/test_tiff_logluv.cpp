/**
 * @file
 *
 * SGI LogLuv, against libtiff's float decode.
 *
 * test_tiff_oracle.cpp compares in RGBA8 because that is what libtiff's RGBA
 * reader returns, and for LogLuv that reader is a tone map - it saturates
 * every pixel brighter than Y = 1, a fifth of off_luv32.tif. So LogLuv is
 * compared here instead, against SGILOGDATAFMT_FLOAT: Y for LogL, CIE XYZ for
 * LogLuv, which is libtiff's own decode and not a policy on top of one.
 *
 * Our raster holds LogLuv as linear RGB in sRGB's primaries, so libtiff's XYZ
 * is taken through the same matrix (libs/color's, built from the same named
 * gamut) before comparing. That makes this a check of the storage - the
 * run-length planes, the 24-bit packing, strips, tiles, both byte orders -
 * and of the code tables in libs/color, which are themselves pinned to
 * libtiff there. The tolerance is float rounding, scaled to the pixel's own
 * magnitude, because a LogLuv picture spans thirty decades and an absolute
 * tolerance would be meaningless at one end of it.
 *
 * The population is libtiffpic's three LogLuv files and the twelve variants
 * `make oracle-tools` writes with libtiff (tests/data/tiff-logluv): every
 * form, both byte orders, strips and tiles over a picture that is a whole
 * number of neither, values past each form's ceiling, zero, negative LogL.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/color/color.h>
#include <ghoti.io/color/math.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "../../oracle_gate.h"

namespace {

std::string variants_dir() {
  return std::string(GIMG_TEST_DATA_TIFF) + "/../tiff-logluv";
}
std::string corpus_dir() {
  return std::string(GIMG_TEST_DATA_TIFF) + "/../tiff-corpus";
}
std::string out_dir() { return std::string(GIMG_TEST_OUT_TIFF) + "/logluv"; }

std::vector<uint8_t> read_file(const std::string & path) {
  std::ifstream f(path, std::ios::binary);
  return std::vector<uint8_t>(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

struct Theirs {
  uint32_t width = 0, height = 0, channels = 0;
  std::vector<float> values;
};

uint32_t le32(const uint8_t * p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
      ((uint32_t)p[3] << 24);
}

/** libtiff's float decode of each path, by file name. */
std::map<std::string, Theirs> ask_libtiff(
    const std::vector<std::string> & paths) {
  std::map<std::string, Theirs> out;
  const std::string root = oracle_gate::repo_root();
  if (root.empty() ||
      std::system(("mkdir -p \"" + out_dir() + "\"").c_str()) != 0) {
    return out;
  }
  std::string cmd = "\"" + root + "/tools/oracle/oracle-exec\" --scratch \"" +
      out_dir() + "\" libtiff -- \"" + root +
      "/tests/tools/tiff-oracle/build/logluv_libtiff\" --dump \"" +
      out_dir() + "\"";
  for (const std::string & p : paths) {
    cmd += " \"" + p + "\"";
  }
  cmd += " > \"" + out_dir() + "/answers\" 2>/dev/null";
  if (std::system(cmd.c_str()) != 0) {
    return out;
  }
  std::ifstream answers(out_dir() + "/answers");
  std::string line;
  while (std::getline(answers, line)) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos ||
        line.find("status=ok", tab) == std::string::npos) {
      continue;
    }
    const std::string name = line.substr(0, tab);
    const std::vector<uint8_t> b = read_file(out_dir() + "/" + name + ".f32");
    if (b.size() < 16u || std::memcmp(b.data(), "LUVF", 4) != 0) {
      continue;
    }
    Theirs t;
    t.width = le32(&b[4]);
    t.height = le32(&b[8]);
    t.channels = le32(&b[12]);
    const size_t n = (size_t)t.width * t.height * t.channels;
    if (b.size() != 16u + n * 4u) {
      continue;
    }
    t.values.resize(n);
    for (size_t i = 0; i < n; i++) {
      const uint32_t bits = le32(&b[16u + i * 4u]);
      std::memcpy(&t.values[i], &bits, 4);
    }
    out[name] = t;
  }
  return out;
}

struct Ours {
  GIMG_Raster * raster = nullptr;
  GIMG_Doc * doc = nullptr;
  ~Ours() {
    if (raster) { gimg_raster_destroy(raster); }
    if (doc) { gimg_doc_destroy(doc); }
  }
};

GIMG_Result decode(const std::string & path, Ours * o) {
  const std::vector<uint8_t> bytes = read_file(path);
  GIMG_Stream * s = nullptr;
  if (bytes.empty() ||
      gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &o->doc);
  if (r == GIMG_OK) {
    r = gimg_item_decode(gimg_doc_item(o->doc, 0), nullptr, &o->raster);
  }
  gimg_stream_destroy(s);
  return r;
}

std::vector<std::string> population() {
  std::vector<std::string> out;
  for (const char * n : {"off_l16.tif", "off_luv24.tif", "off_luv32.tif"}) {
    out.push_back(corpus_dir() + "/" + n);
  }
  for (const char * kind : {"l16", "luv32", "luv24"}) {
    for (const char * order : {"le", "be"}) {
      for (const char * layout : {"strips", "tiles"}) {
        out.push_back(variants_dir() + "/logluv_" + kind + "_" + order + "_" +
            layout + ".tif");
      }
    }
  }
  return out;
}

std::string base(const std::string & p) {
  const size_t at = p.rfind('/');
  return at == std::string::npos ? p : p.substr(at + 1u);
}

/** Within float rounding of @p want, scaled to @p magnitude. */
bool near(double got, double want, double magnitude) {
  if (got == want) {
    return true;
  }
  return std::fabs(got - want) <= 2e-6 * magnitude + 1e-37;
}

} // namespace

ORACLE_SENTINEL(TiffLogLuv, libtiff)

TEST(TiffLogLuv, EveryFileDecodesAsLibtiffDecodesIt) {
  if (!oracle_gate::reachable("libtiff")) {
    GTEST_SKIP() << "the sentinel above has already failed the run";
  }
  const std::vector<std::string> paths = population();
  for (const std::string & p : paths) {
    ASSERT_FALSE(read_file(p).empty())
        << p << " is missing. Run `make oracle-tools`, which copies the "
                "sample set out of the pinned image and writes the variants.";
  }
  std::printf("  %s\n", oracle_gate::provenance("libtiff").c_str());
  const std::map<std::string, Theirs> refs = ask_libtiff(paths);

  GCOL_Gamut srgb;
  double m[9];
  ASSERT_TRUE(gcol_gamut_named(GCOL_PRIMARIES_SRGB, &srgb));
  ASSERT_EQ(gcol_xyz_to_gamut(&srgb, m), GCOL_OK);

  size_t compared = 0, samples = 0;
  for (const std::string & p : paths) {
    SCOPED_TRACE(base(p));
    const auto found = refs.find(base(p));
    ASSERT_NE(found, refs.end()) << "libtiff did not decode it";
    const Theirs & t = found->second;
    Ours o;
    ASSERT_EQ(decode(p, &o), GIMG_OK);
    const GIMG_Pixel_Format * f = gimg_raster_format(o.raster);
    ASSERT_EQ(f->channel_type, GIMG_CHANNEL_FLOAT);
    ASSERT_EQ(gimg_raster_width(o.raster), t.width);
    ASSERT_EQ(gimg_raster_height(o.raster), t.height);
    ASSERT_EQ(f->channel_count, t.channels == 1u ? 1u : 4u);
    const uint8_t * px = (const uint8_t *)gimg_raster_pixels_const(o.raster);
    const size_t stride = gimg_raster_stride_bytes(o.raster);
    size_t bad = 0;
    for (uint32_t y = 0; y < t.height; y++) {
      const float * row = (const float *)(const void *)(px + y * stride);
      for (uint32_t x = 0; x < t.width; x++) {
        const float * want = &t.values[((size_t)y * t.width + x) * t.channels];
        if (t.channels == 1u) {
          if (!near(row[x], want[0], std::fabs(want[0])) && bad++ < 4) {
            ADD_FAILURE() << "(" << x << "," << y << ") Y " << row[x]
                          << " vs libtiff " << want[0];
          }
          samples++;
          continue;
        }
        const float * got = row + (size_t)x * 4u;
        const double mag = std::fabs(want[0]) + std::fabs(want[1]) +
            std::fabs(want[2]);
        for (int c = 0; c < 3; c++) {
          const double rgb = m[c * 3] * want[0] + m[c * 3 + 1] * want[1] +
              m[c * 3 + 2] * want[2];
          if (!near(got[c], rgb, mag) && bad++ < 4) {
            ADD_FAILURE() << "(" << x << "," << y << ") channel " << c << " "
                          << got[c] << " vs libtiff's XYZ through sRGB "
                          << rgb;
          }
        }
        if (got[3] != 1.0f && bad++ < 4) {
          ADD_FAILURE() << "(" << x << "," << y << ") alpha " << got[3];
        }
        samples += 4u;
      }
    }
    EXPECT_EQ(bad, 0u);
    compared++;
  }
  std::printf("  LogLuv: %zu files, %zu samples, every one as libtiff reads "
              "it\n",
      compared, samples);
  // Fifteen files, or the sweep is not looking at what it names.
  EXPECT_EQ(compared, 15u);
}

// What the colour info says: scene light, measured, in sRGB's primaries,
// with StoNits as the unit when the file states it and unstated when not.
TEST(TiffLogLuv, ColourInfoSaysAbsoluteSceneLight) {
  const std::vector<std::string> paths = population();
  size_t seen = 0, stated = 0;
  for (const std::string & p : paths) {
    if (read_file(p).empty()) {
      continue; // Missing files fail the oracle test above, by name.
    }
    SCOPED_TRACE(base(p));
    Ours o;
    ASSERT_EQ(decode(p, &o), GIMG_OK);
    const GCOL_Color_Info * ci = gimg_raster_color_info_const(o.raster);
    ASSERT_NE(ci, nullptr);
    EXPECT_EQ(ci->transfer, GCOL_TRANSFER_LINEAR);
    EXPECT_EQ(ci->reference, GCOL_REFERENCE_SCENE);
    EXPECT_EQ(ci->sample_scale, GCOL_SAMPLE_SCALE_ABSOLUTE);
    EXPECT_EQ(ci->icc_bytes, nullptr);
    if (gimg_raster_format(o.raster)->channel_count == 4u) {
      EXPECT_TRUE(ci->primaries_stated);
      EXPECT_TRUE(ci->white_stated);
      EXPECT_EQ(gcol_gamut_identify(&ci->gamut, 0.0005), GCOL_PRIMARIES_SRGB);
    }
    // The corpus files and the little-endian variants carry StoNits 179;
    // the big-endian variants carry none.
    const bool says = base(p).rfind("off_", 0) == 0 ||
        base(p).find("_le_") != std::string::npos;
    EXPECT_EQ(ci->white_luminance, says ? 179.0 : 0.0);
    stated += says ? 1u : 0u;
    seen++;
  }
  if (seen == 0u) {
    GTEST_SKIP() << "no LogLuv files; `make oracle-tools` writes them";
  }
  EXPECT_GT(stated, 0u);
  EXPECT_LT(stated, seen);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
