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
 * Which limits a save is held to: GIMG_Save_Options::limits, else the ones the
 * document remembers from its load, else the library defaults.
 *
 * The cap is observed two ways. Sweeps over all seven writers use an
 * allocator that knows the size of every live block, as test_save_memory.cpp
 * does, to show that a cap in the options is honoured to the byte, overrides a
 * remembered one in both directions, writes nothing when it refuses, and
 * leaves no block behind. The effective cap itself is read from inside the
 * save: the destination is a stream of the caller's whose write callback asks
 * the library's own allocation path for a block of a chosen size, which the
 * scope in force accepts or refuses, so that the default, a lifted limit and
 * a remembered one are told apart without a two-gibibyte picture.
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
#include <utility>
#include <vector>

#include "../../src/codec/codec_internal.h"
#include "../../src/container/doc_internal.h"
#include "../../src/core/alloc_internal.h"
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

constexpr size_t kHuge = static_cast<size_t>(1) << 40;
constexpr size_t kMiB = static_cast<size_t>(1) << 20;
constexpr size_t kGiB = static_cast<size_t>(1) << 30;

struct Subject {
  const char * codec;
  const char * label;
  int webp_mode; // -1 not webp, else GIMG_WEBP_COMPRESS_*
  bool palette;
  uint32_t w;
  uint32_t h;
};

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

GIMG_Limits memory_only(size_t cap) {
  GIMG_Limits l = {};
  l.max_memory = cap;
  return l;
}

struct SaveRun {
  GIMG_Result result = GIMG_OK;
  size_t peak = 0;
  size_t sink_bytes = 0;
  size_t bytes_written = 0;
  Bytes out;
  // Blocks the write callback asked for, and whether each was granted.
  std::vector<std::pair<size_t, bool>> probes;
};

struct Probing {
  std::vector<size_t> sizes;
  std::vector<std::pair<size_t, bool>> * result;
  size_t bytes = 0;
};

GIMG_Result probe_write(
    void * ctx, const void *, size_t size, size_t * out_written) {
  Probing * p = static_cast<Probing *>(ctx);
  if (p->result->empty()) {
    for (size_t n : p->sizes) {
      void * b = gimg_malloc(nullptr, n);
      p->result->push_back({n, b != nullptr});
      gimg_free(nullptr, b);
    }
  }
  p->bytes += size;
  *out_written = size;
  return GIMG_OK;
}

/**
 * Build a document in memory (document, raster and destination through one
 * tracked allocator), save it as @p s asks with @p limits in the options
 * (NULL for none), after giving the document @p remembered as the limits of a
 * load (NULL for a document that was not loaded). @p frames is the number of
 * items, each holding a copy of the picture. With @p probe_sizes, the
 * destination is the probing stream and the sizes are asked for from inside
 * the save.
 */
SaveRun save_doc(const Subject & s, const GIMG_Limits * limits,
    const GIMG_Limits * remembered, size_t frames = 1,
    const std::vector<size_t> & probe_sizes = {}) {
  SaveRun run;
  Track t;
  Installed inst(s.codec, t);
  Installed inner_png("png", t);
  Installed inner_bmp("bmp", t);
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create_with_allocator(&t.a, &doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, frames), GIMG_OK);
  for (size_t i = 0; i < frames; ++i) {
    GIMG_Raster * ras = nullptr;
    EXPECT_EQ(gimg_raster_create_with_allocator(&t.a, s.w, s.h,
                  &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &ras),
        GIMG_OK);
    fill(static_cast<uint8_t *>(gimg_raster_pixels(ras)), s.w, s.h, s.palette);
    gimg_item_set_raster(gimg_doc_item(doc, i), ras);
  }
  if (remembered) {
    gimg_limits_resolve(remembered, &doc->load_limits);
    doc->has_load_limits = 1;
  }
  GIMG_Stream * sink = nullptr;
  Probing probing;
  if (probe_sizes.empty()) {
    EXPECT_EQ(
        gimg_stream_create_memory_output_with_allocator(&t.a, &sink), GIMG_OK);
  }
  else {
    probing.sizes = probe_sizes;
    probing.result = &run.probes;
    GIMG_Stream_Callbacks cb = {};
    cb.write = probe_write;
    cb.ctx = &probing;
    EXPECT_EQ(
        gimg_stream_create_callbacks_with_allocator(&t.a, &cb, &sink), GIMG_OK);
  }
  GIMG_Save_Options opts = {};
  if (s.webp_mode >= 0) {
    opts.webp_lossless = static_cast<uint8_t>(s.webp_mode);
    opts.webp_effort = 4;
  }
  opts.limits = limits;
  GIMG_Save_Report report = {};
  const size_t base = t.live;
  t.peak = t.live;
  run.result = gimg_doc_save(doc, sink, s.codec, &opts, &report);
  run.peak = t.peak - base;
  if (probe_sizes.empty()) {
    const void * b = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(sink, &b, &n);
    run.sink_bytes = n;
    if (n) {
      run.out.assign(
          static_cast<const uint8_t *>(b), static_cast<const uint8_t *>(b) + n);
    }
  }
  else {
    run.sink_bytes = probing.bytes;
  }
  run.bytes_written = report.bytes_written;
  gimg_stream_destroy(sink);
  gimg_doc_destroy(doc);
  EXPECT_EQ(t.live, 0u) << s.label << ": a block outlived the save";
  return run;
}

