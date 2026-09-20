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
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
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

TEST(BmpDecode, Rgb32DirtyHighByteIsStillOpaque) {
  // BI_RGB does not define the fourth byte, so a file whose spare bytes are
  // merely dirty must not come out full of holes.  Pillow, GdkPixbuf and
  // netpbm all decode bmpsuite's q/rgb32fakealpha.bmp opaque, and this
  // fixture is that file in miniature.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_32bit_dirty_high_byte.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  expect_pattern(img);
}

TEST(BmpDecode, Rgb32HighByteIsAlphaWhenTheCallerAsksForIt) {
  // GIMG_BMP_RGB32_ALPHA_HEURISTIC is for a caller whose writers are known to
  // put alpha in the undeclared byte.  The fixture's spare bytes ramp 255,
  // 170, 85, 0 reading down from the top.
  const uint8_t expected_alpha[4] = {255, 170, 85, 0};

  GIMG_Load_Options options = {};
  options.bmp_rgb32_alpha = GIMG_BMP_RGB32_ALPHA_HEURISTIC;

  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_32bit_dirty_high_byte.bmp", &options), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  for (uint32_t y = 0; y < 4; y++) {
    for (uint32_t x = 0; x < 4; x++) {
      EXPECT_EQ(img.at(x, y).a, expected_alpha[y])
          << "alpha at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpDecode, Rgb32AllZeroHighBytesStayOpaqueUnderTheHeuristic) {
  // The heuristic's whole point: a file whose spare bytes are uniformly zero
  // is opaque, not invisible, however the caller asked for it to be read.
  GIMG_Load_Options options = {};
  options.bmp_rgb32_alpha = GIMG_BMP_RGB32_ALPHA_HEURISTIC;

  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_32bit_zero_high_byte.bmp", &options), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  expect_pattern(img);
}

TEST(BmpDecode, AlphaBitfieldsMasksFollowTheHeader) {
  // A V3 header carries its four masks inside itself; BI_ALPHABITFIELDS puts
  // them in the sixteen bytes after a plain 40-byte BITMAPINFOHEADER, where
  // the palette would otherwise begin.  bmpsuite's q/rgba32abf.bmp is the
  // same case, and no decoder installed here reads one.
  const uint8_t expected_alpha[4] = {255, 170, 85, 0};

  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_alphabitfields.bmp"), GIMG_OK);
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

TEST(BmpLoad, OversizeColorCountIsClampedRatherThanRejected) {
  // A biClrUsed above 2^bpp names entries no index can reach.  Such files are
  // common and the surplus is unreachable rather than wrong, so the count is
  // clamped and the picture decoded - which is more permissive than Pillow
  // and GdkPixbuf, both of which refuse bmpsuite's q/pal8oversizepal.bmp.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x1_oversize_palette.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 4u);
  // The generator's entry i is (r, g, b) = (i, i * 3, i * 5), truncated.
  for (uint32_t x = 0; x < 4; x++) {
    unsigned int index = (x == 3) ? 255u : x;
    Rgba px = img.at(x, 0);
    EXPECT_EQ(px.r, (uint8_t)index) << "at x=" << x;
    EXPECT_EQ(px.g, (uint8_t)(index * 3u)) << "at x=" << x;
    EXPECT_EQ(px.b, (uint8_t)(index * 5u)) << "at x=" << x;
  }
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
// OS/2 2.x
// ---------------------------------------------------------------------------

TEST(BmpDecode, Os2V2HeaderDecodesLikeAWindowsOne) {
  // A BITMAPCOREHEADER2's first 40 bytes are byte for byte a
  // BITMAPINFOHEADER, so the same picture written under either header must
  // decode the same way.  That is a property of the two layouts and needs no
  // reference decoder to check.
  Loaded windows, os2;
  ASSERT_EQ(windows.load("bmp_8x2_8bit.bmp"), GIMG_OK);
  ASSERT_EQ(windows.decode(), GIMG_OK);
  ASSERT_EQ(os2.load("bmp_8x2_os2v2_64.bmp"), GIMG_OK);
  ASSERT_EQ(os2.decode(), GIMG_OK);

  ASSERT_EQ(os2.width(), windows.width());
  ASSERT_EQ(os2.height(), windows.height());
  for (uint32_t y = 0; y < os2.height(); y++) {
    for (uint32_t x = 0; x < os2.width(); x++) {
      EXPECT_EQ(os2.at(x, y), windows.at(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpDecode, Os2V2HeaderMayStopAtSixteenBytes) {
  // OS/2 2.x lets the header end at any multiple of 4 from 16 to 64, with
  // every field it stops short of reading as zero.  A 16-byte header stops
  // before biCompression and biClrUsed, so the image is uncompressed and the
  // palette is the depth's full size.  bmpsuite carries the same pair as
  // q/pal8os2v2-16.bmp and q/pal8os2v2.bmp.
  Loaded full, tiny;
  ASSERT_EQ(full.load("bmp_8x2_os2v2_64.bmp"), GIMG_OK);
  ASSERT_EQ(full.decode(), GIMG_OK);
  ASSERT_EQ(tiny.load("bmp_8x2_os2v2_16.bmp"), GIMG_OK);
  ASSERT_EQ(tiny.decode(), GIMG_OK);

  ASSERT_EQ(tiny.width(), full.width());
  ASSERT_EQ(tiny.height(), full.height());
  for (uint32_t y = 0; y < tiny.height(); y++) {
    for (uint32_t x = 0; x < tiny.width(); x++) {
      EXPECT_EQ(tiny.at(x, y), full.at(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
}

TEST(BmpLoad, Os2CompressionThreeIsHuffmanAndNotBitfields) {
  // 3 is BI_BITFIELDS to a Windows header and Huffman 1D to an OS/2 one.
  // Reading it as bitfields would look for masks that are not there and
  // decode whatever followed the header as a channel layout.
  Loaded img;
  EXPECT_EQ(img.load("bmp_8x2_os2v2_huffman.bmp"), GIMG_ERR_UNSUPPORTED);
}

TEST(BmpDecode, Os2Rle24) {
  // RLE24 is OS/2's compression 4 - Windows spells BI_JPEG there.  It carries
  // a BGR triple per pixel and uses no palette.  The fixture runs every form
  // through one 8x2 image: an encoded run, an absolute run, a delta, an end
  // of line and an end of bitmap.  Pixels the delta skipped are left at the
  // raster's zeroed value - transparent, not opaque black - which is what the
  // RLE8 and RLE4 paths do with a delta too.
  const Rgba kSkipped = {0, 0, 0, 0};
  const Rgba kWhite = {255, 255, 255, 255};
  const Rgba expected[2][8] = {
      {kCyan, kCyan, kSkipped, kSkipped, kWhite, kWhite, kSkipped, kSkipped},
      {kRed, kRed, kRed, kGreen, kBlue, kYellow, kMagenta, kMagenta},
  };

  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_rle24.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 8u);
  ASSERT_EQ(img.height(), 2u);
  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      EXPECT_EQ(img.at(x, y), expected[y][x])
          << "at (" << x << "," << y << ")";
    }
  }
}

// ---------------------------------------------------------------------------
// Embedded streams (BI_JPEG, BI_PNG)
// ---------------------------------------------------------------------------

namespace {

/**
 * Decode a fixture that is a bare JPEG or PNG, for use as a second opinion on
 * what its BI_JPEG or BI_PNG wrapper should have produced.
 *
 * This is deliberately not a comparison against a hard-coded picture.  What
 * the wrapper has to get right is handing the payload to the right codec
 * untouched; whether that codec is correct is the JPEG and PNG suites' job,
 * and re-asserting it here would only pin the two together.
 */
void expect_same_image(const Loaded & wrapped, const Loaded & bare) {
  ASSERT_EQ(wrapped.width(), bare.width());
  ASSERT_EQ(wrapped.height(), bare.height());
  for (uint32_t y = 0; y < bare.height(); y++) {
    for (uint32_t x = 0; x < bare.width(); x++) {
      EXPECT_EQ(wrapped.at(x, y), bare.at(x, y))
          << "at (" << x << "," << y << ")";
    }
  }
}

} // namespace

TEST(BmpDecode, EmbeddedPngDecodesAsThePayloadDoes) {
  // BI_PNG means the pixel data is a whole PNG stream.  This library has a
  // PNG codec, so the wrapper's work is to find the payload and hand it over.
  Loaded wrapped, bare;
  ASSERT_EQ(wrapped.load("bmp_4x4_embedded_png.bmp"), GIMG_OK);
  ASSERT_EQ(wrapped.decode(), GIMG_OK);
  ASSERT_EQ(bare.load("bmp_4x4_embedded_png_payload.png"), GIMG_OK);
  ASSERT_EQ(bare.decode(), GIMG_OK);
  expect_same_image(wrapped, bare);
}

TEST(BmpDecode, EmbeddedJpegDecodesAsThePayloadDoes) {
  Loaded wrapped, bare;
  ASSERT_EQ(wrapped.load("bmp_4x4_embedded_jpeg.bmp"), GIMG_OK);
  ASSERT_EQ(wrapped.decode(), GIMG_OK);
  ASSERT_EQ(bare.load("bmp_4x4_embedded_jpeg_payload.jpg"), GIMG_OK);
  ASSERT_EQ(bare.decode(), GIMG_OK);
  expect_same_image(wrapped, bare);
}

TEST(BmpLoad, EmbeddedStreamGoesToTheCodecTheHeaderNamed) {
  // The payload is loaded through the codec biCompression named, never
  // through the prober.  Probing would let a BI_PNG wrapper hold another BMP,
  // which could hold another, with no bound on the nesting this side of the
  // stack.  A payload that is not a PNG is therefore a failure, not an
  // invitation to look for something else.
  Loaded img;
  EXPECT_NE(img.load("bmp_embedded_png_not_a_png.bmp"), GIMG_OK);
}

TEST(BmpLoad, EmbeddedStreamHonorsTheCallersLimits) {
  // The load options go down to the inner codec unchanged, so a limit applies
  // to what is inside a wrapper exactly as it would to a file that arrived on
  // its own.  Without that a BI_PNG header of a few dozen bytes would be a
  // way around every cap the caller set.
  GIMG_Limits limits = {};
  limits.max_decoded_pixels = 4;  // The payload is 4x4 = 16.
  GIMG_Load_Options options = {};
  options.limits = &limits;

  Loaded img;
  EXPECT_EQ(img.load("bmp_4x4_embedded_png.bmp", &options), GIMG_ERR_LIMIT);
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

TEST(BmpDecode, Indexed2) {
  // 2 bits per pixel: a Windows CE addition the desktop API never accepted,
  // read through the same bit-unpacking as 1, 4 and 8.  bmpsuite carries
  // q/pal2.bmp and q/pal2color.bmp for the same case.
  const Rgba * palette[4] = {&kRed, &kGreen, &kBlue, &kYellow};
  const unsigned int indices[2][8] = {
      {0, 1, 2, 3, 3, 2, 1, 0},
      {3, 2, 1, 0, 0, 1, 2, 3},
  };

  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_2bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);
  ASSERT_EQ(img.width(), 8u);
  ASSERT_EQ(img.height(), 2u);
  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      EXPECT_EQ(img.at(x, y), *palette[indices[y][x]])
          << "at (" << x << "," << y << ")";
    }
  }
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

TEST(BmpLoad, RejectsRleWithTopDownRows) {
  // A negative biHeight with BI_RLE8: the format does not allow the pair, and
  // decoding one bottom-up produced a silently upside-down image.  GdkPixbuf
  // and netpbm both refuse such a file; bmpsuite carries one as b/rletopdown.
  Loaded img;
  EXPECT_EQ(img.load("bmp_rle8_topdown.bmp"), GIMG_ERR_CORRUPT);
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

TEST(BmpLoad, ReadsThePhysicalResolutionTheHeaderStates) {
  // generate.py's info_header() writes 2835 pixels per metre on both axes,
  // which is 72 dpi.  A BMP states its resolution in the header rather than
  // in an optional chunk, so the document's common metadata is the only place
  // it can go - and where the PNG and JPEG codecs already put theirs.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_24bit_topdown.bmp"), GIMG_OK);
  GIMG_Meta_Common * meta = gimg_doc_meta_common(img.doc());
  ASSERT_NE(meta, nullptr);
  uint32_t x_dpi = 0, y_dpi = 0;
  gimg_meta_common_dpi(meta, &x_dpi, &y_dpi);
  EXPECT_EQ(x_dpi, 72u);
  EXPECT_EQ(y_dpi, 72u);
}

TEST(BmpLoad, StatesNoResolutionForACoreHeader) {
  // BITMAPCOREHEADER has no density fields at all, so there is nothing to
  // report and the document must not claim one.
  Loaded img;
  ASSERT_EQ(img.load("bmp_8x2_core.bmp"), GIMG_OK);
  GIMG_Meta_Common * meta = gimg_doc_meta_common(img.doc());
  if (meta) {
    uint32_t x_dpi = 0, y_dpi = 0;
    gimg_meta_common_dpi(meta, &x_dpi, &y_dpi);
    EXPECT_EQ(x_dpi, 0u);
    EXPECT_EQ(y_dpi, 0u);
  }
}

// ---------------------------------------------------------------------------
// V4 and V5 color
// ---------------------------------------------------------------------------

TEST(BmpDecode, V4CalibratedEndpointsNameTheGamut) {
  // LCS_CALIBRATED_RGB describes the space rather than naming it.  The
  // endpoints are declared CIEXYZ and written by every writer in reach as
  // xyY chromaticities; these are the BT.709 primaries sRGB shares, with a
  // gamma of 2.2.  bmpsuite's g/pal8v4.bmp carries the same values.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_v4_calibrated.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(color->primaries, GIMG_PRIMARIES_SRGB);
  EXPECT_EQ(color->white_point, GIMG_PRIMARIES_SRGB);
  EXPECT_EQ(color->transfer, GIMG_TRANSFER_GAMMA);
  EXPECT_NEAR(color->gamma_value, 2.2, 0.001);
}

TEST(BmpDecode, V4EndpointsDistinguishAdobeRgbFromSrgb) {
  // The two gamuts share their red and blue primaries and differ only in
  // green, so a decoder that looked at fewer than three would call this sRGB.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_v4_adobe.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(color->primaries, GIMG_PRIMARIES_ADOBE_RGB);
}

TEST(BmpDecode, V4GammasThatDisagreeLeaveTheTransferUnsaid) {
  // GIMG_Color_Info holds one transfer function.  Three different gammas
  // describe a space it cannot state, and averaging them would be a claim
  // about the pixels that the file did not make.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_v4_split_gamma.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(color->transfer, GIMG_TRANSFER_UNKNOWN);
  EXPECT_EQ(color->primaries, GIMG_PRIMARIES_SRGB) << "the gamut is still known";
}

TEST(BmpDecode, V5NamesSrgbAndItsRenderingIntent) {
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_v5_srgb.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(color->primaries, GIMG_PRIMARIES_SRGB);
  EXPECT_EQ(color->transfer, GIMG_TRANSFER_SRGB);
  // LCS_GM_GRAPHICS is the relative colorimetric intent.
  EXPECT_EQ(color->intent, GIMG_INTENT_RELATIVE_COLORIMETRIC);
}

TEST(BmpDecode, V5EmbeddedProfileSurvivesIntact) {
  // The profile lives outside the header, at an offset measured from the
  // header's own start.  Nothing here parses it, so what matters is that
  // every byte arrives.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_v5_icc.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  ASSERT_EQ(color->icc_size, 128u);
  ASSERT_NE(color->icc_bytes, nullptr);

  const uint8_t * icc = static_cast<const uint8_t *>(color->icc_bytes);
  // The generator writes the profile length big-endian at 0 and the ICC file
  // signature at 36, then a known ramp.
  EXPECT_EQ(icc[0], 0u);
  EXPECT_EQ(icc[3], 128u);
  EXPECT_EQ(memcmp(icc + 36, "acsp", 4), 0);
  for (size_t i = 40; i < 128; i++) {
    EXPECT_EQ(icc[i], (uint8_t)((i * 7u) & 0xFFu)) << "profile byte " << i;
  }
}

TEST(BmpDecode, V5LinkedProfileIsNotFollowed) {
  // PROFILE_LINKED names a file rather than carrying one.  Opening a path an
  // image file names is acting on data - it is the shape of a directory
  // traversal - so the image decodes untagged instead.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_v5_linked_profile.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(color->icc_size, 0u);
  EXPECT_EQ(color->primaries, GIMG_PRIMARIES_UNKNOWN);
  expect_pattern(img);
}

TEST(BmpDecode, V5ProfilePastTheEndOfTheFileLeavesTheImageAlone) {
  // A picture is not wrong because its colour annotation is, so a profile
  // that runs off the end yields no profile rather than no image.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_v5_icc_past_eof.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(color->icc_size, 0u);
  expect_pattern(img);
}

TEST(BmpDecode, APlainInfoHeaderSaysNothingAboutColor) {
  // An untagged BMP is overwhelmingly an sRGB one, but the file does not say
  // so and neither does this: a decoder that assumed it would be asserting
  // something no byte of the file supports.
  Loaded img;
  ASSERT_EQ(img.load("bmp_4x4_24bit.bmp"), GIMG_OK);
  ASSERT_EQ(img.decode(), GIMG_OK);

  const GIMG_Color_Info * color = gimg_raster_color_info_const(img.raster());
  ASSERT_NE(color, nullptr);
  EXPECT_EQ(color->primaries, GIMG_PRIMARIES_UNKNOWN);
  EXPECT_EQ(color->transfer, GIMG_TRANSFER_UNKNOWN);
  EXPECT_EQ(color->icc_size, 0u);
}

// ---------------------------------------------------------------------------
// What a BMP's colour is good for
// ---------------------------------------------------------------------------

TEST(BmpToPng, AnEmbeddedProfileSurvivesTheConversion) {
  // Reading a V5 profile is only worth doing if it can then go somewhere.  It
  // could not: the PNG writer emitted iCCP from the ancillary chunks a PNG
  // arrived with and never from the raster's colour info, so a document that
  // did not arrive as a PNG lost its colour entirely and the profile was read
  // only to be dropped.
  Loaded bmp;
  ASSERT_EQ(bmp.load("bmp_4x4_v5_icc.bmp"), GIMG_OK);
  ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(bmp.doc(), 0), nullptr),
      GIMG_OK);

  const GIMG_Color_Info * from_bmp =
      gimg_raster_color_info_const(gimg_item_raster(gimg_doc_item(bmp.doc(), 0)));
  ASSERT_NE(from_bmp, nullptr);
  ASSERT_GT(from_bmp->icc_size, 0u);
  std::vector<uint8_t> profile(
      static_cast<const uint8_t *>(from_bmp->icc_bytes),
      static_cast<const uint8_t *>(from_bmp->icc_bytes) + from_bmp->icc_size);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options options = {};
  options.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(bmp.doc(), out, "png", &options, &report), GIMG_OK);

  const void * buffer = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &buffer, &size);
  std::vector<uint8_t> png(static_cast<const uint8_t *>(buffer),
      static_cast<const uint8_t *>(buffer) + size);

  Loaded back;
  ASSERT_EQ(back.load_bytes(png), GIMG_OK);
  ASSERT_EQ(back.decode(), GIMG_OK);
  const GIMG_Color_Info * from_png =
      gimg_raster_color_info_const(back.raster());
  ASSERT_NE(from_png, nullptr);
  ASSERT_EQ(from_png->icc_size, profile.size())
      << "the profile the BMP carried did not reach the PNG";
  EXPECT_EQ(memcmp(from_png->icc_bytes, profile.data(), profile.size()), 0)
      << "the profile reached the PNG but not intact";

  gimg_stream_destroy(out);
}

TEST(BmpToPng, ACalibratedGammaSurvivesAsGama) {
  // bmp_4x4_v4_calibrated.bmp names sRGB's primaries with a gamma of 2.2,
  // which is not sRGB.  It has to come out as gAMA rather than as an sRGB
  // chunk, which would assert a transfer curve the file never stated.
  Loaded bmp;
  ASSERT_EQ(bmp.load("bmp_4x4_v4_calibrated.bmp"), GIMG_OK);
  ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(bmp.doc(), 0), nullptr),
      GIMG_OK);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options options = {};
  options.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(bmp.doc(), out, "png", &options, &report), GIMG_OK);

  const void * buffer = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &buffer, &size);
  std::vector<uint8_t> png(static_cast<const uint8_t *>(buffer),
      static_cast<const uint8_t *>(buffer) + size);

  Loaded back;
  ASSERT_EQ(back.load_bytes(png), GIMG_OK);
  ASSERT_EQ(back.decode(), GIMG_OK);
  const GIMG_Color_Info * from_png =
      gimg_raster_color_info_const(back.raster());
  ASSERT_NE(from_png, nullptr);
  EXPECT_EQ(from_png->transfer, GIMG_TRANSFER_GAMMA);
  EXPECT_NEAR(from_png->gamma_value, 2.2, 0.001);

  gimg_stream_destroy(out);
}

