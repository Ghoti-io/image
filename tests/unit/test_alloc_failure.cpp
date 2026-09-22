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
#include <functional>
#include <string>
#include <vector>

#include "../../src/codec/codec_internal.h"
#include "../../src/meta/exif_internal.h"
#include "../exif_test_utils.h"

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

namespace {

/** One Exif call that allocates, named so a failure says which. */
struct ExifOp {
  const char * name;
  std::function<GIMG_Result(const GIMG_Allocator *, void **, size_t *)> run;
};

} // namespace

/**
 * The Exif entry points give back every block when an allocation fails.
 *
 * These are the other half of the same problem as the loader sweep above, and
 * they are reached differently: each takes an allocator as an argument, so no
 * codec has to be tampered with. What they share is that the arm returning
 * GIMG_ERR_OOM cannot be reached by any input at all - the file is either
 * well-formed or it is not, and neither makes malloc fail - so the failure has
 * to be injected or those arms stay dead forever.
 *
 * The assertion is the same one that matters: not that an error came back, but
 * that nothing was kept. A metadata policy that leaked on its error path would
 * leak once per saved image on a machine under memory pressure, which is
 * exactly when it can least afford to.
 *
 * All seven are clean, and it is worth being exact about why: each of them
 * allocates exactly **once**, so on the failing path there is nothing yet to
 * hand back. That is a structural property, not evidence of careful cleanup,
 * and the sweep's value is in holding it - the bound comes from a clean run's
 * own allocation count, so the day someone adds a second allocation the sweep
 * widens by itself and starts asking a question it could not ask before.
 * Checked rather than asserted: with a scratch buffer planted before the real
 * one and freed only on success, the count went from one to two on its own and
 * the sweep reported "1 block(s) leaked when allocation 2 of 2 failed".
 */
TEST(AllocFailure, EveryFailedExifCallFreesEverythingItTook) {
  const std::vector<uint8_t> gps = exif_test::make_exif_with_gps();
  std::vector<uint8_t> jpg;
  ASSERT_TRUE(read_file(GIMG_TEST_DATA_JPEG, "baseline_8x8_gray.jpg", jpg));

  // Two readers need a blob in the shape their writer produces, so the
  // writers build it first - with the default allocator, since what is under
  // test here is the reader.
  //
  // The TechNote-2 blob is hand-built rather than taken from the writer,
  // deliberately: that writer emits no JPEGTables field, so a blob from it
  // takes the reader's passthrough branch and never reaches the allocation in
  // the reassembly branch - which is the one with an OOM arm worth sweeping.
  std::vector<uint8_t> technote2, uncompressed;
  {
    size_t sos = 0;
    for (size_t i = 2; i + 1 < jpg.size(); i++) {
      if (jpg[i] == 0xFF && jpg[i + 1] == 0xDA) { sos = i; break; }
    }
    ASSERT_GT(sos, 2u) << "fixture has no SOS to split at";
    std::vector<uint8_t> tables(jpg.begin(), jpg.begin() + (long)sos);
    tables.push_back(0xFF);
    tables.push_back(0xD9);
    const std::vector<uint8_t> strip(jpg.begin() + (long)sos, jpg.end());
    technote2 = exif_test::make_exif_with_tiff_jpeg_thumbnail(tables, strip);

    const std::vector<uint8_t> pixels(4u * 2u, 0x5A);
    void * p = nullptr;
    size_t n = 0;
    ASSERT_EQ(gimg_exif_build_with_thumbnail_uncompressed(nullptr, nullptr, 0,
                  pixels.data(), pixels.size(), 4u, 2u, 1u, 8u, &p, &n),
        GIMG_OK);
    uncompressed.assign((uint8_t *)p, (uint8_t *)p + n);
    free(p);
  }

  const ExifOp ops[] = {
      {"strip_gps",
          [&](const GIMG_Allocator * a, void ** o, size_t * n) {
            return gimg_exif_strip_gps(a, gps.data(), gps.size(), o, n);
          }},
      {"normalize",
          [&](const GIMG_Allocator * a, void ** o, size_t * n) {
            return gimg_exif_normalize(a, gps.data(), gps.size(), o, n);
          }},
      {"build_with_thumbnail_jpeg",
          [&](const GIMG_Allocator * a, void ** o, size_t * n) {
            return gimg_exif_build_with_thumbnail_jpeg(
                a, gps.data(), gps.size(), jpg.data(), jpg.size(), o, n);
          }},
      {"build_with_thumbnail_tiff_jpeg",
          [&](const GIMG_Allocator * a, void ** o, size_t * n) {
            return gimg_exif_build_with_thumbnail_tiff_jpeg(
                a, gps.data(), gps.size(), jpg.data(), jpg.size(), o, n);
          }},
      {"build_with_thumbnail_uncompressed",
          [&](const GIMG_Allocator * a, void ** o, size_t * n) {
            static const std::vector<uint8_t> px(4u * 2u, 0x5A);
            return gimg_exif_build_with_thumbnail_uncompressed(a, gps.data(),
                gps.size(), px.data(), px.size(), 4u, 2u, 1u, 8u, o, n);
          }},
      {"embedded_thumbnail_tiff_jpeg",
          [&](const GIMG_Allocator * a, void ** o, size_t * n) {
            return gimg_exif_embedded_thumbnail_tiff_jpeg(
                a, technote2.data(), technote2.size(), o, n);
          }},
      {"embedded_thumbnail_uncompressed",
          [&](const GIMG_Allocator * a, void ** o, size_t * n) {
            uint32_t w = 0, h = 0;
            uint8_t bits = 0;
            uint16_t pm = 0;
            return gimg_exif_embedded_thumbnail_uncompressed(a,
                uncompressed.data(), uncompressed.size(), &w, &h, &bits, &pm,
                o, n);
          }},
  };

  for (const ExifOp & op : ops) {
    // A clean run says how far to sweep, and proves the call does allocate.
    Failing probe;
    init(probe);
    void * out_p = nullptr;
    size_t out_n = 0;
    ASSERT_EQ(op.run(&probe.a, &out_p, &out_n), GIMG_OK)
        << op.name << " must succeed when nothing fails";
    if (out_p) { probe.a.free_fn(probe.a.ctx, out_p); }
    ASSERT_EQ(probe.outstanding, 0)
        << op.name << " leaks on the success path";
    const long total = probe.attempts;
    std::printf("  %-34s %ld allocations\n", op.name, total);
    ASSERT_GT(total, 0)
        << op.name << " allocated nothing, so this sweep tests nothing";

    for (long n = 1; n <= total; n++) {
      Failing f;
      init(f);
      f.fail_at = n;
      void * p = nullptr;
      size_t sz = 0;
      const GIMG_Result r = op.run(&f.a, &p, &sz);
      if (r == GIMG_OK && p) { f.a.free_fn(f.a.ctx, p); }
      EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_OOM)
          << op.name << ": allocation " << n << " of " << total
          << " failed and it returned " << (int)r
          << "; an allocation failure is an OOM, not a claim about the data";
      EXPECT_EQ(f.outstanding, 0)
          << op.name << ": " << f.outstanding
          << " block(s) leaked when allocation " << n << " of " << total
          << " failed";
      if (f.outstanding != 0) { break; }
    }
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
