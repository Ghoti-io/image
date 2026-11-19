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
 * What it costs to walk an APNG or an animated WebP, and that the cheaper way
 * gives the same pixels.
 *
 * A frame of either format is drawn on the canvas the frames before it left,
 * so decoding frame N used to decode frames 0 to N-1 first: an N-frame file
 * cost N(N+1)/2 frame decodes to walk once.  The cost is counted in
 * allocation calls rather than seconds, because every frame decode makes the
 * same handful of them and a count does not wobble with the machine.  The
 * bound is relative: a walk of the whole file may cost at most a few times
 * what decoding only its last frame from a cold document costs, and a
 * quadratic decoder is N/2 times that.
 *
 * Output must not depend on the order frames are asked for in, so every order
 * is compared, byte for byte, with a decode of that frame from a document
 * nothing else has been decoded from.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "../../src/codec/codec_internal.h"
#include "../failing_allocator.h"

using gimg_test::Failing;

namespace {

constexpr uint32_t kSide = 16;

uint32_t next_rand(uint32_t & s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}


uint32_t crc32_of(const uint8_t * p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    c ^= p[i];
    for (int k = 0; k < 8; ++k) { c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u))); }
  }
  return ~c;
}

void put32(std::vector<uint8_t> & v, uint32_t x) {
  for (int s = 24; s >= 0; s -= 8) { v.push_back((uint8_t)(x >> s)); }
}

void chunk(std::vector<uint8_t> & out, const char * type,
    const std::vector<uint8_t> & body) {
  put32(out, (uint32_t)body.size());
  std::vector<uint8_t> t(type, type + 4);
  t.insert(t.end(), body.begin(), body.end());
  out.insert(out.end(), t.begin(), t.end());
  put32(out, crc32_of(t.data(), t.size()));
}

/** zlib stream of stored blocks: no compressor needed to make a fixture. */
std::vector<uint8_t> zlib_stored(const std::vector<uint8_t> & raw) {
  std::vector<uint8_t> z = { 0x78, 0x01 };
  size_t off = 0;
  do {
    const size_t n = std::min<size_t>(raw.size() - off, 65535);
    const bool last = off + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back((uint8_t)n); z.push_back((uint8_t)(n >> 8));
    z.push_back((uint8_t)~n); z.push_back((uint8_t)(~n >> 8));
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    off += n;
  } while (off < raw.size());
  uint32_t a = 1, b = 0;
  for (uint8_t x : raw) { a = (a + x) % 65521u; b = (b + a) % 65521u; }
  put32(z, (b << 16) | a);
  return z;
}

/**
 * A hand-built APNG: the library's own writer emits whole-canvas frames only,
 * which leaves offsets, blend OVER and dispose-to-previous unexercised.  Each
 * frame here is a random rectangle of random RGBA with a random dispose and
 * blend, so a frame that is wrong shows in every frame after it.
 */
std::vector<uint8_t> build_apng(size_t frames) {
  uint32_t seed = 777u;
  std::vector<uint8_t> f;
  const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
  f.insert(f.end(), sig, sig + 8);
  std::vector<uint8_t> b;
  put32(b, kSide); put32(b, kSide);
  b.push_back(8); b.push_back(6); b.push_back(0); b.push_back(0); b.push_back(0);
  chunk(f, "IHDR", b);
  b.clear();
  put32(b, (uint32_t)frames); put32(b, 0);
  chunk(f, "acTL", b);
  uint32_t seq = 0;
  for (size_t k = 0; k < frames; ++k) {
    uint32_t x = 0, y = 0, w = kSide, h = kSide;
    if (k > 0) {
      w = 1 + next_rand(seed) % kSide;
      h = 1 + next_rand(seed) % kSide;
      x = next_rand(seed) % (kSide - w + 1);
      y = next_rand(seed) % (kSide - h + 1);
    }
    b.clear();
    put32(b, seq++); put32(b, w); put32(b, h); put32(b, x); put32(b, y);
    b.push_back(0); b.push_back(10); b.push_back(0); b.push_back(100);
    b.push_back((uint8_t)(next_rand(seed) % 3)); // dispose_op
    b.push_back((uint8_t)(next_rand(seed) % 2)); // blend_op
    chunk(f, "fcTL", b);
    std::vector<uint8_t> raw;
    const uint8_t alphas[4] = { 0, 90, 200, 255 };
    for (uint32_t r = 0; r < h; ++r) {
      raw.push_back(0);
      for (uint32_t c = 0; c < w; ++c) {
        raw.push_back((uint8_t)next_rand(seed));
        raw.push_back((uint8_t)next_rand(seed));
        raw.push_back((uint8_t)next_rand(seed));
        raw.push_back(alphas[next_rand(seed) % 4]);
      }
    }
    std::vector<uint8_t> z = zlib_stored(raw);
    if (k == 0) {
      chunk(f, "IDAT", z);
    }
    else {
      b.clear();
      put32(b, seq++);
      b.insert(b.end(), z.begin(), z.end());
      chunk(f, "fdAT", b);
    }
  }
  chunk(f, "IEND", {});
  return f;
}

