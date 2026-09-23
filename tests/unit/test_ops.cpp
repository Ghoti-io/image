/**
 * @file
 *
 * Unit tests for orientation, pixel format conversion, alpha.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/bitdepth.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

TEST(Ops, ApplyOrientationIdentity) {
  GIMG_Raster * r = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_NE(r, nullptr);
  GIMG_Result res = gimg_ops_apply_orientation(r, GIMG_ORIENTATION_NORMAL);
  EXPECT_EQ(res, GIMG_OK);
  gimg_raster_destroy(r);
}

TEST(Ops, ApplyOrientation180) {
  GIMG_Raster * r = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_NE(r, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  p[0] = 1;
  p[1] = 2;
  p[2] = 3;
  p[3] = 255;
  GIMG_Result res = gimg_ops_apply_orientation(r, GIMG_ORIENTATION_ROTATE_180);
  ASSERT_EQ(res, GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(r);
  unsigned char * last_row = p + stride * (gimg_raster_height(r) - 1);
  size_t bpp = 4;
  unsigned char * last_px = last_row + (gimg_raster_width(r) - 1) * bpp;
  EXPECT_EQ(last_px[0], 1);
  EXPECT_EQ(last_px[1], 2);
  EXPECT_EQ(last_px[2], 3);
  gimg_raster_destroy(r);
}

TEST(Ops, ConvertSameFormatCopy) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      3, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, &dst);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_width(dst), 3u);
  EXPECT_EQ(gimg_raster_height(dst), 2u);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, ConvertUnsupportedReturnsError) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_GRAY8, &dst);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(dst, nullptr);
  gimg_raster_destroy(src);
}

TEST(Ops, BitdepthSampleConversions) {
  EXPECT_EQ(gimg_bitdepth_8_to_12(0), 0);
  EXPECT_EQ(gimg_bitdepth_8_to_12(255), 4095u);
  EXPECT_EQ(gimg_bitdepth_8_to_16(0), 0);
  EXPECT_EQ(gimg_bitdepth_8_to_16(255), 65535u);
  EXPECT_EQ(gimg_bitdepth_12_to_8(0), 0);
  EXPECT_EQ(gimg_bitdepth_12_to_8(4095), 255);
  EXPECT_EQ(gimg_bitdepth_12_to_16(0), 0);
  EXPECT_EQ(gimg_bitdepth_12_to_16(4095), 65535u);
  EXPECT_EQ(gimg_bitdepth_16_to_8(0), 0);
  EXPECT_EQ(gimg_bitdepth_16_to_8(65535), 255);
  EXPECT_EQ(gimg_bitdepth_16_to_12(65535), 4095u);
}

/** Every widening conversion replicates high bits, so each maps its maximum
 * onto the destination maximum and is monotonic.  A conversion that merely
 * left-justifies breaks both: it cannot reach the top of the range, and
 * 8 -> 12 -> 16 then disagrees with 8 -> 16. */
TEST(Ops, BitdepthWideningIsConsistent) {
  EXPECT_EQ(gimg_bitdepth_8_to_16(255), gimg_bitdepth_12_to_16(4095))
      << "widening 8 and 12 bit maxima must both reach 65535";
  EXPECT_EQ(gimg_bitdepth_12_to_16(gimg_bitdepth_8_to_12(255)),
      gimg_bitdepth_8_to_16(255))
      << "8 -> 12 -> 16 must agree with 8 -> 16";
  EXPECT_EQ(gimg_bitdepth_12_to_16(gimg_bitdepth_8_to_12(0)),
      gimg_bitdepth_8_to_16(0));
  unsigned prev = 0;
  for (unsigned v = 0; v <= 4095u; v++) {
    unsigned got = gimg_bitdepth_12_to_16((uint16_t)v);
    ASSERT_GE(got, prev) << "12 -> 16 must be monotonic at " << v;
    prev = got;
  }
  // Widening then narrowing returns the original sample.
  for (unsigned v = 0; v <= 4095u; v++) {
    ASSERT_EQ(gimg_bitdepth_16_to_12(gimg_bitdepth_12_to_16((uint16_t)v)), v)
        << "12 -> 16 -> 12 must round-trip at " << v;
  }
  for (unsigned v = 0; v <= 255u; v++) {
    ASSERT_EQ(gimg_bitdepth_16_to_8(gimg_bitdepth_8_to_16((uint8_t)v)), v)
        << "8 -> 16 -> 8 must round-trip at " << v;
    ASSERT_EQ(gimg_bitdepth_12_to_8(gimg_bitdepth_8_to_12((uint8_t)v)), v)
        << "8 -> 12 -> 8 must round-trip at " << v;
  }
  // Narrowing maps the source maximum onto the destination maximum.
  EXPECT_EQ(gimg_bitdepth_16_to_8(65535), 255);
  EXPECT_EQ(gimg_bitdepth_16_to_12(65535), 4095u);
  EXPECT_EQ(gimg_bitdepth_12_to_8(4095), 255);
  EXPECT_EQ(gimg_bitdepth_16_to_8(0), 0);
  EXPECT_EQ(gimg_bitdepth_16_to_12(0), 0);
  // Mid-gray stays mid-gray across every widening.
  EXPECT_NEAR(gimg_bitdepth_8_to_16(128) / 257.0, 128.0, 0.5);
  EXPECT_NEAR(gimg_bitdepth_12_to_16(2048) / 16.0037, 2048.0, 1.0);
}

