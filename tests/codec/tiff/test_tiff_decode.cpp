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
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
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

TEST(TiffDecode, EverySubByteDepthUnpacksMostSignificantBitFirst) {
  // A row is padded to a byte boundary (TIFF 6.0 section 3), so six pixels at
  // four bits is three bytes and not two and a half. A decoder that computed
  // two and a half shears the picture one pixel further left on every row,
  // which is why the fixtures are six wide rather than a multiple of eight.
  struct Depth {
    const char * file;
    unsigned bits;
  };
  const Depth depths[] = {
      {"tiff_6x5_gray1.tif", 1u},
      {"tiff_6x5_gray2.tif", 2u},
      {"tiff_6x5_gray4.tif", 4u},
  };
  for (const Depth & d : depths) {
    SCOPED_TRACE(d.file);
    Loaded img;
    ASSERT_EQ(img.load(d.file), GIMG_OK) << img.reasons();
    const std::vector<uint8_t> p = img.pixels();
    ASSERT_EQ(p.size(), 6u * 5u);
    const unsigned top = (1u << d.bits) - 1u;
    for (uint32_t i = 0; i < 30u; i++) {
      // The generator's ramp, and the expansion the format implies: 255 is
      // divisible by 1, 3 and 15, so this conversion is exact at every
      // sub-byte depth and no reader has a rounding choice to make.
      const unsigned stored = (i * top) / 29u;
      EXPECT_EQ((unsigned)p[i], (stored * 255u) / top)
          << "sample " << i << " at " << d.bits << " bits";
    }
  }
}

TEST(TiffDecode, SixteenBitsStaySixteenBitsAndBothOrdersAgree) {
  Loaded le, be;
  ASSERT_EQ(le.load("tiff_4x4_gray16_le.tif"), GIMG_OK) << le.reasons();
  ASSERT_EQ(be.load("tiff_4x4_gray16_be.tif"), GIMG_OK) << be.reasons();
  const std::vector<uint8_t> a = le.pixels();
  const std::vector<uint8_t> b = be.pixels();
  ASSERT_EQ(a.size(), 4u * 4u * 2u) << "a 16-bit file must not be narrowed";
  // The raster carries a copy of the format, not the address of the library's
  // constant, so the fields are what say which format it is.
  const GIMG_Pixel_Format * gray16 = gimg_raster_format(le.raster());
  ASSERT_NE(gray16, nullptr);
  EXPECT_EQ(gray16->channel_count, 1u);
  EXPECT_EQ(gimg_pixel_format_channel_bits(gray16, 0), 16u);
  // The byte-order pair again, and at sixteen bits it is the case that
  // matters: this is the one place a file's declared order reaches past the
  // header and into the pixels.
  EXPECT_EQ(a, b);
  const uint16_t * v = (const uint16_t *)(const void *)a.data();
  for (uint32_t i = 0; i < 16u; i++) {
    EXPECT_EQ((unsigned)v[i], (i * 65535u) / 15u) << "sample " << i;
  }

  Loaded rgb;
  ASSERT_EQ(rgb.load("tiff_4x4_rgb16.tif"), GIMG_OK) << rgb.reasons();
  const std::vector<uint8_t> c = rgb.pixels();
  ASSERT_EQ(c.size(), 4u * 4u * 8u);
  const GIMG_Pixel_Format * rgba16 = gimg_raster_format(rgb.raster());
  ASSERT_NE(rgba16, nullptr);
  EXPECT_EQ(rgba16->channel_count, 4u);
  EXPECT_EQ(gimg_pixel_format_channel_bits(rgba16, 0), 16u);
  const uint16_t * w = (const uint16_t *)(const void *)c.data();
  EXPECT_EQ((unsigned)w[3], 65535u) << "an RGB image has no transparency";
}

