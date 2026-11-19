/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * What a save holds, and what the operations hold, measured.
 *
 * gimg_doc_save() runs inside the same cumulative memory scope as a load and a
 * decode, with the limits the document remembers from its load (the library
 * defaults for a document built in memory). The tests here use an allocator
 * that knows the size of every live block, and require of each of the seven
 * writers that
 *
 *  - the live total above the start of the call never passes the cap, at any
 *    cap;
 *  - a cap that holds the largest single block but not the peak is refused
 *    with GIMG_ERR_LIMIT, which only a sum can do;
 *  - a refusal leaves the destination stream with no byte written; and
 *  - nothing is left allocated after a refusal.
 *
 * The operations have no way to state a limit (see api-options.md), so they
 * are measured and checked against an allocator that refuses: the peak is the
 * working set, an allocator that allows exactly that peak lets the call
 * succeed, one that allows a byte less makes it fail cleanly.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <utility>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "../../src/codec/codec_internal.h"
#include "../../src/container/doc_internal.h"
#include "../../src/core/budget_internal.h"

namespace {

using Bytes = std::vector<uint8_t>;

/**
 * An allocator that records the live total and its high-water mark, and
 * refuses a request that would take the live total past `refuse_over`. A
 * realloc counts as a change of size in place.
 */
struct Track {
  GIMG_Allocator a{};
  size_t live = 0;
  size_t peak = 0;
  size_t max_request = 0;
  size_t refuse_over = SIZE_MAX;
  static constexpr size_t kHead = 16;

  void seen(size_t request) {
    if (request > max_request) {
      max_request = request;
    }
    if (live > peak) {
      peak = live;
    }
  }
  static void * m(void * ctx, size_t n) {
    Track * t = static_cast<Track *>(ctx);
    if (t->refuse_over != SIZE_MAX && n > t->refuse_over - t->live) {
      return nullptr;
    }
    uint8_t * p = static_cast<uint8_t *>(std::malloc(n + kHead));
    if (!p) {
      return nullptr;
    }
    std::memcpy(p, &n, sizeof(n));
    t->live += n;
    t->seen(n);
    return p + kHead;
  }
  static void * c(void * ctx, size_t n, size_t sz) {
    if (sz != 0 && n > SIZE_MAX / sz) {
      return nullptr;
    }
    void * p = m(ctx, n * sz);
    if (p) {
      std::memset(p, 0, n * sz);
    }
    return p;
  }
  static void f(void * ctx, void * p) {
    if (!p) {
      return;
    }
    Track * t = static_cast<Track *>(ctx);
    uint8_t * base = static_cast<uint8_t *>(p) - kHead;
    size_t n;
    std::memcpy(&n, base, sizeof(n));
    t->live -= n;
    std::free(base);
  }
  static void * r(void * ctx, void * p, size_t n) {
    if (!p) {
      return m(ctx, n);
    }
    Track * t = static_cast<Track *>(ctx);
    uint8_t * base = static_cast<uint8_t *>(p) - kHead;
    size_t old;
    std::memcpy(&old, base, sizeof(old));
    if (n > old && t->refuse_over != SIZE_MAX &&
        n - old > t->refuse_over - t->live) {
      return nullptr;
    }
    uint8_t * q = static_cast<uint8_t *>(std::realloc(base, n + kHead));
    if (!q) {
      return nullptr;
    }
    std::memcpy(q, &n, sizeof(n));
    t->live = t->live - old + n;
    t->seen(n);
    return q + kHead;
  }
  Track() {
    a.ctx = this;
    a.malloc_fn = m;
    a.calloc_fn = c;
    a.realloc_fn = r;
    a.free_fn = f;
  }
};

/** Swaps a tracker into one codec for the life of the object. */
struct Installed {
  GIMG_Codec * codec;
  const GIMG_Allocator * saved;
  Installed(const char * name, Track & t) : codec(gimg_codec_by_name(name)) {
    saved = codec ? codec->allocator : nullptr;
    if (codec) {
      codec->allocator = &t.a;
    }
  }
  ~Installed() {
    if (codec) {
      codec->allocator = saved;
    }
  }
};

bool report_wanted() {
  return std::getenv("GIMG_TEST_MEM_REPORT") != nullptr;
}

// ---------------------------------------------------------------- saves ----

struct Subject {
  const char * codec;
  const char * label;
  int webp_mode; // -1 not webp, else GIMG_WEBP_COMPRESS_*
  bool palette;  // few colours, for the format that needs them
  uint32_t w;
  uint32_t h;
};

/**
 * The cap the reference measurements run under: one nothing reaches, but a
 * cap all the same. A save with no cap at all is not staged (BMP, JPEG and
 * TIFF hand their bytes over as they go), so its peak is not the peak of the
 * same save under a cap, which is what the sweeps compare.
 */
constexpr size_t kHuge = static_cast<size_t>(1) << 40;

const GIMG_Pixel_Format & rgba8() {
  return GIMG_PIXEL_RGBA8;
}

/** A noisy RGBA picture (it does not compress, so working sets are large), or
 * sixteen flat colours for a format that holds no more than a palette. */
void fill(uint8_t * p, uint32_t w, uint32_t h, bool palette) {
  uint32_t s = 20260101u;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t * q = p + (static_cast<size_t>(y) * w + x) * 4u;
      if (palette) {
        const uint32_t k = ((x / 12u) + (y / 10u)) % 16u;
        q[0] = static_cast<uint8_t>(k * 16u);
        q[1] = static_cast<uint8_t>(255u - k * 16u);
        q[2] = static_cast<uint8_t>(k * 7u);
        q[3] = 255;
      }
      else {
        for (int c = 0; c < 3; ++c) {
          s = s * 1664525u + 1013904223u;
          q[c] = static_cast<uint8_t>(x * 255u / w + ((s >> 24) & 15u));
        }
        q[3] = 255;
      }
    }
  }
}

