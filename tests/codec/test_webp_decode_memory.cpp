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
 * What it costs, in memory, to decode a still WebP, and that a hostile header
 * cannot make the decoder ask for more than the limits allow.
 *
 * The cost is measured through an allocator that tracks live bytes and their
 * high-water mark, swapped into the codec the way the animation-cost test
 * swaps a counting one.  The necessary minimum of a decode is its output
 * raster (four bytes a pixel) plus a working set; the bounds here are stated
 * against the raster, so they hold at every size.  Set GIMG_TEST_MEM_REPORT
 * to print the measured figures.
 *
 * The hostile files are assembled by hand: a 16384 x 16384 lossless header
 * whose codes make every pixel free, a lossy key frame of that size, a
 * 65536-group meta image in a file with no room for the groups, and a colour
 * cache of 2^15 entries.  Each is decoded with a small max_memory and is
 * required to be refused without a request over the cap reaching the
 * allocator, and the small ones without a request over a few tens of
 * kilobytes.
 */

#include <cmath>
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

/** An allocator that knows the size of every live block. */
struct Track {
  GIMG_Allocator a{};
  size_t live = 0;
  size_t peak = 0;
  size_t max_request = 0;
  long blocks = 0;

  static constexpr size_t kHead = 16;

  static void note(Track * t, size_t request) {
    if (request > t->max_request) {
      t->max_request = request;
    }
    if (t->live > t->peak) {
      t->peak = t->live;
    }
  }
  static void * m(void * ctx, size_t n) {
    Track * t = static_cast<Track *>(ctx);
    note(t, n);
    uint8_t * p = static_cast<uint8_t *>(std::malloc(n + kHead));
    if (!p) {
      return nullptr;
    }
    std::memcpy(p, &n, sizeof(n));
    t->live += n;
    t->blocks++;
    note(t, 0);
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
    t->blocks--;
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
    note(t, n);
    // A block that grows is briefly two blocks to an allocator that cannot
    // grow in place, so count it that way.
    if (n > old) {
      t->live += n;
      note(t, 0);
      t->live -= n;
    }
    uint8_t * q = static_cast<uint8_t *>(std::realloc(base, n + kHead));
    if (!q) {
      return nullptr;
    }
    std::memcpy(q, &n, sizeof(n));
    t->live = t->live - old + n;
    note(t, 0);
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

/** Swaps a tracker into the WebP codec for the life of the object. */
struct Installed {
  GIMG_Codec * codec;
  const GIMG_Allocator * saved;
  explicit Installed(Track & t) : codec(gimg_codec_by_name("webp")) {
    saved = codec->allocator;
    codec->allocator = &t.a;
  }
  ~Installed() { codec->allocator = saved; }
};

/** Mean absolute error over the colour samples, which lossy coding bounds. */
double mean_rgb_error(const Bytes & a, const Bytes & b) {
  if (a.size() != b.size() || a.empty()) {
    return 1e9;
  }
  double sum = 0;
  for (size_t i = 0; i < a.size(); i += 4) {
    for (size_t k = 0; k < 3; ++k) {
      sum += std::abs(static_cast<int>(a[i + k]) - static_cast<int>(b[i + k]));
    }
  }
  return sum / (static_cast<double>(a.size() / 4u) * 3.0);
}

Bytes raster_bytes(GIMG_Raster * r) {
  const size_t w = gimg_raster_width(r);
  const size_t h = gimg_raster_height(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  const uint8_t * px = static_cast<const uint8_t *>(gimg_raster_pixels(r));
  Bytes v;
  for (size_t y = 0; y < h; ++y) {
    v.insert(v.end(), px + y * stride, px + y * stride + w * 4u);
  }
  return v;
}

bool report_on() { return std::getenv("GIMG_TEST_MEM_REPORT") != nullptr; }

enum class Kind { Photo, Palette, AlphaPhoto };

/** A deterministic picture with smooth areas, edges and some noise. */
Bytes make_pixels(uint32_t w, uint32_t h, Kind kind) {
  Bytes v(static_cast<size_t>(w) * h * 4u);
  uint32_t s = 12345u;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      s = s * 1664525u + 1013904223u;
      const uint32_t noise = (s >> 24) & 7u;
      uint8_t * p = &v[(static_cast<size_t>(y) * w + x) * 4u];
      if (kind == Kind::Palette) {
        static const uint8_t pal[4][3] = { { 0, 0, 0 }, { 255, 0, 0 },
          { 0, 200, 40 }, { 20, 30, 250 } };
        const unsigned i = ((x / 7u) + (y / 5u)) & 3u;
        p[0] = pal[i][0];
        p[1] = pal[i][1];
        p[2] = pal[i][2];
        p[3] = 255;
        continue;
      }
      p[0] = static_cast<uint8_t>((x * 255u) / w + noise);
      p[1] = static_cast<uint8_t>((y * 255u) / h + noise);
      p[2] = static_cast<uint8_t>(((x ^ y) & 64u) ? 200u + noise : 40u + noise);
      p[3] = kind == Kind::AlphaPhoto
          ? static_cast<uint8_t>(((x + y) * 255u) / (w + h))
          : 255;
    }
  }
  return v;
}

GIMG_Raster * raster_of(uint32_t w, uint32_t h, const Bytes & px) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  uint8_t * dst = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < h; ++y) {
    std::memcpy(dst + y * stride, &px[static_cast<size_t>(y) * w * 4u],
        static_cast<size_t>(w) * 4u);
  }
  return r;
}

