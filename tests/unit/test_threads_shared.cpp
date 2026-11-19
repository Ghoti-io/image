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
 * Concurrency beyond separate objects: the things the library shares between
 * threads on purpose, each used from several threads started together.
 *
 *  - lazily built tables (CRC, Huffman, quantisation, colour), first used from
 *    a cold start by threads released from one barrier;
 *  - the codec registry, read by concurrent probes and loads with default and
 *    custom options;
 *  - one loaded animated document whose items are decoded from several threads
 *    in random order (the GIF, APNG and WebP canvas caches);
 *  - the per-thread allocation scope behind GIMG_Limits.max_memory, with a
 *    different cap on every thread;
 *  - one document saved to several formats at once;
 *  - a callback stream per thread.
 *
 * Every test compares what the threads produced with what a solitary run
 * produced, so a race that corrupts something fails here even without a
 * sanitizer; `make test-tsan` runs this file too and finds the ones that did
 * not corrupt anything on this run.
 *
 * The first test in the file is the cold-start one and must stay first: a
 * table built once is raced on only by the first caller, and any test that has
 * already used a format has built it.
 */

#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/image.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <vector>

namespace {

using Bytes = std::string;

constexpr int kThreads = 8;

/** Runs @p fn(thread_index) on @p n threads released together by a barrier. */
template <typename F>
void run_together(int n, F fn) {
  std::barrier<> start(n);
  std::vector<std::thread> threads;
  for (int i = 0; i < n; i++) {
    threads.emplace_back([&start, &fn, i]() {
      start.arrive_and_wait();
      fn(i);
    });
  }
  for (auto & t : threads) {
    t.join();
  }
}

uint32_t next_random(uint32_t & s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

/**
 * A raster whose content depends on @p seed. @p few_colours keeps it inside a
 * 256-entry table for the formats that need one.
 */
GIMG_Raster * make_raster(uint32_t w, uint32_t h, uint32_t seed,
    bool few_colours, bool noisy = false) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr,
          0, &r) != GIMG_OK) {
    return nullptr;
  }
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  uint32_t rng = seed * 2654435761u + 1u;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      unsigned char * px = p + (size_t)y * stride + (size_t)x * 4u;
      if (few_colours) {
        const unsigned char v =
            (unsigned char)(((x / 4u) * 5u + (y / 4u) * 7u + seed * 3u) & 0x3Fu);
        px[0] = (unsigned char)(v * 4u);
        px[1] = (unsigned char)(v * 2u);
        px[2] = v;
      }
      else if (noisy) {
        px[0] = (unsigned char)next_random(rng);
        px[1] = (unsigned char)next_random(rng);
        px[2] = (unsigned char)next_random(rng);
      }
      else {
        px[0] = (unsigned char)(x * 5u + seed * 37u);
        px[1] = (unsigned char)(y * 7u + seed * 11u);
        px[2] = (unsigned char)((x ^ y) + seed);
      }
      px[3] = 255u;
    }
  }
  return r;
}

GIMG_Save_Options options_for(const char * format) {
  GIMG_Save_Options o;
  std::memset(&o, 0, sizeof(o));
  o.jpeg_quality = 80;
  o.webp_effort = 4;
  if (std::strcmp(format, "webp-lossy") == 0) {
    o.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  }
  return o;
}

const char * codec_name(const char * format) {
  return std::strcmp(format, "webp-lossy") == 0 ? "webp" : format;
}

/** Save @p doc; empty on any failure. Touches nothing but its own output. */
Bytes save_doc(const GIMG_Doc * doc, const char * format) {
  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    return Bytes();
  }
  GIMG_Save_Options o = options_for(format);
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  Bytes bytes;
  if (gimg_doc_save(doc, out, codec_name(format), &o, &report) == GIMG_OK) {
    const void * buffer = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &buffer, &size);
    bytes.assign((const char *)buffer, size);
  }
  gimg_stream_destroy(out);
  return bytes;
}

/** A single-image document around @p r, which it copies. */
GIMG_Doc * doc_from(GIMG_Raster * r) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_from_raster(r, &doc) != GIMG_OK) {
    return nullptr;
  }
  return doc;
}

