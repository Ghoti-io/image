/**
 * @file
 *
 * What the writers do when the sink runs out of room.
 *
 * Every writer here checks the result of every write and hands it back. None
 * of those checks runs against a memory output stream, which grows on demand
 * and never refuses, so they are unreachable from any document - and a writer
 * that dropped one would pass the whole suite. It would carry on past a
 * failed write and return GIMG_OK having produced half a file, which is the
 * worst shape a bug of this kind can take: the caller is told the save
 * worked.
 *
 * So the sink is made to fail. A stream that accepts N bytes and then refuses
 * everything (tests/budget_stream.h), with N swept from zero to the length of
 * a good file, puts the failure at every write boundary the writer has.
 *
 * Two things are asserted, and the first is the one that matters:
 *
 *   - **Every budget below the file's length produces an error.** Not "some
 *     do" and not "the return code is one of these" - a writer that needs
 *     more room than it has must say so, at every single point where it might
 *     run out. A swallowed write failure shows up here as a budget that
 *     returned GIMG_OK.
 *   - Nothing is leaked on the way out, which is the same contract the
 *     allocation sweep holds and is checked the same way.
 *
 * The configurations are shared with that sweep, in tests/save_cases.h.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <vector>

#include "../../src/codec/codec_internal.h"
#include "../budget_stream.h"
#include "../failing_allocator.h"
#include "../save_cases.h"

namespace {

using gimg_test::BudgetStream;
using gimg_test::Failing;
using gimg_test::init;
using gimg_test::make_raster;
using gimg_test::SaveCase;

/**
 * Save @p doc into @p stream, counting the codec's allocations so a failed
 * write can be checked for having handed everything back.
 */
GIMG_Result save_into(const char * codec_name, const GIMG_Doc * doc,
    const GIMG_Save_Options * options, GIMG_Stream * stream, Failing & f,
    size_t * out_reported) {
  GIMG_Codec * codec = gimg_codec_by_name(codec_name);
  if (!codec) { return GIMG_ERR_UNSUPPORTED; }
  const GIMG_Allocator * saved = codec->allocator;
  codec->allocator = &f.a;
  GIMG_Save_Report report = {};
  const GIMG_Result r = gimg_doc_save(doc, stream, codec_name, options, &report);
  if (out_reported) { *out_reported = report.bytes_written; }
  codec->allocator = saved;
  return r;
}

} // namespace

/**
 * A sink that runs out of room is noticed, wherever it runs out.
 *
 * The raster is 16x16 rather than the 32x32 the allocation sweep uses, for
 * one reason: this sweep is quadratic in the output length - one whole save
 * per byte of it - and 16x16 keeps every case's file in the hundreds of
 * bytes. The header segments, where nearly all the distinct write arms live,
 * are the same size either way.
 */
