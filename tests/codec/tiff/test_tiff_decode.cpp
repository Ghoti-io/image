/**
 * @file
 *
 * What the TIFF decoder makes of the fixtures in tests/data/tiff/.
 *
 * Three of the tests here need no reference decoder, which is the point of
 * them: they assert a property the format guarantees, so they are pinned to
 * TIFF 6.0 rather than to this library's own opinion.
 *
 *   - **The byte-order pair.** The same picture written "II" and "MM" must
 *     decode to the same bytes. Nothing else in this suite can catch a
 *     field read from the wrong end.
 *   - **Tiled against stripped.** The same picture cut into tiles and cut
 *     into strips must decode alike. That is the decode model's central
 *     claim - that a tile is a storage layout and not something a caller
 *     sees - stated as a test rather than as a comment.
 *   - **The ramp itself.** The fixtures carry a gradient with no symmetry,
 *     computed here from the same formula the generator used, so a decode
 *     that transposed or mirrored the image would fail rather than merely
 *     disagree with a sibling.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
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

namespace {

std::vector<uint8_t> slurp(const std::string & name) {
  std::ifstream f(std::string(GIMG_TEST_DATA_TIFF) + "/" + name,
      std::ios::binary);
  return std::vector<uint8_t>(
      (std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

/** A loaded document that cleans up after itself. */
class Loaded {
public:
  GIMG_Result load(const std::string & name) {
    bytes_ = slurp(name);
    if (bytes_.empty()) { return GIMG_ERR_IO; }
    if (gimg_stream_create_memory(bytes_.data(), bytes_.size(), &stream_) !=
        GIMG_OK) {
      return GIMG_ERR_INTERNAL;
    }
    return gimg_doc_load(stream_, nullptr, &diag_, &doc_);
  }
  ~Loaded() {
    if (raster_) { gimg_raster_destroy(raster_); }
    gimg_diagnostics_destroy(&diag_);
    if (doc_) { gimg_doc_destroy(doc_); }
    if (stream_) { gimg_stream_destroy(stream_); }
  }
  GIMG_Doc * doc() const { return doc_; }

  /** Item @p index decoded, as a flat copy of its rows without the stride. */
  std::vector<uint8_t> pixels(size_t index = 0) {
    std::vector<uint8_t> out;
    if (raster_) { gimg_raster_destroy(raster_); raster_ = nullptr; }
    if (gimg_item_decode(gimg_doc_item(doc_, index), nullptr, &raster_) !=
            GIMG_OK ||
        !raster_) {
      return out;
    }
    const uint32_t w = gimg_raster_width(raster_);
    const uint32_t h = gimg_raster_height(raster_);
    const size_t bpp = gimg_raster_bytes_per_pixel(
        gimg_raster_format(raster_));
    const size_t stride = gimg_raster_stride_bytes(raster_);
    const uint8_t * p = (const uint8_t *)gimg_raster_pixels(raster_);
    out.reserve((size_t)w * h * bpp);
    for (uint32_t y = 0; y < h; y++) {
      out.insert(out.end(), p + (y * stride), p + (y * stride) + (w * bpp));
    }
    return out;
  }
  GIMG_Raster * raster() const { return raster_; }