Bytes encode(uint32_t w, uint32_t h, const Bytes & px, bool lossy) {
  GIMG_Raster * r = raster_of(w, h, px);
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
  opts.webp_effort = 6;
  opts.webp_exact = 1;
  EXPECT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);
  const void * b = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &b, &n);
  Bytes file(static_cast<const uint8_t *>(b), static_cast<const uint8_t *>(b) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return file;
}

/** What one load and one decode cost, in bytes above what was live before. */
struct Cost {
  GIMG_Result load = GIMG_OK;
  GIMG_Result decode = GIMG_OK;
  size_t peak = 0;       ///< High-water mark of the decode, above its start.
  size_t retained = 0;   ///< Live after the decode returned, above its start.
  size_t max_request = 0;///< Largest single request the codec made, load included.
  size_t file_live = 0;  ///< What the loaded document holds before a decode.
  Bytes pixels;
};

Cost measure(const Bytes & file, size_t max_memory = 0,
    size_t max_pixels = 0) {
  Cost c;
  Track t;
  Installed inst(t);
  GIMG_Stream * in = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in), GIMG_OK);
  GIMG_Limits lim = {};
  lim.max_memory = max_memory;
  lim.max_decoded_pixels = max_pixels;
  GIMG_Load_Options lo = {};
  lo.limits = &lim;
  GIMG_Doc * doc = nullptr;
  c.load = gimg_doc_load(in, &lo, nullptr, &doc);
  if (c.load == GIMG_OK) {
    c.file_live = t.live;
    t.peak = t.live;
    const size_t base = t.live;
    GIMG_Raster * r = nullptr;
    c.decode = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r);
    c.peak = t.peak - base;
    c.retained = t.live - base;
    if (r) {
      c.pixels = raster_bytes(r);
      gimg_raster_destroy(r);
    }
  }
  c.max_request = t.max_request;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  EXPECT_EQ(t.live, 0u) << "a block outlived the document";
  return c;
}

void print(const char * what, uint32_t w, uint32_t h, const Cost & c) {
  if (!report_on()) {
    return;
  }
  std::printf("MEM %-14s %5ux%-5u raster=%zu peak=%zu (%.2fx) retained=%zu "
              "max_request=%zu\n",
      what, w, h, static_cast<size_t>(w) * h * 4u, c.peak,
      static_cast<double>(c.peak) / (static_cast<double>(w) * h * 4.0),
      c.retained, c.max_request);
}

/** Bit writer, least significant bit first, as VP8L reads. */
struct Bits {
  Bytes b;
  uint64_t acc = 0;
  int n = 0;
  void put(uint32_t v, int bits) {
    acc |= static_cast<uint64_t>(v) << n;
    n += bits;
    while (n >= 8) {
      b.push_back(static_cast<uint8_t>(acc));
      acc >>= 8;
      n -= 8;
    }
  }
  Bytes done() {
    if (n > 0) {
      b.push_back(static_cast<uint8_t>(acc));
      acc = 0;
      n = 0;
    }
    return b;
  }
};

/** A simple prefix code naming one symbol: every pixel through it is free. */
void simple_code(Bits & w, uint32_t symbol) {
  w.put(1, 1);       // simple
  w.put(0, 1);       // one symbol
  w.put(1, 1);       // an eight-bit symbol
  w.put(symbol, 8);
}

void lossless_header(Bits & w, uint32_t width, uint32_t height) {
  w.put(0x2f, 8);
  w.put(width - 1u, 14);
  w.put(height - 1u, 14);
  w.put(0, 1);
  w.put(0, 3);
}