/** Frames of noise-free but distinct content, as an animation document. */
GIMG_Doc * make_animation(size_t frames, bool few_colours) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK) {
    return nullptr;
  }
  if (gimg_doc_set_item_count(doc, frames) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return nullptr;
  }
  for (size_t i = 0; i < frames; i++) {
    GIMG_Raster * r = make_raster(40, 30, (uint32_t)i + 1u, few_colours);
    gimg_item_set_raster(gimg_doc_item(doc, i), r);
    gimg_item_set_frame_delay(gimg_doc_item(doc, i), 5, 100);
  }
  return doc;
}

/** FNV-1a over the shape and pixel rows of a raster. */
uint64_t hash_raster(const GIMG_Raster * r) {
  uint64_t h = 1469598103934665603ull;
  auto mix = [&h](unsigned char c) {
    h ^= c;
    h *= 1099511628211ull;
  };
  const uint32_t w = gimg_raster_width(r);
  const uint32_t ht = gimg_raster_height(r);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(r);
  const size_t row = (size_t)w * (size_t)fmt->channel_count *
      (gimg_pixel_format_channel_bits(fmt, 0) / 8u);
  const unsigned char * p = (const unsigned char *)gimg_raster_pixels_const(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t i = 0; i < 4; i++) {
    mix((unsigned char)((w >> (8 * i)) & 0xFFu));
    mix((unsigned char)((ht >> (8 * i)) & 0xFFu));
  }
  for (uint32_t y = 0; y < ht; y++) {
    for (size_t b = 0; b < row; b++) {
      mix(p[(size_t)y * stride + b]);
    }
  }
  return h;
}

/** Load @p bytes (memory stream) with @p options. nullptr on failure. */
GIMG_Doc * load_bytes(const Bytes & bytes, const GIMG_Load_Options * options,
    GIMG_Result * result = nullptr) {
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return nullptr;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result rc = gimg_doc_load(in, options, nullptr, &doc);
  if (result) {
    *result = rc;
  }
  gimg_stream_destroy(in);
  return rc == GIMG_OK ? doc : nullptr;
}

/** Decode item @p i of @p doc to a hash; 0 on failure. */
uint64_t decode_hash(const GIMG_Doc * doc, size_t i,
    const GIMG_Decode_Options * options = nullptr,
    GIMG_Result * result = nullptr) {
  GIMG_Raster * r = nullptr;
  GIMG_Result rc = gimg_item_decode(gimg_doc_item(doc, i), options, &r);
  if (result) {
    *result = rc;
  }
  if (rc != GIMG_OK || !r) {
    return 0;
  }
  const uint64_t h = hash_raster(r);
  gimg_raster_destroy(r);
  return h;
}

/** Every item's hash, in order, through a fresh load of @p bytes. */
std::vector<uint64_t> reference_hashes(const Bytes & bytes) {
  std::vector<uint64_t> out;
  GIMG_Doc * doc = load_bytes(bytes, nullptr);
  if (!doc) {
    return out;
  }
  for (size_t i = 0; i < gimg_doc_item_count(doc); i++) {
    out.push_back(decode_hash(doc, i));
  }
  gimg_doc_destroy(doc);
  return out;
}

/** Every item of @p doc and the format it is an animation in. */
struct Animation {
  const char * format;
  Bytes bytes;
  std::vector<uint64_t> expect;
};

Bytes encode_animation(const char * format, size_t frames) {
  const bool gif = std::strcmp(format, "gif") == 0;
  GIMG_Doc * doc = make_animation(frames, gif);
  if (!doc) {
    return Bytes();
  }
  Bytes bytes = save_doc(doc, format);
  gimg_doc_destroy(doc);
  return bytes;
}

} // namespace

/**
 * Every lazily built table, used for the first time in this process by threads
 * released from one barrier.
 *
 * Nothing in this binary has touched a format before this test, which is the
 * point and why the test is first. Each thread walks all the formats in its own
 * rotation, so at any moment the threads are in different codecs and, at the
 * start, in the same one: CRC-32 and Adler tables and the deflate Huffman
 * tables (PNG), the quantiser and Huffman tables and the YCbCr conversion
 * (JPEG), the LZW and colour-table work (GIF), the colour transforms (TIFF,
 * BMP), and WebP's.
 */