TEST(TiffDecode, APaletteIsTheSamePictureAtEveryIndexWidth) {
  // The same four colours and the same (x + y) % 4 picture, once with
  // eight-bit indices and once with four-bit. Nothing about the picture
  // depends on how wide its indices are, so the two rasters are the same
  // bytes - a property that needs no reference decoder and catches an
  // unpacking error that a single-depth test would call correct.
  Loaded eight, four;
  ASSERT_EQ(eight.load("tiff_4x4_palette.tif"), GIMG_OK) << eight.reasons();
  ASSERT_EQ(four.load("tiff_4x4_palette4.tif"), GIMG_OK) << four.reasons();
  const std::vector<uint8_t> a = eight.pixels();
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a, four.pixels());
}

TEST(TiffDecode, PlanarAndInterleavedAreTheSamePicture) {
  // PlanarConfiguration 2 is a storage layout and nothing else: the same
  // picture with its channels written one plane after another. The third
  // member of the family this codec's tests are built on - the byte-order
  // pair and the tiled/stripped pair being the other two - and like them it
  // needs no reference decoder to be worth running.
  Loaded interleaved, planar;
  ASSERT_EQ(interleaved.load("tiff_4x4_rgb.tif"), GIMG_OK)
      << interleaved.reasons();
  ASSERT_EQ(planar.load("tiff_4x4_rgb_planar.tif"), GIMG_OK)
      << planar.reasons();
  const std::vector<uint8_t> a = interleaved.pixels();
  ASSERT_FALSE(a.empty());
  EXPECT_EQ(a, planar.pixels());
}

TEST(TiffDecode, ASeparatedImageComesBackAsInk) {
  // "Separated" is the specification's word (section 16) and CMYK is what
  // every file that uses it means. It comes back as CMYK rather than
  // converted to RGB, because converting is a colour decision the caller can
  // make with gimg_ops_convert and the codec cannot unmake.
  Loaded img;
  ASSERT_EQ(img.load("tiff_4x4_cmyk.tif"), GIMG_OK) << img.reasons();
  // pixels() is what decodes, so the raster only exists after it.
  const std::vector<uint8_t> p = img.pixels();
  ASSERT_EQ(p.size(), 4u * 4u * 4u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(img.raster());
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK);
  EXPECT_EQ(fmt->channel_count, 4u);
  for (size_t i = 0; i < 16u; i++) {
    const uint32_t x = (uint32_t)(i % 4), y = (uint32_t)(i / 4);
    EXPECT_EQ(p[i * 4 + 0], (uint8_t)(x * 255 / 3)) << "cyan at " << i;
    EXPECT_EQ(p[i * 4 + 1], (uint8_t)(y * 255 / 3)) << "magenta at " << i;
    EXPECT_EQ(p[i * 4 + 3], 16u) << "black at " << i;
  }
}

TEST(TiffDecode, EveryCompressionIsTheSamePicture) {
  // One picture, six spellings. A compression is a storage layout like the
  // others this file tests - the byte orders, the tile grid, the planes - so
  // every member of this family must decode to the same bytes as the stored
  // one, and a subtly wrong decompressor fails rather than merely looking
  // plausible.
  //
  // The LZW fixture is worth a word. Its encoder is written out in
  // tests/data/tiff/generate.py from TIFF 6.0 section 13 rather than taken
  // from a library, and the decoder is Ghoti.io Compress's, so the two ends
  // are independent implementations of the same text. That matters most for
  // the early code-width change - the width goes up at 511 rather than 512 -
  // which every LZW-in-TIFF implementation has had to discover, and which
  // shifts every code after the first 254 by one bit when it is missed.
  const char * const files[] = {
      "tiff_16x8_none.tif",
      "tiff_16x8_packbits.tif",
      "tiff_16x8_lzw.tif",
      "tiff_16x8_deflate.tif",
      "tiff_16x8_lzw_predictor.tif",
      "tiff_16x8_deflate_predictor.tif",
  };
  Loaded stored;
  ASSERT_EQ(stored.load(files[0]), GIMG_OK) << stored.reasons();
  const std::vector<uint8_t> want = stored.pixels();
  ASSERT_EQ(want, gray_ramp(16, 8));

  for (const char * name : files) {
    SCOPED_TRACE(name);
    Loaded img;
    ASSERT_EQ(img.load(name), GIMG_OK) << img.reasons();
    EXPECT_EQ(img.pixels(), want);
  }
}

