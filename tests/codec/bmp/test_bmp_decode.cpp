/**
 * @file
 *
 * BMP load and decode tests.  Fixtures come from tests/data/bmp/, generated
 * by tests/data/bmp/generate.py; the valid ones were cross-checked against
 * Pillow, and the two places where this decoder deliberately differs from it
 * are called out at the tests concerned.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

#include "bmp_test_utils.h"

using bmp_test::Loaded;
using bmp_test::Rgba;

namespace {

/**
 * The 4x4 pattern the fixtures encode, in top-down order.
 *
 * Every component differs across the image, so a channel swap, a row flip, or
 * a stride error all show up as a specific wrong pixel rather than as a
 * plausible-looking image.
 */
const Rgba kPattern[4][4] = {
    {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 0, 255}},
    {{255, 0, 255, 255}, {0, 255, 255, 255}, {255, 255, 255, 255},
        {0, 0, 0, 255}},
    {{128, 0, 0, 255}, {0, 128, 0, 255}, {0, 0, 128, 255}, {128, 128, 0, 255}},
    {{64, 32, 16, 255}, {16, 32, 64, 255}, {200, 100, 50, 255},
        {50, 100, 200, 255}},
};

/** Palette shared by the indexed and RLE fixtures. */
const Rgba kRed = {255, 0, 0, 255};
const Rgba kGreen = {0, 255, 0, 255};
const Rgba kBlue = {0, 0, 255, 255};
const Rgba kYellow = {255, 255, 0, 255};
const Rgba kMagenta = {255, 0, 255, 255};
const Rgba kCyan = {0, 255, 255, 255};

void expect_pattern(const Loaded & img) {
  ASSERT_EQ(img.width(), 4u);
  ASSERT_EQ(img.height(), 4u);
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      EXPECT_EQ(img.at(x, y), kPattern[y][x]) << "at (" << x << "," << y << ")";
    }
  }
}

} // namespace

// ---------------------------------------------------------------------------
// Registration and probing
// ---------------------------------------------------------------------------

TEST(BmpCodec, IsRegistered) {
  GIMG_Codec * codec = gimg_codec_by_name("bmp");
  ASSERT_NE(codec, nullptr);
  unsigned int caps = gimg_codec_capabilities(codec);
  EXPECT_TRUE(caps & GIMG_CAP_READ);
  EXPECT_TRUE(caps & GIMG_CAP_WRITE);
  EXPECT_TRUE(caps & GIMG_CAP_PALETTE);
}

TEST(BmpCodec, ProbeIdentifiesBmp) {
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(bmp_test::load_file("bmp_4x4_24bit.bmp", bytes));

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &s), GIMG_OK);

  GIMG_Probe_Result probe = {};
  EXPECT_EQ(gimg_probe(s, &probe), GIMG_OK);
  ASSERT_NE(probe.format_name, nullptr);
  EXPECT_STREQ(probe.format_name, "bmp");
  EXPECT_GT(probe.confidence, 0u);

  gimg_stream_destroy(s);
}

// ---------------------------------------------------------------------------
// True color
// ---------------------------------------------------------------------------

TEST(BmpDecode, Rgb24) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_24bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  expect_pattern(img);
}

TEST(BmpDecode, Rgb24TopDown) {
  // A negative biHeight means the rows are already top-down.  Decoding it as
  // bottom-up would produce a vertically mirrored image that still passes any
  // test asserting only dimensions.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_24bit_topdown.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  expect_pattern(img);
}

TEST(BmpDecode, SinglePixel) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_1x1_24bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  EXPECT_EQ(img.width(), 1u);
  EXPECT_EQ(img.height(), 1u);
  EXPECT_EQ(img.at(0, 0), (Rgba{17, 34, 51, 255}));
}