void put32le(Bytes & v, uint32_t x) {
  for (int s = 0; s < 32; s += 8) {
    v.push_back(static_cast<uint8_t>(x >> s));
  }
}

/** A still WebP holding one chunk. */
Bytes riff(const char * fourcc, const Bytes & payload) {
  Bytes f = { 'R', 'I', 'F', 'F' };
  const size_t padded = payload.size() + (payload.size() & 1u);
  put32le(f, static_cast<uint32_t>(4u + 8u + padded));
  f.insert(f.end(), { 'W', 'E', 'B', 'P' });
  f.insert(f.end(), fourcc, fourcc + 4);
  put32le(f, static_cast<uint32_t>(payload.size()));
  f.insert(f.end(), payload.begin(), payload.end());
  if (payload.size() & 1u) {
    f.push_back(0);
  }
  return f;
}

// ---------------------------------------------------------------------------
// Peak and retained memory of a decode
// ---------------------------------------------------------------------------

constexpr uint32_t kW = 512;
constexpr uint32_t kH = 384;
constexpr size_t kRaster = static_cast<size_t>(kW) * kH * 4u;

TEST(WebpDecodeMemory, LosslessWithTransformsHoldsTheImageOnce) {
  const Bytes px = make_pixels(kW, kH, Kind::Photo);
  const Cost c = measure(encode(kW, kH, px, false));
  print("lossless", kW, kH, c);
  ASSERT_EQ(c.decode, GIMG_OK);
  EXPECT_EQ(c.pixels, px) << "a lossless file decodes to its source";
  // The output raster and a small working set: not a second or third copy.
  EXPECT_LE(c.peak, kRaster + kRaster / 4u);
  EXPECT_LE(c.retained, kRaster + 1024u);
  EXPECT_LE(c.max_request, kRaster);
}

TEST(WebpDecodeMemory, LosslessPaletteHoldsTheImageOnce) {
  const Bytes px = make_pixels(kW, kH, Kind::Palette);
  const Cost c = measure(encode(kW, kH, px, false));
  print("lossless-pal", kW, kH, c);
  ASSERT_EQ(c.decode, GIMG_OK);
  EXPECT_EQ(c.pixels, px);
  EXPECT_LE(c.peak, kRaster + kRaster / 4u);
  EXPECT_LE(c.retained, kRaster + 1024u);
}

TEST(WebpDecodeMemory, LossyHoldsPlanesBesideTheRasterOnly) {
  const Bytes px = make_pixels(kW, kH, Kind::Photo);
  const Cost c = measure(encode(kW, kH, px, true));
  print("lossy", kW, kH, c);
  ASSERT_EQ(c.decode, GIMG_OK);
  // Three planes at 1.5 bytes a pixel and the raster at 4; the three
  // reference pictures a still frame never reads are not among them.
  EXPECT_LE(c.peak, kRaster + kRaster * 3u / 4u);
  EXPECT_LE(c.retained, kRaster + 1024u);
  EXPECT_LE(c.max_request, kRaster);
  EXPECT_LT(mean_rgb_error(c.pixels, px), 8.0) << "a lossy decode of the source";
}

TEST(WebpDecodeMemory, LossyWithAlphaNeverHoldsTheAlphaDecoderBesideTheColor) {
  const Bytes px = make_pixels(kW, kH, Kind::AlphaPhoto);
  const Cost c = measure(encode(kW, kH, px, true));
  print("lossy+alpha", kW, kH, c);
  ASSERT_EQ(c.decode, GIMG_OK);
  EXPECT_LE(c.peak, kRaster + kRaster * 3u / 4u);
  EXPECT_LE(c.retained, kRaster + 1024u);
  EXPECT_LE(c.max_request, kRaster);
  // The alpha plane is in the raster.
  bool varies = false;
  for (size_t i = 3; i < c.pixels.size(); i += 4) {
    if (c.pixels[i] != c.pixels[3]) {
      varies = true;
      break;
    }
  }
  EXPECT_TRUE(varies);
  EXPECT_LT(mean_rgb_error(c.pixels, px), 8.0);
  for (size_t i = 3; i < px.size(); i += 4) {
    ASSERT_EQ(c.pixels[i], px[i]) << "alpha is stored losslessly, byte " << i;
  }
}