const std::vector<Subject> & subjects() {
  static const std::vector<Subject> v = {
      {"bmp", "bmp", -1, false, 192, 160},
      {"gif", "gif", -1, true, 192, 160},
      {"ico", "ico", -1, false, 128, 96},
      {"jpeg", "jpeg", -1, false, 192, 160},
      {"png", "png", -1, false, 192, 160},
      {"tiff", "tiff", -1, false, 192, 160},
      {"webp", "webp lossless", GIMG_WEBP_COMPRESS_LOSSLESS, false, 192, 160},
      {"webp", "webp lossy", GIMG_WEBP_COMPRESS_LOSSY, false, 192, 160},
  };
  return v;
}

/** What one save did, measured above its start. */
struct SaveRun {
  GIMG_Result result = GIMG_OK;
  size_t peak = 0;
  size_t max_request = 0;
  size_t sink_bytes = 0;
  size_t bytes_written = 0;
  Bytes out;
};

/**
 * Build a document in memory (document, raster and destination all through
 * one tracked allocator), give it @p cap as the memory limit it remembers,
 * and save it. @p premultiplied stores the same opaque pixels in the
 * premultiplied form, which a save copies straight before it writes.
 */
SaveRun save_in_memory(
    const Subject & s, size_t cap, bool premultiplied = false) {
  SaveRun run;
  Track t;
  Installed inst(s.codec, t);
  // An icon is written through the PNG and BMP writers.
  Installed inner_png("png", t);
  Installed inner_bmp("bmp", t);
  GIMG_Pixel_Format fmt = rgba8();
  if (premultiplied) {
    fmt.alpha_form = GIMG_ALPHA_PREMULTIPLIED;
  }
  GIMG_Raster * ras = nullptr;
  EXPECT_EQ(gimg_raster_create_with_allocator(&t.a, s.w, s.h, &fmt,
                GIMG_RASTER_OWNED, nullptr, 0, &ras),
      GIMG_OK);
  fill(static_cast<uint8_t *>(gimg_raster_pixels(ras)), s.w, s.h, s.palette);
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create_with_allocator(&t.a, &doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), ras);
  if (cap != 0) {
    memset(&doc->load_limits, 0, sizeof(doc->load_limits));
    doc->load_limits.max_decoded_pixels = GIMG_LIMIT_NONE;
    doc->load_limits.max_metadata_size = GIMG_LIMIT_NONE;
    doc->load_limits.max_memory = cap;
    doc->has_load_limits = 1;
  }
  GIMG_Stream * sink = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output_with_allocator(&t.a, &sink),
      GIMG_OK);

  GIMG_Save_Options opts = {};
  if (s.webp_mode >= 0) {
    opts.webp_lossless = static_cast<uint8_t>(s.webp_mode);
    opts.webp_effort = 4;
  }
  GIMG_Save_Report report = {};
  const size_t base = t.live;
  t.peak = t.live;
  t.max_request = 0;
  run.result = gimg_doc_save(doc, sink, s.codec, &opts, &report);
  run.peak = t.peak - base;
  run.max_request = t.max_request;
  const void * b = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(sink, &b, &n);
  run.sink_bytes = n;
  run.bytes_written = report.bytes_written;
  if (n) {
    run.out.assign(static_cast<const uint8_t *>(b),
        static_cast<const uint8_t *>(b) + n);
  }
  gimg_stream_destroy(sink);
  gimg_doc_destroy(doc);
  EXPECT_EQ(t.live, 0u) << s.label << " cap " << cap
                        << ": a block outlived the save";
  return run;
}