const Subject & subject(const char * label) {
  for (const Subject & s : subjects()) {
    if (strcmp(s.label, label) == 0) {
      return s;
    }
  }
  std::abort();
}

/** Whether the block of @p n the save asked for from inside was granted. */
bool granted(const SaveRun & r, size_t n) {
  for (const auto & p : r.probes) {
    if (p.first == n) {
      return p.second;
    }
  }
  ADD_FAILURE() << "no probe of " << n;
  return false;
}

} // namespace

TEST(SaveLimits, ADocumentBuiltInMemoryIsHeldToTheDefaultCap) {
  const Subject & s = subject("png");
  // PNG hands the file over inside the scope, so the probes see its cap.
  const SaveRun r = save_doc(s, nullptr, nullptr, 1,
      {kGiB, GIMG_DEFAULT_MAX_MEMORY - kGiB / 2, GIMG_DEFAULT_MAX_MEMORY,
          GIMG_DEFAULT_MAX_MEMORY + kGiB});
  ASSERT_EQ(r.result, GIMG_OK);
  ASSERT_GT(r.sink_bytes, 0u);
  EXPECT_TRUE(granted(r, kGiB));
  EXPECT_TRUE(granted(r, GIMG_DEFAULT_MAX_MEMORY - kGiB / 2));
  EXPECT_FALSE(granted(r, GIMG_DEFAULT_MAX_MEMORY));
  EXPECT_FALSE(granted(r, GIMG_DEFAULT_MAX_MEMORY + kGiB));
}

TEST(SaveLimits, LimitNoneLiftsTheCapForAnyDocument) {
  const Subject & s = subject("png");
  const size_t big = GIMG_DEFAULT_MAX_MEMORY + kGiB;
  const GIMG_Limits none = memory_only(GIMG_LIMIT_NONE);
  const GIMG_Limits tiny = memory_only(64 * kMiB);
  // Built in memory.
  SaveRun r = save_doc(s, &none, nullptr, 1, {big});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_TRUE(granted(r, big));
  // Loaded with a small cap: the options win.
  r = save_doc(s, &none, &tiny, 1, {big});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_TRUE(granted(r, big));
}

TEST(SaveLimits, ALimitsStructInTheOptionsIsTakenWholeSoZeroIsTheDefault) {
  const Subject & s = subject("png");
  const GIMG_Limits zero = {};
  const GIMG_Limits none = memory_only(GIMG_LIMIT_NONE);
  // The document remembers no cap at all, and a present struct of zeros is
  // the default, as at load: it does not fall through to the remembered one.
  const SaveRun r = save_doc(s, &zero, &none, 1,
      {GIMG_DEFAULT_MAX_MEMORY - kGiB / 2, GIMG_DEFAULT_MAX_MEMORY + kGiB});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_TRUE(granted(r, GIMG_DEFAULT_MAX_MEMORY - kGiB / 2));
  EXPECT_FALSE(granted(r, GIMG_DEFAULT_MAX_MEMORY + kGiB));
}

TEST(SaveLimits, NullLimitsFallBackToTheRememberedOnesThenTheDefault) {
  const Subject & s = subject("png");
  const GIMG_Limits remembered = memory_only(64 * kMiB);
  SaveRun r = save_doc(s, nullptr, &remembered, 1, {32 * kMiB, 128 * kMiB});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_TRUE(granted(r, 32 * kMiB));
  EXPECT_FALSE(granted(r, 128 * kMiB));
  // A remembered cap of zero is itself the default, so is not looser than it.
  const GIMG_Limits zero = {};
  r = save_doc(s, nullptr, &zero, 1, {GIMG_DEFAULT_MAX_MEMORY + kGiB});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_FALSE(granted(r, GIMG_DEFAULT_MAX_MEMORY + kGiB));
  // Remembered NONE lifts it, with no options.
  const GIMG_Limits none = memory_only(GIMG_LIMIT_NONE);
  r = save_doc(s, nullptr, &none, 1, {GIMG_DEFAULT_MAX_MEMORY + kGiB});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_TRUE(granted(r, GIMG_DEFAULT_MAX_MEMORY + kGiB));
}