TEST(WebpDecodeMemory, TheBoundsHoldAtEverySize) {
  // The bounds are multiples of the raster plus a fixed allowance for the
  // tables, so they have to hold from a few kilobytes of raster to several
  // megabytes, and at a size that is not a multiple of a macroblock.
  const uint32_t sizes[][2] = { { 64, 48 }, { 250, 190 }, { 1024, 768 } };
  constexpr size_t kAllowance = 64u * 1024u;
  for (const auto & sz : sizes) {
    const uint32_t w = sz[0];
    const uint32_t h = sz[1];
    const size_t raster = static_cast<size_t>(w) * h * 4u;
    SCOPED_TRACE(std::to_string(w) + "x" + std::to_string(h));
    {
      const Bytes px = make_pixels(w, h, Kind::Photo);
      const Cost c = measure(encode(w, h, px, false));
      print("lossless", w, h, c);
      ASSERT_EQ(c.decode, GIMG_OK);
      EXPECT_EQ(c.pixels, px);
      EXPECT_LE(c.peak, raster * 13u / 10u + kAllowance);
    }
    {
      const Cost c = measure(encode(w, h, make_pixels(w, h, Kind::Photo), true));
      print("lossy", w, h, c);
      ASSERT_EQ(c.decode, GIMG_OK);
      EXPECT_LE(c.peak, raster * 16u / 10u + kAllowance);
    }
    {
      const Cost c =
          measure(encode(w, h, make_pixels(w, h, Kind::AlphaPhoto), true));
      print("lossy+alpha", w, h, c);
      ASSERT_EQ(c.decode, GIMG_OK);
      EXPECT_LE(c.peak, raster * 19u / 10u + kAllowance);
    }
  }
}

// ---------------------------------------------------------------------------
// max_memory is enforced before each large allocation
// ---------------------------------------------------------------------------

TEST(WebpDecodeMemory, MaxMemoryIsTheLiveTotalAndTheRasterAloneIsNotEnough) {
  // max_memory bounds the sum of the blocks a call holds at once (it used to
  // bound each block, and this test then gave the raster as the cap that
  // admitted a decode). The raster is the largest block and is held beside a
  // working set, so a cap of exactly the raster now refuses; the measured
  // peak of an unlimited decode, which the tracker overstates when a block
  // grows (it counts old and new together), admits.
  struct Case {
    const char * name;
    Kind kind;
    bool lossy;
  } cases[] = { { "lossless", Kind::Photo, false },
    { "lossless palette", Kind::Palette, false },
    { "lossy", Kind::Photo, true }, { "lossy+alpha", Kind::AlphaPhoto, true } };
  for (const Case & k : cases) {
    SCOPED_TRACE(k.name);
    const Bytes file = encode(kW, kH, make_pixels(kW, kH, k.kind), k.lossy);
    const Cost open = measure(file, GIMG_LIMIT_NONE);
    EXPECT_EQ(open.load, GIMG_OK);
    EXPECT_EQ(open.decode, GIMG_OK);
    EXPECT_GT(open.peak, kRaster) << "the decode holds a working set";
    const Cost ok = measure(file, open.peak);
    EXPECT_EQ(ok.load, GIMG_OK);
    EXPECT_EQ(ok.decode, GIMG_OK) << "the whole measured peak is admitted";
    for (size_t cap : { kRaster, kRaster - 1u }) {
      const Cost refused = measure(file, cap);
      EXPECT_EQ(refused.load, GIMG_OK);
      EXPECT_EQ(refused.decode, GIMG_ERR_LIMIT) << "cap " << cap;
      EXPECT_LE(refused.max_request, cap);
      EXPECT_EQ(refused.retained, 0u);
    }
  }
}

TEST(WebpDecodeMemory, AFileLargerThanMaxMemoryIsRefusedAtLoad) {
  const Bytes file = encode(kW, kH, make_pixels(kW, kH, Kind::Photo), false);
  const Cost c = measure(file, file.size() - 1u);
  EXPECT_EQ(c.load, GIMG_ERR_LIMIT);
  EXPECT_LT(c.max_request, file.size());
}

// ---------------------------------------------------------------------------
// Hostile headers
// ---------------------------------------------------------------------------

