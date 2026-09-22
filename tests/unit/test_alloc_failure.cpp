/**
 * @file
 *
 * What the codecs do when an allocation fails partway through a load.
 *
 * An error-cleanup path is unreachable from a well-formed file: the arms that
 * free partial state and return GIMG_ERR_OOM only run when something ran out
 * of memory, which no fixture can arrange. So this injects the failure. An
 * allocator that fails its Nth call, swept over every N a successful load
 * uses, reaches each of those arms in turn.
 *
 * The assertion is not "it returned an error". It is that **every block the
 * codec took is handed back** whichever allocation failed, so a caller who
 * retries after an OOM is not leaking a little more each time. A crash or a
 * leak here is a real defect; a clean error is the contract.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "../../src/codec/codec_internal.h"

namespace {

/** An allocator that fails one chosen call and counts what it hands out. */
struct Failing {
  GIMG_Allocator a{};
  long attempts = 0;    ///< Allocation calls seen.
  long fail_at = -1;    ///< 1-based call to fail; -1 never fails.
  long outstanding = 0; ///< Blocks handed out and not yet returned.

  bool should_fail() {
    attempts++;
    return fail_at >= 0 && attempts == fail_at;
  }
};

void * f_malloc(void * ctx, size_t size) {
  Failing * f = (Failing *)ctx;
  if (f->should_fail()) { return nullptr; }
  void * p = malloc(size ? size : 1u);
  if (p) { f->outstanding++; }
  return p;
}
void * f_calloc(void * ctx, size_t n, size_t size) {
  Failing * f = (Failing *)ctx;
  if (f->should_fail()) { return nullptr; }
  if (n != 0 && size > (size_t)-1 / n) { return nullptr; } // overflow is failure
  void * p = calloc(n ? n : 1u, size ? size : 1u);
  if (p) { f->outstanding++; }
  return p;
}
void * f_realloc(void * ctx, void * ptr, size_t size) {
  Failing * f = (Failing *)ctx;
  if (f->should_fail()) { return nullptr; }
  void * p = realloc(ptr, size ? size : 1u);
  if (p && !ptr) { f->outstanding++; }
  return p;
}
void f_free(void * ctx, void * ptr) {
  Failing * f = (Failing *)ctx;
  if (ptr) {
    f->outstanding--;
    free(ptr);
  }
}

void init(Failing & f) {
  f.a.ctx = &f;
  f.a.malloc_fn = f_malloc;
  f.a.calloc_fn = f_calloc;
  f.a.realloc_fn = f_realloc;
  f.a.free_fn = f_free;
}

bool read_file(const char * dir, const char * name, std::vector<uint8_t> & out) {
  std::string path = std::string(dir) + "/" + name;
  FILE * fp = fopen(path.c_str(), "rb");
  if (!fp) { return false; }
  fseek(fp, 0, SEEK_END);
  long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  out.resize((size_t)n);
  const bool ok = n >= 0 && fread(out.data(), 1u, (size_t)n, fp) == (size_t)n;
  fclose(fp);
  return ok;
}

/** Load `bytes` with the named codec's allocator swapped for `f`. */
GIMG_Result load_with(const char * codec_name, const std::vector<uint8_t> & bytes,
    Failing & f) {
  GIMG_Codec * codec = gimg_codec_by_name(codec_name);
  if (!codec) { return GIMG_ERR_UNSUPPORTED; }
  const GIMG_Allocator * saved = codec->allocator;
  codec->allocator = &f.a;

  GIMG_Stream * in = nullptr;
  // The stream keeps the default allocator: only the codec's own allocations
  // are under test, and a stream that failed to build would not reach one.
  GIMG_Result r = gimg_stream_create_memory(bytes.data(), bytes.size(), &in);
  if (r == GIMG_OK) {
    GIMG_Doc * doc = nullptr;
    r = gimg_doc_load(in, nullptr, nullptr, &doc);
    if (r == GIMG_OK && doc) {
      gimg_doc_destroy(doc);
    }
    else if (doc) {
      ADD_FAILURE() << "load failed but still set *out_doc";
      gimg_doc_destroy(doc);
    }
    gimg_stream_destroy(in);
  }
  codec->allocator = saved;
  return r;
}

struct Case {
  const char * codec;
  const char * dir;
  const char * file;
};

} // namespace

/**
 * Every allocation failure during a load leaves nothing behind.
 *
 * The sweep is bounded by a clean run's allocation count, so it grows with
 * the loader rather than being a number someone picked.
 */
TEST(AllocFailure, EveryFailedLoadFreesEverythingItTook) {
  const Case cases[] = {
      {"jpeg", GIMG_TEST_DATA_JPEG, "plain_gray.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "progressive_sample.jpg"},
      {"png", GIMG_TEST_DATA_PNG, "png_exif.png"},
  };
  for (const Case & c : cases) {
    std::vector<uint8_t> bytes;
    if (!read_file(c.dir, c.file, bytes)) {
      ADD_FAILURE() << "missing fixture " << c.file;
      continue;
    }

    // A clean run first: it must succeed, and it says how far to sweep.
    Failing probe;
    init(probe);
    ASSERT_EQ(load_with(c.codec, bytes, probe), GIMG_OK)
        << c.file << " must load when nothing fails";
    ASSERT_EQ(probe.outstanding, 0)
        << c.file << " leaks on the success path: " << probe.outstanding
        << " blocks";
    const long total = probe.attempts;
    std::printf("  %-24s %ld allocations through the codec allocator\n",
        c.file, total);
    ASSERT_GT(total, 0) << c.file << " made no allocations through the codec "
                           "allocator, so this sweep would test nothing";

    for (long n = 1; n <= total; n++) {
      Failing f;
      init(f);
      f.fail_at = n;
      const GIMG_Result r = load_with(c.codec, bytes, f);
      EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_OOM || r == GIMG_ERR_CORRUPT ||
          r == GIMG_ERR_FORMAT || r == GIMG_ERR_LIMIT ||
          r == GIMG_ERR_UNSUPPORTED)
          << c.file << ": allocation " << n << " of " << total
          << " failed and the load returned " << (int)r;
      EXPECT_EQ(f.outstanding, 0)
          << c.file << ": " << f.outstanding
          << " block(s) leaked when allocation " << n << " of " << total
          << " failed";
      if (f.outstanding != 0) {
        break; // One report per fixture is enough to act on.
      }
    }
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