TEST(WriteFailure, EveryTruncationPointIsReported) {
  long budgets = 0;
  for (const SaveCase & c : gimg_test::save_cases()) {
    GIMG_Raster * raster = make_raster(*c.format,
        c.min_side ? c.min_side : 16u, c.min_side ? c.min_side : 16u,
        c.levels, c.run);
    ASSERT_NE(raster, nullptr) << c.name << ": could not build a raster";
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK) << c.name;
    gimg_raster_destroy(raster);
    if (c.decorate) { c.decorate(doc); }

    // How long a good file is, and so how far to sweep.  A budget of exactly
    // that must succeed: it is the control that says the harness is imposing
    // a budget rather than breaking every write.
    size_t length = 0;
    {
      Failing probe;
      init(probe);
      GIMG_Stream * out = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
      const GIMG_Result r =
          save_into(c.codec, doc, &c.options, out, probe, &length);
      const void * data = nullptr;
      size_t size = 0;
      gimg_stream_output_buffer(out, &data, &size);
      length = size;
      gimg_stream_destroy(out);
      ASSERT_EQ(r, GIMG_OK) << c.name << " must save into a stream with room";
      ASSERT_EQ(probe.outstanding, 0) << c.name << " leaks on the success path";
      ASSERT_GT(length, 0u) << c.name << " wrote nothing";
    }

    {
      BudgetStream exact(length);
      ASSERT_TRUE(exact.usable()) << c.name;
      Failing f;
      init(f);
      EXPECT_EQ(save_into(c.codec, doc, &c.options, exact.get(), f, nullptr),
          GIMG_OK)
          << c.name << ": a budget of exactly the file's " << length
          << " bytes must succeed, or this sweep is measuring a broken sink "
             "rather than a writer";
      EXPECT_EQ(exact.written(), length) << c.name;
      EXPECT_EQ(f.outstanding, 0) << c.name;
    }

    long refused = 0;
    long accepted = 0;
    for (size_t budget = 0; budget < length; budget++) {
      BudgetStream sink(budget);
      ASSERT_TRUE(sink.usable()) << c.name << " at budget " << budget;
      Failing f;
      init(f);
      const GIMG_Result r =
          save_into(c.codec, doc, &c.options, sink.get(), f, nullptr);
      budgets++;
      if (r == GIMG_OK) {
        accepted++;
        if (accepted == 1) {
          ADD_FAILURE()
              << c.name << ": a sink that took only " << budget << " of "
              << length
              << " bytes was reported as a successful save; a write failure "
                 "was swallowed somewhere above it";
        }
      }
      else {
        refused++;
      }
      EXPECT_EQ(f.outstanding, 0)
          << c.name << ": " << f.outstanding
          << " block(s) leaked when the sink gave out after " << budget
          << " of " << length << " bytes";
      if (f.outstanding != 0) { break; }
      EXPECT_LE(sink.written(), budget)
          << c.name << ": wrote past a sink that had only " << budget
          << " bytes of room";
    }
    std::printf("  %-26s %6zu bytes, %ld truncations, %ld swallowed\n", c.name,
        length, refused, accepted);
    gimg_doc_destroy(doc);
  }
  std::printf("  %ld budgets swept\n", budgets);
}

namespace {

bool read_whole(const std::string & path, std::vector<uint8_t> & out) {
  FILE * fp = fopen(path.c_str(), "rb");
  if (!fp) { return false; }
  fseek(fp, 0, SEEK_END);
  const long n = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  out.resize(n > 0 ? (size_t)n : 0u);
  const bool ok = n >= 0 && fread(out.data(), 1u, (size_t)n, fp) == (size_t)n;
  fclose(fp);
  return ok;
}

} // namespace

/**
 * The same sweep, over documents that came from a file rather than a raster.
 *
 * A document built from a bare raster has no codec state on it: no ancillary
 * chunks a PNG arrived with, no suggested palette, no tRNS the loader
 * derived, no APP segments a JPEG carried. Nearly every "write it back"
 * branch in the writers is behind exactly that state, so the sweep above -
 * which builds every one of its documents from a raster - reaches the
 * writers' own output and none of the preservation paths. In png_save.c
 * alone that was most of what coverage reported untested.
 *
 * Re-saving a loaded fixture is what puts that state on the path. The
 * assertion is the one that matters above: a budget below the file's length
 * has to produce an error, wherever the writer runs out.
 *
 * The budget sweep is dense over the first kilobyte and coarse after it. The
 * arms this is reaching are chunk and segment writers, which sit before the
 * pixel data in all four formats; sweeping every byte of a 200KB re-save
 * would spend almost all of it inside one compressed block.
 */