/**
 * For one writer: the live total never passes the cap, whatever the cap; a cap
 * that holds the largest block but not the peak is refused; and a refusal
 * writes nothing.
 */
void check_writer(const Subject & s) {
  const SaveRun open = save_in_memory(s, kHuge);
  ASSERT_EQ(open.result, GIMG_OK) << s.label;
  ASSERT_GT(open.sink_bytes, 0u) << s.label;
  ASSERT_EQ(open.bytes_written, open.sink_bytes) << s.label;
  if (report_wanted()) {
    std::printf("  %-14s %4ux%-4u peak=%zu max_request=%zu file=%zu\n", s.label,
        s.w, s.h, open.peak, open.max_request, open.sink_bytes);
  }
  // The premise: the save holds more than its largest block, so a bound on
  // each request could not tell this working set from a small one.
  ASSERT_GT(open.peak, open.max_request)
      << s.label << ": the save holds one block at a time, which cannot "
      << "show a sum bound";

  const size_t lo = open.max_request;
  const size_t hi = open.peak;
  size_t between = 0;
  size_t refused_between = 0;
  std::vector<size_t> caps = {1, lo / 2, lo - 1, lo, lo + 1,
      lo + (hi - lo) / 4, lo + (hi - lo) / 2, hi - (hi - lo) / 4, hi - 1, hi,
      hi + 1, hi * 2};
  for (size_t cap : caps) {
    const SaveRun got = save_in_memory(s, cap);
    EXPECT_LE(got.peak, cap) << s.label << " cap " << cap
                             << ": live bytes passed the cap";
    if (got.result == GIMG_OK) {
      EXPECT_EQ(got.out, open.out)
          << s.label << " cap " << cap << ": the cap changed the file";
      EXPECT_EQ(got.bytes_written, got.sink_bytes);
    }
    else {
      EXPECT_EQ(got.result, GIMG_ERR_LIMIT)
          << s.label << " cap " << cap << ": result " << got.result;
      EXPECT_EQ(got.sink_bytes, 0u)
          << s.label << " cap " << cap << ": a refused save wrote bytes";
      EXPECT_EQ(got.bytes_written, 0u);
    }
    if (cap >= hi) {
      EXPECT_EQ(got.result, GIMG_OK)
          << s.label << " cap " << cap << " covers the whole peak " << hi;
    }
    if (cap >= lo && cap < hi) {
      between++;
      if (got.result == GIMG_ERR_LIMIT) {
        refused_between++;
      }
    }
  }
  EXPECT_GT(between, 0u) << s.label;
  EXPECT_EQ(refused_between, between)
      << s.label << ": a cap at or above every single request but below the "
      << "peak was accepted, so the save is not bound by a sum";
}

} // namespace

