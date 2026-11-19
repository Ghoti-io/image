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
 * A few bytes of WebP must not cost memory or time in proportion to the
 * picture they declare.
 *
 * A lossy key frame states its size in ten bytes, and the decoder used to
 * size the mode records, the planes and the non-zero maps from it before it
 * had read anything, then run its whole macroblock loop over a first
 * partition that had ended after a hundred.  The three files below are what a
 * fuzzer found: each is a lossy key frame of 11012 x 9984, 11115 x 11115 or
 * 16383 x 12287 pixels in 52 to 91 bytes of VP8 (the third carries a GIF
 * after its RIFF), and each cost the decoder hundreds of megabytes or many
 * seconds before it said CORRUPT.
 *
 * What is measured is what the allocator is asked for, in total and in its
 * largest request: both are deterministic, and the work the decoder does is
 * bounded by them because it grows its buffers as it consumes the data.  Set
 * GIMG_TEST_MEM_REPORT to print the figures.
 *
 * The other half of the contract is that a file which is legitimately tiny
 * and huge still decodes: a flat picture of 4096 x 4096 is a few hundred
 * bytes in either WebP and must come out whole.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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

// The three fuzzer artifacts, byte for byte.

const uint8_t kFrame11115[] = {
    0x52, 0x49, 0x46, 0x46, 0x4a, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x20, 0x3e, 0x00, 0x00, 0x00, 0xf0, 0x01, 0x00, 0x9d,
    0x01, 0x2a, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x93,
    0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b,
    0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b,
    0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0x6b, 0xff, 0xff, 0x8c, 0x00, 0x7e, 0x2f,
    0xea, 0xff, 0xff, 0x8c, 0x00, 0x2f, 0xea, 0x00, 0x2f, 0xea, 0xf0, 0x01,
    0x00, 0xb5, 0x66, 0x70, 0x97, 0x00, 0x00,
};

const uint8_t kFrame16383[] = {
    0x52, 0x49, 0x46, 0x46, 0x2c, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x20, 0x20, 0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x9d,
    0x01, 0x2a, 0xff, 0xff, 0xff, 0xef, 0x02, 0x00, 0x35, 0x25, 0x5c, 0xff,
    0xfd, 0x22, 0x59, 0xb6, 0x00, 0x00, 0xfe, 0xfd, 0xb3, 0x9f, 0x04, 0x67,
    0x3c, 0x20, 0x00, 0x00,
};