  /** Whether any diagnostic's text contains @p needle. */
  bool said(const char * needle) const {
    for (size_t i = 0; i < diag_.count; i++) {
      const char * a = diag_.items[i].recommended_action;
      if (a && std::string(a).find(needle) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  std::string reasons() const {
    std::string s;
    for (size_t i = 0; i < diag_.count; i++) {
      if (diag_.items[i].recommended_action) {
        s += std::string(s.empty() ? "" : " | ") +
            diag_.items[i].recommended_action;
      }
    }
    return s.empty() ? "(nothing)" : s;
  }

private:
  std::vector<uint8_t> bytes_;
  GIMG_Stream * stream_ = nullptr;
  GIMG_Doc * doc_ = nullptr;
  GIMG_Raster * raster_ = nullptr;
  GIMG_Diagnostics diag_ = {};
};

/** The gradient tests/data/tiff/generate.py writes, recomputed here. */
std::vector<uint8_t> gray_ramp(uint32_t w, uint32_t h) {
  std::vector<uint8_t> out;
  const uint32_t last = (w * h) > 1u ? (w * h - 1u) : 1u;
  for (uint32_t i = 0; i < w * h; i++) {
    out.push_back((uint8_t)((i * 255u) / last));
  }
  return out;
}

} // namespace

TEST(TiffDecode, TheSamePictureInBothByteOrdersDecodesAlike) {
  Loaded le, be;
  ASSERT_EQ(le.load("tiff_4x4_gray8_le.tif"), GIMG_OK) << le.reasons();
  ASSERT_EQ(be.load("tiff_4x4_gray8_be.tif"), GIMG_OK) << be.reasons();
  const std::vector<uint8_t> a = le.pixels();
  const std::vector<uint8_t> b = be.pixels();
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a, b) << "a field was read from the wrong end of its bytes";
  // And both are the picture the generator wrote, not merely each other.
  EXPECT_EQ(a, gray_ramp(4, 4));
}

TEST(TiffDecode, ATiledImageAndAStrippedOneAgree) {
  // Six pixels across with four-pixel tiles, so the right-hand and bottom
  // tiles are half padding. The padding is stored and must be dropped.
  Loaded tiled, stripped;
  ASSERT_EQ(tiled.load("tiff_6x6_tiled.tif"), GIMG_OK) << tiled.reasons();
  ASSERT_EQ(stripped.load("tiff_6x6_stripped.tif"), GIMG_OK)
      << stripped.reasons();
  const std::vector<uint8_t> t = tiled.pixels();
  const std::vector<uint8_t> s = stripped.pixels();
  ASSERT_FALSE(t.empty());
  EXPECT_EQ(t, s) << "a tile is a storage layout; the picture is the same";
  EXPECT_EQ(t, gray_ramp(6, 6));
  EXPECT_EQ(gimg_raster_width(tiled.raster()), 6u);
  EXPECT_EQ(gimg_raster_height(tiled.raster()), 6u);
}

TEST(TiffDecode, SeveralStripsCarryOnePicture) {
  Loaded img;
  ASSERT_EQ(img.load("tiff_8x8_four_strips.tif"), GIMG_OK) << img.reasons();
  EXPECT_EQ(img.pixels(), gray_ramp(8, 8));
}

TEST(TiffDecode, WhiteIsZeroIsTheComplementOfBlackIsZero) {
  Loaded black, white;
  ASSERT_EQ(black.load("tiff_4x4_gray8_le.tif"), GIMG_OK);
  ASSERT_EQ(white.load("tiff_4x4_whitezero.tif"), GIMG_OK);
  const std::vector<uint8_t> b = black.pixels();
  const std::vector<uint8_t> w = white.pixels();
  ASSERT_EQ(b.size(), w.size());
  ASSERT_FALSE(b.empty());
  for (size_t i = 0; i < b.size(); i++) {
    EXPECT_EQ((int)w[i], 255 - (int)b[i])
        << "sample " << i << ": zero is white in one file and black in the "
                             "other, and the samples are the same bytes";
  }
}

TEST(TiffDecode, AnRgbImageComesBackAsOpaqueRgba) {
  Loaded img;
  ASSERT_EQ(img.load("tiff_4x4_rgb.tif"), GIMG_OK) << img.reasons();
  const std::vector<uint8_t> p = img.pixels();
  ASSERT_EQ(p.size(), 4u * 4u * 4u);
  for (size_t i = 0; i < 16; i++) {
    const uint32_t x = (uint32_t)(i % 4), y = (uint32_t)(i / 4);
    EXPECT_EQ(p[i * 4 + 0], (uint8_t)(x * 255 / 3));
    EXPECT_EQ(p[i * 4 + 1], (uint8_t)(y * 255 / 3));
    EXPECT_EQ(p[i * 4 + 3], 255u) << "an RGB image has no transparency";
  }
}

TEST(TiffDecode, AssociatedAlphaIsDividedBackOut) {
  // The two files hold the same picture, one with the colour premultiplied by
  // its alpha and one without. This library's RGBA8 is unassociated, so both
  // must come back the same - to within the rounding that multiplying and
  // dividing by an eight-bit alpha costs.
  Loaded plain, assoc;
  ASSERT_EQ(plain.load("tiff_4x4_rgba.tif"), GIMG_OK) << plain.reasons();
  ASSERT_EQ(assoc.load("tiff_4x4_rgba_associated.tif"), GIMG_OK)
      << assoc.reasons();
  const std::vector<uint8_t> a = plain.pixels();
  const std::vector<uint8_t> b = assoc.pixels();
  ASSERT_EQ(a.size(), b.size());
  ASSERT_FALSE(a.empty());
  for (size_t i = 0; i < a.size(); i += 4) {
    EXPECT_EQ(a[i + 3], b[i + 3]) << "alpha itself is not premultiplied";
    if (a[i + 3] == 0u) {
      continue; // A fully transparent pixel keeps no colour to compare.
    }
    for (size_t k = 0; k < 3; k++) {
      // The round trip through an alpha of `a` cannot be exact: the error is
      // bounded by one step of that alpha, which is 255/alpha.
      const int tolerance = 1 + (255 / (int)a[i + 3]);
      EXPECT_NEAR((int)a[i + k], (int)b[i + k], tolerance)
          << "pixel " << (i / 4) << " channel " << k << " at alpha "
          << (int)a[i + 3];
    }
  }
}

TEST(TiffDecode, APaletteImageIsExpandedThroughItsColorMap) {
  Loaded img;
  ASSERT_EQ(img.load("tiff_4x4_palette.tif"), GIMG_OK) << img.reasons();
  const std::vector<uint8_t> p = img.pixels();
  ASSERT_EQ(p.size(), 4u * 4u * 4u);
  // The generator's map: index 0 black, 1 red, 2 green, 3 a pale blue; the
  // picture is (x + y) % 4.
  const uint8_t want[4][3] = {
      {0, 0, 0}, {255, 0, 0}, {0, 255, 0}, {128, 128, 255}};
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      const size_t at = ((size_t)y * 4u + x) * 4u;
      const unsigned idx = (x + y) % 4u;
      for (size_t k = 0; k < 3; k++) {
        EXPECT_NEAR((int)p[at + k], (int)want[idx][k], 1)
            << "pixel (" << x << "," << y << ") channel " << k;
      }
      EXPECT_EQ(p[at + 3], 255u);
    }
  }
}