TEST(SaveMemory, EveryWriterHoldsItsLiveBytesToTheCap) {
  for (const Subject & s : subjects()) {
    check_writer(s);
  }
}

TEST(SaveMemory, WorkingSetGrowsWithThePictureAndStaysUnderTwelveRasters) {
  // The figures api-options.md gives: a save holds a small multiple of the
  // raster it is given, so the default cap admits a picture of a few hundred
  // megabytes and a smaller cap is refused in proportion. Measured at three
  // sizes; each is also required to be refused one byte under its own peak.
  for (const Subject & base : subjects()) {
    size_t previous = 0;
    for (uint32_t k : {1u, 2u, 4u}) {
      Subject s = base;
      s.w = base.w / 2 * k;
      s.h = base.h / 2 * k;
      const size_t raster_bytes = static_cast<size_t>(s.w) * s.h * 4u;
      const SaveRun open = save_in_memory(s, kHuge);
      ASSERT_EQ(open.result, GIMG_OK) << s.label;
      if (report_wanted()) {
        std::printf("  %-14s %4ux%-4u peak=%zu = %.2f rasters\n", s.label, s.w,
            s.h, open.peak,
            static_cast<double>(open.peak) / static_cast<double>(raster_bytes));
      }
      EXPECT_LE(open.peak, raster_bytes * 12u + 262144u) << s.label;
      EXPECT_GT(open.peak, previous) << s.label << ": peak did not grow";
      previous = open.peak;
      const SaveRun tight = save_in_memory(s, open.peak - 1);
      EXPECT_EQ(tight.result, GIMG_ERR_LIMIT) << s.label << " " << s.w;
      EXPECT_EQ(tight.sink_bytes, 0u);
      const SaveRun fits = save_in_memory(s, open.peak);
      EXPECT_EQ(fits.result, GIMG_OK) << s.label << " " << s.w;
    }
  }
}

TEST(SaveMemory, ADocumentWithNoRememberedLimitsIsNotCapped) {
  // Nothing was said, and GIMG_Save_Options has no field to lift a default, so
  // the save runs as it always did.
  for (const Subject & s : subjects()) {
    const SaveRun got = save_in_memory(s, 0);
    EXPECT_EQ(got.result, GIMG_OK) << s.label;
    EXPECT_GT(got.sink_bytes, 0u) << s.label;
  }
}

TEST(SaveMemory, PremultipliedCopyIsCountedAgainstTheCap) {
  // A premultiplied raster is saved through a straight copy of itself, which
  // the caller did not allocate and cannot see. It is held beside the staged
  // file for the length of the save, so the same pixels cost a raster more
  // than they do stored straight, and a cap between the two refuses one and
  // not the other.
  const Subject s = {"png", "png premultiplied", -1, false, 192, 160};
  const size_t raster_bytes = static_cast<size_t>(s.w) * s.h * 4u;
  const SaveRun plain = save_in_memory(s, kHuge, false);
  const SaveRun pre = save_in_memory(s, kHuge, true);
  ASSERT_EQ(plain.result, GIMG_OK);
  ASSERT_EQ(pre.result, GIMG_OK);
  EXPECT_EQ(pre.out, plain.out) << "opaque pixels are the same either way";
  EXPECT_GE(pre.peak, plain.peak + raster_bytes)
      << "the straight copy is not in the live total";
  if (report_wanted()) {
    std::printf("  premultiplied png: peak=%zu, straight %zu\n", pre.peak,
        plain.peak);
  }

  const SaveRun ok_plain = save_in_memory(s, pre.peak - 1, false);
  EXPECT_EQ(ok_plain.result, GIMG_OK) << "a cap the straight raster fits";
  const SaveRun refused = save_in_memory(s, pre.peak - 1, true);
  EXPECT_EQ(refused.result, GIMG_ERR_LIMIT);
  EXPECT_EQ(refused.sink_bytes, 0u);
  const SaveRun fits = save_in_memory(s, pre.peak, true);
  EXPECT_EQ(fits.result, GIMG_OK);
  EXPECT_LE(fits.peak, pre.peak);
}