TEST(BmpDecode, Rgb16IsFiveFiveFive) {
  // 16-bit BI_RGB is 5-5-5 with the top bit unused.  Reading it as 5-6-5
  // shifts red down one bit and takes green from the wrong span, so pure red
  // (0x7C00) would decode as a dark red-green mix instead.
  const Rgba expected[4][4] = {
      {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
          {255, 255, 0, 255}},
      {{255, 0, 255, 255}, {0, 255, 255, 255}, {255, 255, 255, 255},
          {0, 0, 0, 255}},
      {{132, 0, 0, 255}, {0, 132, 0, 255}, {0, 0, 132, 255},
          {132, 132, 0, 255}},
      {{66, 33, 16, 255}, {16, 33, 66, 255}, {206, 99, 49, 255},
          {49, 99, 206, 255}},
  };

  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_16bit_555.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 4u);
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      EXPECT_EQ(img.at(x, y), expected[y][x]) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpDecode, Rgb16BitfieldsFiveSixFive) {
  // Green gets six bits here, so the 128 in the pattern lands on a different
  // quantization step than in the 5-5-5 file above.
  const Rgba expected[4][4] = {
      {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255},
          {255, 255, 0, 255}},
      {{255, 0, 255, 255}, {0, 255, 255, 255}, {255, 255, 255, 255},
          {0, 0, 0, 255}},
      {{132, 0, 0, 255}, {0, 130, 0, 255}, {0, 0, 132, 255},
          {132, 130, 0, 255}},
      {{66, 32, 16, 255}, {16, 32, 66, 255}, {206, 101, 49, 255},
          {49, 101, 206, 255}},
  };

  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_16bit_565.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      EXPECT_EQ(img.at(x, y), expected[y][x]) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpDecode, Rgb32ZeroHighByteIsOpaque) {
  // 32-bit BI_RGB leaves the fourth byte undefined and plenty of writers
  // store zero there.  Honoring it as alpha would decode the whole image as
  // fully transparent.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_32bit_zero_high_byte.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  expect_pattern(img);
}

TEST(BmpDecode, Rgb32BitfieldsAlphaIsHonored) {
  // With an explicit alpha mask the channel is real and must survive.  The
  // fixture ramps alpha per row: 255, 170, 85, 0 from the top.
  //
  // Pillow opens this file as RGB and discards the alpha entirely, so it is
  // not a reference for this case; the masks in the V3 header are
  // unambiguous.
  const uint8_t expected_alpha[4] = {255, 170, 85, 0};

  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_32bit_alpha.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      Rgba px = img.at(x, y);
      EXPECT_EQ(px.a, expected_alpha[y]) << "alpha at (" << x << "," << y << ")";
      EXPECT_EQ(px.r, kPattern[y][x].r) << "red at (" << x << "," << y << ")";
      EXPECT_EQ(px.g, kPattern[y][x].g) << "green at (" << x << "," << y << ")";
      EXPECT_EQ(px.b, kPattern[y][x].b) << "blue at (" << x << "," << y << ")";
    }
  }
}

// ---------------------------------------------------------------------------
// Indexed
// ---------------------------------------------------------------------------

TEST(BmpDecode, Indexed8) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_8bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 8u);
  ASSERT_EQ(img.height(), 2u);

  const Rgba row0[8] = {kRed, kGreen, kBlue, kYellow, kMagenta, kCyan, kRed,
      kGreen};
  const Rgba row1[8] = {kCyan, kMagenta, kYellow, kBlue, kGreen, kRed, kCyan,
      kMagenta};
  for (uint32_t x = 0; x < 8; x++) {
    EXPECT_EQ(img.at(x, 0), row0[x]) << "row 0, x=" << x;
    EXPECT_EQ(img.at(x, 1), row1[x]) << "row 1, x=" << x;
  }
}

TEST(BmpDecode, Indexed4) {
  // Two indices per byte: an off-by-one in the shift swaps every pair.
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_4bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const Rgba row0[8] = {kRed, kGreen, kBlue, kYellow, kMagenta, kCyan, kRed,
      kGreen};
  for (uint32_t x = 0; x < 8; x++) {
    EXPECT_EQ(img.at(x, 0), row0[x]) << "row 0, x=" << x;
  }
  EXPECT_EQ(img.at(0, 1), kCyan);
  EXPECT_EQ(img.at(7, 1), kMagenta);
}

TEST(BmpDecode, Indexed1) {
  // A checkerboard, so a shift error in the bit unpacking inverts the image
  // rather than producing something that still looks like a checkerboard.
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x4_1bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 8u);
  ASSERT_EQ(img.height(), 4u);

  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      bool white = ((x + y) % 2) == 0;
      Rgba expected = white ? Rgba{255, 255, 255, 255} : Rgba{0, 0, 0, 255};
      EXPECT_EQ(img.at(x, y), expected) << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpDecode, BitmapCoreHeader) {
  // The OS/2 12-byte header: 16-bit dimensions, 3-byte palette entries, and
  // no biClrUsed, so the palette is the full 2^bpp.
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_core.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 8u);
  ASSERT_EQ(img.height(), 2u);
  EXPECT_EQ(img.at(0, 0), kRed);
  EXPECT_EQ(img.at(5, 0), kCyan);
  EXPECT_EQ(img.at(0, 1), kCyan);
}

