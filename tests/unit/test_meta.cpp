/**
 * @file
 *
 * Unit tests for metadata common and raw.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <ghoti.io/image/meta.h>
#include <gtest/gtest.h>

TEST(MetaCommon, CreateSetGetOrientation) {
  GIMG_Meta_Common * m = nullptr;
  GIMG_Result r = gimg_meta_common_create(&m);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(gimg_meta_common_orientation(m), GIMG_ORIENTATION_UNKNOWN);
  gimg_meta_common_set_orientation(m, GIMG_ORIENTATION_ROTATE_90_CW);
  EXPECT_EQ(gimg_meta_common_orientation(m), GIMG_ORIENTATION_ROTATE_90_CW);
  gimg_meta_common_destroy(m);
}

TEST(MetaCommon, SetGetDpi) {
  GIMG_Meta_Common * m = nullptr;
  gimg_meta_common_create(&m);
  gimg_meta_common_set_dpi(m, 300, 150);
  uint32_t x = 0, y = 0;
  gimg_meta_common_dpi(m, &x, &y);
  EXPECT_EQ(x, 300u);
  EXPECT_EQ(y, 150u);
  gimg_meta_common_destroy(m);
}

TEST(MetaCommon, SetGetDescription) {
  GIMG_Meta_Common * m = nullptr;
  GIMG_Result r = gimg_meta_common_create(&m);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(m, nullptr);
  EXPECT_EQ(gimg_meta_common_description(m), nullptr);
  r = gimg_meta_common_set_description(m, "Hello");
  ASSERT_EQ(r, GIMG_OK);
  const char * desc = gimg_meta_common_description(m);
  ASSERT_NE(desc, nullptr);
  EXPECT_STREQ(desc, "Hello");
  r = gimg_meta_common_set_description(m, nullptr);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(gimg_meta_common_description(m), nullptr);
  gimg_meta_common_destroy(m);
}

TEST(MetaRaw, AttachAndGet) {
  GIMG_Meta_Raw * raw = nullptr;
  GIMG_Result r = gimg_meta_raw_create(&raw);
  ASSERT_EQ(r, GIMG_OK);
  const unsigned char data[] = {1, 2, 3};
  r = gimg_meta_raw_attach(raw, "fmt", 42, data, sizeof(data));
  ASSERT_EQ(r, GIMG_OK);
  size_t size = 0;
  r = gimg_meta_raw_get(raw, "fmt", 42, nullptr, &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(size, 3u);
  unsigned char buf[4];
  size = sizeof(buf);
  r = gimg_meta_raw_get(raw, "fmt", 42, buf, &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(size, 3u);
  EXPECT_EQ(buf[0], 1);
  EXPECT_EQ(buf[1], 2);
  EXPECT_EQ(buf[2], 3);
  gimg_meta_raw_destroy(raw);
}

TEST(MetaRaw, GetNotFound) {
  GIMG_Meta_Raw * raw = nullptr;
  gimg_meta_raw_create(&raw);
  size_t size = 0;
  GIMG_Result r = gimg_meta_raw_get(raw, "x", 1, nullptr, &size);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  gimg_meta_raw_destroy(raw);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