const uint8_t kFrame11012Behind[] = {
    0x52, 0x49, 0x46, 0x46, 0x2d, 0x00, 0x00, 0x00, 0x57, 0x45, 0x42, 0x50,
    0x56, 0x50, 0x38, 0x20, 0x20, 0x00, 0x00, 0x00, 0x90, 0x01, 0x00, 0x9d,
    0x01, 0x2a, 0x04, 0x2b, 0x00, 0x27, 0x00, 0x00, 0x00, 0x70, 0x6e, 0x67,
    0x69, 0x48, 0x47, 0x49, 0x46, 0x38, 0x00, 0x04, 0x00, 0x02, 0x61, 0x39,
    0xf2, 0x00, 0x00, 0xff, 0x00, 0x00, 0x00, 0xff, 0x00, 0x00, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
    0xff, 0x1e, 0x3c, 0x5a, 0xc8, 0xb4, 0xa0, 0x21, 0xff, 0x0b, 0x4f, 0x45,
    0x54, 0x53, 0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x05,
    0x00, 0x00, 0x21, 0x25, 0xf9, 0x04, 0x00, 0x07, 0x00, 0x00, 0x00, 0x2c,
    0x00, 0xaa, 0x7f, 0xaa, 0xaa, 0xaa, 0xff, 0xaa, 0xd4, 0x00, 0xff, 0x7f,
    0x55, 0xff, 0xaa, 0xaa, 0xff, 0xd4, 0xff, 0xff, 0xff, 0x08, 0x00, 0x08,
    0x00, 0x08, 0x00, 0x61, 0x20, 0x66, 0x69, 0x78, 0x74, 0x75, 0x72, 0x65,
    0x00, 0x41, 0x63, 0x6d, 0x65, 0x00, 0x00, 0x46, 0x6c, 0x61, 0x74, 0x62,
    0x65, 0x64, 0x00, 0x67, 0x68, 0x6f, 0x74, 0x69, 0x00, 0x31, 0x30, 0x31,
    0x33, 0x3a, 0x31, 0x30, 0x3a, 0x30, 0x34, 0x20, 0x30, 0x31, 0x3a, 0x30,
    0x39, 0x3a, 0x30, 0x30, 0x00, 0x64, 0x65, 0x73, 0x6b, 0x00, 0x00, 0x3c,
    0x3f, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x65, 0x74, 0x20, 0x62, 0x65, 0x67,
    0x69, 0x6e, 0x3d, 0x22, 0x22, 0x20, 0x69, 0x64, 0x3d, 0x22, 0x57, 0x35,
    0x4d, 0x30, 0x4d, 0x70, 0x43, 0x65, 0x68, 0x69, 0x48, 0x7a, 0x72, 0x65,
    0x53, 0x7a, 0x4e, 0x54, 0x63, 0x7a, 0x6b, 0x63, 0x39, 0x64, 0x22, 0x3f,
    0x3e, 0x3c, 0x78, 0x3a, 0x78, 0x6d, 0x70, 0x6d, 0x65, 0x74, 0x61, 0x20,
    0x78, 0x6d, 0x6c, 0x6e, 0x73, 0x3a, 0x78, 0x3d, 0x22, 0x61, 0x64, 0x6f,
    0x62, 0x65, 0x3a, 0x6e, 0x73, 0x3a, 0x6d, 0x65, 0x74, 0x61, 0x2f, 0x22,
    0x3e, 0x3c, 0x2f, 0x78, 0x3a, 0x78, 0x6d, 0x70, 0x6d, 0x65, 0x74, 0x61,
    0x3e, 0x3c, 0x3f, 0x78, 0x70, 0x61, 0x63, 0x6b, 0x65, 0x74, 0x20, 0x65,
    0x6e, 0x64, 0x3d, 0x22, 0x77, 0x22, 0x3f, 0x3e, 0x00, 0x70, 0x68, 0x6f,
    0x74, 0x6f, 0x00, 0x65, 0x64, 0x69, 0x74, 0x6f, 0x72, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x28, 0x41, 0x44, 0x42, 0x45, 0x6d, 0x6e, 0x74, 0x72, 0x52,
    0x47, 0x42, 0x20, 0x58, 0x59, 0x5a, 0x20, 0x00, 0x01, 0x02, 0x03, 0x04,
    0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x7a, 0x19, 0x1a, 0x1b,
    0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33,
    0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x4b,
    0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f, 0x60, 0x61, 0x62, 0x63,
    0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
    0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x7b,
    0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x90, 0x91, 0x92, 0x93,
    0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
    0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0x00, 0x00, 0x00, 0x04, 0x00, 0x02, 0x00,
    0x00, 0x03, 0x06, 0x08, 0x21, 0x33, 0x12, 0x72, 0x65, 0x53, 0x7a, 0x4e,
    0x54, 0x63, 0x00, 0xb3, 0xfe, 0x03, 0x01, 0x00, 0xfd, 0x9f, 0x04, 0x67,
    0x20, 0x01, 0x3c, 0x20, 0x00, 0x00, 0x00, 0x3b,
};