TEST(BmpDecode, PaletteFixtureMatchesIndexedFixture) {
  // The 4-bit and 8-bit fixtures encode the same indices through the same
  // palette, so their decoded output must be identical.  This catches a bug
  // in exactly one of the two unpacking paths.
  Loaded four;
  Loaded eight;
  ASSERT_EQ(four.load("bmp_8x2_4bit.bmp"), GIMG_OK);
  ASSERT_EQ(four.decode(), GIMG_OK);
  ASSERT_EQ(eight.load("bmp_8x2_8bit.bmp"), GIMG_OK);
  ASSERT_EQ(eight.decode(), GIMG_OK);

  ASSERT_EQ(four.width(), eight.width());
  ASSERT_EQ(four.height(), eight.height());
  for (uint32_t y = 0; y < four.height(); y++) {
    for (uint32_t x = 0; x < four.width(); x++) {
      EXPECT_EQ(four.at(x, y), eight.at(x, y)) << "at (" << x << "," << y << ")";
    }
  }
}

// ---------------------------------------------------------------------------
// RLE
// ---------------------------------------------------------------------------

TEST(BmpDecode, Rle8) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_rle8.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 8u);
  ASSERT_EQ(img.height(), 2u);

  // Row 0 is the last row encoded: a single run of eight.
  for (uint32_t x = 0; x < 8; x++) {
    EXPECT_EQ(img.at(x, 0), kRed) << "row 0, x=" << x;
  }
  // Row 1: an encoded run of four, then an absolute run of four.
  const Rgba row1[8] = {kBlue, kBlue, kBlue, kBlue, kGreen, kBlue, kYellow,
      kMagenta};
  for (uint32_t x = 0; x < 8; x++) {
    EXPECT_EQ(img.at(x, 1), row1[x]) << "row 1, x=" << x;
  }
}

TEST(BmpDecode, Rle4) {
  // An encoded RLE4 run alternates the two nibbles of the value byte, so 0x12
  // expands to 1,2,1,2 rather than to four copies of one index.
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_rle4.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  for (uint32_t x = 0; x < 8; x++) {
    EXPECT_EQ(img.at(x, 0), kRed) << "row 0, x=" << x;
  }
  const Rgba row1[8] = {kGreen, kBlue, kGreen, kBlue, kYellow, kMagenta, kCyan,
      kRed};
  for (uint32_t x = 0; x < 8; x++) {
    EXPECT_EQ(img.at(x, 1), row1[x]) << "row 1, x=" << x;
  }
}

TEST(BmpDecode, Rle8DeltaResumesAtTheOffsetPosition) {
  // Per the BMP specification, the two bytes after a delta escape move the
  // current position right and down, and encoding continues from there.
  // Pixels the delta skipped are never written and keep the raster's zeroed
  // value.
  //
  // Pillow stops emitting after the delta in this file, so it is not a
  // reference here.
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_rle8_delta.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  // Row 1 (the first encoded row): two green, three skipped, three blue.
  EXPECT_EQ(img.at(0, 1), kGreen);
  EXPECT_EQ(img.at(1, 1), kGreen);
  for (uint32_t x = 2; x < 5; x++) {
    EXPECT_EQ(img.at(x, 1), (Rgba{0, 0, 0, 0})) << "skipped pixel x=" << x;
  }
  for (uint32_t x = 5; x < 8; x++) {
    EXPECT_EQ(img.at(x, 1), kBlue) << "after delta, x=" << x;
  }
}

// ---------------------------------------------------------------------------
// Malformed input
// ---------------------------------------------------------------------------

TEST(BmpLoad, RejectsBadMagic) {
  Loaded img;
  GIMG_Result r = img.load("bmp_bad_magic.bmp");
  EXPECT_NE(r, GIMG_OK);
}

