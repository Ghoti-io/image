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
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <functional>
#include <string>
#include <vector>

#include "../failing_allocator.h"
#include "../save_cases.h"
#include "../../src/codec/codec_internal.h"
#include "../../src/meta/exif_internal.h"
#include "../exif_test_utils.h"

namespace {

using gimg_test::Failing;
using gimg_test::init;
using gimg_test::make_raster;
using gimg_test::SaveCase;

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

/**
 * Load `bytes` and decode its first item, with the codec's allocator swapped
 * for `f` across both.
 *
 * One allocator for the whole thing rather than one swapped in for the decode:
 * the doc and its codec_private are built during the load, and freeing them
 * through a different allocator than built them is a bug in the test rather
 * than a test of the library. @p out_after_load receives the allocation count
 * at the moment the load finished, which is what lets the sweep inject into
 * the decode alone.
 */
GIMG_Result load_and_decode_with(const char * codec_name,
    const std::vector<uint8_t> & bytes, Failing & f, long * out_after_load) {
  GIMG_Codec * codec = gimg_codec_by_name(codec_name);
  if (!codec) { return GIMG_ERR_UNSUPPORTED; }
  const GIMG_Allocator * saved = codec->allocator;
  codec->allocator = &f.a;

  GIMG_Stream * in = nullptr;
  GIMG_Result r = gimg_stream_create_memory(bytes.data(), bytes.size(), &in);
  if (r == GIMG_OK) {
    GIMG_Doc * doc = nullptr;
    r = gimg_doc_load(in, nullptr, nullptr, &doc);
    if (out_after_load) { *out_after_load = f.attempts; }
    if (r == GIMG_OK && doc) {
      GIMG_Item * item = gimg_doc_item(doc, 0);
      GIMG_Raster * raster = nullptr;
      r = item ? gimg_item_decode(item, nullptr, &raster) : GIMG_ERR_INTERNAL;
      if (raster) { gimg_raster_destroy(raster); }
    }
    if (doc) { gimg_doc_destroy(doc); }
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
      // Each of these reaches a segment the two above do not, and so a
      // different set of allocations and of arms that free them: arithmetic
      // conditioning tables, restart intervals, a hierarchical sequence with
      // its per-frame state, and a lossless frame.  A sweep is only ever as
      // wide as the paths its fixtures walk.
      {"jpeg", GIMG_TEST_DATA_JPEG, "arith_rgb_64x64_420.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "libjpeg_restart_rgb.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "hier_gray_2level.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "hier_gray_lossless.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "baseline_gray12.jpg"},
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
/**
 * What the codecs do when an allocation fails partway through a **decode**.
 *
 * The sweep above stops at gimg_doc_load(), which parses the file and keeps
 * its segments. Most of what a codec allocates comes later: the coefficient
 * buffers, the per-component planes, the reference frame a hierarchical
 * sequence builds up. None of the arms that free those was on any sweep, so
 * the three `fail:` blocks in the hierarchical decoder - fifteen lines of
 * cleanup between them - had never run.
 *
 * Injection starts after the load has finished, so a failure lands in the
 * decode rather than re-testing what the load sweep already covers. The
 * assertion is the same one and it is the point: whichever allocation failed,
 * every block is handed back.
 */
TEST(AllocFailure, EveryFailedDecodeFreesEverythingItTook) {
  const Case cases[] = {
      // Each walks a decoder the others do not. The two hierarchical ones are
      // here for the per-frame planes and the reference frame they carry
      // between frames, which is the deepest partial state in the library.
      {"jpeg", GIMG_TEST_DATA_JPEG, "hier_gray_2level.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "hier_gray_lossless.jpg"},
      // A hierarchical sequence whose frames are progressive reaches a third
      // per-frame decoder, with its own partial state to unwind.
      {"jpeg", GIMG_TEST_DATA_JPEG, "hier_rgb_progressive.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "hier_noninterleaved_444.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "progressive_sample.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "arith_rgb_64x64_420.jpg"},
      {"jpeg", GIMG_TEST_DATA_JPEG, "baseline_gray12.jpg"},
      {"png", GIMG_TEST_DATA_PNG, "png_exif.png"},
  };
  for (const Case & c : cases) {
    std::vector<uint8_t> bytes;
    if (!read_file(c.dir, c.file, bytes)) {
      ADD_FAILURE() << "missing fixture " << c.file;
      continue;
    }

    // A clean run first: it must decode, and it says how far to sweep.
    Failing probe;
    init(probe);
    long after_load = 0;
    ASSERT_EQ(load_and_decode_with(c.codec, bytes, probe, &after_load), GIMG_OK)
        << c.file << " must decode when nothing fails";
    ASSERT_EQ(probe.outstanding, 0)
        << c.file << " leaks on the success path: " << probe.outstanding
        << " blocks";
    const long total = probe.attempts;
    std::printf("  %-24s %ld allocations, %ld of them after the load\n",
        c.file, total, total - after_load);
    std::fflush(stdout);
    ASSERT_GT(total, after_load)
        << c.file << " allocated nothing while decoding, so this sweep would "
                     "test nothing beyond the load sweep";

    for (long n = after_load + 1; n <= total; n++) {
      Failing f;
      init(f);
      f.fail_at = n;
      long ignored = 0;
      const GIMG_Result r =
          load_and_decode_with(c.codec, bytes, f, &ignored);
      EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_OOM || r == GIMG_ERR_CORRUPT ||
          r == GIMG_ERR_FORMAT || r == GIMG_ERR_LIMIT ||
          r == GIMG_ERR_UNSUPPORTED)
          << c.file << ": allocation " << n << " of " << total
          << " failed and the decode returned " << (int)r;
      EXPECT_EQ(f.outstanding, 0)
          << c.file << ": " << f.outstanding
          << " block(s) leaked when allocation " << n << " of " << total
          << " failed";
    }
  }
}

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

namespace {

/** Save @p doc through @p codec_name with that codec's allocator swapped. */
GIMG_Result save_with(const char * codec_name, const GIMG_Doc * doc,
    const GIMG_Save_Options * options, Failing & f, size_t * out_bytes) {
  GIMG_Codec * codec = gimg_codec_by_name(codec_name);
  if (!codec) { return GIMG_ERR_UNSUPPORTED; }
  const GIMG_Allocator * saved = codec->allocator;
  codec->allocator = &f.a;

  GIMG_Stream * out = nullptr;
  // The stream keeps the default allocator for the same reason the loader
  // sweep gives the input stream one: what is under test is the codec's
  // allocations, and a stream that failed to exist would not reach them.
  GIMG_Result r = gimg_stream_create_memory_output(&out);
  if (r == GIMG_OK) {
    GIMG_Save_Report report = {};
    r = gimg_doc_save(doc, out, codec_name, options, &report);
    if (out_bytes) { *out_bytes = report.bytes_written; }
    gimg_stream_destroy(out);
  }
  codec->allocator = saved;
  return r;
}

} // namespace

namespace {
/** Save through @p codec_name with its allocator swapped, keeping the bytes. */
GIMG_Result save_bytes(const char * codec_name, const GIMG_Doc * doc,
    const GIMG_Save_Options * options, Failing & f,
    std::vector<uint8_t> * out) {
  GIMG_Codec * codec = gimg_codec_by_name(codec_name);
  if (!codec) { return GIMG_ERR_UNSUPPORTED; }
  const GIMG_Allocator * saved = codec->allocator;
  codec->allocator = &f.a;
  GIMG_Stream * os = nullptr;
  GIMG_Result r = gimg_stream_create_memory_output(&os);
  if (r == GIMG_OK) {
    GIMG_Save_Report report = {};
    r = gimg_doc_save(doc, os, codec_name, options, &report);
    if (r == GIMG_OK && out) {
      const void * p = nullptr;
      size_t n = 0;
      gimg_stream_output_buffer(os, &p, &n);
      out->assign((const uint8_t *)p, (const uint8_t *)p + n);
    }
    gimg_stream_destroy(os);
  }
  codec->allocator = saved;
  return r;
}

/** Decode @p bytes; false if it will not load or will not decode. */
bool decode_pixels(const std::vector<uint8_t> & bytes,
    std::vector<uint8_t> & px, uint32_t * w, uint32_t * h) {
  GIMG_Stream * is = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &is) != GIMG_OK) {
    return false;
  }
  GIMG_Doc * d = nullptr;
  if (gimg_doc_load(is, nullptr, nullptr, &d) != GIMG_OK) {
    gimg_stream_destroy(is);
    return false;
  }
  GIMG_Raster * r = nullptr;
  const bool ok =
      gimg_item_decode(gimg_doc_item(d, 0), nullptr, &r) == GIMG_OK && r;
  if (ok) {
    *w = gimg_raster_width(r);
    *h = gimg_raster_height(r);
    const auto * q = (const unsigned char *)gimg_raster_pixels_const(r);
    px.assign(q, q + gimg_raster_stride_bytes(r) * (*h));
    gimg_raster_destroy(r);
  }
  gimg_doc_destroy(d);
  gimg_stream_destroy(is);
  return ok;
}
} // namespace

/**
 * A save that returns GIMG_OK wrote a file that is the same picture.
 *
 * The sweep below allows a save to return GIMG_OK even though an allocation
 * failed, and checks only that nothing leaked.  That left the interesting
 * half unasked: whether the file it did write is any good.  It was not.
 * jpeg_bit_writer grew its buffer through bit_writer_put_byte, which returned
 * silently when the growth failed, and bit_writer_put_bits has no return value
 * at all, so the bytes simply went missing from the entropy-coded segment and
 * every caller finished with an unconditional GIMG_OK.  Seventeen of the JPEG
 * configurations here could be made to write a corrupt scan and call it
 * success; libjpeg reads the smaller ones as "extraneous bytes before marker
 * 0xd9" and the progressive ones as "premature end of data segment".
 *
 * The arithmetic coder never had the bug, because its sink carries an oom flag
 * that its callers check - which is why the arithmetic rows were the clean
 * ones while every Huffman row failed.
 *
 * What is asserted is the picture, not the bytes.  A writer is allowed to
 * react to a failed allocation by writing a different file: PNG drops back to
 * a cheaper compression strategy and emits a much larger one, and a codec may
 * leave out metadata it could not build a buffer for.  What it may not do is
 * claim success and hand back something that decodes differently, or at all.
 */
TEST(AllocFailure, ASaveThatSucceedsWroteThePictureItWasGiven) {
  const std::vector<SaveCase> cases = gimg_test::save_cases();
  long checked = 0, tolerated = 0;
  for (const SaveCase & c : cases) {
    SCOPED_TRACE(c.name);
    GIMG_Raster * raster = make_raster(*c.format, 32u, 32u, c.levels);
    ASSERT_NE(raster, nullptr) << c.name << ": could not build a raster";
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK) << c.name;
    gimg_raster_destroy(raster);
    if (c.decorate) { c.decorate(doc); }

    Failing probe;
    init(probe);
    std::vector<uint8_t> clean;
    ASSERT_EQ(save_bytes(c.codec, doc, &c.options, probe, &clean), GIMG_OK)
        << c.name << " must save when nothing fails";
    const long total = probe.attempts;
    ASSERT_GT(total, 0) << c.name << " allocated nothing; nothing to sweep";
    std::vector<uint8_t> want;
    uint32_t ww = 0, wh = 0;
    ASSERT_TRUE(decode_pixels(clean, want, &ww, &wh))
        << c.name << ": the clean save does not decode, so this sweep would "
                     "compare nothing";

    for (long n = 1; n <= total; n++) {
      Failing f;
      init(f);
      f.fail_at = n;
      std::vector<uint8_t> got;
      if (save_bytes(c.codec, doc, &c.options, f, &got) != GIMG_OK) {
        continue; // Refusing is always allowed.
      }
      checked++;
      if (got != clean) { tolerated++; }
      std::vector<uint8_t> px;
      uint32_t gw = 0, gh = 0;
      ASSERT_TRUE(decode_pixels(got, px, &gw, &gh))
          << c.name << ": allocation " << n << " of " << total
          << " failed, the save returned OK, and the file it wrote does not "
             "decode";
      EXPECT_EQ(gw, ww) << c.name << ": allocation " << n << " changed width";
      EXPECT_EQ(gh, wh) << c.name << ": allocation " << n << " changed height";
      EXPECT_TRUE(px == want)
          << c.name << ": allocation " << n << " of " << total
          << " failed, the save returned OK, and the picture came back "
             "different";
      if (px != want) { break; }
    }
    gimg_doc_destroy(doc);
  }
  std::printf("  %ld successful saves checked, %ld wrote different bytes for "
              "the same picture\n",
      checked, tolerated);
  ASSERT_GT(checked, 0);
}

/**
 * Every allocation failure during a save leaves nothing behind.
 *
 * The mirror of the load sweep above, and the reason it is worth having
 * separately is that the writers allocate for different things: a scan buffer
 * per component, a Huffman code table, a colour-converted copy of the raster,
 * a synthesized ICC profile, an Exif blob with a thumbnail in it.  None of
 * those arms is reachable from any document, however malformed, because a
 * document is either saveable or it is not and neither makes malloc fail.
 *
 * The configurations come from tests/save_cases.h, shared with the
 * write-failure sweep, so a writer option added there widens both.
 */
TEST(AllocFailure, EveryFailedSaveFreesEverythingItTook) {
  const std::vector<SaveCase> cases = gimg_test::save_cases();

  long injected = 0;
  for (const SaveCase & c : cases) {
    GIMG_Raster * raster = make_raster(*c.format, 32u, 32u, c.levels);
    ASSERT_NE(raster, nullptr) << c.name << ": could not build a raster";
    GIMG_Doc * doc = nullptr;
    // The document is built with the default allocator so that the sweep
    // counts what the *save* takes and nothing else.
    ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK) << c.name;
    gimg_raster_destroy(raster);
    if (c.decorate) { c.decorate(doc); }

    Failing probe;
    init(probe);
    size_t bytes = 0;
    const GIMG_Result clean = save_with(c.codec, doc, &c.options, probe, &bytes);
    ASSERT_EQ(clean, GIMG_OK)
        << c.name << " must save when nothing fails (returned " << (int)clean
        << ")";
    ASSERT_GT(bytes, 0u) << c.name << " wrote nothing";
    ASSERT_EQ(probe.outstanding, 0)
        << c.name << " leaks on the success path: " << probe.outstanding
        << " blocks";
    const long total = probe.attempts;
    std::printf("  %-26s %4ld allocations, %6zu bytes\n", c.name, total, bytes);
    ASSERT_GT(total, 0)
        << c.name << " made no allocations through the codec allocator, so "
                     "this sweep would test nothing";

    for (long n = 1; n <= total; n++) {
      Failing f;
      init(f);
      f.fail_at = n;
      const GIMG_Result r = save_with(c.codec, doc, &c.options, f, nullptr);
      injected++;
      EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_OOM)
          << c.name << ": allocation " << n << " of " << total
          << " failed and the save returned " << (int)r
          << "; an allocation failure is an OOM, not a claim about the image";
      EXPECT_EQ(f.outstanding, 0)
          << c.name << ": " << f.outstanding
          << " block(s) leaked when allocation " << n << " of " << total
          << " failed";
      if (f.outstanding != 0) { break; }
    }
    gimg_doc_destroy(doc);
  }
  std::printf("  %ld injected save failures\n", injected);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
