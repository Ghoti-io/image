/**
 * @file
 *
 * Unit tests for PNG common: row bytes and Adam7 pass dimensions (shared by
 * decode and save).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>

#include "../../src/codec/png/png_internal.h"

TEST(PngCommon, RowBytesGrayscale) {
  // color_type 0: 1 sample per pixel
  EXPECT_EQ(gimg_png_row_bytes(0, 8, 100), 100u);
  EXPECT_EQ(gimg_png_row_bytes(0, 16, 100), 200u);
  EXPECT_EQ(gimg_png_row_bytes(0, 8, 0), 0u);
}

TEST(PngCommon, RowBytesRgb) {
  // color_type 2: 3 samples per pixel
  EXPECT_EQ(gimg_png_row_bytes(2, 8, 10), 30u);
  EXPECT_EQ(gimg_png_row_bytes(2, 16, 10), 60u);
}

TEST(PngCommon, RowBytesPalette) {
  // color_type 3: 1 byte per pixel (index)
  EXPECT_EQ(gimg_png_row_bytes(3, 8, 50), 50u);
}

TEST(PngCommon, RowBytesGrayAlpha) {
  // color_type 4: 2 samples per pixel
  EXPECT_EQ(gimg_png_row_bytes(4, 8, 10), 20u);
  EXPECT_EQ(gimg_png_row_bytes(4, 16, 10), 40u);
}

TEST(PngCommon, RowBytesRgba) {
  // color_type 6: 4 samples per pixel
  EXPECT_EQ(gimg_png_row_bytes(6, 8, 10), 40u);
  EXPECT_EQ(gimg_png_row_bytes(6, 16, 10), 80u);
}

TEST(PngCommon, RowBytesInvalidColorType) {
  EXPECT_EQ(gimg_png_row_bytes(1, 8, 10), 0u);
  EXPECT_EQ(gimg_png_row_bytes(5, 8, 10), 0u);
  EXPECT_EQ(gimg_png_row_bytes(7, 8, 10), 0u);
}

TEST(PngCommon, RowBytesFromIhdr) {
  gimg_png_ihdr_t ihdr = {};
  ihdr.color_type = 6;
  ihdr.bit_depth = 8;
  ihdr.width = 16;
  ihdr.height = 16;
  EXPECT_EQ(gimg_png_row_bytes_from_ihdr(&ihdr, 16), 64u);
  EXPECT_EQ(gimg_png_row_bytes_from_ihdr(&ihdr, 8), 32u);
  EXPECT_EQ(gimg_png_row_bytes_from_ihdr(nullptr, 10), 0u);
}

TEST(PngCommon, Adam7PassDims) {
  uint32_t pw = 0;
  uint32_t ph = 0;
  // 8x8 image: pass 0 has 1x1
  gimg_png_adam7_pass_dims(8, 8, 0, &pw, &ph);
  EXPECT_EQ(pw, 1u);
  EXPECT_EQ(ph, 1u);
  gimg_png_adam7_pass_dims(8, 8, 1, &pw, &ph);
  EXPECT_EQ(pw, 1u);
  EXPECT_EQ(ph, 1u);
  // 16x16: pass 0 is 2x2
  gimg_png_adam7_pass_dims(16, 16, 0, &pw, &ph);
  EXPECT_EQ(pw, 2u);
  EXPECT_EQ(ph, 2u);
  // 4x4 image: pass 0 is 1x1 (ceil(4/8)); pass 1 has x_offset=4 so width 0
  gimg_png_adam7_pass_dims(4, 4, 0, &pw, &ph);
  EXPECT_EQ(pw, 1u);
  EXPECT_EQ(ph, 1u);
  gimg_png_adam7_pass_dims(4, 4, 1, &pw, &ph);
  EXPECT_EQ(pw, 0u);
  EXPECT_EQ(ph, 1u);
}

TEST(PngCommon, Adam7RawSize) {
  size_t out = 0;
  // Non-interlaced equivalent for 8x8 RGBA8: 8 * (1 + 32) = 264
  // Adam7: sum of (1 + row_bytes) * pass_height over 7 passes
  bool ok = gimg_png_adam7_raw_size(8, 8, 6, 8, &out);
  EXPECT_TRUE(ok);
  EXPECT_GT(out, 0u);
  // Overflow case: huge dimensions might overflow
  ok = gimg_png_adam7_raw_size(1, 1, 0, 8, &out);
  EXPECT_TRUE(ok);
  EXPECT_GT(out, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