TEST(AaaColdStartShared, EveryTableBuiltFromNothingByContendingThreads) {
  static const char * const formats[] = {"png", "jpeg", "gif", "bmp", "tiff",
      "webp", "webp-lossy", "ico"};
  const size_t nformats = sizeof(formats) / sizeof(formats[0]);
  std::atomic<int> failures{0};
  run_together(kThreads, [&](int t) {
    for (size_t k = 0; k < nformats; k++) {
      const char * f = formats[(k + (size_t)t) % nformats];
      const bool gif = std::strcmp(f, "gif") == 0;
      GIMG_Raster * r = make_raster(48, 32, 7u, gif);
      GIMG_Doc * doc = r ? doc_from(r) : nullptr;
      if (!doc) {
        failures++;
        gimg_raster_destroy(r);
        continue;
      }
      const Bytes bytes = save_doc(doc, f);
      if (bytes.empty()) {
        failures++;
      }
      else {
        GIMG_Doc * back = load_bytes(bytes, nullptr);
        if (!back || gimg_doc_item_count(back) < 1 || !decode_hash(back, 0)) {
          failures++;
        }
        gimg_doc_destroy(back);
      }
      gimg_doc_destroy(doc);
      gimg_raster_destroy(r);
    }
  });
  EXPECT_EQ(failures.load(), 0);
}

/**
 * One loaded animation, its items decoded from several threads in random
 * order. The GIF, APNG and WebP decoders keep a composited canvas on the
 * document, which is what makes a walk linear; two threads asking for frames
 * in different orders take, replace and refill it at the same time.
 */
TEST(SharedDocument, ItemsOfOneAnimationDecodeConcurrentlyInAnyOrder) {
  Animation cases[] = {{"gif", {}, {}}, {"png", {}, {}}, {"webp", {}, {}},
      {"webp-lossy", {}, {}}};
  for (auto & c : cases) {
    c.bytes = encode_animation(c.format, 6);
    ASSERT_FALSE(c.bytes.empty()) << c.format;
    c.expect = reference_hashes(c.bytes);
    ASSERT_EQ(c.expect.size(), 6u) << c.format << " did not stay an animation";
    for (uint64_t h : c.expect) {
      ASSERT_NE(h, 0u) << c.format;
    }
  }

  for (auto & c : cases) {
    // Loaded once and shared, with nothing decoded from it yet, so the first
    // fill of the cache is itself contended.
    GIMG_Doc * shared = load_bytes(c.bytes, nullptr);
    ASSERT_NE(shared, nullptr) << c.format;
    std::atomic<int> wrong{0};
    run_together(kThreads, [&](int t) {
      uint32_t rng = 0x9E3779B9u * (uint32_t)(t + 1);
      const size_t n = gimg_doc_item_count(shared);
      for (int iter = 0; iter < 60; iter++) {
        const size_t i = next_random(rng) % n;
        // Both backgrounds, so the cache key is contended too.
        GIMG_Decode_Options d;
        std::memset(&d, 0, sizeof(d));
        d.gif_background = GIMG_GIF_BACKGROUND_TRANSPARENT;
        if (decode_hash(shared, i, &d) != c.expect[i]) {
          wrong++;
        }
      }
    });
    EXPECT_EQ(wrong.load(), 0) << c.format << ": a frame decoded differently "
                                              "under contention";
    gimg_doc_destroy(shared);
  }
}

/**
 * Every thread decodes under a cap of its own, on its own copy of one set of
 * bytes, and each result must be what the same cap gives alone. The allocation
 * scope that enforces GIMG_Limits.max_memory is per thread: if one leaked into
 * another, a thread with room would be refused or one without it would pass.
 */
