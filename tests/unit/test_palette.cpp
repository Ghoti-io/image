/**
 * @file
 *
 * Unit tests for colour counting, palette building and colour reduction.
 *
 * The quality of a quantizer is not something a unit test can assert - "the
 * picture still looks right" has no threshold that is not arbitrary.  What is
 * checked here are the properties a caller can rely on: that an image within
 * budget is returned untouched, that the result never holds a colour outside
 * the palette, that the count asked for is never exceeded, and that the
 * transparent case behaves.  How good the choices are was measured against two
 * quantizers that are not ours; see documentation/modules/palette.md.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <cstring>
#include <gtest/gtest.h>
#include <set>
#include <vector>

namespace {

struct Rgba {
  uint8_t r, g, b, a;
};

GIMG_Raster * make(uint32_t w, uint32_t h, Rgba (*fn)(uint32_t, uint32_t),
    const GIMG_Pixel_Format * fmt = &GIMG_PIXEL_RGBA8) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(w, h, fmt, GIMG_RASTER_OWNED, nullptr, 0, &raster) !=
      GIMG_OK) {
    return nullptr;
  }
  auto * base = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  const size_t bpp = fmt->channel_count;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      const Rgba p = fn(x, y);
      uint8_t * px = base + y * stride + x * bpp;
      if (bpp == 4u) {
        px[0] = p.r;
        px[1] = p.g;
        px[2] = p.b;
        px[3] = p.a;
      }
      else {
        px[0] = p.r;
      }
    }
  }
  return raster;
}

/** Every distinct colour in a raster, as a packed key. */
std::set<uint32_t> colors_of(const GIMG_Raster * raster) {
  std::set<uint32_t> out;
  const auto * base =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  const size_t bpp = gimg_raster_format(raster)->channel_count;
  for (uint32_t y = 0; y < gimg_raster_height(raster); y++) {
    for (uint32_t x = 0; x < gimg_raster_width(raster); x++) {
      const uint8_t * px = base + y * stride + x * bpp;
      if (bpp == 1u) {
        out.insert(uint32_t(px[0]));
        continue;
      }
      // A fully transparent pixel is one colour whatever lies under it.
      if (px[3] == 0u) {
        out.insert(0u);
        continue;
      }
      out.insert((uint32_t(px[0]) << 24) | (uint32_t(px[1]) << 16) |
          (uint32_t(px[2]) << 8) | uint32_t(px[3]));
    }
  }
  return out;
}

std::set<uint32_t> entries_of(const GIMG_Palette & p) {
  std::set<uint32_t> out;
  for (uint16_t i = 0; i < p.count; i++) {
    if (p.entries[i][3] == 0u) {
      out.insert(0u);
      continue;
    }
    out.insert((uint32_t(p.entries[i][0]) << 24) |
        (uint32_t(p.entries[i][1]) << 16) | (uint32_t(p.entries[i][2]) << 8) |
        uint32_t(p.entries[i][3]));
  }
  return out;
}

/** Four colours in a checkerboard: well under any budget. */
Rgba four(uint32_t x, uint32_t y) {
  static const Rgba c[4] = {{10, 20, 30, 255}, {200, 0, 0, 255},
      {0, 200, 0, 255}, {0, 0, 200, 255}};
  return c[((x / 3u) + (y / 2u)) % 4u];
}

/** A smooth ramp: many thousands of colours, nothing repeated much. */
Rgba ramp(uint32_t x, uint32_t y) {
  return Rgba{static_cast<uint8_t>(x), static_cast<uint8_t>(y),
      static_cast<uint8_t>((x * 3u + y * 5u) & 0xFFu), 255};
}

/** A ramp with a fully transparent stripe, its RGB deliberately varied. */
Rgba ramp_with_hole(uint32_t x, uint32_t y) {
  if (y < 4u) {
    return Rgba{static_cast<uint8_t>(x * 7u), static_cast<uint8_t>(y * 13u),
        99u, 0};
  }
  return ramp(x, y);
}

Rgba gray_ramp(uint32_t x, uint32_t y) {
  const uint8_t v = static_cast<uint8_t>((x * 4u + y) & 0xFFu);
  return Rgba{v, v, v, 255};
}

} // namespace

TEST(Palette, CountsDistinctColours) {
  GIMG_Raster * r = make(12, 8, four);
  ASSERT_NE(r, nullptr);
  size_t n = 0;
  bool exact = false;
  EXPECT_EQ(gimg_ops_count_colors(r, 0, &n, &exact), GIMG_OK);
  EXPECT_EQ(n, 4u);
  EXPECT_TRUE(exact);
  gimg_raster_destroy(r);
}