TEST(WriteFailure, EveryTruncationPointIsReportedWhenRewritingAFile) {
  const std::string root = std::string(GIMG_TEST_DATA_JPEG) + "/..";
  struct Dir {
    const char * codec;
    std::string path;
    const char * ext;
  } dirs[] = {
      {"jpeg", std::string(GIMG_TEST_DATA_JPEG), ".jpg"},
      {"png", std::string(GIMG_TEST_DATA_PNG), ".png"},
      {"bmp", root + "/bmp", ".bmp"},
      {"gif", root + "/gif", ".gif"},
  };

  long files = 0, skipped = 0, budgets = 0, swallowed = 0;
  for (const Dir & d : dirs) {
    GIMG_Codec * codec = gimg_codec_by_name(d.codec);
    ASSERT_NE(codec, nullptr) << d.codec;
    DIR * dp = opendir(d.path.c_str());
    ASSERT_NE(dp, nullptr) << "cannot read fixture directory " << d.path;
    std::vector<std::string> names;
    while (struct dirent * e = readdir(dp)) {
      const std::string n = e->d_name;
      const size_t el = strlen(d.ext);
      if (n.size() > el && n.compare(n.size() - el, el, d.ext) == 0) {
        names.push_back(n);
      }
    }
    closedir(dp);
    std::sort(names.begin(), names.end());

    for (const std::string & name : names) {
      std::vector<uint8_t> bytes;
      if (!read_whole(d.path + "/" + name, bytes) || bytes.empty()) {
        continue;
      }

      // The allocator is swapped in for the whole life of the document, not
      // for the saves alone. A document carries state its loader built - the
      // ancillary chunks, the palettes, the decoded canvas a GIF frame needs
      // - and freeing that through a different allocator than took it is a
      // mistake in the harness, not a finding about the writer. Swapping it
      // for the save only reported seven GIF fixtures as leaking a frame
      // buffer each, and none of them does.
      Failing f;
      init(f);
      const GIMG_Allocator * saved = codec->allocator;
      codec->allocator = &f.a;

      GIMG_Stream * in = nullptr;
      GIMG_Doc * doc = nullptr;
      GIMG_Result loaded = GIMG_ERR_INTERNAL;
      if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) ==
          GIMG_OK) {
        loaded = gimg_doc_load(in, nullptr, nullptr, &doc);
        gimg_stream_destroy(in);
      }
      if (loaded != GIMG_OK || !doc) {
        if (doc) { gimg_doc_destroy(doc); }
        codec->allocator = saved;
        skipped++;
        continue;
      }

      GIMG_Save_Options opts = {};
      opts.quality = 80;
      GIMG_Save_Report report = {};
      size_t length = 0;
      GIMG_Stream * out = nullptr;
      GIMG_Result first = GIMG_ERR_INTERNAL;
      if (gimg_stream_create_memory_output(&out) == GIMG_OK) {
        first = gimg_doc_save(doc, out, d.codec, &opts, &report);
        const void * data = nullptr;
        size_t size = 0;
        gimg_stream_output_buffer(out, &data, &size);
        length = size;
        gimg_stream_destroy(out);
      }
      // Plenty of fixtures load into something their own writer will not take
      // back - a twelve-bit frame asked for at eight, a colour model the
      // writer refuses. That is a refusal, not a write failure.
      if (first != GIMG_OK || length == 0u) {
        gimg_doc_destroy(doc);
        codec->allocator = saved;
        EXPECT_EQ(f.outstanding, 0)
            << name << " leaks when its own writer refuses it";
        skipped++;
        continue;
      }
      files++;

      // Dense where the segment and chunk writers are, coarse through the
      // pixel data behind them: the arms this sweep is here for sit before
      // the pixels in all four formats.
      const size_t dense = length < 512u ? length : 512u;
      const size_t step = length > dense ? (length - dense) / 24u + 1u : 1u;
      for (size_t budget = 0; budget < length;
           budget += (budget < dense ? 1u : step)) {
        BudgetStream sink(budget);
        ASSERT_TRUE(sink.usable()) << name << " at budget " << budget;
        GIMG_Save_Report r2 = {};
        const GIMG_Result r =
            gimg_doc_save(doc, sink.get(), d.codec, &opts, &r2);
        budgets++;
        if (r == GIMG_OK) {
          swallowed++;
          ADD_FAILURE() << name << ": a sink that took only " << budget
                        << " of " << length
                        << " bytes was reported as a successful save";
        }
        EXPECT_LE(sink.written(), budget)
            << name << ": wrote past a sink that had only " << budget
            << " bytes of room";
      }

      gimg_doc_destroy(doc);
      codec->allocator = saved;
      // One check per fixture rather than one per budget, because the
      // document outlives the budgets: anything a failed save kept is still
      // outstanding here.
      EXPECT_EQ(f.outstanding, 0)
          << name << ": " << f.outstanding
          << " block(s) outstanding after " << length
          << " budgets of re-saving and destroying the document";
    }
  }
  std::printf("  %ld fixtures re-saved, %ld skipped, %ld budgets, "
              "%ld swallowed\n", files, skipped, budgets, swallowed);
  ASSERT_GT(files, 100)
      << "only " << files << " fixtures were re-saved, which is too few for "
         "this sweep to be reaching the preservation paths it is here for";
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