/** An allocator that knows the size of every live block and what was asked. */
struct Track {
  GIMG_Allocator a{};
  size_t live = 0;
  size_t peak = 0;
  size_t max_request = 0;
  size_t requested = 0; ///< Sum of every request, whether or not it was freed.

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
    t->requested += n;
    uint8_t * p = static_cast<uint8_t *>(std::malloc(n + kHead));
    if (!p) {
      return nullptr;
    }
    std::memcpy(p, &n, sizeof(n));
    t->live += n;
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
    t->requested += n;
    if (n > old) {
      // A block that grows is briefly two blocks to an allocator that cannot
      // grow in place, so count it that way.
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

struct Cost {
  GIMG_Result load = GIMG_OK;
  GIMG_Result decode = GIMG_OK;
  size_t max_request = 0; ///< Largest single request of the decode.
  size_t requested = 0;   ///< Everything the decode asked for.
  size_t peak = 0;        ///< High-water mark of live bytes.
  size_t retained = 0;    ///< Live after the decode returned, above the load.
  double cpu_ms = 0;      ///< Reported, never asserted.
  Bytes pixels;
};

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

/** Load and decode item 0 of @a file; keep the pixels when @a keep. */
Cost measure(const Bytes & file, size_t max_memory = 0, bool keep = false) {
  Cost c;
  Track t;
  Installed inst(t);
  GIMG_Stream * in = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in), GIMG_OK);
  GIMG_Limits lim = {};
  lim.max_memory = max_memory;
  GIMG_Load_Options lo = {};
  lo.limits = &lim;
  GIMG_Doc * doc = nullptr;
  const std::clock_t t0 = std::clock();
  c.load = gimg_doc_load(in, &lo, nullptr, &doc);
  if (c.load == GIMG_OK) {
    // The load's own requests (its chunk table is a few hundred kilobytes)
    // are not the decode's.
    const size_t base = t.live;
    t.max_request = 0;
    t.requested = 0;
    t.peak = t.live;
    GIMG_Raster * r = nullptr;
    c.decode = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r);
    c.retained = t.live - base;
    if (r) {
      if (keep) {
        c.pixels = raster_bytes(r);
      }
      gimg_raster_destroy(r);
    }
  }
  c.cpu_ms = 1000.0 * static_cast<double>(std::clock() - t0) / CLOCKS_PER_SEC;
  t.peak = t.peak > t.live ? t.peak : t.live;
  c.max_request = t.max_request;
  c.requested = t.requested;
  c.peak = t.peak;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
  EXPECT_EQ(t.live, 0u) << "a block outlived the document";
  return c;
}

void print(const char * what, const Cost & c) {
  if (!std::getenv("GIMG_TEST_MEM_REPORT")) {
    return;
  }
  std::printf("AMP %-28s load=%d decode=%d requested=%zu max_request=%zu "
              "peak=%zu retained=%zu cpu=%.1f ms\n",
      what, static_cast<int>(c.load), static_cast<int>(c.decode), c.requested,
      c.max_request, c.peak, c.retained, c.cpu_ms);
}

// ---------------------------------------------------------------------------
// Building blocks
// ---------------------------------------------------------------------------

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

/** A prefix code naming one symbol: every pixel through it costs no bits. */
void one_symbol_code(Bits & w, uint32_t symbol) {
  w.put(1, 1); // simple
  w.put(0, 1); // one symbol
  w.put(1, 1); // an eight-bit symbol
  w.put(symbol, 8);
}

/** A prefix code naming two symbols: every symbol through it costs a bit. */
void two_symbol_code(Bits & w, uint32_t first, uint32_t second) {
  w.put(1, 1);
  w.put(1, 1); // two symbols
  w.put(1, 1);
  w.put(first, 8);
  w.put(second, 8);
}

void put32le(Bytes & v, uint32_t x) {
  for (int s = 0; s < 32; s += 8) {
    v.push_back(static_cast<uint8_t>(x >> s));
  }
}

Bytes chunk(const char * fourcc, const uint8_t * p, size_t n) {
  Bytes c(fourcc, fourcc + 4);
  put32le(c, static_cast<uint32_t>(n));
  c.insert(c.end(), p, p + n);
  if (n & 1u) {
    c.push_back(0);
  }
  return c;
}

Bytes chunk(const char * fourcc, const Bytes & p) {
  return chunk(fourcc, p.data(), p.size());
}

Bytes riff(const std::vector<Bytes> & chunks) {
  size_t body = 4;
  for (const Bytes & c : chunks) {
    body += c.size();
  }
  Bytes f = { 'R', 'I', 'F', 'F' };
  put32le(f, static_cast<uint32_t>(body));
  f.insert(f.end(), { 'W', 'E', 'B', 'P' });
  for (const Bytes & c : chunks) {
    f.insert(f.end(), c.begin(), c.end());
  }
  return f;
}

/** The VP8X chunk of a still with an alpha plane. */
Bytes vp8x_alpha(uint32_t w, uint32_t h) {
  Bytes p = { 0x10, 0, 0, 0 };
  for (uint32_t v : { w - 1u, h - 1u }) {
    p.push_back(static_cast<uint8_t>(v));
    p.push_back(static_cast<uint8_t>(v >> 8));
    p.push_back(static_cast<uint8_t>(v >> 16));
  }
  return chunk("VP8X", p);
}