TEST(WebpDecodeMemory, HugeLosslessDimensionsAreRefusedAtTheCap) {
  // 16384 x 16384, no transform, no cache, no meta image, five one-symbol
  // codes: every pixel costs zero bits, so the file is about thirty bytes and
  // the picture four gibibytes.
  Bits w;
  lossless_header(w, 16384, 16384);
  w.put(0, 1); // no transform
  w.put(0, 1); // no colour cache
  w.put(0, 1); // no meta image
  for (int i = 0; i < 5; ++i) {
    simple_code(w, 0);
  }
  const Bytes file = riff("VP8L", w.done());
  const size_t cap = 1u << 20;
  const Cost c = measure(file, cap, static_cast<size_t>(1) << 30);
  EXPECT_EQ(c.load, GIMG_OK);
  EXPECT_EQ(c.decode, GIMG_ERR_LIMIT);
  EXPECT_LE(c.max_request, cap);
  EXPECT_EQ(c.retained, 0u);
}

TEST(WebpDecodeMemory, HugeLossyDimensionsAreRefusedWithoutALargeRequest) {
  // A key frame of 16383 x 16383 with a two-byte first partition.  The
  // partition cannot hold the picture's macroblocks, so the frame is corrupt
  // as soon as that is seen and nothing is sized from its declared size; a
  // cap is never reached, which is what the request bound below says.
  Bytes p = { 0x50, 0x00, 0x00, 0x9d, 0x01, 0x2a, 0xff, 0x3f, 0xff, 0x3f, 0, 0 };
  p[0] = static_cast<uint8_t>((2u << 5) | (1u << 4));
  const Bytes file = riff("VP8 ", p);
  const size_t cap = 1u << 18;
  const Cost c = measure(file, cap, static_cast<size_t>(1) << 30);
  EXPECT_EQ(c.load, GIMG_OK);
  EXPECT_EQ(c.decode, GIMG_ERR_CORRUPT);
  EXPECT_LE(c.max_request, cap);
}

TEST(WebpDecodeMemory, ManyHuffmanGroupsAreRefusedBeforeAnythingIsSizedFromThem) {
  // A 64 x 64 picture whose 16 x 16 meta image names group 65535, with the
  // file ending where the groups' codes should begin: 65536 groups need at
  // least 20 bits each, and the file has none.  Nothing may be sized from the
  // count: not a map of it, not the groups.
  Bits w;
  lossless_header(w, 64, 64);
  w.put(0, 1); // no transform
  w.put(0, 1); // no colour cache
  w.put(1, 1); // meta image
  w.put(0, 3); // precision 2
  w.put(0, 1); // its colour cache: none
  simple_code(w, 255); // green: group low byte
  simple_code(w, 255); // red: group high byte
  simple_code(w, 0);
  simple_code(w, 0);
  simple_code(w, 0);
  const Bytes file = riff("VP8L", w.done());
  const Cost c = measure(file);
  EXPECT_EQ(c.load, GIMG_OK);
  EXPECT_EQ(c.decode, GIMG_ERR_CORRUPT);
  // The 64 x 64 raster is 16 KiB.  A map of 65536 groups is 256 KiB and the
  // group array more; what the load and the codec's own tables need is under
  // a hundred kilobytes.
  EXPECT_LT(c.max_request, 128u * 1024u);
  EXPECT_EQ(c.retained, 0u);
}

TEST(WebpDecodeMemory, ColorCacheOfHugeBitCountIsRefusedWithoutAllocating) {
  Bits w;
  lossless_header(w, 64, 64);
  w.put(0, 1);  // no transform
  w.put(1, 1);  // a colour cache
  w.put(15, 4); // of 2^15 entries: the format allows up to 2^11
  for (int i = 0; i < 8; ++i) {
    w.put(0, 8);
  }
  const Bytes file = riff("VP8L", w.done());
  const Cost c = measure(file);
  EXPECT_EQ(c.load, GIMG_OK);
  EXPECT_EQ(c.decode, GIMG_ERR_CORRUPT);
  EXPECT_LT(c.max_request, 128u * 1024u);
  EXPECT_EQ(c.retained, 0u);
}

TEST(WebpDecodeMemory, ASmallPictureDoesNotPayForALargeOnesTables) {
  // A 2 x 2 picture of one-symbol codes: the raster is sixteen bytes, and the
  // tables it needs are nothing like the half-megabyte the largest picture
  // would need.
  Bits w;
  lossless_header(w, 2, 2);
  w.put(0, 1);
  w.put(0, 1);
  w.put(0, 1);
  for (int i = 0; i < 5; ++i) {
    simple_code(w, 7);
  }
  const Cost c = measure(riff("VP8L", w.done()));
  ASSERT_EQ(c.decode, GIMG_OK);
  EXPECT_LT(c.peak, 64u * 1024u);
  EXPECT_EQ(c.pixels.size(), 16u);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
