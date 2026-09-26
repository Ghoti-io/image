/**
 * @file
 *
 * Every TIFF fixture, read by libtiff as well as by this library.
 *
 * The TIFF codec's other tests assert properties - that both byte orders
 * agree, that tiles and strips agree, that a gradient comes back as the
 * gradient that was written. Those are worth having and they are all
 * self-consistency: a decoder that read every file through the same wrong
 * idea would satisfy every one of them. This file is the outside opinion.
 *
 * libtiff is the reference implementation of TIFF and the one every other
 * reader was written against, which makes it the strongest oracle available
 * for this format and *not* a second reading of the specification. Where the
 * document is silent, or where files in the wild disagree with it, libtiff
 * has policies; `documentation/formats/tiff.md` records one this codec does
 * not share. A disagreement here is a question to triage and not a verdict on
 * either side, so the sweep names the files rather than only counting them.
 *
 * The comparison is in RGBA8 because that is what libtiff's RGBA interface
 * returns; a grayscale image this library hands back as GRAY8 is widened the
 * obvious way before comparing, which is what libtiff does internally too.
 *
 * **One conversion stands between the two, and it is a contract and not a
 * tolerance.** libtiff's RGBA raster carries *associated* alpha - its own
 * header names the table that does it, `UaToAa`, "Unassociated alpha to
 * associated alpha conversion LUT" - so a file storing unassociated samples
 * comes back premultiplied. This library's RGBA8 is unassociated, following
 * PNG. Comparing them means putting one into the other's space, and the
 * direction chosen is to premultiply ours, because that is the direction with
 * no division in it and so no case where the two disagree only about
 * rounding. It is applied to every pixel of every file identically, which is
 * what makes it a change of units rather than an exception sized to pass a
 * known failure. Alpha itself is compared unconverted, because nothing
 * transforms it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <vector>

#include "../../oracle_gate.h"

namespace {

/** One decoded image, as RGBA8 with the stride taken out. */
struct Image {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
  bool ok = false;
};

std::string data_dir() { return std::string(GIMG_TEST_DATA_TIFF); }
std::string out_dir() { return std::string(GIMG_TEST_OUT_TIFF); }

std::vector<std::string> fixtures() {
  std::vector<std::string> names;
  DIR * d = opendir(data_dir().c_str());
  if (!d) { return names; }
  while (struct dirent * e = readdir(d)) {
    const std::string n = e->d_name;
    if (n.size() > 4u && n.compare(n.size() - 4u, 4u, ".tif") == 0) {
      names.push_back(n);
    }
  }
  closedir(d);
  std::sort(names.begin(), names.end());
  return names;
}

/** What this library makes of a file, widened to RGBA8. */
Image ours(const std::string & name) {
  Image img;
  std::ifstream f(data_dir() + "/" + name, std::ios::binary);
  const std::vector<uint8_t> bytes(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (bytes.empty()) { return img; }
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
    return img;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) == GIMG_OK && doc) {
    GIMG_Raster * raster = nullptr;
    if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) == GIMG_OK &&
        raster) {
      img.width = gimg_raster_width(raster);
      img.height = gimg_raster_height(raster);
      const size_t bpp =
          gimg_raster_bytes_per_pixel(gimg_raster_format(raster));
      const size_t stride = gimg_raster_stride_bytes(raster);
      const uint8_t * p = (const uint8_t *)gimg_raster_pixels(raster);
      img.rgba.resize((size_t)img.width * img.height * 4u);
      for (uint32_t y = 0; y < img.height; y++) {
        for (uint32_t x = 0; x < img.width; x++) {
          const uint8_t * src = p + ((size_t)y * stride) + ((size_t)x * bpp);
          uint8_t * dst = img.rgba.data() +
              (((size_t)y * img.width + x) * 4u);
          if (bpp == 1u) {
            dst[0] = dst[1] = dst[2] = src[0];
            dst[3] = 255u;
          }
          else {
            std::memcpy(dst, src, 4u);
          }
        }
      }
      img.ok = true;
    }
    if (raster) { gimg_raster_destroy(raster); }
  }
  if (doc) { gimg_doc_destroy(doc); }
  gimg_stream_destroy(s);
  return img;
}