TEST(Ops, ConvertBitDepthGray8To16) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  size_t src_stride = gimg_raster_stride_bytes(src);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(src);
  p[0] = 0;
  p[1] = 255;
  p[0 + src_stride] = 128;
  p[1 + src_stride] = 64;
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_bit_depth(src, 16, &dst);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_width(dst), 2u);
  EXPECT_EQ(gimg_raster_height(dst), 2u);
  EXPECT_EQ(gimg_raster_format(dst)->bits_per_channel[0], 16);
  size_t stride = gimg_raster_stride_bytes(dst);
  const unsigned char * base =
      (const unsigned char *)gimg_raster_pixels_const(dst);
  const uint16_t * row0 = (const uint16_t *)base;
  const uint16_t * row1 = (const uint16_t *)(base + stride);
  EXPECT_EQ(row0[0], 0);
  EXPECT_EQ(row0[1], 65535u);
  EXPECT_EQ(row1[0], (128u << 8) | 128u);  // 8→16 replicate (v<<8)|v
  EXPECT_EQ(row1[1], (64u << 8) | 64u);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, ConvertBitDepthGray16To12) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      1, 2, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  size_t src_stride = gimg_raster_stride_bytes(src);
  unsigned char * base = (unsigned char *)gimg_raster_pixels(src);
  *(uint16_t *)base = 0;
  *(uint16_t *)(base + src_stride) = 65535;
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_bit_depth(src, 12, &dst);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(dst, nullptr);
  EXPECT_EQ(gimg_raster_format(dst)->bits_per_channel[0], 12);
  size_t dst_stride = gimg_raster_stride_bytes(dst);
  const unsigned char * dbase =
      (const unsigned char *)gimg_raster_pixels_const(dst);
  EXPECT_EQ(*(const uint16_t *)dbase, 0);
  EXPECT_EQ(*(const uint16_t *)(dbase + dst_stride), 4095u);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, ConvertBitDepthInvalidDstBitsReturnsError) {
  GIMG_Raster * src = nullptr;
  gimg_raster_create(
      1, 1, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0, &src);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  GIMG_Result r = gimg_ops_convert_bit_depth(src, 7, &dst);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(dst, nullptr);
  gimg_raster_destroy(src);
}