TEST(Palette, CountingGivesUpAtTheLimitAndSaysSo) {
  // The question a caller actually has is "is this too many", and a photograph
  // has hundreds of thousands.  Past the limit the exact number costs more to
  // find than it is worth, so counting stops and reports that it stopped.
  GIMG_Raster * r = make(64, 64, ramp);
  ASSERT_NE(r, nullptr);
  size_t n = 0;
  bool exact = true;
  EXPECT_EQ(gimg_ops_count_colors(r, 16, &n, &exact), GIMG_OK);
  EXPECT_EQ(n, 16u);
  EXPECT_FALSE(exact);
  gimg_raster_destroy(r);
}

TEST(Palette, EveryTransparentPixelIsOneColour) {
  // What lies under alpha 0 is never shown and never stored, so an image with
  // a transparent border must not spend its palette on colours nobody can see.
  GIMG_Raster * r = make(16, 16, ramp_with_hole);
  ASSERT_NE(r, nullptr);
  size_t with_hole = 0;
  ASSERT_EQ(gimg_ops_count_colors(r, 0, &with_hole, nullptr), GIMG_OK);

  GIMG_Raster * opaque = make(16, 12, ramp);
  ASSERT_NE(opaque, nullptr);
  size_t plain = 0;
  ASSERT_EQ(gimg_ops_count_colors(opaque, 0, &plain, nullptr), GIMG_OK);

  // The stripe is 16x4 pixels of varying RGB under alpha 0; all of it is one.
  EXPECT_EQ(with_hole, plain + 1u);
  gimg_raster_destroy(r);
  gimg_raster_destroy(opaque);
}

TEST(Palette, ExactPaletteOfASmallImage) {
  GIMG_Raster * r = make(12, 8, four);
  ASSERT_NE(r, nullptr);
  GIMG_Palette p;
  ASSERT_EQ(gimg_ops_palette_from_raster(r, 256, &p), GIMG_OK);
  EXPECT_EQ(p.count, 4u);
  EXPECT_EQ(entries_of(p), colors_of(r));
  gimg_raster_destroy(r);
}

TEST(Palette, ExactPaletteIsRefusedWhenThereAreTooMany) {
  // The same answer the palette writers give, and the signal to quantize.
  GIMG_Raster * r = make(64, 64, ramp);
  ASSERT_NE(r, nullptr);
  GIMG_Palette p;
  EXPECT_EQ(gimg_ops_palette_from_raster(r, 256, &p), GIMG_ERR_UNSUPPORTED);
  gimg_raster_destroy(r);
}

