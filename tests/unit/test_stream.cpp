/**
 * @file
 *
 * Unit tests for GIMG_STREAM (memory stream: read, peek, skip, seek, tell).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

TEST(StreamMemory, CreateAndRead) {
  const unsigned char data[] = {1, 2, 3, 4, 5};
  GIMG_STREAM * s = nullptr;
  GIMG_RESULT r = gimg_stream_create_memory(data, sizeof(data), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);
  unsigned char buf[3];
  size_t n = 0;
  r = gimg_stream_read(s, buf, 3, &n);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(n, 3u);
  EXPECT_EQ(buf[0], 1);
  EXPECT_EQ(buf[1], 2);
  EXPECT_EQ(buf[2], 3);
  EXPECT_EQ(gimg_stream_tell(s), 3u);
  gimg_stream_destroy(s);
}

TEST(StreamMemory, PeekDoesNotConsume) {
  const unsigned char data[] = {10, 20};
  GIMG_STREAM * s = nullptr;
  gimg_stream_create_memory(data, 2, &s);
  unsigned char buf[2];
  size_t avail = 0;
  GIMG_RESULT r = gimg_stream_peek(s, buf, 2, &avail);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(avail, 2u);
  EXPECT_EQ(buf[0], 10);
  EXPECT_EQ(buf[1], 20);
  EXPECT_EQ(gimg_stream_tell(s), 0u);
  r = gimg_stream_read(s, buf, 1, &avail);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(gimg_stream_tell(s), 1u);
  gimg_stream_destroy(s);
}

TEST(StreamMemory, Skip) {
  const unsigned char data[] = {1, 2, 3, 4};
  GIMG_STREAM * s = nullptr;
  gimg_stream_create_memory(data, 4, &s);
  size_t skipped = 0;
  GIMG_RESULT r = gimg_stream_skip(s, 2, &skipped);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(skipped, 2u);
  EXPECT_EQ(gimg_stream_tell(s), 2u);
  unsigned char b;
  size_t n;
  r = gimg_stream_read(s, &b, 1, &n);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(b, 3);
  gimg_stream_destroy(s);
}

TEST(StreamMemory, SeekAndTell) {
  const unsigned char data[] = {1, 2, 3};
  GIMG_STREAM * s = nullptr;
  gimg_stream_create_memory(data, 3, &s);
  GIMG_RESULT r = gimg_stream_seek(s, 2);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(gimg_stream_tell(s), 2u);
  EXPECT_EQ(gimg_stream_size(s), 3u);
  unsigned char b;
  size_t n;
  r = gimg_stream_read(s, &b, 1, &n);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(b, 3);
  gimg_stream_destroy(s);
}

TEST(StreamMemory, LimitsDefault) {
  GIMG_LIMITS lim = {};
  gimg_limits_default(&lim);
  EXPECT_EQ(lim.max_decoded_pixels, 0u);
  EXPECT_EQ(lim.max_frame_count, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