/**
 * An animation whose frames are each the previous canvas with a moving patch
 * of translucent pixels on it, so that blend, dispose and sub-rectangles all
 * change what a later frame looks like.
 */
std::vector<uint8_t> build_animation(
    const char * format, size_t frames, bool allow_previous, bool lossy) {
  if (!strcmp(format, "png")) {
    return build_apng(frames);
  }
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, frames), GIMG_OK);
  uint32_t seed = 12345u;
  std::vector<uint8_t> canvas((size_t)kSide * kSide * 4u);
  for (auto & b : canvas) { b = (uint8_t)next_rand(seed); }
  for (size_t k = 0; k < frames; ++k) {
    // A patch of a few pixels: position, size and alpha all vary.
    const uint32_t px = next_rand(seed) % (kSide - 4);
    const uint32_t py = next_rand(seed) % (kSide - 4);
    const uint8_t alphas[3] = { 0, 128, 255 };
    for (uint32_t y = py; y < py + 4; ++y) {
      for (uint32_t x = px; x < px + 4; ++x) {
        uint8_t * p = &canvas[((size_t)y * kSide + x) * 4u];
        p[0] = (uint8_t)next_rand(seed);
        p[1] = (uint8_t)next_rand(seed);
        p[2] = (uint8_t)next_rand(seed);
        p[3] = alphas[next_rand(seed) % 3];
      }
    }
    GIMG_Raster * r = nullptr;
    EXPECT_EQ(gimg_raster_create(kSide, kSide, &GIMG_PIXEL_RGBA8,
                  GIMG_RASTER_OWNED, nullptr, 0, &r),
        GIMG_OK);
    uint8_t * dst = static_cast<uint8_t *>(gimg_raster_pixels(r));
    const size_t stride = gimg_raster_stride_bytes(r);
    for (uint32_t y = 0; y < kSide; ++y) {
      memcpy(dst + y * stride, &canvas[(size_t)y * kSide * 4u], kSide * 4u);
    }
    GIMG_Item * item = gimg_doc_item(doc, k);
    gimg_item_set_raster(item, r);
    gimg_item_set_frame_delay(item, 40, 1000);
    const uint32_t d = next_rand(seed) % (allow_previous ? 3u : 2u);
    gimg_item_set_dispose_op(item,
        d == 0 ? GIMG_DISPOSE_NONE
               : d == 1 ? GIMG_DISPOSE_BACKGROUND : GIMG_DISPOSE_PREVIOUS);
  }
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  if (lossy) {
    opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  }
  opts.webp_effort = 1;
  GIMG_Save_Report report = {};
  EXPECT_EQ(gimg_doc_save(doc, out, format, &opts, &report), GIMG_OK);
  const void * bytes = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &bytes, &n);
  std::vector<uint8_t> v(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return v;
}

std::vector<uint8_t> raster_bytes(GIMG_Raster * r) {
  const size_t w = gimg_raster_width(r);
  const size_t h = gimg_raster_height(r);
  const size_t bpp = gimg_raster_bytes_per_pixel(gimg_raster_format(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  const uint8_t * px = static_cast<const uint8_t *>(gimg_raster_pixels(r));
  std::vector<uint8_t> v;
  for (size_t y = 0; y < h; ++y) {
    v.insert(v.end(), px + y * stride, px + y * stride + w * bpp);
  }
  return v;
}

/** One document loaded and decoded under a counting allocator. */
struct Walk {
  const char * codec_name;
  GIMG_Codec * codec;
  const GIMG_Allocator * saved;
  Failing counter;
  GIMG_Stream * in = nullptr;
  GIMG_Doc * doc = nullptr;
  long load_attempts = 0;

  Walk(const char * name, const std::vector<uint8_t> & bytes)
      : codec_name(name) {
    gimg_test::init(counter);
    codec = gimg_codec_by_name(name);
    saved = codec->allocator;
    codec->allocator = &counter.a;
    EXPECT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &in),
        GIMG_OK);
    EXPECT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
    load_attempts = counter.attempts;
  }
  ~Walk() {
    // The document is freed through the allocator it was built with, so the
    // codec keeps the counter until the document is gone.
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
    codec->allocator = saved;
  }
  size_t frames() const { return gimg_doc_item_count(doc); }
  /** Allocation calls made by decodes so far. */
  long cost() const { return counter.attempts - load_attempts; }
  std::vector<uint8_t> decode(size_t i, const GIMG_Decode_Options * o = nullptr) {
    GIMG_Raster * r = nullptr;
    EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, i), o, &r), GIMG_OK) << i;
    std::vector<uint8_t> v;
    if (r) {
      v = raster_bytes(r);
      gimg_raster_destroy(r);
    }
    return v;
  }
};