/** The payload of the first chunk of a file that is just RIFF, WEBP, one chunk. */
Bytes first_payload(const uint8_t * file, size_t n) {
  (void)n;
  const uint32_t size = static_cast<uint32_t>(file[16]) |
      (static_cast<uint32_t>(file[17]) << 8) |
      (static_cast<uint32_t>(file[18]) << 16) |
      (static_cast<uint32_t>(file[19]) << 24);
  return Bytes(file + 20, file + 20 + size);
}

Bytes make_pixels(uint32_t w, uint32_t h, bool flat) {
  Bytes v(static_cast<size_t>(w) * h * 4u);
  uint32_t s = 12345u;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      s = s * 1664525u + 1013904223u;
      uint8_t * p = &v[(static_cast<size_t>(y) * w + x) * 4u];
      if (flat) {
        p[0] = 200;
        p[1] = 90;
        p[2] = 30;
        p[3] = 255;
        continue;
      }
      const uint32_t noise = (s >> 24) & 7u;
      p[0] = static_cast<uint8_t>((x * 255u) / w + noise);
      p[1] = static_cast<uint8_t>((y * 255u) / h + noise);
      p[2] = static_cast<uint8_t>(((x ^ y) & 64u) ? 200u + noise : 40u + noise);
      p[3] = 255;
    }
  }
  return v;
}