TEST(Palette, AnImageWithinBudgetComesBackExactly) {
  // The property that makes quantizing safe to call unconditionally: below the
  // limit it is a copy, and nothing is averaged.
  GIMG_Raster * r = make(12, 8, four);
  ASSERT_NE(r, nullptr);
  GIMG_Quantize_Options q;
  memset(&q, 0, sizeof(q));
  GIMG_Raster * out = nullptr;
  GIMG_Palette p;
  ASSERT_EQ(gimg_ops_quantize(r, &q, &out, &p), GIMG_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(p.count, 4u);
  EXPECT_TRUE(gimg_ops_raster_equal(r, out));
  gimg_raster_destroy(out);
  gimg_raster_destroy(r);
}

TEST(Palette, ReducesToTheCountAsked) {
  GIMG_Raster * r = make(64, 64, ramp);
  ASSERT_NE(r, nullptr);
  for (uint16_t want : {2u, 7u, 16u, 64u, 256u}) {
    GIMG_Quantize_Options q;
    memset(&q, 0, sizeof(q));
    q.max_colors = want;
    GIMG_Raster * out = nullptr;
    GIMG_Palette p;
    ASSERT_EQ(gimg_ops_quantize(r, &q, &out, &p), GIMG_OK) << want;
    ASSERT_NE(out, nullptr);
    EXPECT_LE(p.count, want) << want;
    // Nothing in the result that is not in the palette, and no more distinct
    // values than were asked for.  The second follows from the first, and both
    // are what every palette writer downstream is about to rely on.
    const std::set<uint32_t> got = colors_of(out);
    const std::set<uint32_t> table = entries_of(p);
    EXPECT_LE(got.size(), size_t(want)) << want;
    for (uint32_t c : got) {
      EXPECT_TRUE(table.count(c) != 0u) << "colour outside the palette, " << want;
    }
    EXPECT_EQ(gimg_raster_width(out), 64u);
    EXPECT_EQ(gimg_raster_height(out), 64u);
    gimg_raster_destroy(out);
  }
  gimg_raster_destroy(r);
}

TEST(Palette, DitheringStaysInsideThePalette) {
  // Error diffusion adds the previous pixel's error before choosing, so the
  // value looked up is not one of the source's colours.  What is written still
  // has to be an entry.
  GIMG_Raster * r = make(64, 64, ramp);
  ASSERT_NE(r, nullptr);
  GIMG_Quantize_Options q;
  memset(&q, 0, sizeof(q));
  q.max_colors = 8u;
  q.dither = GIMG_DITHER_FLOYD_STEINBERG;
  GIMG_Raster * out = nullptr;
  GIMG_Palette p;
  ASSERT_EQ(gimg_ops_quantize(r, &q, &out, &p), GIMG_OK);
  ASSERT_NE(out, nullptr);
  const std::set<uint32_t> table = entries_of(p);
  for (uint32_t c : colors_of(out)) {
    EXPECT_TRUE(table.count(c) != 0u);
  }
  gimg_raster_destroy(out);
  gimg_raster_destroy(r);
}

TEST(Palette, DitheringChangesTheResultAndKeepsTransparencyClean) {
  // A dithered pixel is deliberately the wrong colour so its neighbourhood
  // averages right, so the two results must differ.  A transparent pixel has
  // no colour to be wrong about: it must come out transparent, not carrying
  // its neighbour's error.
  GIMG_Raster * r = make(32, 32, ramp_with_hole);
  ASSERT_NE(r, nullptr);
  GIMG_Quantize_Options q;
  memset(&q, 0, sizeof(q));
  q.max_colors = 8u;
  GIMG_Raster * plain = nullptr;
  ASSERT_EQ(gimg_ops_quantize(r, &q, &plain, nullptr), GIMG_OK);
  q.dither = GIMG_DITHER_FLOYD_STEINBERG;
  GIMG_Raster * dithered = nullptr;
  ASSERT_EQ(gimg_ops_quantize(r, &q, &dithered, nullptr), GIMG_OK);
  EXPECT_FALSE(gimg_ops_raster_equal(plain, dithered));

  const auto * base =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(dithered));
  const size_t stride = gimg_raster_stride_bytes(dithered);
  for (uint32_t y = 0; y < 4u; y++) {
    for (uint32_t x = 0; x < 32u; x++) {
      EXPECT_EQ(base[y * stride + x * 4u + 3u], 0u)
          << "transparent pixel took a colour at " << x << "," << y;
    }
  }
  gimg_raster_destroy(dithered);
  gimg_raster_destroy(plain);
  gimg_raster_destroy(r);
}