TEST(BmpToPng, AGammaPngCannotStateIsNotWritten) {
  // bmp_4x4_v4_huge_gamma.bmp carries a V4 gamma at the top of what a 16.16
  // field holds.  PNG's gAMA cannot state it, and the conversion used to be
  // undefined behaviour rather than a large number.
  Loaded bmp;
  ASSERT_EQ(bmp.load("bmp_4x4_v4_huge_gamma.bmp"), GIMG_OK);
  ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(bmp.doc(), 0), nullptr),
      GIMG_OK);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options options = {};
  options.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(bmp.doc(), out, "png", &options, &report), GIMG_OK);

  const void * buffer = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &buffer, &size);
  std::vector<uint8_t> png(static_cast<const uint8_t *>(buffer),
      static_cast<const uint8_t *>(buffer) + size);

  Loaded back;
  ASSERT_EQ(back.load_bytes(png), GIMG_OK);
  ASSERT_EQ(back.decode(), GIMG_OK);
  const GIMG_Color_Info * from_png =
      gimg_raster_color_info_const(back.raster());
  ASSERT_NE(from_png, nullptr);
  EXPECT_EQ(from_png->transfer, GIMG_TRANSFER_UNKNOWN)
      << "a gamma gAMA cannot hold should go unsaid";

  gimg_stream_destroy(out);
}

