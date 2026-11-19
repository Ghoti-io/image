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
 * max_memory bounds the sum of what a load or a decode holds, not each
 * allocation.
 *
 * Until this existed the cap was compared with each request alone, so a
 * decode whose working set was several blocks, each under the cap, passed it
 * whatever they added up to. The tests here use an allocator that knows the
 * size of every live block, run each format's decode with the cap at every
 * level from just above its largest block to just under its peak, and
 * require two things of each run: the live total the allocator saw never
 * passed the cap, and a cap between the largest block and the peak is
 * GIMG_ERR_LIMIT rather than a success. The second is the sum bound by
 * itself: the largest request fits the cap in every one of those runs, so
 * only a total can refuse them.
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
#include <string>
#include <vector>

#include "../../src/codec/codec_internal.h"

namespace {

using Bytes = std::vector<uint8_t>;

/**
 * An allocator that records the live total and its high-water mark. A
 * realloc counts as a change of size in place, which is what the cap is
 * defined against; an allocator that moves a block holds both copies for a
 * moment, and that is not counted by the library or here.
 */
struct Track {
  GIMG_Allocator a{};
  size_t live = 0;
  size_t peak = 0;
  size_t max_request = 0;
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

GIMG_Limits unlimited() {
  GIMG_Limits l = {};
  l.max_decoded_pixels = GIMG_LIMIT_NONE;
  l.max_memory = GIMG_LIMIT_NONE;
  l.max_metadata_size = GIMG_LIMIT_NONE;
  return l;
}

/** A noisy RGBA picture: it does not compress, so working sets are large. */
Bytes pixels(uint32_t w, uint32_t h) {
  Bytes v(static_cast<size_t>(w) * h * 4u);
  uint32_t s = 20260101u;
  for (size_t i = 0; i < v.size(); ++i) {
    s = s * 1664525u + 1013904223u;
    const size_t px = i / 4u;
    const uint32_t smooth = static_cast<uint32_t>((px % w) * 255u / w);
    v[i] = static_cast<uint8_t>(((i & 3u) == 3u) ? 255u
                                                  : smooth + ((s >> 24) & 15u));
  }
  return v;
}

Bytes encode(const char * format, uint32_t w, uint32_t h, int lossy) {
  const Bytes px = pixels(w, h);
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  std::memcpy(gimg_raster_pixels(r), px.data(), px.size());
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  const bool webp = std::strcmp(format, "webp") == 0;
  if (webp) {
    opts.webp_lossless =
        lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
    opts.webp_effort = 4;
  }
  GIMG_Save_Report report = {};
  EXPECT_EQ(gimg_doc_save(doc, out, format, webp ? &opts : nullptr, &report),
      GIMG_OK)
      << format;
  const void * b = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &b, &n);
  Bytes file(static_cast<const uint8_t *>(b), static_cast<const uint8_t *>(b) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return file;
}

/** What one call (the load, or the decode) did, measured above its start. */
struct Run {
  GIMG_Result result = GIMG_OK;
  size_t peak = 0;
  size_t max_request = 0;
};

/**
 * Load the file under @p load_cap, then decode its first item under
 * @p decode_cap, recording each call. The decode is skipped when the load
 * fails.
 */
struct Pair {
  Run load;
  Run decode;
  bool decoded = false;
};

Pair run(const char * format, const Bytes & file, size_t load_cap,
    size_t decode_cap) {
  Pair out;
  Track t;
  Installed inst(format, t);
  GIMG_Stream * in = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in), GIMG_OK);
  GIMG_Limits lim = unlimited();
  lim.max_memory = load_cap;
  GIMG_Load_Options lo = {};
  lo.limits = &lim;
  GIMG_Doc * doc = nullptr;
  size_t base = t.live;
  t.peak = t.live;
  t.max_request = 0;
  out.load.result = gimg_doc_load(in, &lo, nullptr, &doc);
  out.load.peak = t.peak - base;
  out.load.max_request = t.max_request;
  if (out.load.result == GIMG_OK) {
    GIMG_Limits dl = unlimited();
    dl.max_memory = decode_cap;
    GIMG_Decode_Options dopt = {};
    dopt.limits = &dl;
    base = t.live;
    t.peak = t.live;
    t.max_request = 0;
    GIMG_Raster * r = nullptr;
    out.decode.result = gimg_item_decode(gimg_doc_item(doc, 0), &dopt, &r);
    out.decode.peak = t.peak - base;
    out.decode.max_request = t.max_request;
    out.decoded = true;
    if (r) {
      gimg_raster_destroy(r);
    }
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  EXPECT_EQ(t.live, 0u) << format << ": a block outlived the document";
  return out;
}

struct Subject {
  const char * codec;
  const char * label;
  Bytes file;
};

std::vector<Subject> subjects() {
  std::vector<Subject> v;
  v.push_back({"webp", "webp lossless", encode("webp", 192, 160, 0)});
  v.push_back({"webp", "webp lossy", encode("webp", 192, 160, 1)});
  v.push_back({"png", "png", encode("png", 192, 160, 0)});
  v.push_back({"tiff", "tiff", encode("tiff", 192, 160, 0)});
  v.push_back({"bmp", "bmp", encode("bmp", 192, 160, 0)});
  v.push_back({"jpeg", "jpeg", encode("jpeg", 192, 160, 0)});
  return v;
}

/**
 * For one call of one format: the live total the allocator saw never passes
 * the cap, whatever the cap, and a cap that holds the largest block but not
 * the peak is refused.
 */
void check_call(const Subject & s, bool decode) {
  const Pair open = run(s.codec, s.file, GIMG_LIMIT_NONE, GIMG_LIMIT_NONE);
  ASSERT_EQ(open.load.result, GIMG_OK) << s.label;
  ASSERT_TRUE(open.decoded) << s.label;
  ASSERT_EQ(open.decode.result, GIMG_OK) << s.label;
  const Run & ref = decode ? open.decode : open.load;
  if (std::getenv("GIMG_TEST_MEM_REPORT")) {
    std::printf("  %-14s %-6s peak=%zu max_request=%zu\n", s.label,
        decode ? "decode" : "load", ref.peak, ref.max_request);
  }
  // The premise: the call holds more than its largest block, so the old
  // per-allocation cap could not have told this working set from a small one.
  ASSERT_GT(ref.peak, ref.max_request)
      << s.label << ": the call holds one block at a time, which cannot show "
      << "a sum bound";

  size_t refused_between = 0;
  size_t between = 0;
  const size_t lo = ref.max_request;
  const size_t hi = ref.peak;
  std::vector<size_t> caps = {1, lo / 2, lo - 1, lo, lo + 1,
      lo + (hi - lo) / 4, lo + (hi - lo) / 2, hi - (hi - lo) / 4, hi - 1, hi,
      hi + 1, hi * 2};
  for (size_t cap : caps) {
    const Pair p = decode ? run(s.codec, s.file, GIMG_LIMIT_NONE, cap)
                          : run(s.codec, s.file, cap, GIMG_LIMIT_NONE);
    const Run & got = decode ? p.decode : p.load;
    if (decode && !p.decoded) {
      continue;
    }
    EXPECT_LE(got.peak, cap) << s.label << " cap " << cap
                             << ": live bytes passed the cap";
    EXPECT_TRUE(got.result == GIMG_OK || got.result == GIMG_ERR_LIMIT)
        << s.label << " cap " << cap << ": result " << got.result;
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
      << s.label << (decode ? " decode" : " load")
      << ": a cap at or above every single request but below the peak "
         "was accepted, so the cap is not a sum";
}

} // namespace

TEST(MemorySum, DecodeLiveBytesNeverPassTheCapAndASumOverItIsRefused) {
  for (const Subject & s : subjects()) {
    check_call(s, true);
  }
}

TEST(MemorySum, LoadLiveBytesNeverPassTheCap) {
  // Loads hold a working set only in some formats; this checks the bound
  // where the premise holds and the plain bound where it does not.
  for (const Subject & s : subjects()) {
    const Pair open = run(s.codec, s.file, GIMG_LIMIT_NONE, GIMG_LIMIT_NONE);
    ASSERT_EQ(open.load.result, GIMG_OK) << s.label;
    if (open.load.peak > open.load.max_request) {
      check_call(s, false);
      continue;
    }
    for (size_t cap : {open.load.peak, open.load.peak / 2, size_t(1)}) {
      const Pair p = run(s.codec, s.file, cap, GIMG_LIMIT_NONE);
      EXPECT_LE(p.load.peak, cap) << s.label << " cap " << cap;
    }
  }
}

TEST(MemorySum, AReturnedRasterDoesNotChargeTheNextCall) {
  // The raster a decode returns outlives the scope that counted it. Decoding
  // again while it is still held, and freeing it in between, must charge the
  // second call only for its own blocks: a free of a block the scope no longer
  // holds subtracts nothing, so nothing underflows and nothing is double
  // counted.
  const Subject s = {"webp", "webp lossless", encode("webp", 192, 160, 0)};
  const Pair open = run(s.codec, s.file, GIMG_LIMIT_NONE, GIMG_LIMIT_NONE);
  ASSERT_EQ(open.decode.result, GIMG_OK);
  const size_t need = open.decode.peak;

  Track t;
  Installed inst(s.codec, t);
  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(s.file.data(), s.file.size(), &in),
      GIMG_OK);
  GIMG_Limits lim = unlimited();
  GIMG_Load_Options lo = {};
  lo.limits = &lim;
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in, &lo, nullptr, &doc), GIMG_OK);
  GIMG_Limits dl = unlimited();
  dl.max_memory = need;
  GIMG_Decode_Options dopt = {};
  dopt.limits = &dl;
  GIMG_Raster * first = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), &dopt, &first), GIMG_OK);
  GIMG_Raster * second = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), &dopt, &second), GIMG_OK)
      << "the first raster, still held, was charged to the second call";
  gimg_raster_destroy(first);
  gimg_raster_destroy(second);
  GIMG_Raster * third = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), &dopt, &third), GIMG_OK);
  gimg_raster_destroy(third);
  dl.max_memory = need - 1;
  GIMG_Raster * refused = nullptr;
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), &dopt, &refused),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(refused, nullptr);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  EXPECT_EQ(t.live, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