TEST(Palette, OnePaletteForSeveralFrames) {
  // Frames quantized separately get slightly different colours for the parts
  // that did not change, and the animation shimmers.  One table for all of
  // them is the reason building and applying are separate calls.
  GIMG_Raster * a = make(32, 32, ramp);
  GIMG_Raster * b = make(32, 32, gray_ramp);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  const GIMG_Raster * frames[2] = {a, b};
  GIMG_Quantize_Options q;
  memset(&q, 0, sizeof(q));
  q.max_colors = 32u;
  GIMG_Palette shared;
  ASSERT_EQ(gimg_ops_palette_build(frames, 2u, &q, &shared), GIMG_OK);
  EXPECT_LE(shared.count, 32u);

  const std::set<uint32_t> table = entries_of(shared);
  for (const GIMG_Raster * f : frames) {
    GIMG_Raster * out = nullptr;
    ASSERT_EQ(
        gimg_ops_palette_apply(f, &shared, GIMG_DITHER_NONE, &out), GIMG_OK);
    for (uint32_t c : colors_of(out)) {
      EXPECT_TRUE(table.count(c) != 0u);
    }
    gimg_raster_destroy(out);
  }
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

TEST(Palette, ACallerSuppliedPaletteIsUsedAsGiven) {
  // Applying a table nobody built from the image: a brand palette, a previous
  // frame's table, the palette a file arrived with.
  GIMG_Raster * r = make(16, 16, ramp);
  ASSERT_NE(r, nullptr);
  GIMG_Palette p;
  memset(&p, 0, sizeof(p));
  p.count = 2u;
  p.entries[0][0] = 0u;
  p.entries[0][3] = 255u;
  p.entries[1][0] = p.entries[1][1] = p.entries[1][2] = 255u;
  p.entries[1][3] = 255u;
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(gimg_ops_palette_apply(r, &p, GIMG_DITHER_NONE, &out), GIMG_OK);
  const std::set<uint32_t> got = colors_of(out);
  EXPECT_LE(got.size(), 2u);
  for (uint32_t c : got) {
    EXPECT_TRUE(entries_of(p).count(c) != 0u);
  }
  gimg_raster_destroy(out);
  gimg_raster_destroy(r);
}

TEST(Palette, GrayscaleIsReducedInItsOwnTerms) {
  GIMG_Raster * r = make(32, 32, gray_ramp, &GIMG_PIXEL_GRAY8);
  ASSERT_NE(r, nullptr);
  GIMG_Quantize_Options q;
  memset(&q, 0, sizeof(q));
  q.max_colors = 8u;
  GIMG_Raster * out = nullptr;
  GIMG_Palette p;
  ASSERT_EQ(gimg_ops_quantize(r, &q, &out, &p), GIMG_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_LE(p.count, 8u);
  EXPECT_LE(colors_of(out).size(), 8u);
  // A palette built from grayscale pixels holds nothing but opaque grays.
  for (uint16_t i = 0; i < p.count; i++) {
    EXPECT_EQ(p.entries[i][0], p.entries[i][1]);
    EXPECT_EQ(p.entries[i][1], p.entries[i][2]);
    EXPECT_EQ(p.entries[i][3], 255u);
  }
  gimg_raster_destroy(out);
  gimg_raster_destroy(r);
}

TEST(Palette, AColourPaletteOnAGrayscaleRasterIsRefused) {
  // One sample cannot hold three.  Keeping a third of each colour and calling
  // it the result would be wrong quietly; this is wrong loudly.
  GIMG_Raster * r = make(8, 8, gray_ramp, &GIMG_PIXEL_GRAY8);
  ASSERT_NE(r, nullptr);
  GIMG_Palette p;
  memset(&p, 0, sizeof(p));
  p.count = 1u;
  p.entries[0][0] = 200u;
  p.entries[0][1] = 30u;
  p.entries[0][2] = 30u;
  p.entries[0][3] = 255u;
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_ops_palette_apply(r, &p, GIMG_DITHER_NONE, &out),
      GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(r);
}

TEST(Palette, AnEmptyPaletteIsRefused) {
  GIMG_Raster * r = make(4, 4, four);
  ASSERT_NE(r, nullptr);
  GIMG_Palette p;
  memset(&p, 0, sizeof(p));
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_ops_palette_apply(r, &p, GIMG_DITHER_NONE, &out),
      GIMG_ERR_INTERNAL);
  gimg_raster_destroy(r);
}

TEST(Palette, SixteenBitRastersAreRefusedRatherThanTruncated) {
  // Every palette format here stores eight-bit entries.  Reducing at a greater
  // precision and then throwing the precision away is not a reduction anyone
  // asked for, so the caller converts first and can see that they did.
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  size_t n = 0;
  EXPECT_EQ(gimg_ops_count_colors(r, 0, &n, nullptr), GIMG_ERR_UNSUPPORTED);
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(gimg_ops_quantize(r, nullptr, &out, nullptr), GIMG_ERR_UNSUPPORTED);
  gimg_raster_destroy(r);
}

TEST(Palette, ASingleColourImageReducesToOneEntry) {
  GIMG_Raster * r = make(8, 8, [](uint32_t, uint32_t) {
    return Rgba{7, 8, 9, 255};
  });
  ASSERT_NE(r, nullptr);
  GIMG_Quantize_Options q;
  memset(&q, 0, sizeof(q));
  q.max_colors = 64u;
  GIMG_Raster * out = nullptr;
  GIMG_Palette p;
  ASSERT_EQ(gimg_ops_quantize(r, &q, &out, &p), GIMG_OK);
  EXPECT_EQ(p.count, 1u);
  EXPECT_TRUE(gimg_ops_raster_equal(r, out));
  gimg_raster_destroy(out);
  gimg_raster_destroy(r);
}

TEST(Palette, TwoColoursIsTheSmallestAnimationWorthAsking) {
  // max_colors 1 is legal and means one colour for the whole image.
  GIMG_Raster * r = make(32, 32, ramp);
  ASSERT_NE(r, nullptr);
  GIMG_Quantize_Options q;
  memset(&q, 0, sizeof(q));
  q.max_colors = 1u;
  GIMG_Raster * out = nullptr;
  GIMG_Palette p;
  ASSERT_EQ(gimg_ops_quantize(r, &q, &out, &p), GIMG_OK);
  EXPECT_EQ(p.count, 1u);
  EXPECT_EQ(colors_of(out).size(), 1u);
  gimg_raster_destroy(out);
  gimg_raster_destroy(r);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