TEST(TiffDecode, AnAbsurdRowsPerStripIsBoundedByThePicture) {
  // The fuzzer's first find. Nothing in this file contradicts anything else -
  // one strip, one offset, one byte count - and it declares 536,870,920 rows
  // per strip for an image eight rows tall. Sizing the decompression buffer
  // by the tag rather than by the picture asked for 8.6 GB of memory.
  //
  // It is a perfectly ordinary picture and decodes to one.
  Loaded img;
  ASSERT_EQ(img.load("tiff_16x8_absurd_rows_per_strip.tif"), GIMG_OK)
      << img.reasons();
  EXPECT_EQ(img.pixels(), gray_ramp(16, 8));
}

TEST(TiffDecode, MetadataSurvivesALoadAndASave) {
  // A TIFF is what professional colour work is stored in, so a profile that
  // arrived has to leave again: a file whose profile was dropped in passing
  // is a file whose colours mean something else, and nothing says so. The
  // same goes for the orientation a camera wrote and the XMP a cataloguing
  // tool did.
  //
  // The round trip is the assertion rather than the read, because a reader
  // that keeps a profile and a writer that drops it look identical from
  // inside the reader.
  Loaded img;
  ASSERT_EQ(img.load("tiff_4x4_metadata.tif"), GIMG_OK) << img.reasons();
  const std::vector<uint8_t> want_pixels = img.pixels();
  ASSERT_FALSE(want_pixels.empty());

  const GIMG_Color_Info * info = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(info, nullptr);
  ASSERT_NE(info->icc_bytes, nullptr) << "the profile did not reach the raster";
  const size_t icc_size = info->icc_size;
  // Four bytes of size, sixteen of signatures, then 256 of filler: the
  // number is the generator's, and a change there should fail here.
  EXPECT_EQ(icc_size, 276u);
  std::vector<uint8_t> want_icc(
      (const uint8_t *)info->icc_bytes, (const uint8_t *)info->icc_bytes + icc_size);

  GIMG_Meta_Common * common = gimg_doc_meta_common(img.doc());
  ASSERT_NE(common, nullptr);
  EXPECT_EQ((int)gimg_meta_common_orientation(common), 6);
  ASSERT_NE(gimg_meta_common_description(common), nullptr);
  EXPECT_STREQ(gimg_meta_common_description(common), "a fixture");

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(img.doc());
  ASSERT_NE(raw, nullptr);
  size_t xmp_size = 0;
  ASSERT_EQ(gimg_meta_raw_get(raw, "tiff", 700u, nullptr, &xmp_size), GIMG_OK);
  EXPECT_GT(xmp_size, 0u);

  // Out and back.
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  GIMG_Save_Report rep = {};
  ASSERT_EQ(gimg_doc_save(img.doc(), out, "tiff", &opts, &rep), GIMG_OK);
  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &bytes, &size);

  GIMG_Stream * back = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, size, &back), GIMG_OK);
  GIMG_Doc * again = nullptr;
  ASSERT_EQ(gimg_doc_load(back, nullptr, nullptr, &again), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(again, 0), nullptr, &raster),
      GIMG_OK);

  const GIMG_Color_Info * back_info = gimg_raster_color_info_const(raster);
  ASSERT_NE(back_info, nullptr);
  ASSERT_NE(back_info->icc_bytes, nullptr) << "the profile was dropped on save";
  ASSERT_EQ(back_info->icc_size, want_icc.size());
  EXPECT_EQ(std::memcmp(back_info->icc_bytes, want_icc.data(),
                want_icc.size()),
      0)
      << "the profile changed on the way out and back";

  // **The orientation is applied, not carried.** gimg_item_decode rotates
  // the raster for every codec here, so the pixels that reach a writer are
  // the display image and the file it writes declares no orientation -
  // absent means 1, which is what those pixels are. Writing the source's tag
  // beside already-rotated pixels would have the next reader rotate them
  // again, and the first draft of this writer did exactly that.
  //
  // So the assertion is that the picture survives, not that the tag does.
  GIMG_Meta_Common * back_common = gimg_doc_meta_common(again);
  if (back_common) {
    const GIMG_Orientation back_orient =
        gimg_meta_common_orientation(back_common);
    EXPECT_TRUE(back_orient == GIMG_ORIENTATION_UNKNOWN ||
        back_orient == GIMG_ORIENTATION_NORMAL)
        << "the written file re-declared an orientation its pixels already "
           "have, so reading it back rotates them twice";
  }
  {
    const uint32_t w = gimg_raster_width(raster);
    const uint32_t h = gimg_raster_height(raster);
    const size_t stride = gimg_raster_stride_bytes(raster);
    const size_t bpp = gimg_raster_bytes_per_pixel(
        gimg_raster_format(raster));
    std::vector<uint8_t> flat;
    const uint8_t * p = (const uint8_t *)gimg_raster_pixels(raster);
    for (uint32_t y = 0; y < h; y++) {
      flat.insert(flat.end(), p + (y * stride), p + (y * stride) + (w * bpp));
    }
    EXPECT_EQ(flat, want_pixels)
        << "the picture changed on the way out and back";
  }
  ASSERT_NE(gimg_meta_common_description(back_common), nullptr);
  EXPECT_STREQ(gimg_meta_common_description(back_common), "a fixture");
  GIMG_Meta_Raw * back_raw = gimg_doc_meta_raw(again);
  ASSERT_NE(back_raw, nullptr);
  size_t back_xmp = 0;
  EXPECT_EQ(gimg_meta_raw_get(back_raw, "tiff", 700u, nullptr, &back_xmp),
      GIMG_OK);
  EXPECT_EQ(back_xmp, xmp_size);

  gimg_raster_destroy(raster);
  gimg_doc_destroy(again);
  gimg_stream_destroy(back);
  gimg_stream_destroy(out);
}

