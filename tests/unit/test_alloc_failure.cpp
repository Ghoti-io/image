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
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/ops.h>
#include <gtest/gtest.h>
#include <dirent.h>
#include <algorithm>
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
  // Every fixture in the tree, for the same reason the decode sweep
  // enumerates: which segments a loader walks is decided by the file, and a
  // list of names samples an axis whose members each answer differently.  The
  // eight names this used to carry never reached a thumbnail, so the arms that
  // give one back when the document cannot take it had never run - and the
  // loader attaches thumbnails from three different places.
  const std::string root = std::string(GIMG_TEST_DATA_JPEG) + "/..";
  const std::string bmp_dir = root + "/bmp";
  const std::string gif_dir = root + "/gif";
  struct Dir {
    const char * codec;
    const char * path;
    const char * ext;
  } dirs[] = {
      {"jpeg", GIMG_TEST_DATA_JPEG, ".jpg"},
      {"png", GIMG_TEST_DATA_PNG, ".png"},
      {"bmp", bmp_dir.c_str(), ".bmp"},
      {"gif", gif_dir.c_str(), ".gif"},
  };

  long swept = 0, skipped = 0, injections = 0;
  std::vector<std::string> seen;
  for (const Dir & d : dirs) {
    DIR * dp = opendir(d.path);
    if (!dp) {
      ADD_FAILURE() << "cannot read fixture directory " << d.path;
      continue;
    }
    std::vector<std::string> names;
    while (struct dirent * e = readdir(dp)) {
      const std::string n = e->d_name;
      if (n.size() > strlen(d.ext) &&
          n.compare(n.size() - strlen(d.ext), strlen(d.ext), d.ext) == 0) {
        names.push_back(n);
      }
    }
    closedir(dp);
    std::sort(names.begin(), names.end());

    for (const std::string & name : names) {
      std::vector<uint8_t> bytes;
      if (!read_file(d.path, name.c_str(), bytes)) { continue; }

      // A clean run first: it says how far to sweep, and a fixture that does
      // not load on its own is not a subject - plenty here are deliberately
      // malformed.
      Failing probe;
      init(probe);
      if (load_with(d.codec, bytes, probe) != GIMG_OK) {
        skipped++;
        continue;
      }
      ASSERT_EQ(probe.outstanding, 0)
          << name << " leaks on the success path: " << probe.outstanding
          << " blocks";
      const long total = probe.attempts;
      if (total == 0) { skipped++; continue; }
      swept++;
      seen.push_back(name);

      for (long n = 1; n <= total; n++) {
        Failing f;
        init(f);
        f.fail_at = n;
        const GIMG_Result r = load_with(d.codec, bytes, f);
        injections++;
        EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_OOM ||
            r == GIMG_ERR_CORRUPT || r == GIMG_ERR_FORMAT ||
            r == GIMG_ERR_LIMIT || r == GIMG_ERR_UNSUPPORTED)
            << name << ": allocation " << n << " of " << total
            << " failed and the load returned " << (int)r;
        EXPECT_EQ(f.outstanding, 0)
            << name << ": " << f.outstanding
            << " block(s) leaked when allocation " << n << " of " << total
            << " failed";
        if (f.outstanding != 0) {
          break; // One report per fixture is enough to act on.
        }
      }
    }
  }

  std::printf("  %ld fixtures swept, %ld skipped, %ld injected loads\n",
      swept, skipped, injections);
  ASSERT_GT(swept, 50)
      << "only " << swept << " fixtures loaded cleanly - a sweep this narrow "
                             "is not measuring what it claims to";

  // The segments that are easiest to lose.  Each carries state the others do
  // not - arithmetic conditioning tables, a restart interval, a hierarchical
  // sequence's per-frame state, a lossless frame, an Exif thumbnail, a JFXX
  // one - so if a rename takes one out of the tree this sweep says so rather
  // than quietly narrowing.
  const char * required[] = {"plain_gray.jpg", "progressive_sample.jpg",
      "arith_rgb_64x64_420.jpg", "libjpeg_restart_rgb.jpg",
      "hier_gray_2level.jpg", "hier_gray_lossless.jpg", "baseline_gray12.jpg",
      "png_exif.png"};
  for (const char * want : required) {
    EXPECT_NE(std::find(seen.begin(), seen.end(), std::string(want)),
        seen.end())
        << want << " is no longer among the fixtures this sweep loads";
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
  // Every fixture in the tree, not a chosen few. Which decoder a file reaches
  // is decided by the file - precision, scan count, component count, entropy
  // coder - so a list of names is a sample of an axis whose members each
  // answer differently, and the earlier list of six reached one of the
  // hierarchical decoder's three cleanup blocks. Enumerating is cheap here:
  // the sweep is bounded by what a clean decode allocates, which is tens of
  // calls, not by the size of the picture.
  // bmp and gif have no define of their own; they are siblings of the two
  // that do, which is how the conversion sweep reaches them as well.
  const std::string root = std::string(GIMG_TEST_DATA_JPEG) + "/..";
  const std::string bmp_dir = root + "/bmp";
  const std::string gif_dir = root + "/gif";
  struct Dir {
    const char * codec;
    const char * path;
    const char * ext;
  } dirs[] = {
      {"jpeg", GIMG_TEST_DATA_JPEG, ".jpg"},
      {"png", GIMG_TEST_DATA_PNG, ".png"},
      {"bmp", bmp_dir.c_str(), ".bmp"},
      {"gif", gif_dir.c_str(), ".gif"},
  };

  long swept = 0, skipped = 0, injections = 0;
  std::vector<std::string> seen;
  for (const Dir & d : dirs) {
    DIR * dp = opendir(d.path);
    if (!dp) {
      ADD_FAILURE() << "cannot read fixture directory " << d.path;
      continue;
    }
    std::vector<std::string> names;
    while (struct dirent * e = readdir(dp)) {
      const std::string n = e->d_name;
      if (n.size() > strlen(d.ext) &&
          n.compare(n.size() - strlen(d.ext), strlen(d.ext), d.ext) == 0) {
        names.push_back(n);
      }
    }
    closedir(dp);
    std::sort(names.begin(), names.end());

    for (const std::string & name : names) {
      std::vector<uint8_t> bytes;
      if (!read_file(d.path, name.c_str(), bytes)) { continue; }

      // A clean run first: it says how far to sweep, and a fixture that does
      // not decode on its own is not a subject - plenty here are deliberately
      // malformed.
      Failing probe;
      init(probe);
      long after_load = 0;
      if (load_and_decode_with(d.codec, bytes, probe, &after_load) != GIMG_OK) {
        skipped++;
        continue;
      }
      ASSERT_EQ(probe.outstanding, 0)
          << name << " leaks on the success path: " << probe.outstanding
          << " blocks";
      const long total = probe.attempts;
      if (total <= after_load) { skipped++; continue; }
      swept++;
      seen.push_back(name);

      for (long n = after_load + 1; n <= total; n++) {
        Failing f;
        init(f);
        f.fail_at = n;
        long ignored = 0;
        const GIMG_Result r =
            load_and_decode_with(d.codec, bytes, f, &ignored);
        injections++;
        EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_OOM ||
            r == GIMG_ERR_CORRUPT || r == GIMG_ERR_FORMAT ||
            r == GIMG_ERR_LIMIT || r == GIMG_ERR_UNSUPPORTED)
            << name << ": allocation " << n << " of " << total
            << " failed and the decode returned " << (int)r;
        EXPECT_EQ(f.outstanding, 0)
            << name << ": " << f.outstanding
            << " block(s) leaked when allocation " << n << " of " << total
            << " failed";
      }
    }
  }

  std::printf("  %ld fixtures swept, %ld skipped, %ld injected decodes\n",
      swept, skipped, injections);
  ASSERT_GT(swept, 50)
      << "only " << swept << " fixtures decoded cleanly - a sweep this narrow "
                             "is not measuring what it claims to";

  // The decoders that are easiest to lose. Each reaches machinery none of the
  // others does, so if a rename or a move takes one out of the tree this
  // sweep should say so rather than quietly narrowing.
  const char * required[] = {"hier_gray_2level.jpg", "hier_gray_lossless.jpg",
      "hier_rgb_progressive.jpg", "hier_noninterleaved_444.jpg",
      "progressive_sample.jpg", "arith_rgb_64x64_420.jpg",
      "baseline_gray12.jpg", "cmyk_ljt_sub.jpg"};
  for (const char * want : required) {
    EXPECT_NE(std::find(seen.begin(), seen.end(), std::string(want)),
        seen.end())
        << want << " is no longer among the fixtures this sweep decodes";
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
    GIMG_Raster * raster = make_raster(*c.format,
        c.min_side ? c.min_side : 32u, c.min_side ? c.min_side : 32u,
        c.levels, c.run);
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
    GIMG_Raster * raster = make_raster(*c.format,
        c.min_side ? c.min_side : 32u, c.min_side ? c.min_side : 32u,
        c.levels, c.run);
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

namespace {

/**
 * One operation, driven entirely through a raster built on the test's own
 * allocator.
 *
 * The ops layer takes no allocator argument: every buffer it makes comes from
 * gimg_raster_allocator() of whatever it was given. So the source raster is
 * what carries the failing allocator in, and the sweep has to rebuild it for
 * every injection - which is also why `setup` and `run` are separate. The
 * count after `setup` is the first allocation the operation itself makes, and
 * injecting below that would only be testing gimg_raster_create.
 */
struct OpCase {
  const char * name;
  /** Build the inputs. Returns false if the shape could not be made. */
  bool (*setup)(const GIMG_Allocator * a, GIMG_Raster ** src,
      GIMG_Raster ** aux);
  /** Do the work and hand back everything it produced. */
  GIMG_Result (*run)(GIMG_Raster * src, GIMG_Raster * aux);
  /**
   * True for an operation that works in the buffers it was given.
   *
   * Compositing is the one here: it writes into the destination and takes
   * nothing. Such a case is kept in the list rather than left out, so that
   * the day it starts allocating this sweep picks it up instead of silently
   * never having covered it - the printed count is what would move.
   */
  bool allocates_nothing = false;
};

GIMG_Raster * op_raster(const GIMG_Allocator * a, uint32_t w, uint32_t h,
    const GIMG_Pixel_Format & fmt) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create_with_allocator(
          a, w, h, &fmt, GIMG_RASTER_OWNED, nullptr, 0, &r) != GIMG_OK) {
    return nullptr;
  }
  unsigned char * px = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  const unsigned ch = fmt.channel_count;
  const unsigned bits = fmt.bits_per_channel[0];
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      for (unsigned c = 0; c < ch; c++) {
        // Enough distinct colours that a quantizer has boxes to split, and a
        // gradient rather than noise so a resampler's taps differ.
        const unsigned v = (x * 11u + y * 7u + c * 23u) & 0xFFu;
        if (bits <= 8u) { px[y * stride + (x * ch + c)] = (unsigned char)v; }
        else {
          ((uint16_t *)(px + y * stride))[x * ch + c] =
              (uint16_t)(v * ((1u << bits) - 1u) / 255u);
        }
      }
    }
  }
  return r;
}