TEST(Ops, AlphaPremultiplyUnpremultiplyRoundTrip) {
  GIMG_Raster * r = nullptr;
  gimg_raster_create(
      1, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_NE(r, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  p[0] = 100;
  p[1] = 150;
  p[2] = 200;
  p[3] = 128;
  GIMG_Result r1 = gimg_alpha_premultiply(r);
  ASSERT_EQ(r1, GIMG_OK);
  GIMG_Result r2 = gimg_alpha_unpremultiply(r);
  ASSERT_EQ(r2, GIMG_OK);
  EXPECT_NEAR((int)p[0], 100, 2);
  EXPECT_NEAR((int)p[1], 150, 2);
  EXPECT_NEAR((int)p[2], 200, 2);
  EXPECT_EQ(p[3], 128);
  gimg_raster_destroy(r);
}

TEST(Ops, RasterEqualIdentical) {
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &b);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  unsigned char * pa = (unsigned char *)gimg_raster_pixels(a);
  unsigned char * pb = (unsigned char *)gimg_raster_pixels(b);
  for (int i = 0; i < 16; i++) {
    pa[i] = pb[i] = (unsigned char)i;
  }
  EXPECT_TRUE(gimg_ops_raster_equal(a, b));
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

TEST(Ops, RasterEqualDifferentDimensions) {
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  gimg_raster_create(
      2, 3, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &b);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_FALSE(gimg_ops_raster_equal(a, b));
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

TEST(Ops, RasterEqualOnePixelDifferent) {
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &b);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  unsigned char * pa = (unsigned char *)gimg_raster_pixels(a);
  unsigned char * pb = (unsigned char *)gimg_raster_pixels(b);
  for (int i = 0; i < 16; i++) {
    pa[i] = pb[i] = (unsigned char)i;
  }
  pb[5] = 99;
  EXPECT_FALSE(gimg_ops_raster_equal(a, b));
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
}

TEST(Ops, RasterEqualNull) {
  GIMG_Raster * a = nullptr;
  gimg_raster_create(
      1, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &a);
  ASSERT_NE(a, nullptr);
  EXPECT_FALSE(gimg_ops_raster_equal(nullptr, a));
  EXPECT_FALSE(gimg_ops_raster_equal(a, nullptr));
  gimg_raster_destroy(a);
}

// Alpha zero, at both ends of the premultiply pair.
//
// Premultiplying a transparent pixel zeroes its colour, because the colour is
// multiplied by nothing; unpremultiplying one leaves it alone, because there
// is nothing to divide by and 0/0 is not a colour.  The two are not each
// other's inverse there, which is the whole reason both arms exist - and
// neither had ever run, because the only test of the pair uses one pixel at
// alpha 128.
TEST(Ops, ATransparentPixelIsZeroedGoingInAndLeftAloneComingBack) {
  GIMG_Raster * r = nullptr;
  gimg_raster_create(
      2, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r);
  ASSERT_NE(r, nullptr);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  // Transparent, with colour under it that nobody can see.
  p[0] = 200; p[1] = 100; p[2] = 50; p[3] = 0;
  // Opaque, as a control: whatever happens to it is not about alpha zero.
  p[4] = 200; p[5] = 100; p[6] = 50; p[7] = 255;

  ASSERT_EQ(gimg_alpha_premultiply(r), GIMG_OK);
  EXPECT_EQ(p[0], 0) << "colour under alpha zero is multiplied away";
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  EXPECT_EQ(p[3], 0) << "and the alpha itself is untouched";
  EXPECT_EQ(p[4], 200) << "control: an opaque pixel keeps its colour";
  EXPECT_EQ(p[5], 100);
  EXPECT_EQ(p[6], 50);

  ASSERT_EQ(gimg_alpha_unpremultiply(r), GIMG_OK);
  EXPECT_EQ(p[0], 0) << "there is nothing to divide by, so nothing changes - "
                        "the pair is not an inverse at alpha zero, and the "
                        "colour that was there is gone for good";
  EXPECT_EQ(p[1], 0);
  EXPECT_EQ(p[2], 0);
  EXPECT_EQ(p[3], 0);
  EXPECT_EQ(p[4], 200);
  gimg_raster_destroy(r);
}

// Equality compares the format, not only the pixels.
//
// gimg_ops_raster_equal() checks the channel model, type and count, then the
// bits per channel, and only then the bytes.  Only the dimensions and the
// bytes had ever been asked: every test of it compares two RGBA8 rasters, so
// two rasters of different formats had never been handed to it at all.
//
// The pairs below differ in one thing each, and each is a pair a caller could
// plausibly hold - a grey raster beside a colour one, and eight bits beside
// sixteen.
TEST(Ops, RastersOfDifferentFormatsAreNotEqual) {
  struct Case {
    const GIMG_Pixel_Format * a;
    const GIMG_Pixel_Format * b;
    const char * what;
  };
  const Case cases[] = {
      {&GIMG_PIXEL_GRAY8, &GIMG_PIXEL_RGBA8, "channel count and model"},
      {&GIMG_PIXEL_GRAY8, &GIMG_PIXEL_GRAY16, "bits per channel"},
      {&GIMG_PIXEL_RGBA8, &GIMG_PIXEL_RGBA16, "bits per channel, four of them"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    GIMG_Raster * a = nullptr;
    GIMG_Raster * b = nullptr;
    ASSERT_EQ(gimg_raster_create(2, 2, c.a, GIMG_RASTER_OWNED, nullptr, 0, &a),
        GIMG_OK);
    ASSERT_EQ(gimg_raster_create(2, 2, c.b, GIMG_RASTER_OWNED, nullptr, 0, &b),
        GIMG_OK);
    // Zeroed both ways, so nothing but the format can be the difference.
    memset(gimg_raster_pixels(a), 0,
        gimg_raster_stride_bytes(a) * gimg_raster_height(a));
    memset(gimg_raster_pixels(b), 0,
        gimg_raster_stride_bytes(b) * gimg_raster_height(b));
    EXPECT_FALSE(gimg_ops_raster_equal(a, b));
    EXPECT_FALSE(gimg_ops_raster_equal(b, a)) << "and the other way round";
    EXPECT_TRUE(gimg_ops_raster_equal(a, a))
        << "control: each is equal to itself, so the comparison is not "
           "simply answering false";
    EXPECT_TRUE(gimg_ops_raster_equal(b, b));
    gimg_raster_destroy(a);
    gimg_raster_destroy(b);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---------------------------------------------------------------------------
// The six orientations that are not the identity or a half turn.
//
// CIPA DC-008 Table 6 gives eight orientations. Only 1 and 3 were implemented;
// the other six returned GIMG_ERR_UNSUPPORTED, and gimg_item_decode turns a
// failed orientation into a failed decode - so an image whose metadata said
// "rotate 90", which is what a camera writes for anything held upright, could
// not be decoded at all.
//
// The fixture is 3 wide and 2 high so that a transform which exchanges the
// axes is visible in the dimensions, and asymmetric so that a transform
// applied backwards is visible in the pixels:
//
//     1 2 3
//     4 5 6
// ---------------------------------------------------------------------------

namespace {

/** A 3x2 GRAY8 raster holding 1..6 in reading order. */
GIMG_Raster * MakeGray3x2() {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(3, 2, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0,
          &r) != GIMG_OK) {
    return nullptr;
  }
  unsigned char * p = static_cast<unsigned char *>(gimg_raster_pixels(r));
  size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < 2; y++) {
    for (uint32_t x = 0; x < 3; x++) {
      p[y * stride + x] = static_cast<unsigned char>(y * 3 + x + 1);
    }
  }
  return r;
}

/** Pixels of a GRAY8 raster in reading order, ignoring stride padding. */
std::vector<unsigned char> GrayPixels(const GIMG_Raster * r) {
  const unsigned char * p =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(r));
  size_t stride = gimg_raster_stride_bytes(r);
  uint32_t w = gimg_raster_width(r);
  uint32_t h = gimg_raster_height(r);
  std::vector<unsigned char> out;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      out.push_back(p[y * stride + x]);
    }
  }
  return out;
}

} // namespace

