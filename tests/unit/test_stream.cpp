/**
 * @file
 *
 * Unit tests for GIMG_Stream (memory stream: read, peek, skip, seek, tell).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

TEST(StreamMemory, CreateAndRead) {
  const unsigned char data[] = {1, 2, 3, 4, 5};
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(data, sizeof(data), &s);
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
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(data, 2, &s);
  unsigned char buf[2];
  size_t avail = 0;
  GIMG_Result r = gimg_stream_peek(s, buf, 2, &avail);
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
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(data, 4, &s);
  size_t skipped = 0;
  GIMG_Result r = gimg_stream_skip(s, 2, &skipped);
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
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(data, 3, &s);
  GIMG_Result r = gimg_stream_seek(s, 2);
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

TEST(StreamMemory, ANonSeekableStreamDoesNotKnowWhereItsEndIs) {
  // The length passed to the constructor is what the test harness happens to
  // have; a caller reading from a pipe has none to pass. A stream that
  // answered one anyway would let a decoder measure a file it cannot measure,
  // and the two BMP guards that exist for exactly that case - RLE and the
  // bitmap array, both of which need a length they cannot compute - would be
  // unreachable from any input.
  const unsigned char data[] = {1, 2, 3};
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_no_seek(data, sizeof(data), &s),
      GIMG_OK);
  EXPECT_EQ(gimg_stream_size(s), GIMG_STREAM_SIZE_UNKNOWN);
  EXPECT_EQ(gimg_stream_tell(s), (size_t)-1);
  EXPECT_EQ(gimg_stream_seek(s, 0u), GIMG_ERR_UNSUPPORTED);
  // Reading still works; it is the measuring that does not.
  unsigned char b = 0;
  size_t n = 0;
  EXPECT_EQ(gimg_stream_read(s, &b, 1u, &n), GIMG_OK);
  EXPECT_EQ(n, 1u);
  EXPECT_EQ(b, 1);
  gimg_stream_destroy(s);

  // A seekable stream of no bytes answers zero, which is a length. Zero and
  // "not known" are different answers and callers must not conflate them.
  GIMG_Stream * empty = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, 0u, &empty), GIMG_OK);
  EXPECT_EQ(gimg_stream_size(empty), 0u);
  gimg_stream_destroy(empty);

  EXPECT_EQ(gimg_stream_size(nullptr), GIMG_STREAM_SIZE_UNKNOWN);
}

TEST(StreamMemory, LimitsDefault) {
  GIMG_Limits lim = {};
  gimg_limits_default(&lim);
  EXPECT_EQ(lim.max_decoded_pixels, 0u);
  EXPECT_EQ(lim.max_frame_count, 0u);
}

// A stream that has failed stays failed, and every call says so.
//
// GIMG_Stream keeps the first error it hit and each entry point checks it
// before doing anything - which is what stops a caller that ignores one
// return value from going on to read whatever happens to be in the buffer.
// Five of those checks had never run: nothing in the suite ever called a
// stream again after a failure, so every one of them was untested.
//
// A seek past the end is the cheapest way in, and it is also the only thing
// that sets the error without an allocator having to fail.
TEST(StreamMemory, AFailedStreamStaysFailedAndReportsNothingRead) {
  const unsigned char data[] = {1, 2, 3, 4, 5};
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, sizeof(data), &s), GIMG_OK);

  // Control: before the failure every one of these works.
  unsigned char buf[3] = {0, 0, 0};
  size_t n = 99;
  ASSERT_EQ(gimg_stream_read(s, buf, 3, &n), GIMG_OK);
  ASSERT_EQ(n, 3u);
  ASSERT_EQ(gimg_stream_seek(s, 0), GIMG_OK);
  ASSERT_EQ(gimg_stream_peek(s, buf, 3, &n), GIMG_OK);
  ASSERT_EQ(n, 3u);
  ASSERT_EQ(gimg_stream_skip(s, 1, &n), GIMG_OK);
  ASSERT_EQ(n, 1u);
  ASSERT_EQ(gimg_stream_error(s), GIMG_OK);

  // Seeking past the end is an I/O error, and it sticks.
  ASSERT_EQ(gimg_stream_seek(s, sizeof(data) + 1u), GIMG_ERR_IO);
  ASSERT_EQ(gimg_stream_error(s), GIMG_ERR_IO)
      << "the error must be remembered, not just returned once";

  // Now every entry point reports it, and reports nothing transferred.
  n = 99;
  EXPECT_EQ(gimg_stream_read(s, buf, 3, &n), GIMG_ERR_IO);
  EXPECT_EQ(n, 0u) << "a failed read must not claim to have read anything";
  EXPECT_EQ(gimg_stream_read_exact(s, buf, 3), GIMG_ERR_IO);
  n = 99;
  EXPECT_EQ(gimg_stream_peek(s, buf, 3, &n), GIMG_ERR_IO);
  EXPECT_EQ(n, 0u);
  n = 99;
  EXPECT_EQ(gimg_stream_skip(s, 1, &n), GIMG_ERR_IO);
  EXPECT_EQ(n, 0u);
  EXPECT_EQ(gimg_stream_error(s), GIMG_ERR_IO) << "and it is still set";

  // Seeking somewhere legal does not clear it: there is no way to un-fail a
  // stream, which is the point of keeping the error at all.
  EXPECT_EQ(gimg_stream_seek(s, 0), GIMG_OK);
  EXPECT_EQ(gimg_stream_error(s), GIMG_ERR_IO);
  n = 99;
  EXPECT_EQ(gimg_stream_read(s, buf, 3, &n), GIMG_ERR_IO);
  EXPECT_EQ(n, 0u);

  gimg_stream_destroy(s);
}

// The same for an output stream, where writing is what a caller does next.
TEST(StreamMemory, AFailedOutputStreamRefusesToWrite) {
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&s), GIMG_OK);

  size_t n = 99;
  const unsigned char payload[] = {7, 8, 9};
  ASSERT_EQ(gimg_stream_write(s, payload, sizeof(payload), &n), GIMG_OK);
  ASSERT_EQ(n, 3u);

  // Writing nothing is not an error and writes nothing.  It is answered
  // before the buffer is even looked at, so a NULL buffer with a size of zero
  // is fine as well - which is what a caller passing an empty vector's data()
  // hands over.
  n = 99;
  EXPECT_EQ(gimg_stream_write(s, payload, 0, &n), GIMG_OK);
  EXPECT_EQ(n, 0u);
  n = 99;
  EXPECT_EQ(gimg_stream_write(s, nullptr, 0, &n), GIMG_OK);
  EXPECT_EQ(n, 0u);
  // What an output stream has written is what gimg_stream_output_buffer
  // reports.  gimg_stream_size() is the capacity of the buffer behind it -
  // 4096 after three bytes - so it is not the thing to ask here.
  const void * written = nullptr;
  size_t written_size = 99;
  gimg_stream_output_buffer(s, &written, &written_size);
  EXPECT_EQ(written_size, 3u) << "none of that changed the contents";

  // Reading an output stream is not an error either; there is simply nothing
  // to read, and a caller that asks gets told so rather than refused.
  unsigned char buf[3] = {0, 0, 0};
  n = 99;
  EXPECT_EQ(gimg_stream_read(s, buf, 3, &n), GIMG_OK);
  EXPECT_EQ(n, 0u);

  ASSERT_EQ(gimg_stream_seek(s, gimg_stream_size(s) + 1u), GIMG_ERR_IO);
  // Past the buffer's capacity, which is what seek measures against.
  ASSERT_EQ(gimg_stream_error(s), GIMG_ERR_IO);

  n = 99;
  EXPECT_EQ(gimg_stream_write(s, payload, sizeof(payload), &n), GIMG_ERR_IO);
  EXPECT_EQ(n, 0u) << "a failed write must not claim to have written anything";
  gimg_stream_output_buffer(s, &written, &written_size);
  EXPECT_EQ(written_size, 3u)
      << "and it must not have written anything either";

  gimg_stream_destroy(s);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