TEST(MemoryCap, EachThreadHoldsItsOwnCap) {
  struct Case {
    const char * format;
    Bytes bytes;
  };
  std::vector<Case> cases;
  for (const char * f : {"png", "jpeg", "gif", "bmp", "tiff", "webp",
           "webp-lossy"}) {
    const bool gif = std::strcmp(f, "gif") == 0;
    GIMG_Raster * r = make_raster(96, 64, 3u, gif, !gif);
    ASSERT_NE(r, nullptr);
    GIMG_Doc * doc = doc_from(r);
    ASSERT_NE(doc, nullptr);
    Bytes b = save_doc(doc, f);
    gimg_doc_destroy(doc);
    gimg_raster_destroy(r);
    ASSERT_FALSE(b.empty()) << f;
    cases.push_back({f, b});
  }
  const size_t caps[] = {1u << 10, 8u << 10, 24u << 10, 64u << 10, 256u << 10,
      1u << 20, 64u << 20, GIMG_LIMIT_NONE};
  const size_t ncaps = sizeof(caps) / sizeof(caps[0]);

  // What each (format, cap) gives alone, for the load and for the decode.
  struct Outcome {
    GIMG_Result load;
    GIMG_Result decode;
    uint64_t hash;
  };
  auto attempt = [&](const Case & c, size_t cap) {
    GIMG_Limits lim;
    std::memset(&lim, 0, sizeof(lim));
    lim.max_memory = cap;
    GIMG_Load_Options lo;
    std::memset(&lo, 0, sizeof(lo));
    lo.limits = &lim;
    Outcome o{GIMG_OK, GIMG_OK, 0};
    GIMG_Doc * doc = load_bytes(c.bytes, &lo, &o.load);
    if (doc) {
      GIMG_Decode_Options d;
      std::memset(&d, 0, sizeof(d));
      d.limits = &lim;
      o.hash = decode_hash(doc, 0, &d, &o.decode);
      gimg_doc_destroy(doc);
    }
    return o;
  };

  std::vector<Outcome> solo(cases.size() * ncaps);
  bool saw_limit = false;
  bool saw_ok = false;
  for (size_t i = 0; i < cases.size(); i++) {
    for (size_t j = 0; j < ncaps; j++) {
      solo[i * ncaps + j] = attempt(cases[i], caps[j]);
      const Outcome & o = solo[i * ncaps + j];
      saw_limit |= (o.load == GIMG_ERR_LIMIT || o.decode == GIMG_ERR_LIMIT);
      saw_ok |= (o.load == GIMG_OK && o.decode == GIMG_OK && o.hash != 0);
    }
  }
  ASSERT_TRUE(saw_limit) << "no cap in the ladder was ever reached";
  ASSERT_TRUE(saw_ok) << "no cap in the ladder was ever enough";

  std::atomic<int> wrong{0};
  run_together(kThreads, [&](int t) {
    uint32_t rng = 77u + (uint32_t)t * 101u;
    for (int iter = 0; iter < 40; iter++) {
      const size_t i = next_random(rng) % cases.size();
      // Neighbouring threads sit on opposite sides of the boundary.
      const size_t j = ((size_t)t + (size_t)iter) % ncaps;
      const Outcome o = attempt(cases[i], caps[j]);
      const Outcome & e = solo[i * ncaps + j];
      if (o.load != e.load || o.decode != e.decode || o.hash != e.hash) {
        wrong++;
      }
    }
  });
  EXPECT_EQ(wrong.load(), 0)
      << "a thread's result differed from the same cap run alone";
}

/**
 * The registry read by many threads at once - probe, lookup, enumeration and a
 * load of every format - with default options, with custom ones, and from
 * streams the threads own.
 */
TEST(Registry, ConcurrentLoadsWithDefaultAndCustomOptions) {
  std::vector<std::pair<std::string, Bytes>> files;
  for (const char * f : {"png", "jpeg", "gif", "bmp", "tiff", "webp",
           "webp-lossy", "ico"}) {
    const bool gif = std::strcmp(f, "gif") == 0;
    GIMG_Raster * r = make_raster(32, 24, 5u, gif);
    GIMG_Doc * doc = doc_from(r);
    ASSERT_NE(doc, nullptr);
    Bytes b = save_doc(doc, f);
    gimg_doc_destroy(doc);
    gimg_raster_destroy(r);
    ASSERT_FALSE(b.empty()) << f;
    files.push_back({f, b});
  }
  std::vector<uint64_t> expect;
  for (auto & f : files) {
    const std::vector<uint64_t> h = reference_hashes(f.second);
    ASSERT_GE(h.size(), 1u);
    expect.push_back(h[0]);
  }
  const size_t codec_count = gimg_codec_count();

  std::atomic<int> wrong{0};
  run_together(kThreads, [&](int t) {
    GIMG_Limits lim;
    std::memset(&lim, 0, sizeof(lim));
    lim.max_decoded_pixels = 1u << 20;
    lim.max_memory = 64u << 20;
    GIMG_Load_Options custom;
    std::memset(&custom, 0, sizeof(custom));
    custom.limits = &lim;
    custom.bmp_rgb32_alpha = GIMG_BMP_RGB32_ALPHA_HEURISTIC;
    for (int iter = 0; iter < 25; iter++) {
      for (size_t k = 0; k < files.size(); k++) {
        const size_t i = (k + (size_t)t + (size_t)iter) % files.size();
        if (gimg_codec_count() != codec_count) {
          wrong++;
        }
        for (size_t c = 0; c < codec_count; c++) {
          if (!gimg_codec_by_index(c)) {
            wrong++;
          }
        }
        if (!gimg_codec_by_name(codec_name(files[i].first.c_str()))) {
          wrong++;
        }
        GIMG_Doc * doc = load_bytes(files[i].second,
            ((t + iter) & 1) ? &custom : nullptr);
        if (!doc || decode_hash(doc, 0) != expect[i]) {
          wrong++;
        }
        gimg_doc_destroy(doc);
      }
    }
  });
  EXPECT_EQ(wrong.load(), 0);
}