TEST(BmpLoad, RejectsZeroWidth) {
  Loaded img;
  EXPECT_EQ(img.load("bmp_zero_width.bmp"), GIMG_ERR_CORRUPT);
}

TEST(BmpLoad, RejectsUnknownHeaderSize) {
  Loaded img;
  EXPECT_EQ(img.load("bmp_unknown_header_size.bmp"), GIMG_ERR_UNSUPPORTED);
}

TEST(BmpLoad, RejectsNonContiguousMask) {
  // A mask whose set bits are not one run has no unambiguous shift, so the
  // file cannot be interpreted rather than guessed at.
  Loaded img;
  EXPECT_EQ(img.load("bmp_noncontiguous_mask.bmp"), GIMG_ERR_CORRUPT);
}

TEST(BmpLoad, RejectsTruncatedPixelData) {
  // The header claims four rows of 24-bit pixels; only one is present.
  Loaded img;
  EXPECT_NE(img.load("bmp_truncated_pixels.bmp"), GIMG_OK);
}

TEST(BmpLoad, RejectsOffsetPastEndOfFile) {
  Loaded img;
  EXPECT_EQ(img.load("bmp_offset_past_eof.bmp"), GIMG_ERR_CORRUPT);
}

TEST(BmpDecode, RejectsPaletteIndexOutOfRange) {
  // The pixel data references index 7 with only two palette entries in the
  // file.  Reading it would be an out-of-bounds read of the palette array.
  Loaded img;
  ASSERT_EQ(img.load("bmp_bad_palette_index.bmp"), GIMG_OK);
  EXPECT_EQ(img.decode(), GIMG_ERR_CORRUPT);
}

TEST(BmpLoad, ReportsDiagnosticsOnFailure) {
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(bmp_test::load_file("bmp_unknown_header_size.bmp", bytes));

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &s), GIMG_OK);

  GIMG_Diagnostics diags = {};
  gimg_diagnostics_init(&diags, nullptr);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, &diags, &doc);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  EXPECT_GT(diags.count, 0u) << "a rejected file should say why";

  gimg_diagnostics_clear(&diags);
  gimg_stream_destroy(s);
}

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

TEST(BmpLoad, EnforcesMaxDecodedPixels) {
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(bmp_test::load_file("bmp_4x4_24bit.bmp", bytes));

  GIMG_Limits limits;
  gimg_limits_default(&limits);
  limits.max_decoded_pixels = 15; // The image has 16.

  GIMG_Load_Options options = {};
  options.limits = &limits;

  Loaded img;
  EXPECT_EQ(img.load_bytes(bytes, &options), GIMG_ERR_LIMIT);
}

TEST(BmpLoad, EnforcesMaxMemory) {
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(bmp_test::load_file("bmp_4x4_24bit.bmp", bytes));

  GIMG_Limits limits;
  gimg_limits_default(&limits);
  limits.max_memory = 8; // Far below the 48 bytes of pixel data.

  GIMG_Load_Options options = {};
  options.limits = &limits;

  Loaded img;
  EXPECT_EQ(img.load_bytes(bytes, &options), GIMG_ERR_LIMIT);
}

TEST(BmpLoad, AllowsLimitsThatFit) {
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(bmp_test::load_file("bmp_4x4_24bit.bmp", bytes));

  GIMG_Limits limits;
  gimg_limits_default(&limits);
  limits.max_decoded_pixels = 16;
  limits.max_memory = 1024;

  GIMG_Load_Options options = {};
  options.limits = &limits;

  Loaded img;
  ASSERT_EQ(img.load_bytes(bytes, &options), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  expect_pattern(img);
}

// ---------------------------------------------------------------------------
// Document shape
// ---------------------------------------------------------------------------

TEST(BmpLoad, ProducesASingleItem) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_24bit.bmp"), GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(img.doc()), 1u);
}

TEST(BmpDecode, RejectsItemIndexBeyondTheFirst) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_24bit.bmp"), GIMG_OK);
  EXPECT_EQ(gimg_doc_item(img.doc(), 1), nullptr);
}

TEST(BmpDecode, ProducesRgba8) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_24bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  const GIMG_Pixel_Format * f = gimg_raster_format(img.raster());
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->channel_model, GIMG_CHANNEL_RGBA);
  EXPECT_EQ(f->channel_count, 4);
  EXPECT_EQ(f->bits_per_channel[0], 8);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