TEST(Ops, EveryExifOrientationProducesTheImageItNames) {
  struct Case {
    GIMG_Orientation orientation;
    uint32_t width;
    uint32_t height;
    std::vector<unsigned char> expected;
    const char * name;
  };
  // Each expectation is read off the definition in CIPA DC-008 Table 6, not
  // off this implementation.
  const std::vector<Case> cases = {
      {GIMG_ORIENTATION_FLIP_H, 3, 2, {3, 2, 1, 6, 5, 4}, "mirror horizontal"},
      {GIMG_ORIENTATION_ROTATE_180, 3, 2, {6, 5, 4, 3, 2, 1}, "rotate 180"},
      {GIMG_ORIENTATION_FLIP_V, 3, 2, {4, 5, 6, 1, 2, 3}, "mirror vertical"},
      {GIMG_ORIENTATION_TRANSPOSE, 2, 3, {1, 4, 2, 5, 3, 6}, "transpose"},
      {GIMG_ORIENTATION_ROTATE_90_CW, 2, 3, {4, 1, 5, 2, 6, 3}, "rotate 90 CW"},
      {GIMG_ORIENTATION_TRANSVERSE, 2, 3, {6, 3, 5, 2, 4, 1}, "transverse"},
      {GIMG_ORIENTATION_ROTATE_90_CCW, 2, 3, {3, 6, 2, 5, 1, 4},
          "rotate 90 CCW"},
  };
  for (const Case & c : cases) {
    GIMG_Raster * r = MakeGray3x2();
    ASSERT_NE(r, nullptr) << c.name;
    ASSERT_EQ(gimg_ops_apply_orientation(r, c.orientation), GIMG_OK) << c.name;
    EXPECT_EQ(gimg_raster_width(r), c.width) << c.name;
    EXPECT_EQ(gimg_raster_height(r), c.height) << c.name;
    EXPECT_EQ(GrayPixels(r), c.expected) << c.name;
    gimg_raster_destroy(r);
  }
}