bool setup_rgba(const GIMG_Allocator * a, GIMG_Raster ** src,
    GIMG_Raster ** aux) {
  *aux = nullptr;
  *src = op_raster(a, 12, 9, GIMG_PIXEL_RGBA8);
  return *src != nullptr;
}
bool setup_gray(const GIMG_Allocator * a, GIMG_Raster ** src,
    GIMG_Raster ** aux) {
  *aux = nullptr;
  *src = op_raster(a, 12, 9, GIMG_PIXEL_GRAY8);
  return *src != nullptr;
}
bool setup_rgba16(const GIMG_Allocator * a, GIMG_Raster ** src,
    GIMG_Raster ** aux) {
  *aux = nullptr;
  *src = op_raster(a, 10, 7, GIMG_PIXEL_RGBA16);
  return *src != nullptr;
}
bool setup_cmyk(const GIMG_Allocator * a, GIMG_Raster ** src,
    GIMG_Raster ** aux) {
  *aux = nullptr;
  *src = op_raster(a, 10, 7, GIMG_PIXEL_CMYK8);
  if (!*src) { return false; }
  // The CMYK -> RGB conversion refuses a raster that does not say which way
  // round its samples are, so the polarity is part of building this input
  // rather than part of what is being swept.
  GIMG_Color_Info info;
  gimg_color_info_default(&info);
  info.cmyk_polarity = GIMG_CMYK_POLARITY_INK;
  if (gimg_raster_set_color_info(*src, &info) != GIMG_OK) {
    gimg_raster_destroy(*src);
    *src = nullptr;
    return false;
  }
  return true;
}
bool setup_pair(const GIMG_Allocator * a, GIMG_Raster ** src,
    GIMG_Raster ** aux) {
  *src = op_raster(a, 12, 9, GIMG_PIXEL_RGBA8);
  *aux = op_raster(a, 5, 4, GIMG_PIXEL_RGBA8);
  return *src && *aux;
}