TEST(SaveLimits, OptionLimitsOverrideRememberedOnesBothWays) {
  const Subject & s = subject("png");
  const GIMG_Limits loose = memory_only(256 * kMiB);
  const GIMG_Limits tight = memory_only(16 * kMiB);
  // Tighter than the document's: the options win.
  SaveRun r = save_doc(s, &tight, &loose, 1, {8 * kMiB, 32 * kMiB});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_TRUE(granted(r, 8 * kMiB));
  EXPECT_FALSE(granted(r, 32 * kMiB));
  // Looser than the document's: the options win.
  r = save_doc(s, &loose, &tight, 1, {128 * kMiB, 512 * kMiB});
  ASSERT_EQ(r.result, GIMG_OK);
  EXPECT_TRUE(granted(r, 128 * kMiB));
  EXPECT_FALSE(granted(r, 512 * kMiB));
}

namespace {

/**
 * One writer: the options' cap, to the byte, over a document that remembers
 * the opposite. Live bytes never pass it, a refusal is GIMG_ERR_LIMIT with an
 * empty sink and nothing left allocated, and the file does not depend on the
 * cap.
 */
void check_option_cap(const Subject & s) {
  const GIMG_Limits huge = memory_only(kHuge);
  const GIMG_Limits one = memory_only(1);
  const SaveRun open = save_doc(s, &huge, &one);
  ASSERT_EQ(open.result, GIMG_OK) << s.label;
  ASSERT_GT(open.sink_bytes, 0u) << s.label;
  ASSERT_EQ(open.bytes_written, open.sink_bytes) << s.label;
  const size_t peak = open.peak;
  ASSERT_GT(peak, 0u);
  for (size_t cap :
      {size_t(1), peak / 4, peak / 2, peak - 1, peak, peak + 1, peak * 2}) {
    const GIMG_Limits mine = memory_only(cap);
    for (size_t other : {size_t(1), kHuge}) {
      // The document remembers a cap, and the options' is the one that counts,
      // whichever side of it the remembered one lies.
      const GIMG_Limits remembered = memory_only(other);
      const SaveRun got = save_doc(s, &mine, &remembered);
      EXPECT_LE(got.peak, cap) << s.label << " cap " << cap;
      if (cap >= peak) {
        EXPECT_EQ(got.result, GIMG_OK) << s.label << " cap " << cap;
        EXPECT_EQ(got.out, open.out) << s.label << " cap " << cap;
      }
      else {
        EXPECT_EQ(got.result, GIMG_ERR_LIMIT) << s.label << " cap " << cap;
        EXPECT_EQ(got.sink_bytes, 0u)
            << s.label << " cap " << cap << ": a refused save wrote bytes";
        EXPECT_EQ(got.bytes_written, 0u);
      }
    }
  }
}

} // namespace

TEST(SaveLimits,
    EveryWriterHoldsTheOptionCapAndAnOverriddenRememberedOneIsMoot) {
  for (const Subject & s : subjects()) {
    check_option_cap(s);
  }
}

TEST(SaveLimits, ARefusedSaveWritesNothingToACallersStream) {
  // A stream of the caller's, not a memory stream: it sees every byte handed
  // to it, so a partial file would be counted.
  for (const Subject & s : subjects()) {
    const GIMG_Limits one = memory_only(1);
    const SaveRun r = save_doc(s, &one, nullptr, 1, {1});
    EXPECT_EQ(r.result, GIMG_ERR_LIMIT) << s.label;
    EXPECT_EQ(r.sink_bytes, 0u) << s.label;
    EXPECT_TRUE(r.probes.empty()) << s.label << ": the write ran";
  }
}

TEST(SaveLimits, TheOtherFieldsAreIgnoredByASave) {
  // Pixel, frame, metadata and chunk caps bound what a file makes a reader
  // allocate. A writer is handed a raster that exists, so a save under the
  // tightest of them still writes.
  GIMG_Limits l = {};
  l.max_decoded_pixels = 1;
  l.max_frame_count = 1;
  l.max_metadata_size = 1;
  l.max_chunk_size = 1;
  l.max_memory = GIMG_LIMIT_NONE;
  for (const Subject & s : subjects()) {
    const SaveRun r = save_doc(s, &l, nullptr);
    EXPECT_EQ(r.result, GIMG_OK) << s.label;
    EXPECT_GT(r.sink_bytes, 0u) << s.label;
  }
  const SaveRun gif = save_doc(subject("gif"), &l, nullptr, 3);
  EXPECT_EQ(gif.result, GIMG_OK);
  EXPECT_GT(gif.sink_bytes, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