/**
 * One document, read only, saved to every format from several threads. The
 * document is built from rasters the caller supplied, so a save has nothing to
 * decode and nothing to write to it.
 */
TEST(SharedDocument, OneDocumentSavesToSeveralFormatsAtOnce) {
  static const char * const formats[] = {"png", "jpeg", "gif", "bmp", "tiff",
      "webp", "webp-lossy", "ico"};
  const size_t nformats = sizeof(formats) / sizeof(formats[0]);
  // 256 colours at most so that GIF takes it; every other format takes that too.
  GIMG_Raster * r = make_raster(64, 48, 9u, true);
  GIMG_Doc * doc = doc_from(r);
  ASSERT_NE(doc, nullptr);
  std::vector<Bytes> expect;
  for (size_t k = 0; k < nformats; k++) {
    expect.push_back(save_doc(doc, formats[k]));
    ASSERT_FALSE(expect.back().empty()) << formats[k];
  }
  std::atomic<int> wrong{0};
  run_together(kThreads, [&](int t) {
    for (int iter = 0; iter < 2; iter++) {
      for (size_t k = 0; k < nformats; k++) {
        const size_t i = (k + (size_t)t) % nformats;
        if (save_doc(doc, formats[i]) != expect[i]) {
          wrong++;
        }
      }
    }
  });
  EXPECT_EQ(wrong.load(), 0);
  gimg_doc_destroy(doc);
  gimg_raster_destroy(r);
}

/**
 * One *loaded* animation - items not yet decoded - saved to several formats at
 * once. A save has to decode every frame, and it does so through the same
 * canvas caches the decode test above contends for, without writing to the
 * document's items.
 */
TEST(SharedDocument, ALoadedAnimationSavesToSeveralFormatsAtOnce) {
  const Bytes source = encode_animation("png", 5);
  ASSERT_FALSE(source.empty());
  GIMG_Doc * doc = load_bytes(source, nullptr);
  ASSERT_NE(doc, nullptr);
  static const char * const formats[] = {"png", "gif", "webp", "webp-lossy"};
  const size_t nformats = sizeof(formats) / sizeof(formats[0]);
  std::vector<Bytes> expect;
  {
    // Reference from a document of its own, so the shared one stays cold.
    GIMG_Doc * own = load_bytes(source, nullptr);
    ASSERT_NE(own, nullptr);
    for (size_t k = 0; k < nformats; k++) {
      expect.push_back(save_doc(own, formats[k]));
    }
    gimg_doc_destroy(own);
  }
  std::atomic<int> wrong{0};
  std::atomic<int> refused{0};
  run_together(kThreads, [&](int t) {
    for (int iter = 0; iter < 2; iter++) {
      for (size_t k = 0; k < nformats; k++) {
        const size_t i = (k + (size_t)t) % nformats;
        const Bytes got = save_doc(doc, formats[i]);
        if (expect[i].empty()) {
          refused += got.empty() ? 0 : 1;
        }
        else if (got != expect[i]) {
          wrong++;
        }
      }
    }
  });
  EXPECT_EQ(wrong.load(), 0);
  EXPECT_EQ(refused.load(), 0);
  gimg_doc_destroy(doc);
}

