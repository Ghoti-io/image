/**
 * @file
 *
 * Unit tests for internal safe-math helpers (overflow-safe pixel count and
 * size).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <ghoti.io/image/core.h>
#include <gtest/gtest.h>

// Include internal header for overflow-safe helpers.
#include "../../src/core/safe_math_internal.h"

TEST(SafeMath, SafePixelCountZeroDimensions) {
  size_t out = 99;
  EXPECT_EQ(gimg_safe_pixel_count(0, 0, &out), GIMG_OK);
  EXPECT_EQ(out, 0u);
  out = 99;
  EXPECT_EQ(gimg_safe_pixel_count(100, 0, &out), GIMG_OK);
  EXPECT_EQ(out, 0u);
  out = 99;
  EXPECT_EQ(gimg_safe_pixel_count(0, 100, &out), GIMG_OK);
  EXPECT_EQ(out, 0u);
}

TEST(SafeMath, SafePixelCountSmall) {
  size_t out = 0;
  EXPECT_EQ(gimg_safe_pixel_count(10, 20, &out), GIMG_OK);
  EXPECT_EQ(out, 200u);
}

TEST(SafeMath, SafePixelCountOverflow) {
  // On 32-bit size_t, 0x10000 * 0x10000 overflows; on 64-bit it fits.
  size_t out = 99;
  uint32_t big = 0x10000u; // 65536
  GIMG_Result r = gimg_safe_pixel_count(big, big, &out);
  if (sizeof(size_t) * 8 <= 32) {
    EXPECT_EQ(r, GIMG_ERR_LIMIT);
  }
  else {
    EXPECT_EQ(r, GIMG_OK);
    EXPECT_EQ(out, (size_t)0x10000u * 0x10000u);
  }
}

TEST(SafeMath, SafeMulSizeZero) {
  size_t out = 99;
  EXPECT_TRUE(gimg_safe_mul_size(0, 100, &out));
  EXPECT_EQ(out, 0u);
  out = 99;
  EXPECT_TRUE(gimg_safe_mul_size(100, 0, &out));
  EXPECT_EQ(out, 0u);
}

TEST(SafeMath, SafeMulSizeNormal) {
  size_t out = 0;
  EXPECT_TRUE(gimg_safe_mul_size(100, 200, &out));
  EXPECT_EQ(out, 20000u);
}

TEST(SafeMath, SafeAddSizeNormal) {
  size_t out = 0;
  EXPECT_TRUE(gimg_safe_add_size(100, 200, &out));
  EXPECT_EQ(out, 300u);
}

TEST(SafeMath, SafeAddSizeOverflow) {
  size_t out = 99;
  EXPECT_FALSE(gimg_safe_add_size(SIZE_MAX, 1, &out));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
