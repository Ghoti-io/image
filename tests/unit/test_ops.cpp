/**
 * @file
 *
 * Unit tests for orientation, pixel format conversion, alpha.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/bitdepth.h>
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
  // Mid-grey stays mid-grey across every widening.
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