TEST(TiffDecode, APyramidLevelIsNotAnotherPage) {
  // TIFF spells "a smaller copy of this image" two ways: a directory in the
  // main chain whose NewSubfileType has bit 0 set, and a SubIFD hanging off
  // the full-size page (Technical Note 1). Both are read, and both produce
  // the same thing in the document model - a GIMG_ITEM_LEVEL of the page it
  // belongs to, rather than a second picture.
  //
  // That distinction is the reason the role exists. A caller counting the
  // pictures in a document must not count a thumbnail of one of them, and
  // before this nothing in the API could tell it not to.
  const char * const files[] = {
      "tiff_pyramid_chain.tif", "tiff_pyramid_subifd.tif"};
  for (const char * file : files) {
    SCOPED_TRACE(file);
    Loaded img;
    ASSERT_EQ(img.load(file), GIMG_OK) << img.reasons();
    ASSERT_EQ(gimg_doc_item_count(img.doc()), 2u);

    EXPECT_EQ(gimg_item_role(gimg_doc_item(img.doc(), 0)), GIMG_ITEM_IMAGE);
    EXPECT_EQ(gimg_item_role(gimg_doc_item(img.doc(), 1)), GIMG_ITEM_LEVEL);
    EXPECT_EQ(gimg_item_role_subject(gimg_doc_item(img.doc(), 1)), 0u)
        << "a level is a version of the page it belongs to";

    // And it really is the smaller copy, not a second decode of the first.
    EXPECT_EQ(img.pixels(0), gray_ramp(8, 8));
    const std::vector<uint8_t> level = img.pixels(1);
    ASSERT_EQ(level.size(), 4u * 4u);
    for (uint32_t y = 0; y < 4u; y++) {
      for (uint32_t x = 0; x < 4u; x++) {
        // The generator takes every other pixel of the full-size image.
        EXPECT_EQ(level[(y * 4u) + x], gray_ramp(8, 8)[(y * 2u * 8u) + (x * 2u)])
            << "level pixel (" << x << "," << y << ")";
      }
    }
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
      {"tiff_ccitt_unsupported.tif", GIMG_ERR_UNSUPPORTED,
          "compression method this codec does not undo"},
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