TEST(Ops, OrientationsComposeBackToTheOriginal) {
  // The expectations above could all be wrong in the same way. These identities
  // hold whatever the transforms are, as long as they are the transforms their
  // names claim: a mirror is its own inverse, and four quarter turns are none.
  GIMG_Raster * original = MakeGray3x2();
  ASSERT_NE(original, nullptr);
  std::vector<unsigned char> reference = GrayPixels(original);
  gimg_raster_destroy(original);

  const GIMG_Orientation involutions[] = {GIMG_ORIENTATION_FLIP_H,
      GIMG_ORIENTATION_FLIP_V, GIMG_ORIENTATION_ROTATE_180,
      GIMG_ORIENTATION_TRANSPOSE, GIMG_ORIENTATION_TRANSVERSE};
  for (GIMG_Orientation o : involutions) {
    GIMG_Raster * r = MakeGray3x2();
    ASSERT_NE(r, nullptr);
    ASSERT_EQ(gimg_ops_apply_orientation(r, o), GIMG_OK);
    ASSERT_EQ(gimg_ops_apply_orientation(r, o), GIMG_OK);
    EXPECT_EQ(gimg_raster_width(r), 3u);
    EXPECT_EQ(gimg_raster_height(r), 2u);
    EXPECT_EQ(GrayPixels(r), reference) << "applied twice, orientation " << o;
    gimg_raster_destroy(r);
  }

  GIMG_Raster * turning = MakeGray3x2();
  ASSERT_NE(turning, nullptr);
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(
        gimg_ops_apply_orientation(turning, GIMG_ORIENTATION_ROTATE_90_CW),
        GIMG_OK);
  }
  EXPECT_EQ(gimg_raster_width(turning), 3u);
  EXPECT_EQ(gimg_raster_height(turning), 2u);
  EXPECT_EQ(GrayPixels(turning), reference) << "four quarter turns clockwise";
  gimg_raster_destroy(turning);

  GIMG_Raster * both = MakeGray3x2();
  ASSERT_NE(both, nullptr);
  ASSERT_EQ(gimg_ops_apply_orientation(both, GIMG_ORIENTATION_ROTATE_90_CW),
      GIMG_OK);
  ASSERT_EQ(gimg_ops_apply_orientation(both, GIMG_ORIENTATION_ROTATE_90_CCW),
      GIMG_OK);
  EXPECT_EQ(GrayPixels(both), reference) << "one turn each way";
  gimg_raster_destroy(both);
}

TEST(Ops, AQuarterTurnWorksOnAWiderPixelToo) {
  // The remap copies bytes_per_pixel bytes at a time; RGBA8 catches an
  // implementation that assumed one byte per pixel.
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(
                2, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  unsigned char * p = static_cast<unsigned char *>(gimg_raster_pixels(r));
  const unsigned char left[4] = {10, 20, 30, 40};
  const unsigned char right[4] = {50, 60, 70, 80};
  std::memcpy(p, left, 4);
  std::memcpy(p + 4, right, 4);

  ASSERT_EQ(gimg_ops_apply_orientation(r, GIMG_ORIENTATION_ROTATE_90_CW),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r), 1u);
  EXPECT_EQ(gimg_raster_height(r), 2u);
  const unsigned char * q =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(r));
  size_t stride = gimg_raster_stride_bytes(r);
  // Turning a row of two clockwise puts the left pixel on top.
  EXPECT_EQ(std::memcmp(q, left, 4), 0);
  EXPECT_EQ(std::memcmp(q + stride, right, 4), 0);
  gimg_raster_destroy(r);
}

namespace {

/** A raster tagged with a color space and a small ICC profile. */
static GIMG_Raster * tagged_raster(const GIMG_Pixel_Format * fmt,
    std::vector<uint8_t> & profile_out) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(4, 4, fmt, GIMG_RASTER_OWNED, nullptr, 0, &r) !=
          GIMG_OK ||
      !r) {
    return nullptr;
  }
  std::memset(gimg_raster_pixels(r), 0x20,
      gimg_raster_stride_bytes(r) * 4u);
  profile_out.assign(128, 0);
  profile_out[3] = 128;
  std::memcpy(profile_out.data() + 36, "acsp", 4);
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.primaries = GIMG_PRIMARIES_ADOBE_RGB;
  ci.white_point = GIMG_PRIMARIES_ADOBE_RGB;
  ci.transfer = GIMG_TRANSFER_GAMMA;
  ci.gamma_value = 2.2;
  ci.intent = GIMG_INTENT_SATURATION;
  ci.icc_bytes = profile_out.data();
  ci.icc_size = profile_out.size();
  if (gimg_raster_set_color_info(r, &ci) != GIMG_OK) {
    gimg_raster_destroy(r);
    return nullptr;
  }
  return r;
}

