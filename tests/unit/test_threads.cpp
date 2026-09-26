/**
 * @file
 *
 * Concurrency: two threads using the library on separate documents must not
 * interfere, and must produce the same bytes they would alone.
 *
 * The interesting assertion here is **not** "nothing crashed". It is that a
 * concurrent save produces the same file as a solitary one. The JPEG encoder
 * kept its reciprocal quantizer tables - derived from the caller's quality
 * setting - in file-scope statics, so two encodes at different qualities
 * quantized each other's coefficients. That is wrong output, not a crash, and
 * a test that only watched for crashes would have passed over it.
 *
 * Run under ThreadSanitizer with `make test-tsan` for the other half: this
 * file finds corruption that happens to occur, TSan finds the races whether
 * they occur on this run or not.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/image.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

namespace {

/**
 * A raster whose content depends on the seed, so that two threads are not
 * doing identical work.
 *
 * @p few_colours keeps the image inside the 256 a colour table can hold, for
 * the formats that store one. GIF refuses more, and rightly - which colours to
 * discard is gimg_ops_quantize()'s decision, not a writer's.
 */
GIMG_Raster * make_image(uint32_t seed, bool few_colours) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(48, 32, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr,
          0, &r) != GIMG_OK) {
    return nullptr;
  }
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < 32; y++) {
    for (uint32_t x = 0; x < 48; x++) {
      unsigned char * px = p + (size_t)y * stride + (size_t)x * 4u;
      if (few_colours) {
        const unsigned char v = (unsigned char)((x * 5u + y * 7u + seed) & 0xFFu);
        px[0] = v;
        px[1] = v;
        px[2] = v;
      }
      else {
        px[0] = (unsigned char)(x * 5u + seed * 37u);
        px[1] = (unsigned char)(y * 7u + seed * 11u);
        px[2] = (unsigned char)((x ^ y) + seed);
      }
      px[3] = 255u;
    }
  }
  return r;
}

/** Save one image and hand back the bytes. Empty on any failure. */
std::string save_once(uint32_t seed, const char * format, uint8_t quality) {
  // GIF stores a colour table, so it needs an image that fits in one.
  const bool few_colours = (std::strcmp(format, "gif") == 0);
  GIMG_Raster * r = make_image(seed, few_colours);
  if (!r) {
    return std::string();
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_from_raster(r, &doc) != GIMG_OK) {
    gimg_raster_destroy(r);
    return std::string();
  }
  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_raster_destroy(r);
    return std::string();
  }
  GIMG_Save_Options options;
  memset(&options, 0, sizeof(options));
  options.jpeg_quality = quality;
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  std::string bytes;
  if (gimg_doc_save(doc, out, format, &options, &report) == GIMG_OK) {
    const void * buffer = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &buffer, &size);
    bytes.assign((const char *)buffer, size);
  }
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_raster_destroy(r);
  return bytes;
}

} // namespace

/**
 * Reading the version from several threads, **the first read included**.
 *
 * gimg_version_string() used to fill a static buffer behind an unsynchronized
 * `initialized` flag: the first caller wrote the buffer, later callers only
 * read it. The whole of that race therefore lives in the *first* call, which
 * is precisely what a test destroys by asking for the value before it starts
 * its threads. An earlier version of this test opened with
 *
 *     const std::string expected = gimg_version_string();
 *
 * and then compared every thread against it. That one line filled the buffer
 * single-threaded, so the eight threads did nothing but read and TSan reported
 * a clean run against the very bug this is here to catch. It is the same
 * warm-up flaw as the one AaaColdStart.ConcurrentSavesFromNothing exists to
 * avoid, and it was sitting two tests below it.
 *
 * So no call is made from this thread until after the join, and each thread is
 * compared against what the other threads saw rather than against a value
 * prepared for them. The integer accessors used to build the reference return
 * a constant and touch nothing, so reading them afterwards warms nothing.
 */
TEST(AaaColdStart, TheVersionIsSafeToReadFromNothing) {
  std::vector<std::string> seen(8);
  std::atomic<int> unstable{0};
  std::vector<std::thread> threads;
  for (int i = 0; i < 8; i++) {
    threads.emplace_back([&seen, &unstable, i]() {
      // The first call in the process happens here, on one of eight threads.
      const std::string mine = gimg_version_string();
      for (int j = 0; j < 500; j++) {
        if (std::string(gimg_version_string()) != mine) {
          unstable++;
        }
      }
      seen[(size_t)i] = mine;
    });
  }
  for (auto & t : threads) {
    t.join();
  }

  char expected[64];
  std::snprintf(expected, sizeof(expected), "%u.%u.%u", gimg_version_major(),
      gimg_version_minor(), gimg_version_patch());
  EXPECT_EQ(unstable.load(), 0) << "the string changed under a reader";
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ(seen[(size_t)i], std::string(expected))
        << "thread " << i << " saw a different version string";
  }
}