namespace {

/**
 * A BMP header describing an image of the given size, with no pixel data
 * behind it.
 *
 * This is the shape of a decompression bomb: a hundred bytes that name an
 * image of any size the fields allow. What stops one is GIMG_Limits, and
 * nothing here had ever asserted that it does.
 */
std::vector<uint8_t> header_naming(int32_t width, int32_t height,
    uint16_t bit_count, size_t trailing_bytes = 64) {
  std::vector<uint8_t> out(14u + 40u + trailing_bytes, 0);
  auto put32 = [&out](size_t at, uint32_t v) {
    out[at] = (uint8_t)(v & 0xFFu);
    out[at + 1] = (uint8_t)((v >> 8) & 0xFFu);
    out[at + 2] = (uint8_t)((v >> 16) & 0xFFu);
    out[at + 3] = (uint8_t)((v >> 24) & 0xFFu);
  };
  auto put16 = [&out](size_t at, uint16_t v) {
    out[at] = (uint8_t)(v & 0xFFu);
    out[at + 1] = (uint8_t)((v >> 8) & 0xFFu);
  };
  out[0] = 'B';
  out[1] = 'M';
  put32(2, (uint32_t)out.size());
  put32(10, 14u + 40u);
  put32(14, 40u);          // biSize
  put32(18, (uint32_t)width);
  put32(22, (uint32_t)height);
  put16(26, 1u);           // biPlanes
  put16(28, bit_count);
  put32(30, 0u);           // BI_RGB
  return out;
}

} // namespace

TEST(BmpDecode, APixelCountOverTheLimitIsRefusedBeforeItIsAllocated) {
  // 46340 squared is just under 2^31 pixels, so the header is legal and the
  // raster would be about eight gigabytes.  The file naming it is 118 bytes.
  std::vector<uint8_t> bomb = header_naming(46340, 46340, 32u);

  GIMG_Limits limits;
  gimg_limits_default(&limits);
  limits.max_decoded_pixels = 1024u * 1024u;
  GIMG_Load_Options options = {};
  options.limits = &limits;

  Loaded img;
  EXPECT_EQ(img.load_bytes(bomb, &options), GIMG_ERR_LIMIT);
}

TEST(BmpDecode, NoLimitsMeansTheHeaderIsStillCheckedAgainstTheFile) {
  // With no limits at all, a header naming more than the file holds must
  // still be refused - on the bytes actually present, not on a cap.  This is
  // what stops the unlimited default from being a way to read past the end.
  std::vector<uint8_t> bomb = header_naming(46340, 46340, 32u);
  Loaded img;
  GIMG_Result r = img.load_bytes(bomb, nullptr);
  EXPECT_NE(r, GIMG_OK)
      << "a 118-byte file cannot hold eight gigabytes of pixels";
}