/** Everything GIMG_Color_Info says, compared field by field. */
static void expect_same_color(const GIMG_Color_Info * got,
    const std::vector<uint8_t> & profile) {
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->primaries, GIMG_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(got->white_point, GIMG_PRIMARIES_ADOBE_RGB);
  EXPECT_EQ(got->transfer, GIMG_TRANSFER_GAMMA);
  EXPECT_DOUBLE_EQ(got->gamma_value, 2.2);
  EXPECT_EQ(got->intent, GIMG_INTENT_SATURATION);
  ASSERT_EQ(got->icc_size, profile.size());
  ASSERT_NE(got->icc_bytes, nullptr);
  EXPECT_EQ(std::memcmp(got->icc_bytes, profile.data(), profile.size()), 0);
}

} // namespace

TEST(Ops, ConvertBitDepthKeepsTheColorSpaceAndProfile) {
  // Restating a sample at a different precision does not change what it
  // means, so the color space still describes the result.  Dropping it made a
  // 16-bit PNG carrying an iCCP come out of a save as JPEG untagged, because
  // that writer converts to 12 bits on the way.
  for (uint8_t bits : {(uint8_t)8, (uint8_t)12, (uint8_t)16}) {
    std::vector<uint8_t> profile;
    GIMG_Raster * src = tagged_raster(&GIMG_PIXEL_RGBA16, profile);
    ASSERT_NE(src, nullptr);
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(gimg_ops_convert_bit_depth(src, bits, &dst), GIMG_OK)
        << "converting to " << (int)bits << " bits";
    ASSERT_NE(dst, nullptr);
    {
      SCOPED_TRACE(testing::Message() << "target " << (int)bits << " bits");
      expect_same_color(gimg_raster_color_info_const(dst), profile);
    }
    // Deep-copied, not aliased: the result must outlive the source.
    EXPECT_NE(gimg_raster_color_info_const(dst)->icc_bytes,
        gimg_raster_color_info_const(src)->icc_bytes);
    gimg_raster_destroy(src);
    expect_same_color(gimg_raster_color_info_const(dst), profile);
    gimg_raster_destroy(dst);
  }
}

TEST(Ops, ConvertBitDepthOnAGrayRasterKeepsTheColorSpaceToo) {
  std::vector<uint8_t> profile;
  GIMG_Raster * src = tagged_raster(&GIMG_PIXEL_GRAY8, profile);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(gimg_ops_convert_bit_depth(src, 16, &dst), GIMG_OK);
  ASSERT_NE(dst, nullptr);
  expect_same_color(gimg_raster_color_info_const(dst), profile);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, ConvertPixelFormatKeepsTheColorSpaceAndProfile) {
  std::vector<uint8_t> profile;
  GIMG_Raster * src = tagged_raster(&GIMG_PIXEL_RGBA8, profile);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(
      gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, &dst), GIMG_OK);
  ASSERT_NE(dst, nullptr);
  expect_same_color(gimg_raster_color_info_const(dst), profile);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, AnUntaggedRasterStaysUntaggedThroughAConversion) {
  GIMG_Raster * src = nullptr;
  ASSERT_EQ(gimg_raster_create(
                4, 4, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &src),
      GIMG_OK);
  std::memset(gimg_raster_pixels(src), 0, gimg_raster_stride_bytes(src) * 4u);
  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(gimg_ops_convert_bit_depth(src, 16, &dst), GIMG_OK);
  ASSERT_NE(dst, nullptr);
  const GIMG_Color_Info * ci = gimg_raster_color_info_const(dst);
  ASSERT_NE(ci, nullptr);
  EXPECT_EQ(ci->icc_size, 0u);
  EXPECT_EQ(ci->primaries, GIMG_PRIMARIES_UNKNOWN);
  EXPECT_EQ(ci->transfer, GIMG_TRANSFER_UNKNOWN);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

namespace {

/** A 1x1 CMYK8 raster holding one sample set, with the given polarity. */
static GIMG_Raster * one_cmyk_pixel(uint8_t c, uint8_t m, uint8_t y, uint8_t k,
    GIMG_CMYK_Polarity polarity) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(
          1, 1, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, nullptr, 0, &r) !=
          GIMG_OK ||
      !r) {
    return nullptr;
  }
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(r));
  px[0] = c;
  px[1] = m;
  px[2] = y;
  px[3] = k;
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.cmyk_polarity = polarity;
  if (gimg_raster_set_color_info(r, &ci) != GIMG_OK) {
    gimg_raster_destroy(r);
    return nullptr;
  }
  return r;
}

} // namespace