TEST(TiffDecode, EveryDirectoryIsItsOwnPicture) {
  Loaded img;
  ASSERT_EQ(img.load("tiff_two_pages.tif"), GIMG_OK) << img.reasons();
  ASSERT_EQ(gimg_doc_item_count(img.doc()), 2u);
  // Pages, not frames: a multi-page TIFF is several pictures, and each item
  // says so rather than being a moment of one.
  for (size_t i = 0; i < 2; i++) {
    EXPECT_EQ(gimg_item_role(gimg_doc_item(img.doc(), i)), GIMG_ITEM_IMAGE);
  }
  const std::vector<uint8_t> first = img.pixels(0);
  EXPECT_EQ(first, gray_ramp(4, 4));
  const std::vector<uint8_t> second = img.pixels(1);
  ASSERT_EQ(second.size(), first.size());
  for (size_t i = 0; i < first.size(); i++) {
    EXPECT_EQ((int)second[i], 255 - (int)first[i]);
  }
}

TEST(TiffDecode, EveryRefusalSaysWhichRuleItBroke) {
  struct Case {
    const char * file;
    GIMG_Result expect;
    const char * reason;
  };
  const Case cases[] = {
      {"tiff_bad_magic.tif", GIMG_ERR_UNSUPPORTED, "BigTIFF"},
      {"tiff_lzw_unsupported.tif", GIMG_ERR_UNSUPPORTED, "compressed"},
      {"tiff_no_photometric.tif", GIMG_ERR_CORRUPT,
          "no PhotometricInterpretation"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.file);
    Loaded img;
    EXPECT_EQ(img.load(c.file), c.expect);
    EXPECT_TRUE(img.said(c.reason))
        << "expected a diagnostic naming \"" << c.reason << "\"; got "
        << img.reasons();
  }
}

TEST(TiffDecode, AStreamThatCannotSayHowLongItIsIsRefused) {
  // Every offset in a TIFF is absolute and may point backwards, so the file
  // cannot be read as it arrives. The refusal is the same one the BMP loader
  // gives for run-length data and for a bitmap array, for the same reason.
  const std::vector<uint8_t> bytes = slurp("tiff_4x4_gray8_le.tif");
  ASSERT_FALSE(bytes.empty());
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory_no_seek(bytes.data(), bytes.size(), &s),
      GIMG_OK);
  GIMG_Diagnostics diag = {};
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_load(s, nullptr, &diag, &doc), GIMG_ERR_UNSUPPORTED);
  bool said = false;
  for (size_t i = 0; i < diag.count; i++) {
    const char * a = diag.items[i].recommended_action;
    if (a && std::string(a).find("sized stream") != std::string::npos) {
      said = true;
    }
  }
  EXPECT_TRUE(said);
  gimg_diagnostics_destroy(&diag);
  if (doc) { gimg_doc_destroy(doc); }
  gimg_stream_destroy(s);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