namespace {

/** A file of the format, made from the picture above. */
Bytes make_file(const Subject & s) {
  const SaveRun r = save_in_memory(s, kHuge);
  EXPECT_EQ(r.result, GIMG_OK) << s.label;
  return r.out;
}

/**
 * Load @p file under @p load_cap, keep no raster, and save the document: the
 * writer decodes the item itself. Records the save only.
 */
SaveRun save_loaded(const char * from, const Bytes & file, size_t load_cap,
    const char * to, bool decode_first) {
  SaveRun run;
  Track t;
  Installed in_codec(from, t);
  Installed out_codec(to, t);
  GIMG_Stream * in = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in), GIMG_OK);
  GIMG_Limits lim = {};
  lim.max_decoded_pixels = GIMG_LIMIT_NONE;
  lim.max_metadata_size = GIMG_LIMIT_NONE;
  lim.max_memory = load_cap;
  GIMG_Load_Options lo = {};
  lo.limits = &lim;
  GIMG_Doc * doc = nullptr;
  run.result = gimg_doc_load(in, &lo, nullptr, &doc);
  if (run.result != GIMG_OK) {
    gimg_stream_destroy(in);
    return run;
  }
  if (decode_first) {
    // The decode is its own call, under no cap but its own.
    GIMG_Limits free_lim = lim;
    free_lim.max_memory = GIMG_LIMIT_NONE;
    GIMG_Decode_Options dopt = {};
    dopt.limits = &free_lim;
    EXPECT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, 0), &dopt), GIMG_OK);
  }
  GIMG_Stream * sink = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output_with_allocator(&t.a, &sink),
      GIMG_OK);
  GIMG_Save_Report report = {};
  const size_t base = t.live;
  t.peak = t.live;
  t.max_request = 0;
  run.result = gimg_doc_save(doc, sink, to, nullptr, &report);
  run.peak = t.peak - base;
  run.max_request = t.max_request;
  const void * b = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(sink, &b, &n);
  run.sink_bytes = n;
  gimg_stream_destroy(sink);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  EXPECT_EQ(t.live, 0u) << "a block outlived the save";
  return run;
}

} // namespace

TEST(SaveMemory, ASaveThatDecodesItsOwnItemIsHeldToTheCapAsOneCall) {
  // No raster is attached, so the writer decodes the item (a nested scope
  // under the limits the document remembers) and then writes. The decode and
  // the write are one save and share one cap: the raster the decode returns
  // is live while the file is composed.
  const Subject src = {"png", "png", -1, false, 192, 160};
  const Bytes file = make_file(src);
  for (const char * to : {"png", "bmp", "tiff", "jpeg"}) {
    const SaveRun open =
        save_loaded("png", file, kHuge, to, false);
    ASSERT_EQ(open.result, GIMG_OK) << to;
    ASSERT_GT(open.sink_bytes, 0u) << to;
    const size_t raster_bytes = 192u * 160u * 4u;
    EXPECT_GE(open.peak, raster_bytes)
        << to << ": the decoded raster is not in the live total";
    if (report_wanted()) {
      std::printf("  png -> %-5s decode+save peak=%zu\n", to, open.peak);
    }
    for (size_t cap : {open.peak, open.peak - 1, open.peak / 2,
             open.max_request, open.max_request / 2}) {
      const SaveRun got = save_loaded("png", file, cap, to, false);
      if (got.result == GIMG_ERR_LIMIT || got.result == GIMG_OK) {
        EXPECT_LE(got.peak, cap) << to << " cap " << cap;
      }
      if (got.result != GIMG_OK) {
        EXPECT_EQ(got.sink_bytes, 0u) << to << " cap " << cap;
      }
      if (cap >= open.peak) {
        EXPECT_EQ(got.result, GIMG_OK) << to << " cap " << cap;
      }
      else if (cap >= open.max_request) {
        EXPECT_EQ(got.result, GIMG_ERR_LIMIT)
            << to << " cap " << cap << ": a sum over the cap was accepted";
      }
    }
  }
}