Bytes encode(uint32_t w, uint32_t h, const Bytes & px, bool lossy) {
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
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless =
      lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
  opts.webp_effort = 1;
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

// ---------------------------------------------------------------------------
// The fuzzer's files
// ---------------------------------------------------------------------------

struct Artifact {
  const char * name;
  const uint8_t * data;
  size_t size;
  uint32_t width;
  uint32_t height;
};

const Artifact kArtifacts[] = {
  { "11115 x 11115 in 91 bytes", kFrame11115, sizeof(kFrame11115), 11115, 11115 },
  { "16383 x 12287 in 52 bytes", kFrame16383, sizeof(kFrame16383), 16383, 12287 },
  { "11012 x 9984 in a longer file", kFrame11012Behind,
    sizeof(kFrame11012Behind), 11012, 9984 },
};

TEST(WebpDecodeAmplification, FuzzerFilesAreRefusedForWhatTheyHold) {
  for (const Artifact & a : kArtifacts) {
    SCOPED_TRACE(a.name);
    const Bytes file(a.data, a.data + a.size);
    // The decoder had to read ten bytes of header to learn the size; what a
    // frame of that size would cost is hundreds of megabytes.
    const size_t declared = static_cast<size_t>(a.width) * a.height * 3u / 2u;
    for (size_t cap : { static_cast<size_t>(0), static_cast<size_t>(1) << 20,
             static_cast<size_t>(1) << 21 }) {
      const Cost c = measure(file, cap);
      print(a.name, c);
      ASSERT_EQ(c.load, GIMG_OK) << "cap " << cap;
      // Refused as corrupt, not as over a limit, even by a small cap: the
      // decoder never reaches a request the cap would refuse.
      EXPECT_EQ(c.decode, GIMG_ERR_CORRUPT) << "cap " << cap;
      EXPECT_LE(c.max_request, 48u * 1024u);
      EXPECT_LE(c.requested, 160u * 1024u);
      EXPECT_LT(c.requested, declared / 1000u);
      EXPECT_EQ(c.retained, 0u);
    }
  }
}

TEST(WebpDecodeAmplification, ARealFileWithAHugeSizeWrittenOverItsHeader) {
  // The same defect made from a sound file: a 96 x 64 lossy picture whose key
  // frame header is rewritten to say 16383 x 16383.  Its first partition holds
  // the modes of twelve macroblocks, and the header asks for a million.
  Bytes file = encode(96, 64, make_pixels(96, 64, false), true);
  ASSERT_GT(file.size(), 30u);
  ASSERT_EQ(std::memcmp(&file[12], "VP8 ", 4), 0);
  const size_t at = 20 + 6; // chunk payload + frame tag + start code
  ASSERT_EQ(file[at - 3], 0x9d);
  file[at] = 0xff;
  file[at + 1] = 0x3f;
  file[at + 2] = 0xff;
  file[at + 3] = 0x3f;
  const Cost c = measure(file);
  print("real file, size rewritten", c);
  EXPECT_EQ(c.decode, GIMG_ERR_CORRUPT);
  EXPECT_LE(c.max_request, 48u * 1024u);
  EXPECT_LE(c.requested, 256u * 1024u);
  EXPECT_EQ(c.retained, 0u);
}

TEST(WebpDecodeAmplification, ATruncatedLossyFileStopsWhereItsDataStops) {
  // A sound 1024 x 768 lossy file cut to a third: its token partition ends
  // in the first third of the picture's rows, and the planes were allocated
  // for all of them.  Now they are only as large as the rows that were read.
  const uint32_t w = 1024;
  const uint32_t h = 768;
  const Bytes whole = encode(w, h, make_pixels(w, h, false), true);
  const Cost full = measure(whole);
  ASSERT_EQ(full.decode, GIMG_OK);
  print("whole", full);

  ASSERT_EQ(std::memcmp(&whole[12], "VP8 ", 4), 0);
  const size_t payload = whole.size() - 20;
  Bytes cut(whole.begin(), whole.begin() + 20 + payload / 3u);
  const uint32_t n = static_cast<uint32_t>(payload / 3u);
  for (int i = 0; i < 4; ++i) {
    cut[16 + i] = static_cast<uint8_t>(n >> (8 * i));
  }
  const uint32_t riff_size = static_cast<uint32_t>(cut.size() - 8u);
  for (int i = 0; i < 4; ++i) {
    cut[4 + i] = static_cast<uint8_t>(riff_size >> (8 * i));
  }
  const Cost part = measure(cut);
  print("cut to a third", part);
  EXPECT_EQ(part.decode, GIMG_ERR_CORRUPT);
  // The planes of the whole picture are a request of about 1.5 bytes a
  // pixel; the cut file never makes one that large.
  EXPECT_LT(part.max_request, static_cast<size_t>(w) * h * 3u / 4u);
  EXPECT_LT(part.requested, full.requested * 3u / 4u);
  EXPECT_EQ(part.retained, 0u);
}

TEST(WebpDecodeAmplification, AlphaPlaneIsNotBuiltForAFrameThatCannotHoldItsMacroblocks) {
  // An ALPH chunk whose five codes are one symbol each makes a plane of any
  // size for no bits, so the plane is sound; the VP8 key frame after it is
  // the 52-byte file above, 16383 x 12287 in a partition of twelve bytes.
  // The alpha plane is decoded first, and is two hundred megabytes of
  // samples and eight hundred of working words; the colour frame decides
  // first.
  Bits a;
  a.put(1, 8); // ALPH header: lossless compression, no filter
  Bits w;
  w.put(0, 1); // no transform
  w.put(0, 1); // no colour cache
  w.put(0, 1); // no meta image
  for (int i = 0; i < 5; ++i) {
    one_symbol_code(w, 0);
  }
  Bytes alph = { 0x01 };
  const Bytes body = w.done();
  alph.insert(alph.end(), body.begin(), body.end());
  const Bytes vp8 = first_payload(kFrame16383, sizeof(kFrame16383));
  const Bytes file = riff({ vp8x_alpha(16383, 12287), chunk("ALPH", alph),
      chunk("VP8 ", vp8) });
  const Cost c = measure(file);
  print("alpha before a hopeless frame", c);
  ASSERT_EQ(c.load, GIMG_OK);
  EXPECT_EQ(c.decode, GIMG_ERR_CORRUPT);
  EXPECT_LE(c.max_request, 48u * 1024u);
  EXPECT_LE(c.requested, 160u * 1024u);
  EXPECT_EQ(c.retained, 0u);
}

TEST(WebpDecodeAmplification, LosslessCodesThatCostABitAPixelAreRefusedBeforeTheBuffer) {
  // 11115 x 11115 with two-symbol codes: each pixel costs at least a bit, at
  // most 4096 pixels come from one symbol, and the stream has a few bytes.
  Bits w;
  w.put(0x2f, 8);
  w.put(11115u - 1u, 14);
  w.put(11115u - 1u, 14);
  w.put(0, 1);
  w.put(0, 3);
  w.put(0, 1); // no transform
  w.put(0, 1); // no colour cache
  w.put(0, 1); // no meta image
  for (int i = 0; i < 5; ++i) {
    two_symbol_code(w, 0, 255);
  }
  for (int i = 0; i < 64; ++i) {
    w.put(0x5a, 8);
  }
  const Bytes file = riff({ chunk("VP8L", w.done()) });
  const Cost c = measure(file);
  print("lossless, two-symbol codes", c);
  ASSERT_EQ(c.load, GIMG_OK);
  EXPECT_EQ(c.decode, GIMG_ERR_CORRUPT);
  EXPECT_LE(c.max_request, 128u * 1024u);
  EXPECT_LE(c.requested, 256u * 1024u);
  EXPECT_EQ(c.retained, 0u);
}

// ---------------------------------------------------------------------------
// What must still decode
// ---------------------------------------------------------------------------

TEST(WebpDecodeAmplification, AFlatPictureOf4096SquareIsStillAFewBytesAndStillDecodes) {
  const uint32_t n = 4096;
  const Bytes px = make_pixels(n, n, true);
  for (int lossy = 0; lossy < 2; ++lossy) {
    SCOPED_TRACE(lossy ? "lossy" : "lossless");
    const Bytes file = encode(n, n, px, lossy != 0);
    // It is the compression ratio a hostile file can claim, and a sound one
    // really has: a raster of 64 MiB from a few tens of kilobytes.
    EXPECT_LT(file.size(), 64u * 1024u);
    const Cost c = measure(file, 0, true);
    print(lossy ? "flat 4096 lossy" : "flat 4096 lossless", c);
    ASSERT_EQ(c.load, GIMG_OK);
    ASSERT_EQ(c.decode, GIMG_OK);
    ASSERT_EQ(c.pixels.size(), px.size());
    if (!lossy) {
      EXPECT_EQ(c.pixels, px);
    }
    else {
      // Flat in, flat out, and the color the encoder stored within rounding.
      size_t off = 0;
      for (size_t i = 0; i < c.pixels.size(); i += 4) {
        if (std::memcmp(&c.pixels[i], &c.pixels[0], 4) != 0) {
          off++;
        }
      }
      EXPECT_EQ(off, 0u);
      for (int k = 0; k < 3; ++k) {
        EXPECT_LE(std::abs(static_cast<int>(c.pixels[k]) - px[k]), 6);
      }
      EXPECT_EQ(c.pixels[3], 255);
    }
    // The growth of the buffers is by doubling: a sound large file is a
    // constant factor from the raster, as before.
    EXPECT_LE(c.peak, static_cast<size_t>(n) * n * 4u * 2u);
  }
}

TEST(WebpDecodeAmplification, AStreamOfOneBitAPixelIsNotRefusedByTheBound) {
  // 64 x 64 pixels from a two-symbol green code: exactly one bit each, so the
  // stream is the least the bound allows to be a sound one.  It decodes, to
  // the pattern written.
  const uint32_t n = 64;
  Bits w;
  w.put(0x2f, 8);
  w.put(n - 1u, 14);
  w.put(n - 1u, 14);
  w.put(0, 1);
  w.put(0, 3);
  w.put(0, 1);
  w.put(0, 1);
  w.put(0, 1);
  two_symbol_code(w, 7, 200); // green: the code is 0 for 7 and 1 for 200
  one_symbol_code(w, 1);      // red
  one_symbol_code(w, 2);      // blue
  one_symbol_code(w, 255);    // alpha
  one_symbol_code(w, 0);      // distance, never used
  for (uint32_t i = 0; i < n * n; ++i) {
    w.put(i & 1u, 1);
  }
  const Bytes file = riff({ chunk("VP8L", w.done()) });
  const Cost c = measure(file, 0, true);
  print("one bit a pixel", c);
  ASSERT_EQ(c.load, GIMG_OK);
  ASSERT_EQ(c.decode, GIMG_OK);
  ASSERT_EQ(c.pixels.size(), static_cast<size_t>(n) * n * 4u);
  for (uint32_t i = 0; i < n * n; ++i) {
    ASSERT_EQ(c.pixels[i * 4u + 1], (i & 1u) ? 200 : 7) << i;
    ASSERT_EQ(c.pixels[i * 4u + 0], 1);
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