GIMG_Result resize_with(GIMG_Raster * src, GIMG_Resample_Filter filter,
    GIMG_Resample_Space space, uint32_t w, uint32_t h) {
  GIMG_Resize_Options o;
  gimg_resize_options_default(&o);
  o.filter = filter;
  o.space = space;
  GIMG_Raster * out = nullptr;
  const GIMG_Result r = gimg_ops_resize(src, w, h, &o, &out);
  if (out) { gimg_raster_destroy(out); }
  return r;
}

GIMG_Result quantize_with(GIMG_Raster * src, uint16_t colors,
    GIMG_Dither dither) {
  GIMG_Quantize_Options q = {};
  q.max_colors = colors;
  q.dither = dither;
  GIMG_Raster * out = nullptr;
  GIMG_Palette pal = {};
  const GIMG_Result r = gimg_ops_quantize(src, &q, &out, &pal);
  if (out) { gimg_raster_destroy(out); }
  return r;
}

const OpCase & op_cases_at(size_t i);
size_t op_case_count(void);

const OpCase kOps[] = {
    {"resize up, auto", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(s, GIMG_FILTER_AUTO, GIMG_RESAMPLE_SPACE_ENCODED,
              25, 19);
        }},
    {"resize down, box", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(
              s, GIMG_FILTER_BOX, GIMG_RESAMPLE_SPACE_ENCODED, 4, 3);
        }},
    {"resize, lanczos3", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(
              s, GIMG_FILTER_LANCZOS3, GIMG_RESAMPLE_SPACE_ENCODED, 7, 20);
        }},
    {"resize, nearest", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(
              s, GIMG_FILTER_NEAREST, GIMG_RESAMPLE_SPACE_ENCODED, 5, 5);
        }},
    {"resize, linear light", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(s, GIMG_FILTER_TRIANGLE,
              GIMG_RESAMPLE_SPACE_LINEAR, 6, 6);
        }},
    {"resize 16-bit", setup_rgba16,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(s, GIMG_FILTER_CATMULL_ROM,
              GIMG_RESAMPLE_SPACE_ENCODED, 20, 14);
        }},
    {"resize cmyk", setup_cmyk,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(
              s, GIMG_FILTER_TRIANGLE, GIMG_RESAMPLE_SPACE_ENCODED, 5, 4);
        }},
    {"resize gray", setup_gray,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return resize_with(
              s, GIMG_FILTER_TRIANGLE, GIMG_RESAMPLE_SPACE_ENCODED, 30, 4);
        }},
    {"crop", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          GIMG_Raster * out = nullptr;
          const GIMG_Result r = gimg_ops_crop(s, 2, 3, 6, 5, &out);
          if (out) { gimg_raster_destroy(out); }
          return r;
        }},
    {"convert cmyk to rgb", setup_cmyk,
        [](GIMG_Raster * s, GIMG_Raster *) {
          GIMG_Raster * out = nullptr;
          const GIMG_Result r =
              gimg_ops_convert_pixel_format(s, &GIMG_PIXEL_RGBA8, &out);
          if (out) { gimg_raster_destroy(out); }
          return r;
        }},
    {"widen to 16 bits", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          GIMG_Raster * out = nullptr;
          const GIMG_Result r = gimg_ops_convert_bit_depth(s, 16, &out);
          if (out) { gimg_raster_destroy(out); }
          return r;
        }},
    {"quantize 256 colours", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return quantize_with(s, 0u, GIMG_DITHER_NONE);
        }},
    {"quantize to 8", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return quantize_with(s, 8u, GIMG_DITHER_NONE);
        }},
    {"quantize dithered", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return quantize_with(s, 8u, GIMG_DITHER_FLOYD_STEINBERG);
        }},
    {"quantize gray", setup_gray,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return quantize_with(s, 4u, GIMG_DITHER_FLOYD_STEINBERG);
        }},
    {"exact palette", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          GIMG_Palette pal = {};
          return gimg_ops_palette_from_raster(s, 0u, &pal);
        }},
    {"count colours", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          size_t n = 0;
          bool exact = false;
          return gimg_ops_count_colors(s, 0u, &n, &exact);
        }},
    {"one palette for two rasters", setup_pair,
        [](GIMG_Raster * s, GIMG_Raster * aux) {
          const GIMG_Raster * pair[2] = {s, aux};
          GIMG_Quantize_Options q = {};
          q.max_colors = 16u;
          GIMG_Palette pal = {};
          return gimg_ops_palette_build(pair, 2u, &q, &pal);
        }},
    {"apply a palette", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          GIMG_Palette pal = {};
          pal.count = 2u;
          pal.entries[0][3] = 255u;
          pal.entries[1][0] = 255u;
          pal.entries[1][3] = 255u;
          GIMG_Raster * out = nullptr;
          const GIMG_Result r = gimg_ops_palette_apply(
              s, &pal, GIMG_DITHER_FLOYD_STEINBERG, &out);
          if (out) { gimg_raster_destroy(out); }
          return r;
        }},
    {"quarter turn", setup_rgba,
        [](GIMG_Raster * s, GIMG_Raster *) {
          return gimg_ops_rotate_90_cw(s);
        }},
    {"composite over", setup_pair,
        [](GIMG_Raster * s, GIMG_Raster * aux) {
          return gimg_ops_composite(s, aux, 3, 2, GIMG_COMPOSITE_OVER);
        },
        true},
};