TEST(Ops, CmykBecomesRgbTheWayEveryEngineLessLibraryDoesIt) {
  // Each ink is an independent multiplicative filter over white, so a channel
  // is the product of its own colourant and the black.  The samples are the
  // JPEG and Adobe convention here - 0 is full ink - which is what
  // GIMG_CMYK_POLARITY_INK means and what this library's decoder produces.
  //
  // Every expected value below is Pillow's, which is libjpeg's CMYK handling
  // plus its own conversion.  On all fifteen CMYK and YCCK fixtures in
  // tests/data/jpeg, every pixel of every one, the two agree exactly.
  struct Case {
    uint8_t c, m, y, k;
    uint8_t r, g, b;
  };
  static const Case cases[] = {
      {255, 255, 255, 255, 255, 255, 255}, // no ink at all: white
      {0, 0, 0, 0, 0, 0, 0},               // every ink full: black
      {0, 0, 0, 255, 0, 0, 0},             // full C, M, Y, no black
      {255, 255, 255, 0, 0, 0, 0},         // no colourant, full black
      {128, 128, 128, 255, 128, 128, 128}, // half of each colourant
      {255, 0, 0, 255, 255, 0, 0},         // red
      {219, 240, 190, 250, 215, 235, 186}, // rounding, not truncation
  };
  for (const Case & t : cases) {
    SCOPED_TRACE(testing::Message() << "cmyk " << (int)t.c << "," << (int)t.m
                                    << "," << (int)t.y << "," << (int)t.k);
    GIMG_Raster * src =
        one_cmyk_pixel(t.c, t.m, t.y, t.k, GIMG_CMYK_POLARITY_INK);
    ASSERT_NE(src, nullptr);
    GIMG_Raster * dst = nullptr;
    ASSERT_EQ(
        gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, &dst), GIMG_OK);
    ASSERT_NE(dst, nullptr);
    const uint8_t * px =
        static_cast<const uint8_t *>(gimg_raster_pixels_const(dst));
    EXPECT_EQ(px[0], t.r);
    EXPECT_EQ(px[1], t.g);
    EXPECT_EQ(px[2], t.b);
    EXPECT_EQ(px[3], 255) << "CMYK has no alpha, so the result is opaque";
    gimg_raster_destroy(src);
    gimg_raster_destroy(dst);
  }
}

TEST(Ops, ReflectionPolarityIsTheOtherWayRound) {
  // The two readings are photographic negatives of each other, so a raster
  // that says 0 is no ink must come out as the complement of one that says 0
  // is full ink.
  GIMG_Raster * ink =
      one_cmyk_pixel(255, 255, 255, 255, GIMG_CMYK_POLARITY_INK);
  GIMG_Raster * refl =
      one_cmyk_pixel(0, 0, 0, 0, GIMG_CMYK_POLARITY_REFLECTION);
  ASSERT_NE(ink, nullptr);
  ASSERT_NE(refl, nullptr);
  GIMG_Raster * a = nullptr;
  GIMG_Raster * b = nullptr;
  ASSERT_EQ(gimg_ops_convert_pixel_format(ink, &GIMG_PIXEL_RGBA8, &a), GIMG_OK);
  ASSERT_EQ(
      gimg_ops_convert_pixel_format(refl, &GIMG_PIXEL_RGBA8, &b), GIMG_OK);
  const uint8_t * pa = static_cast<const uint8_t *>(gimg_raster_pixels_const(a));
  const uint8_t * pb = static_cast<const uint8_t *>(gimg_raster_pixels_const(b));
  EXPECT_EQ(pa[0], 255);
  EXPECT_EQ(pb[0], 255) << "no ink either way is white";
  gimg_raster_destroy(a);
  gimg_raster_destroy(b);
  gimg_raster_destroy(ink);
  gimg_raster_destroy(refl);

  GIMG_Raster * full = one_cmyk_pixel(255, 255, 255, 255,
      GIMG_CMYK_POLARITY_REFLECTION);
  ASSERT_NE(full, nullptr);
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(
      gimg_ops_convert_pixel_format(full, &GIMG_PIXEL_RGBA8, &out), GIMG_OK);
  const uint8_t * po =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(out));
  EXPECT_EQ(po[0], 0);
  EXPECT_EQ(po[1], 0);
  EXPECT_EQ(po[2], 0) << "every ink at full under this reading is black";
  gimg_raster_destroy(full);
  gimg_raster_destroy(out);
}

