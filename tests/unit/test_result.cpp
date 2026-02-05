/**
 * @file
 *
 * Unit tests for GIMG_Result and gimg_result_string.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <ghoti.io/image/core.h>
#include <gtest/gtest.h>

TEST(Result, OkMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_OK), "ok");
}

TEST(Result, ErrIoMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_IO), "io error");
}

TEST(Result, ErrFormatMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_FORMAT), "format error");
}

TEST(Result, ErrUnsupportedMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_UNSUPPORTED), "unsupported");
}

TEST(Result, ErrLimitMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_LIMIT), "limit exceeded");
}

TEST(Result, ErrCorruptMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_CORRUPT), "corrupt data");
}

TEST(Result, ErrOomMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_OOM), "out of memory");
}

TEST(Result, ErrInternalMapsToString) {
  EXPECT_STREQ(gimg_result_string(GIMG_ERR_INTERNAL), "internal error");
}

TEST(Result, InvalidValueReturnsUnknown) {
  EXPECT_STREQ(gimg_result_string((GIMG_Result)GIMG_RESULT_COUNT), "unknown");
  EXPECT_STREQ(gimg_result_string((GIMG_Result)99), "unknown");
}

TEST(Result, AllValidCodesNonEmpty) {
  for (int i = 0; i < (int)GIMG_RESULT_COUNT; i++) {
    const char * s = gimg_result_string((GIMG_Result)i);
    EXPECT_NE(s, nullptr);
    EXPECT_GT(std::strlen(s), 0u);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