/**
 * Concurrent saves with **nothing run before them**.
 *
 * This test is first in the file deliberately, and the ordering is the whole
 * point. State that is built once and then only read - a lazily initialised
 * table - is written exactly once, on the first call. Any test that saves
 * something single-threaded first has already done that write, so by the time
 * its threads start there is nothing left to race on and a sanitizer finds a
 * clean run.
 *
 * That is not a hypothetical. The JPEG encoder's derived Huffman tables sat
 * behind a `tables_built` flag published with no ordering against the writes
 * it guarded. Reintroduce that bug and ConcurrentSavesMatchSolitaryOnes stays
 * green under ThreadSanitizer, because its solitary pass warms the tables
 * first. This one does not warm anything.
 *
 * It asserts only that the saves succeed. What it is really for is to give
 * `make test-tsan` a cold start to instrument.
 */
TEST(AaaColdStart, ConcurrentSavesFromNothing) {
  std::atomic<int> failures{0};
  std::vector<std::thread> threads;
  const uint8_t qualities[] = {90u, 30u, 60u, 75u};
  for (uint32_t i = 0; i < 4; i++) {
    threads.emplace_back([&failures, &qualities, i]() {
      for (int rep = 0; rep < 3; rep++) {
        if (save_once(100u + i, "jpeg", qualities[i]).empty()) {
          failures++;
        }
      }
    });
  }
  for (auto & t : threads) {
    t.join();
  }
  EXPECT_EQ(failures.load(), 0);
}

/**
 * The same work, done alone and then done by four threads at once, gives the
 * same bytes.
 *
 * The qualities deliberately differ between threads. Sharing a derived
 * quantizer table only shows up when the thing derived from it differs, and
 * every thread asking for quality 90 would hide exactly the defect this is
 * here for.
 */
TEST(Threads, ConcurrentSavesMatchSolitaryOnes) {
  struct Work {
    uint32_t seed;
    const char * format;
    uint8_t jpeg_quality;
  };
  const Work work[] = {
      {1u, "jpeg", 95u},
      {2u, "jpeg", 40u},
      {3u, "jpeg", 75u},
      {4u, "png", 0u},
      {5u, "bmp", 0u},
      {6u, "gif", 0u},
      {7u, "jpeg", 10u},
      {8u, "jpeg", 100u},
  };
  const size_t count = sizeof(work) / sizeof(work[0]);

  // Alone first, so there is something to be right about.
  std::vector<std::string> alone(count);
  for (size_t i = 0; i < count; i++) {
    alone[i] = save_once(work[i].seed, work[i].format, work[i].jpeg_quality);
    ASSERT_FALSE(alone[i].empty())
        << "solitary save of " << work[i].format << " failed";
  }

  // Then all at once, several rounds, because a race need not lose every time.
  for (int round = 0; round < 8; round++) {
    std::vector<std::string> together(count);
    std::vector<std::thread> threads;
    threads.reserve(count);
    for (size_t i = 0; i < count; i++) {
      threads.emplace_back([&together, &work, i]() {
        together[i] =
            save_once(work[i].seed, work[i].format, work[i].jpeg_quality);
      });
    }
    for (auto & t : threads) {
      t.join();
    }
    for (size_t i = 0; i < count; i++) {
      ASSERT_EQ(together[i].size(), alone[i].size())
          << "round " << round << ": " << work[i].format << " at quality "
          << (int)work[i].jpeg_quality << " changed size when run concurrently";
      ASSERT_TRUE(together[i] == alone[i])
          << "round " << round << ": " << work[i].format << " at quality "
          << (int)work[i].jpeg_quality
          << " produced different bytes when run concurrently";
    }
  }
}

/** The version accessors agree with the string, and with each other. */
TEST(Version, TheAccessorsAgreeWithTheString) {
  char expected[64];
  std::snprintf(expected, sizeof(expected), "%u.%u.%u", gimg_version_major(),
      gimg_version_minor(), gimg_version_patch());
  EXPECT_STREQ(gimg_version_string(), expected);
  // Stable across calls: it is a literal, not a buffer someone may refill.
  EXPECT_EQ(gimg_version_string(), gimg_version_string());
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