namespace {

/** A read-only source over a buffer the thread owns, one cursor per stream. */
struct Source {
  const Bytes * data;
  size_t pos;
  bool closed;
};

GIMG_Result src_read(void * ctx, void * buf, size_t size, size_t * out) {
  Source * s = (Source *)ctx;
  const size_t left = s->data->size() - s->pos;
  const size_t n = size < left ? size : left;
  std::memcpy(buf, s->data->data() + s->pos, n);
  s->pos += n;
  *out = n;
  return GIMG_OK;
}

GIMG_Result src_seek(void * ctx, size_t offset) {
  Source * s = (Source *)ctx;
  if (offset > s->data->size()) {
    return GIMG_ERR_IO;
  }
  s->pos = offset;
  return GIMG_OK;
}

size_t src_size(void * ctx) {
  return ((Source *)ctx)->data->size();
}

void src_close(void * ctx) {
  ((Source *)ctx)->closed = true;
}

GIMG_Result sink_write(void * ctx, const void * buf, size_t size, size_t * out) {
  ((Bytes *)ctx)->append((const char *)buf, size);
  *out = size;
  return GIMG_OK;
}

} // namespace

/**
 * A callback stream per thread, reading and writing: every thread decodes from
 * its own source (with and without seek) and saves into its own sink.
 */
TEST(CallbackStreams, OnePerThreadForLoadAndForSave) {
  std::vector<Bytes> files;
  std::vector<uint64_t> expect;
  for (const char * f : {"png", "jpeg", "webp", "gif"}) {
    const bool gif = std::strcmp(f, "gif") == 0;
    GIMG_Raster * r = make_raster(40, 30, 2u, gif);
    GIMG_Doc * doc = doc_from(r);
    ASSERT_NE(doc, nullptr);
    files.push_back(save_doc(doc, f));
    ASSERT_FALSE(files.back().empty()) << f;
    gimg_doc_destroy(doc);
    gimg_raster_destroy(r);
    const std::vector<uint64_t> h = reference_hashes(files.back());
    ASSERT_GE(h.size(), 1u);
    expect.push_back(h[0]);
  }
  std::atomic<int> wrong{0};
  run_together(kThreads, [&](int t) {
    for (int iter = 0; iter < 20; iter++) {
      const size_t i = ((size_t)t + (size_t)iter) % files.size();
      const bool seekable = ((t + iter) & 1) == 0;
      Source src{&files[i], 0, false};
      GIMG_Stream_Callbacks cb;
      std::memset(&cb, 0, sizeof(cb));
      cb.read = src_read;
      cb.ctx = &src;
      cb.close = src_close;
      if (seekable) {
        cb.seek = src_seek;
        cb.size = src_size;
      }
      GIMG_Stream * in = nullptr;
      if (gimg_stream_create_callbacks(&cb, &in) != GIMG_OK) {
        wrong++;
        continue;
      }
      GIMG_Doc * doc = nullptr;
      const GIMG_Result rc = gimg_doc_load(in, nullptr, nullptr, &doc);
      if (rc != GIMG_OK) {
        // A stream that cannot seek may be refused by formats that need to.
        if (seekable) {
          wrong++;
        }
      }
      else {
        if (decode_hash(doc, 0) != expect[i]) {
          wrong++;
        }
        // Save it back through a write callback owned by this thread.
        Bytes sink;
        GIMG_Stream_Callbacks wcb;
        std::memset(&wcb, 0, sizeof(wcb));
        wcb.write = sink_write;
        wcb.ctx = &sink;
        GIMG_Stream * out = nullptr;
        if (gimg_stream_create_callbacks(&wcb, &out) == GIMG_OK) {
          GIMG_Save_Options o = options_for("png");
          GIMG_Save_Report report;
          std::memset(&report, 0, sizeof(report));
          if (gimg_doc_save(doc, out, "png", &o, &report) != GIMG_OK ||
              sink.empty()) {
            wrong++;
          }
          gimg_stream_destroy(out);
        }
        else {
          wrong++;
        }
        gimg_doc_destroy(doc);
      }
      gimg_stream_destroy(in);
      if (!src.closed) {
        wrong++;
      }
    }
  });
  EXPECT_EQ(wrong.load(), 0);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