/** What libtiff makes of the same file, through the pinned image. */
Image theirs(const std::string & name) {
  Image img;
  const std::string root = oracle_gate::repo_root();
  if (root.empty()) { return img; }
  const std::string raw = out_dir() + "/" + name + ".libtiff.raw";
  std::string cmd = "mkdir -p \"" + out_dir() + "\"";
  if (std::system(cmd.c_str()) != 0) { return img; }

  cmd = "\"" + root + "/tools/oracle/oracle-exec\" --scratch \"" + out_dir() +
      "\" libtiff -- \"" + root +
      "/tests/tools/tiff-oracle/build/dump_tiff_pixels_libtiff\" -o \"" +
      raw + "\" \"" + data_dir() + "/" + name + "\" >/dev/null 2>&1";
  if (std::system(cmd.c_str()) != 0) {
    return img; // libtiff refused the file; the caller counts that.
  }
  std::ifstream f(raw, std::ios::binary);
  const std::vector<uint8_t> blob(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (blob.size() < 12u || std::memcmp(blob.data(), "TIFO", 4) != 0) {
    return img;
  }
  auto u32 = [&](size_t at) {
    return (uint32_t)blob[at] | ((uint32_t)blob[at + 1] << 8) |
        ((uint32_t)blob[at + 2] << 16) | ((uint32_t)blob[at + 3] << 24);
  };
  img.width = u32(4);
  img.height = u32(8);
  const size_t want = (size_t)img.width * img.height * 4u;
  if (blob.size() < 12u + want) { return img; }
  img.rgba.assign(blob.begin() + 12, blob.begin() + 12 + (long)want);
  img.ok = true;
  return img;
}

} // namespace

ORACLE_SENTINEL(TiffOracle, libtiff)

TEST(TiffOracle, EveryFixtureLibtiffReadsIsReadTheSameWay) {
  if (!oracle_gate::reachable("libtiff")) {
    GTEST_SKIP() << "the sentinel above has already failed the run";
  }
  const std::vector<std::string> names = fixtures();
  ASSERT_FALSE(names.empty()) << "no fixtures under " << data_dir();

  long agreed = 0, differed = 0, both_refused = 0, only_we_read = 0,
       only_they_read = 0;
  std::vector<std::string> disagreements, we_alone, they_alone;
  for (const std::string & name : names) {
    const Image a = ours(name);
    const Image b = theirs(name);
    if (!a.ok && !b.ok) { both_refused++; continue; }
    if (a.ok && !b.ok) {
      only_we_read++;
      we_alone.push_back(name);
      continue;
    }
    if (!a.ok && b.ok) {
      only_they_read++;
      they_alone.push_back(name);
      continue;
    }
    if (a.width != b.width || a.height != b.height) {
      differed++;
      disagreements.push_back(name + ": geometry, ours " +
          std::to_string(a.width) + "x" + std::to_string(a.height) +
          ", libtiff " + std::to_string(b.width) + "x" +
          std::to_string(b.height));
      continue;
    }
    size_t first_bad = (size_t)-1;
    long bad_samples = 0;
    for (size_t i = 0; i < a.rgba.size(); i += 4u) {
      const unsigned alpha = a.rgba[i + 3u];
      for (size_t k = 0; k < 4u; k++) {
        // Alpha passes straight through; colour is premultiplied into
        // libtiff's space first. See the note at the top of this file.
        const unsigned mine = (k == 3u)
            ? alpha
            : ((unsigned)a.rgba[i + k] * alpha + 127u) / 255u;
        if (mine != b.rgba[i + k]) {
          bad_samples++;
          if (first_bad == (size_t)-1) { first_bad = i + k; }
        }
      }
    }
    if (bad_samples == 0) {
      agreed++;
    }
    else {
      differed++;
      disagreements.push_back(name + ": " + std::to_string(bad_samples) +
          " of " + std::to_string(a.rgba.size()) +
          " samples, first at pixel " + std::to_string(first_bad / 4u) +
          " channel " + std::to_string(first_bad % 4u) + ", ours " +
          std::to_string((int)a.rgba[first_bad]) + " (alpha " +
          std::to_string((int)a.rgba[(first_bad & ~(size_t)3u) + 3u]) +
          ") libtiff " + std::to_string((int)b.rgba[first_bad]));
    }
  }

  std::printf("  %s\n", oracle_gate::provenance("libtiff").c_str());
  std::printf("  %zu fixtures: %ld agreed, %ld differed, %ld both refused, "
              "%ld only we read, %ld only libtiff read\n",
      names.size(), agreed, differed, both_refused, only_we_read,
      only_they_read);

  // What libtiff reads and this codec does not is the to-do list, so it is
  // printed rather than counted. The reverse would be a finding: a file this
  // library reads and the reference implementation will not is either a
  // fixture that is not a TIFF or a place where we are being too generous.
  for (const std::string & n : they_alone) {
    std::printf("    libtiff reads and we do not: %s\n", n.c_str());
  }
  for (const std::string & n : we_alone) {
    std::printf("    we read and libtiff does not: %s\n", n.c_str());
  }

  // The alarm on the sweep itself. A comparison that reached no file agrees
  // with everything, and the count is the only thing that separates "libtiff
  // read eleven files and said the same as us" from "libtiff read none".
  EXPECT_GT(agreed + differed, 8)
      << "only " << (agreed + differed)
      << " files were compared at all, so this sweep is not seeing the corpus";

  for (const std::string & line : disagreements) {
    ADD_FAILURE() << line;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
