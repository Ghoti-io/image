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
    GIMG_Raster * raster = make_raster(*c.format, 16u, 16u, c.levels);
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

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