TEST(SaveMemory, ALoadedDocumentSavesUnderItsLoadLimitsEvenWhenDecoded) {
  // The route a caller has, with nothing but the public API: the limits given
  // to gimg_doc_load() are the ones the save is held to.
  const Subject src = {"png", "png", -1, false, 192, 160};
  const Bytes file = make_file(src);
  const SaveRun open = save_loaded("png", file, kHuge, "tiff", true);
  ASSERT_EQ(open.result, GIMG_OK);
  const SaveRun refused =
      save_loaded("png", file, open.max_request, "tiff", true);
  // The load itself must fit the cap, or there is no document to save.
  if (refused.result != GIMG_OK) {
    EXPECT_TRUE(refused.result == GIMG_ERR_LIMIT) << refused.result;
    EXPECT_EQ(refused.sink_bytes, 0u);
  }
  const SaveRun fits = save_loaded("png", file, open.peak, "tiff", true);
  EXPECT_EQ(fits.result, GIMG_OK);
  EXPECT_LE(fits.peak, open.peak);
}

TEST(SaveMemory, ASaveToAFailingSinkStillReportsTheSinkNotTheCap) {
  // The cap is not the only thing that can stop a save, and a save that fails
  // for another reason must not be relabelled.
  const Subject s = {"png", "png", -1, false, 64, 48};
  GIMG_Raster * ras = nullptr;
  ASSERT_EQ(gimg_raster_create(s.w, s.h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &ras),
      GIMG_OK);
  fill(static_cast<uint8_t *>(gimg_raster_pixels(ras)), s.w, s.h, false);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), ras);
  GIMG_Save_Report report = {};
  // A read-only stream has no write.
  const uint8_t one = 0;
  GIMG_Stream * ro = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(&one, 1, &ro), GIMG_OK);
  const GIMG_Result r = gimg_doc_save(doc, ro, "png", nullptr, &report);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_NE(r, GIMG_ERR_LIMIT);
  gimg_stream_destroy(ro);
  gimg_doc_destroy(doc);
}

// ------------------------------------------------------------------ ops ----