TEST(Ops, CmykWithNoStatedPolarityIsRefusedRatherThanGuessed) {
  // The two readings are negatives of each other, so guessing would produce a
  // plausible picture that might be inverted.  Refusing is the honest answer
  // and the error tells the caller what to state.
  GIMG_Raster * src =
      one_cmyk_pixel(10, 20, 30, 40, GIMG_CMYK_POLARITY_UNKNOWN);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  EXPECT_EQ(gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, &dst),
      GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(dst, nullptr);
  gimg_raster_destroy(src);
}

TEST(Ops, CmykToRgbCarriesNoColorInfo) {
  // Whatever the source said described four ink amounts on some press, and
  // none of it - an embedded profile least of all - is true of the
  // three-channel result.  Copying a CMYK profile onto RGB pixels would
  // label them with a space they are not in.
  std::vector<uint8_t> profile(128, 0);
  std::memcpy(profile.data() + 36, "acsp", 4);
  GIMG_Raster * src =
      one_cmyk_pixel(100, 110, 120, 130, GIMG_CMYK_POLARITY_INK);
  ASSERT_NE(src, nullptr);
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
  ci.icc_bytes = profile.data();
  ci.icc_size = profile.size();
  ci.transfer = GIMG_TRANSFER_GAMMA;
  ci.gamma_value = 1.8;
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(
      gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA8, &dst), GIMG_OK);
  const GIMG_Color_Info * got = gimg_raster_color_info_const(dst);
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got->icc_size, 0u);
  EXPECT_EQ(got->icc_bytes, nullptr);
  EXPECT_EQ(got->transfer, GIMG_TRANSFER_UNKNOWN);
  EXPECT_EQ(got->cmyk_polarity, GIMG_CMYK_POLARITY_UNKNOWN)
      << "the result has no ink channels for a polarity to describe";
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}

TEST(Ops, CmykToRgbAtADifferentSampleWidthIsRefused) {
  // Narrowing or widening is gimg_ops_convert_bit_depth's job; doing both at
  // once would hide which of them the caller asked for.
  GIMG_Raster * src = one_cmyk_pixel(10, 20, 30, 40, GIMG_CMYK_POLARITY_INK);
  ASSERT_NE(src, nullptr);
  GIMG_Raster * dst = nullptr;
  EXPECT_EQ(gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA16, &dst),
      GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(dst, nullptr);
  gimg_raster_destroy(src);
}

TEST(Ops, SixteenBitCmykUsesTheWholeRange) {
  GIMG_Raster * src = nullptr;
  ASSERT_EQ(gimg_raster_create(
                2, 1, &GIMG_PIXEL_CMYK16, GIMG_RASTER_OWNED, nullptr, 0, &src),
      GIMG_OK);
  uint16_t * px = static_cast<uint16_t *>(gimg_raster_pixels(src));
  px[0] = px[1] = px[2] = px[3] = 65535u;      // no ink
  px[4] = px[5] = px[6] = 65535u; px[7] = 0u;  // full black only
  GIMG_Color_Info ci;
  gimg_color_info_default(&ci);
  ci.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
  ASSERT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);

  GIMG_Raster * dst = nullptr;
  ASSERT_EQ(
      gimg_ops_convert_pixel_format(src, &GIMG_PIXEL_RGBA16, &dst), GIMG_OK);
  const uint16_t * out =
      static_cast<const uint16_t *>(gimg_raster_pixels_const(dst));
  EXPECT_EQ(out[0], 65535u);
  EXPECT_EQ(out[3], 65535u) << "opaque at the full 16-bit value, not 255";
  EXPECT_EQ(out[4], 0u);
  EXPECT_EQ(out[5], 0u);
  EXPECT_EQ(out[6], 0u);
  gimg_raster_destroy(src);
  gimg_raster_destroy(dst);
}