size_t op_case_count(void) { return sizeof(kOps) / sizeof(kOps[0]); }
const OpCase & op_cases_at(size_t i) { return kOps[i]; }

} // namespace

/**
 * Every allocation failure inside an operation leaves nothing behind.
 *
 * The three sweeps above cover the codecs; the ops layer had nothing like
 * them, and its OOM arms - the resampler's coefficient tables, the
 * quantizer's boxes and histogram, the intermediate raster a two-pass resize
 * makes - were reached by no test. They are the same shape of arm and fail
 * the same way: a partial structure and an early return.
 */
TEST(AllocFailure, EveryFailedOpFreesEverythingItTook) {
  long injected = 0, silent = 0;
  for (size_t i = 0; i < op_case_count(); i++) {
    const OpCase & c = op_cases_at(i);

    Failing probe;
    init(probe);
    GIMG_Raster * src = nullptr;
    GIMG_Raster * aux = nullptr;
    ASSERT_TRUE(c.setup(&probe.a, &src, &aux))
        << c.name << ": could not build the inputs";
    const long after_setup = probe.attempts;
    const GIMG_Result clean = c.run(src, aux);
    const long total = probe.attempts;
    if (aux) { gimg_raster_destroy(aux); }
    gimg_raster_destroy(src);
    ASSERT_EQ(clean, GIMG_OK)
        << c.name << " must succeed when nothing fails (returned "
        << (int)clean << ")";
    ASSERT_EQ(probe.outstanding, 0)
        << c.name << " leaks on the success path: " << probe.outstanding
        << " blocks";
    if (total == after_setup) {
      EXPECT_TRUE(c.allocates_nothing)
          << c.name << " made no allocation of its own, so this sweep tests "
                       "nothing for it";
      silent++;
      continue;
    }
    EXPECT_FALSE(c.allocates_nothing)
        << c.name << " is listed as allocating nothing and allocated "
        << (total - after_setup) << " times; sweep it properly";

    for (long n = after_setup + 1; n <= total; n++) {
      Failing f;
      init(f);
      GIMG_Raster * s2 = nullptr;
      GIMG_Raster * a2 = nullptr;
      ASSERT_TRUE(c.setup(&f.a, &s2, &a2)) << c.name << ": rebuild failed";
      ASSERT_EQ(f.attempts, after_setup)
          << c.name << ": the inputs cost a different number of allocations "
                       "the second time, so n does not name the same call";
      f.fail_at = n;
      const GIMG_Result r = c.run(s2, a2);
      injected++;
      EXPECT_TRUE(r == GIMG_OK || r == GIMG_ERR_OOM)
          << c.name << ": allocation " << n << " of " << total
          << " failed and the operation returned " << (int)r
          << "; an allocation failure is an OOM, not a claim about the image";
      if (a2) { gimg_raster_destroy(a2); }
      gimg_raster_destroy(s2);
      EXPECT_EQ(f.outstanding, 0)
          << c.name << ": " << f.outstanding
          << " block(s) leaked when allocation " << n << " of " << total
          << " failed";
      if (f.outstanding != 0) { break; }
    }
  }
  std::printf("  %zu operations swept, %ld injected failures, "
              "%ld allocate nothing\n",
      op_case_count(), injected, silent);
  ASSERT_GT(injected, 40)
      << "only " << injected
      << " injections; the operations are not allocating enough for this to "
         "be measuring their cleanup";
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