namespace {

struct OpEnv {
  Track * t;
  GIMG_Raster * src;
  GIMG_Raster * aux; // a second raster of the same size, for composite
  uint32_t w;
  uint32_t h;
};

struct OpCase {
  const char * name;
  /** Run the operation; any raster it makes goes in *out. */
  std::function<GIMG_Result(const OpEnv &, GIMG_Raster **)> run;
  /** Set up the source before the measurement starts, if it needs it. */
  std::function<void(GIMG_Raster *)> prepare;
  OpCase(const char * n,
      std::function<GIMG_Result(const OpEnv &, GIMG_Raster **)> r,
      std::function<void(GIMG_Raster *)> p = nullptr)
      : name(n), run(std::move(r)), prepare(std::move(p)) {}
};

GIMG_Raster * make_raster(Track & t, uint32_t w, uint32_t h) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create_with_allocator(&t.a, w, h, &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  fill(static_cast<uint8_t *>(gimg_raster_pixels(r)), w, h, false);
  return r;
}

std::vector<OpCase> op_cases() {
  std::vector<OpCase> v;
  v.push_back({"transform_color display-p3->srgb", [](const OpEnv & e,
                                                       GIMG_Raster ** o) {
                 GIMG_Color_Transform_Options opts;
                 gimg_color_transform_options_srgb(&opts);
                 return gimg_ops_transform_color(e.src, &opts, o);
               },
      [](GIMG_Raster * src) {
        GCOL_Color_Info ci;
        gcol_color_info_default(&ci);
        EXPECT_TRUE(gcol_color_info_set_gamut(&ci, GCOL_PRIMARIES_DISPLAY_P3));
        ci.transfer = GCOL_TRANSFER_SRGB;
        EXPECT_EQ(gimg_raster_set_color_info(src, &ci), GIMG_OK);
      }});
  v.push_back({"convert_bit_depth 8->16", [](const OpEnv & e, GIMG_Raster ** o) {
                 return gimg_ops_convert_bit_depth(e.src, 16, o);
               }});
  v.push_back({"resize down 1/2", [](const OpEnv & e, GIMG_Raster ** o) {
                 return gimg_ops_resize(e.src, e.w / 2, e.h / 2, nullptr, o);
               }});
  v.push_back({"resize up 2x", [](const OpEnv & e, GIMG_Raster ** o) {
                 return gimg_ops_resize(e.src, e.w * 2, e.h * 2, nullptr, o);
               }});
  v.push_back({"resize linear lanczos", [](const OpEnv & e, GIMG_Raster ** o) {
                 GIMG_Resize_Options opt;
                 gimg_resize_options_default(&opt);
                 opt.space = GIMG_RESAMPLE_SPACE_LINEAR;
                 return gimg_ops_resize(e.src, e.w / 3, e.h / 3, &opt, o);
               }});
  v.push_back({"crop half", [](const OpEnv & e, GIMG_Raster ** o) {
                 return gimg_ops_crop(
                     e.src, e.w / 4, e.h / 4, e.w / 2, e.h / 2, o);
               }});
  v.push_back({"quantize 256 median cut", [](const OpEnv & e,
                                              GIMG_Raster ** o) {
                 GIMG_Quantize_Options q = {};
                 GIMG_Palette p;
                 return gimg_ops_quantize(e.src, &q, o, &p);
               }});
  v.push_back({"quantize 64 floyd-steinberg", [](const OpEnv & e,
                                                  GIMG_Raster ** o) {
                 GIMG_Quantize_Options q = {};
                 q.max_colors = 64;
                 q.dither = GIMG_DITHER_FLOYD_STEINBERG;
                 GIMG_Palette p;
                 return gimg_ops_quantize(e.src, &q, o, &p);
               }});
  v.push_back({"rotate_90_cw (in place)", [](const OpEnv & e, GIMG_Raster **) {
                 return gimg_ops_rotate_90_cw(e.src);
               }});
  v.push_back({"flip_horizontal (in place)", [](const OpEnv & e,
                                                 GIMG_Raster **) {
                 return gimg_ops_flip_horizontal(e.src);
               }});
  v.push_back({"apply_orientation transposed (in place)", [](const OpEnv & e,
                                                              GIMG_Raster **) {
                 return gimg_ops_apply_orientation(
                     e.src, GIMG_ORIENTATION_TRANSPOSE);
               }});
  v.push_back({"composite over (in place)", [](const OpEnv & e, GIMG_Raster **) {
                 return gimg_ops_composite(
                     e.src, e.aux, 0, 0, GIMG_COMPOSITE_OVER);
               }});
  v.push_back({"alpha_premultiply (in place)", [](const OpEnv & e,
                                                   GIMG_Raster **) {
                 return gimg_alpha_premultiply(e.src);
               }});
  v.push_back({"count_colors", [](const OpEnv & e, GIMG_Raster **) {
                 size_t n = 0;
                 bool exact = false;
                 return gimg_ops_count_colors(e.src, 1u << 20, &n, &exact);
               }});
  return v;
}

struct OpRun {
  GIMG_Result result = GIMG_OK;
  size_t peak = 0;
  size_t out_bytes = 0;
};

/**
 * Run one operation; @p limit refuses a live total above base + limit. With
 * @p scope_cap the call runs inside the memory scope a load, a decode or a save
 * opens - as the operations do when a writer or a decoder calls them - and
 * @p hit says whether that scope turned an allocation down.
 */
OpRun run_op(const OpCase & c, uint32_t w, uint32_t h, size_t limit,
    size_t scope_cap = 0, bool * hit = nullptr) {
  OpRun run;
  Track t;
  OpEnv e{&t, make_raster(t, w, h), make_raster(t, w, h), w, h};
  GIMG_Raster * out = nullptr;
  if (c.prepare) {
    c.prepare(e.src);
  }
  const size_t base = t.live;
  t.peak = t.live;
  if (limit != SIZE_MAX) {
    t.refuse_over = base + limit;
  }
  gimg_budget_scope_t scope;
  if (scope_cap) {
    gimg_budget_enter(&scope, scope_cap);
  }
  run.result = c.run(e, &out);
  if (scope_cap) {
    const bool refused = gimg_budget_leave(&scope);
    if (hit) {
      *hit = refused;
    }
  }
  run.peak = t.peak - base;
  if (out) {
    run.out_bytes = static_cast<size_t>(gimg_raster_height(out)) *
        gimg_raster_stride_bytes(out);
  }
  if (run.result != GIMG_OK) {
    EXPECT_EQ(out, nullptr) << c.name << ": a refused call returned a raster";
  }
  gimg_raster_destroy(out);
  gimg_raster_destroy(e.src);
  gimg_raster_destroy(e.aux);
  EXPECT_EQ(t.live, 0u) << c.name << " limit " << limit << ": a block leaked";
  return run;
}

} // namespace