struct Case {
  const char * format;
  bool previous;
  bool lossy;
  const char * label;
};

const Case kCases[] = {
  { "png", true, false, "apng" },
  { "webp", false, false, "webp_lossless" },
  { "webp", false, true, "webp_lossy" },
};

class AnimCost : public ::testing::TestWithParam<Case> {};

std::vector<size_t> order_of(const char * kind, size_t n) {
  std::vector<size_t> o;
  if (!strcmp(kind, "sequential")) {
    for (size_t i = 0; i < n; ++i) { o.push_back(i); }
  }
  else if (!strcmp(kind, "reverse")) {
    for (size_t i = n; i-- > 0;) { o.push_back(i); }
  }
  else if (!strcmp(kind, "random")) {
    uint32_t s = 99u;
    for (size_t i = 0; i < 3 * n; ++i) { o.push_back(next_rand(s) % n); }
  }
  else { // "loop": forward twice, then a stride, as a looping player does
    for (size_t pass = 0; pass < 2; ++pass) {
      for (size_t i = 0; i < n; ++i) { o.push_back(i); }
    }
    for (size_t i = 0; i < n; i += 3) { o.push_back(i); }
  }
  return o;
}

} // namespace

TEST_P(AnimCost, EveryOrderGivesTheColdDecode) {
  const Case c = GetParam();
  const size_t n = 24;
  const std::vector<uint8_t> bytes =
      build_animation(c.format, n, c.previous, c.lossy);
  ASSERT_FALSE(bytes.empty());
  std::vector<std::vector<uint8_t>> cold;
  for (size_t i = 0; i < n; ++i) {
    Walk fresh(c.format, bytes);
    ASSERT_EQ(fresh.frames(), n);
    cold.push_back(fresh.decode(i));
    ASSERT_EQ(cold.back().size(), (size_t)kSide * kSide * 4u);
  }
  for (const char * kind : { "sequential", "reverse", "random", "loop" }) {
    Walk w(c.format, bytes);
    for (size_t i : order_of(kind, n)) {
      EXPECT_EQ(w.decode(i), cold[i]) << c.label << " " << kind << " " << i;
    }
  }
}

TEST_P(AnimCost, WalkingTheFileIsLinear) {
  const Case c = GetParam();
  const size_t n = 48;
  const std::vector<uint8_t> bytes =
      build_animation(c.format, n, c.previous, c.lossy);
  ASSERT_FALSE(bytes.empty());

  long cold_last = 0;
  {
    Walk fresh(c.format, bytes);
    fresh.decode(n - 1);
    cold_last = fresh.cost();
  }
  Walk once(c.format, bytes);
  for (size_t i = 0; i < n; ++i) { once.decode(i); }
  const long one_pass = once.cost();
  // A second pass is a looping player's: it starts over at frame 0.
  for (size_t i = 0; i < n; ++i) { once.decode(i); }
  const long two_passes = once.cost();

  std::printf("[cost] %s frames=%zu cold_last=%ld walk=%ld two_walks=%ld\n",
      c.label, n, cold_last, one_pass, two_passes);
  // Cold last frame costs about n frame decodes; a quadratic walk about n/2
  // times that, a linear one about once.  Three leaves room for the cache's
  // own allocations and none for a quadratic.
  EXPECT_LE(one_pass, 3 * cold_last) << c.label;
  EXPECT_LE(two_passes, 6 * cold_last) << c.label;
}

TEST_P(AnimCost, CacheRespectsTheMemoryCap) {
  const Case c = GetParam();
  const size_t n = 12;
  const std::vector<uint8_t> bytes =
      build_animation(c.format, n, c.previous, c.lossy);
  GIMG_Limits limits = {};
  limits.max_memory = 4u << 20; // every allocation here fits
  GIMG_Decode_Options o = {};
  o.limits = &limits;
  Walk w(c.format, bytes);
  Walk ref(c.format, bytes);
  for (size_t i = 0; i < n; ++i) {
    EXPECT_EQ(w.decode(i, &o), ref.decode(i)) << i;
  }
  // A cap below the canvas refuses, as it always did, cached or not.
  limits.max_memory = 64;
  GIMG_Raster * r = nullptr;
  EXPECT_NE(gimg_item_decode(gimg_doc_item(w.doc, n - 1), &o, &r), GIMG_OK);
  if (r) { gimg_raster_destroy(r); }
}

INSTANTIATE_TEST_SUITE_P(Formats, AnimCost, ::testing::ValuesIn(kCases),
    [](const ::testing::TestParamInfo<Case> & i) {
      return std::string(i.param.label);
    });

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