TEST(OpsMemory, WorkingSetIsTheAllocatorsAndARefusalUnwindsCleanly) {
  for (const auto & dims : std::vector<std::pair<uint32_t, uint32_t>>{
           {32, 24}, {128, 96}, {512, 384}}) {
    const uint32_t w = dims.first;
    const uint32_t h = dims.second;
    for (const OpCase & c : op_cases()) {
      const OpRun open = run_op(c, w, h, SIZE_MAX);
      ASSERT_EQ(open.result, GIMG_OK) << c.name << " " << w << "x" << h;
      if (report_wanted()) {
        std::printf("  %-42s %4ux%-4u peak=%zu (raster %zu, result %zu)\n",
            c.name, w, h, open.peak, static_cast<size_t>(w) * h * 4u,
            open.out_bytes);
      }
      // Exactly the working set: allowed in full, the call succeeds.
      const OpRun fits = run_op(c, w, h, open.peak);
      EXPECT_EQ(fits.result, GIMG_OK) << c.name << " " << w << "x" << h;
      EXPECT_LE(fits.peak, open.peak);
      if (open.peak == 0) {
        continue;
      }
      // A byte less: the call may not take the whole peak, so it is refused
      // (or it found a way round with less), and either way nothing leaks and
      // the live total stays within what was allowed.
      // The same call inside a memory scope, which is where a writer or a
      // decoder runs it: held to the cap it is given, and a cap under the
      // working set is the scope's refusal, not the allocator's.
      bool hit = false;
      const OpRun in_scope = run_op(c, w, h, SIZE_MAX, open.peak, &hit);
      EXPECT_EQ(in_scope.result, GIMG_OK) << c.name << " " << w << "x" << h;
      EXPECT_FALSE(hit) << c.name << ": a cap of the whole peak refused";
      for (size_t cap : {open.peak - 1, open.peak / 2, size_t(1)}) {
        const OpRun under = run_op(c, w, h, SIZE_MAX, cap, &hit);
        EXPECT_LE(under.peak, cap) << c.name << " " << w << "x" << h;
        EXPECT_TRUE(under.result == GIMG_OK || hit)
            << c.name << " cap " << cap << ": failed " << under.result
            << " without the scope refusing";
      }
      for (size_t limit : {open.peak - 1, open.peak / 2, size_t(1)}) {
        const OpRun tight = run_op(c, w, h, limit);
        EXPECT_LE(tight.peak, limit) << c.name << " " << w << "x" << h;
        EXPECT_TRUE(tight.result == GIMG_OK || tight.result == GIMG_ERR_OOM ||
            tight.result == GIMG_ERR_LIMIT)
            << c.name << " limit " << limit << ": " << tight.result;
      }
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
