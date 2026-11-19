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
 * VP8 inter frames inside an ANMF animation.
 *
 * An inter frame continues from the reference pictures and entropy state the
 * frame before it left, so decoding frame N of a run of them used to replay
 * every frame since the last key frame (in fact since frame 0), and an N-frame
 * run cost N(N+1)/2 frame decodes to walk.  The document's cache now holds the
 * decoder's sequence state beside the canvas, and a sequence keeps reference
 * pictures only when the frame after reads them.
 *
 * The library's encoder (like libwebp's) writes key frames only, so the
 * fixtures are built here: the key frames are the library's lossy encoder's,
 * and the inter frames are hand-assembled bitstreams (every macroblock skipped,
 * zero motion, varying which reference it reads, which references it refreshes
 * or copies and the loop-filter level, which is what makes each frame's pixels
 * depend on the state before it).  The writer below is a port of the one in
 * tests/data/webp/verify_vp8_inter.py, which checks the same streams against an
 * outside decoder's key frame.
 *
 * What is asserted is counts, not time: allocation calls for the cost of a
 * walk, live bytes for what the cache holds, and byte equality between every
 * access order and a decode of that frame from a document nothing else was
 * decoded from.  The pixels of the whole fixture are pinned to a checksum taken
 * from the decoder that copied every reference picture on every frame.
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../../src/codec/codec_internal.h"

namespace {

#include "../../src/codec/webp/webp_vp8_proba.inc"

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Counting and tracking allocator, swapped into the WebP codec
// ---------------------------------------------------------------------------

struct Track {
  GIMG_Allocator a{};
  size_t live = 0;
  size_t peak = 0;
  long calls = 0;

  static constexpr size_t kHead = 16;

  static void note(Track * t) {
    if (t->live > t->peak) {
      t->peak = t->live;
    }
  }
  static void * m(void * ctx, size_t n) {
    Track * t = static_cast<Track *>(ctx);
    t->calls++;
    uint8_t * p = static_cast<uint8_t *>(std::malloc(n + kHead));
    if (!p) {
      return nullptr;
    }
    std::memcpy(p, &n, sizeof(n));
    t->live += n;
    note(t);
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
    t->calls++;
    uint8_t * base = static_cast<uint8_t *>(p) - kHead;
    size_t old;
    std::memcpy(&old, base, sizeof(old));
    if (n > old) { // briefly two blocks to an allocator that cannot grow in place
      t->live += n;
      note(t);
      t->live -= n;
    }
    uint8_t * q = static_cast<uint8_t *>(std::realloc(base, n + kHead));
    if (!q) {
      return nullptr;
    }
    std::memcpy(q, &n, sizeof(n));
    t->live = t->live - old + n;
    note(t);
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

/** One document, loaded and decoded under a tracking allocator. */
struct Walk {
  GIMG_Codec * codec;
  const GIMG_Allocator * saved;
  Track track;
  GIMG_Stream * in = nullptr;
  GIMG_Doc * doc = nullptr;
  long load_calls = 0;
  size_t load_live = 0;

  explicit Walk(const Bytes & bytes) {
    codec = gimg_codec_by_name("webp");
    saved = codec->allocator;
    codec->allocator = &track.a;
    EXPECT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &in),
        GIMG_OK);
    EXPECT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
    load_calls = track.calls;
    load_live = track.live;
    track.peak = track.live;
  }
  ~Walk() {
    gimg_doc_destroy(doc); // freed through the allocator it was built with
    gimg_stream_destroy(in);
    codec->allocator = saved;
  }
  size_t frames() const { return gimg_doc_item_count(doc); }
  long cost() const { return track.calls - load_calls; }
  size_t peak_above_load() const { return track.peak - load_live; }
  size_t held_above_load() const { return track.live - load_live; }

  Bytes decode(size_t i, const GIMG_Decode_Options * o = nullptr) {
    GIMG_Raster * r = nullptr;
    EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, i), o, &r), GIMG_OK) << i;
    Bytes v;
    if (r) {
      v = pixels(r);
      gimg_raster_destroy(r);
    }
    return v;
  }
  static Bytes pixels(GIMG_Raster * r) {
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
};

uint32_t crc32_of(const Bytes & b) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint8_t x : b) {
    c ^= x;
    for (int k = 0; k < 8; ++k) {
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

// ---------------------------------------------------------------------------
// Pictures and files made with the library's own encoder
// ---------------------------------------------------------------------------

uint32_t next_rand(uint32_t & s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

Bytes profile_bytes() {
  Bytes p(128, 0);
  p[3] = 128;
  std::memcpy(p.data() + 36, "acsp", 4);
  for (size_t i = 40; i < p.size(); i++) {
    p[i] = static_cast<uint8_t>((i * 7u) & 0xFFu);
  }
  return p;
}

/** Smooth gradients with a few hard edges, so macroblock borders have content. */
Bytes picture(uint32_t w, uint32_t h, uint32_t seed, bool alpha) {
  Bytes px(static_cast<size_t>(w) * h * 4u);
  uint32_t s = seed;
  const uint32_t cx = next_rand(s) % w;
  const uint32_t cy = next_rand(s) % h;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t * p = &px[(static_cast<size_t>(y) * w + x) * 4u];
      const bool edge = (x > cx) != (y > cy);
      p[0] = static_cast<uint8_t>((x * 255u) / w + (edge ? 70u : 0u));
      p[1] = static_cast<uint8_t>((y * 255u) / h + seed * 3u);
      p[2] = static_cast<uint8_t>(((x + y) * 128u) / (w + h) + (edge ? 0u : 90u));
      p[3] = alpha ? static_cast<uint8_t>(60u + (x * 150u) / w) : 255u;
    }
  }
  return px;
}

GIMG_Raster * raster_of(
    uint32_t w, uint32_t h, const Bytes & px, const Bytes * icc) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  uint8_t * dst = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < h; ++y) {
    std::memcpy(dst + y * stride, &px[static_cast<size_t>(y) * w * 4u],
        static_cast<size_t>(w) * 4u);
  }
  if (icc) {
    GCOL_Color_Info ci;
    gcol_color_info_default(&ci);
    ci.icc_bytes = icc->data();
    ci.icc_size = icc->size();
    EXPECT_EQ(gimg_raster_set_color_info(r, &ci), GIMG_OK);
  }
  return r;
}

Bytes save_doc(const GIMG_Doc * doc, const char * format,
    const GIMG_Save_Options * opts, GIMG_Save_Report * report = nullptr) {
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  EXPECT_EQ(gimg_doc_save(doc, out, format, opts, report), GIMG_OK) << format;
  const void * b = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &b, &n);
  Bytes file(
      static_cast<const uint8_t *>(b), static_cast<const uint8_t *>(b) + n);
  gimg_stream_destroy(out);
  return file;
}

/** A still WebP of @p px, lossy or lossless, carrying @p icc when given. */
Bytes encode_still(uint32_t w, uint32_t h, const Bytes & px, bool lossy,
    const Bytes * icc = nullptr) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster_of(w, h, px, icc));
  GIMG_Save_Options opts = {};
  opts.webp_lossless =
      lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
  opts.webp_effort = 1;
  Bytes file = save_doc(doc, "webp", &opts);
  gimg_doc_destroy(doc);
  return file;
}

/** The payload of the first chunk named @p fourcc in a WebP file. */
Bytes chunk_payload(const Bytes & file, const char * fourcc) {
  size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const uint32_t size = file[pos + 4] | (file[pos + 5] << 8) |
        (file[pos + 6] << 16) | (static_cast<uint32_t>(file[pos + 7]) << 24);
    if (std::memcmp(&file[pos], fourcc, 4) == 0) {
      return Bytes(file.begin() + pos + 8, file.begin() + pos + 8 + size);
    }
    pos += 8 + size + (size & 1u);
  }
  return Bytes();
}

// ---------------------------------------------------------------------------
// VP8 inter frames, assembled by hand
// ---------------------------------------------------------------------------

/** The boolean entropy encoder of RFC 6386 section 7, as the encoder keeps it. */
struct BoolWriter {
  int32_t range = 254;
  int32_t value = 0;
  int nb_bits = -8;
  int run = 0;
  Bytes buf;

  static int32_t s32(int64_t v) {
    return static_cast<int32_t>(static_cast<uint32_t>(v & 0xFFFFFFFFll));
  }
  void flush() {
    const int shift = 8 + nb_bits;
    const int32_t bits = value >> shift;
    value = s32(static_cast<int64_t>(value) - (static_cast<int64_t>(bits) << shift));
    nb_bits -= 8;
    if ((bits & 0xff) == 0xff) {
      run++;
      return;
    }
    if ((bits & 0x100) && !buf.empty()) {
      buf.back() = static_cast<uint8_t>(buf.back() + 1);
    }
    if (run) {
      buf.insert(buf.end(), static_cast<size_t>(run),
          static_cast<uint8_t>((bits & 0x100) ? 0x00 : 0xff));
      run = 0;
    }
    buf.push_back(static_cast<uint8_t>(bits & 0xff));
  }
  void renorm() {
    int32_t span = range + 1;
    int shift = 0;
    while (span < 128) {
      span <<= 1;
      shift++;
    }
    range = span - 1;
    value = s32(static_cast<int64_t>(value) << shift);
    nb_bits += shift;
    if (nb_bits > 0) {
      flush();
    }
  }
  void put(int bit, int prob) {
    const int32_t split = (range * prob) >> 8;
    if (bit) {
      value = s32(static_cast<int64_t>(value) + split + 1);
      range -= split + 1;
    }
    else {
      range = split;
    }
    if (range < 127) {
      renorm();
    }
  }
  void uniform(int bit) { put(bit, 128); }
  void literal(uint32_t v, int nbits) {
    for (int s = nbits - 1; s >= 0; --s) {
      uniform(static_cast<int>((v >> s) & 1u));
    }
  }
  Bytes finish() {
    literal(0, 9 - nb_bits);
    nb_bits = 0;
    flush();
    return buf;
  }
};

const uint8_t kMvUpdate[2][19] = {
    {237, 246, 253, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 250,
        250, 252, 254, 254},
    {231, 243, 245, 253, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 251,
        251, 254, 254, 254}};

/** What one inter frame does; every macroblock is skipped, zero motion. */
struct InterSpec {
  int ref;          ///< 0 last, 1 golden, 2 alternate.
  int refresh_gf;
  int refresh_arf;
  int refresh_last;
  int copy_gf;      ///< 0 none, 1 last, 2 alternate; read when !refresh_gf.
  int copy_arf;     ///< 0 none, 1 last, 2 golden; read when !refresh_arf.
  int filter_level; ///< 0..63; moves pixels at macroblock borders.
};

Bytes inter_frame(size_t mb_count, const InterSpec & s) {
  BoolWriter bw;
  bw.uniform(0);                 // segmentation off
  bw.literal(0, 1);              // normal loop filter
  bw.literal(static_cast<uint32_t>(s.filter_level), 6);
  bw.literal(0, 3);              // sharpness
  bw.uniform(0);                 // no loop-filter deltas
  bw.literal(0, 2);              // one coefficient partition
  bw.literal(0, 7);              // quantizer index; skip ignores it
  for (int i = 0; i < 5; ++i) {
    bw.uniform(0);
  }
  bw.uniform(s.refresh_gf);
  bw.uniform(s.refresh_arf);
  if (!s.refresh_gf) {
    bw.literal(static_cast<uint32_t>(s.copy_gf), 2);
  }
  if (!s.refresh_arf) {
    bw.literal(static_cast<uint32_t>(s.copy_arf), 2);
  }
  bw.uniform(0);                 // sign bias golden
  bw.uniform(0);                 // sign bias alternate
  bw.uniform(1);                 // keep token probabilities
  bw.uniform(s.refresh_last);
  for (int t = 0; t < 4; ++t) {
    for (int b = 0; b < 8; ++b) {
      for (int c = 0; c < 3; ++c) {
        for (int p = 0; p < 11; ++p) {
          bw.put(0, gimg_vp8_coeffs_update_proba[t][b][c][p]);
        }
      }
    }
  }
  bw.uniform(1);                 // skip enabled
  bw.literal(128, 8);            // prob_skip_false
  bw.literal(128, 8);            // prob_intra
  bw.literal(128, 8);            // prob_last
  bw.literal(128, 8);            // prob_gf
  bw.uniform(0);                 // keep luma intra probabilities
  bw.uniform(0);                 // keep chroma intra probabilities
  for (const auto & row : kMvUpdate) {
    for (uint8_t prob : row) {
      bw.put(0, prob);
    }
  }
  for (size_t i = 0; i < mb_count; ++i) {
    bw.uniform(1); // skip coefficients
    bw.uniform(1); // inter
    if (s.ref == 0) {
      bw.uniform(0);
    }
    else {
      bw.uniform(1);
      bw.uniform(s.ref == 1 ? 0 : 1);
    }
    // Zero motion. The census of an all-zero neighbourhood, border included,
    // is five, and the mode tree's first probability for it is 234.
    bw.put(0, 234);
  }
  const Bytes part0 = bw.finish();
  const uint32_t tag = 0x11u | (static_cast<uint32_t>(part0.size()) << 5);
  Bytes out = {static_cast<uint8_t>(tag), static_cast<uint8_t>(tag >> 8),
      static_cast<uint8_t>(tag >> 16)};
  out.insert(out.end(), part0.begin(), part0.end());
  out.push_back(0);
  out.push_back(0);
  return out;
}

// ---------------------------------------------------------------------------
// The animated container
// ---------------------------------------------------------------------------

void le(Bytes & b, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
}

void put_chunk(Bytes & out, const char * fourcc, const Bytes & payload) {
  out.insert(out.end(), fourcc, fourcc + 4);
  le(out, static_cast<uint32_t>(payload.size()), 4);
  out.insert(out.end(), payload.begin(), payload.end());
  if (payload.size() & 1u) {
    out.push_back(0);
  }
}

struct AnimFrame {
  const char * kind; ///< "VP8 " or "VP8L".
  Bytes payload;
  uint8_t flags;     ///< Bit 0 dispose to background, bit 1 do not blend.
};

Bytes build_anim(uint32_t w, uint32_t h, const std::vector<AnimFrame> & frames,
    const Bytes * icc = nullptr) {
  Bytes body = {'W', 'E', 'B', 'P'};
  Bytes vp8x;
  vp8x.push_back(static_cast<uint8_t>(0x02u | (icc ? 0x20u : 0u)));
  le(vp8x, 0, 3);
  le(vp8x, w - 1, 3);
  le(vp8x, h - 1, 3);
  put_chunk(body, "VP8X", vp8x);
  if (icc) {
    put_chunk(body, "ICCP", *icc);
  }
  Bytes anim;
  le(anim, 0x00FFFFFFu, 4);
  le(anim, 0, 2);
  put_chunk(body, "ANIM", anim);
  for (const AnimFrame & f : frames) {
    Bytes anmf;
    le(anmf, 0, 3);
    le(anmf, 0, 3);
    le(anmf, w - 1, 3);
    le(anmf, h - 1, 3);
    le(anmf, 40, 3);
    anmf.push_back(f.flags);
    put_chunk(anmf, f.kind, f.payload);
    put_chunk(body, "ANMF", anmf);
  }
  Bytes file = {'R', 'I', 'F', 'F'};
  le(file, static_cast<uint32_t>(body.size()), 4);
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

constexpr uint32_t kW = 48;
constexpr uint32_t kH = 32;
constexpr size_t kMbs = 3 * 2;

/** A run of inter frames that exercises every way a reference can move. */
InterSpec spec_for(size_t j) {
  static const InterSpec table[6] = {
      {0, 0, 0, 1, 0, 0, 20}, // read last; refresh only last
      {1, 0, 0, 1, 0, 0, 10}, // read golden; refresh only last
      {0, 1, 0, 1, 0, 0, 30}, // read last; golden becomes this frame
      {2, 0, 0, 1, 2, 0, 5},  // read alternate; golden copies alternate
      {0, 0, 1, 0, 1, 0, 15}, // read last; alternate becomes this frame,
                              // golden copies last, last is left alone
      {1, 0, 0, 1, 0, 1, 25}, // read golden; alternate copies last
  };
  return table[j % 6];
}

/**
 * K, 20 inter, K2, 12 inter, one lossless frame, K3, 8 inter, K, 4 inter:
 * forty-nine frames, forty-four of them lossy inter frames, in runs that start
 * at a key frame and end at a key frame or a lossless frame.
 */
Bytes build_inter_animation(size_t * inter_count = nullptr) {
  const Bytes k1 = chunk_payload(
      encode_still(kW, kH, picture(kW, kH, 1, false), true), "VP8 ");
  const Bytes k2 = chunk_payload(
      encode_still(kW, kH, picture(kW, kH, 2, false), true), "VP8 ");
  const Bytes k3 = chunk_payload(
      encode_still(kW, kH, picture(kW, kH, 3, false), true), "VP8 ");
  const Bytes ll = chunk_payload(
      encode_still(kW, kH, picture(kW, kH, 4, false), false), "VP8L");
  std::vector<AnimFrame> frames;
  size_t j = 0;
  size_t inter = 0;
  auto flags = [&]() {
    const size_t n = frames.size();
    return static_cast<uint8_t>(((n % 3 == 0) ? 2u : 0u) | ((n % 5 == 0) ? 1u : 0u));
  };
  auto key = [&](const Bytes & p) { frames.push_back({"VP8 ", p, flags()}); };
  auto run = [&](size_t n) {
    for (size_t i = 0; i < n; ++i, ++j, ++inter) {
      frames.push_back({"VP8 ", inter_frame(kMbs, spec_for(j)), flags()});
    }
  };
  key(k1);
  run(20);
  key(k2);
  run(12);
  frames.push_back({"VP8L", ll, flags()});
  key(k3);
  run(8);
  key(k1);
  run(4);
  if (inter_count) {
    *inter_count = inter;
  }
  return build_anim(kW, kH, frames);
}

std::vector<size_t> order_of(const char * kind, size_t n) {
  std::vector<size_t> o;
  if (!std::strcmp(kind, "sequential")) {
    for (size_t i = 0; i < n; ++i) {
      o.push_back(i);
    }
  }
  else if (!std::strcmp(kind, "reverse")) {
    for (size_t i = n; i-- > 0;) {
      o.push_back(i);
    }
  }
  else if (!std::strcmp(kind, "random")) {
    uint32_t s = 4242u;
    for (size_t i = 0; i < 3 * n; ++i) {
      o.push_back(next_rand(s) % n);
    }
  }
  else { // "loop": forward twice, then a stride
    for (size_t pass = 0; pass < 2; ++pass) {
      for (size_t i = 0; i < n; ++i) {
        o.push_back(i);
      }
    }
    for (size_t i = 0; i < n; i += 3) {
      o.push_back(i);
    }
  }
  return o;
}

std::vector<Bytes> cold_frames(const Bytes & file) {
  std::vector<Bytes> cold;
  size_t n = 0;
  {
    Walk probe(file);
    n = probe.frames();
  }
  for (size_t i = 0; i < n; ++i) {
    Walk fresh(file);
    cold.push_back(fresh.decode(i));
  }
  return cold;
}

}  // namespace

// ---------------------------------------------------------------------------
// Inter frames
// ---------------------------------------------------------------------------

TEST(WebpInterRun, FixtureIsWhatItClaimsToBe) {
  size_t inter = 0;
  const Bytes file = build_inter_animation(&inter);
  EXPECT_EQ(inter, 44u);
  Walk w(file);
  ASSERT_EQ(w.frames(), 49u);
  // Every one of the forty-four really is an inter frame: the tag's first bit.
  size_t tagged = 0;
  for (size_t pos = 12; pos + 8 <= file.size();) {
    const uint32_t size = file[pos + 4] | (file[pos + 5] << 8) |
        (file[pos + 6] << 16) | (static_cast<uint32_t>(file[pos + 7]) << 24);
    if (std::memcmp(&file[pos], "ANMF", 4) == 0 &&
        std::memcmp(&file[pos + 8 + 16], "VP8 ", 4) == 0 &&
        (file[pos + 8 + 24] & 1u)) {
      tagged++;
    }
    pos += 8 + size + (size & 1u);
  }
  EXPECT_EQ(tagged, 44u);
  const std::vector<Bytes> cold = cold_frames(file);
  ASSERT_EQ(cold.size(), 49u);
  std::set<uint32_t> distinct;
  for (const Bytes & f : cold) {
    ASSERT_EQ(f.size(), static_cast<size_t>(kW) * kH * 4u);
    distinct.insert(crc32_of(f));
  }
  // Frames that were all alike would let a decoder that drops its state pass.
  std::set<uint32_t> first_run;
  for (size_t i = 1; i <= 20; ++i) {
    first_run.insert(crc32_of(cold[i]));
  }
  std::printf("[fixture] distinct frames %zu of 49, %zu in the first run\n",
      distinct.size(), first_run.size());
  EXPECT_GE(distinct.size(), 20u);
  EXPECT_GE(first_run.size(), 5u);
  // The pixels the references-by-pointer decoder must keep producing, taken
  // from the decoder that copied every reference picture on every frame.
  Bytes all;
  for (const Bytes & f : cold) {
    all.insert(all.end(), f.begin(), f.end());
  }
  EXPECT_EQ(crc32_of(all), 2825369810u);
}

TEST(WebpInterRun, EveryOrderGivesTheColdDecode) {
  const Bytes file = build_inter_animation();
  const std::vector<Bytes> cold = cold_frames(file);
  for (const char * kind : {"sequential", "reverse", "random", "loop"}) {
    Walk w(file);
    for (size_t i : order_of(kind, cold.size())) {
      EXPECT_EQ(w.decode(i), cold[i]) << kind << " " << i;
    }
  }
}

TEST(WebpInterRun, WalkingARunOfInterFramesIsLinear) {
  const Bytes file = build_inter_animation();
  long per_frame = 0;
  {
    Walk one(file);
    one.decode(0);
    per_frame = one.cost();
  }
  Walk once(file);
  const size_t n = once.frames();
  for (size_t i = 0; i < n; ++i) {
    once.decode(i);
  }
  const long one_pass = once.cost();
  // A looping player's second pass starts over at frame 0.
  for (size_t i = 0; i < n; ++i) {
    once.decode(i);
  }
  const long two_passes = once.cost();
  std::printf("[cost] inter frames=%zu per_frame=%ld walk=%ld two_walks=%ld\n",
      n, per_frame, one_pass, two_passes);
  // Decoding every frame once is about n frame decodes.  Each frame replayed
  // from the key frame at the head of its run costs the run's length so far,
  // which is 334 frame decodes for this file against 49; three times n leaves
  // room for the cache's own allocations and none for that.
  EXPECT_LE(one_pass, 3 * per_frame * static_cast<long>(n));
  EXPECT_LE(two_passes, 6 * per_frame * static_cast<long>(n));
}

TEST(WebpInterRun, TheFrameAfterOneCostsOneFrameNotTheRunBeforeIt) {
  const Bytes file = build_inter_animation();
  long per_frame = 0;
  {
    Walk one(file);
    one.decode(0);
    per_frame = one.cost();
  }
  // Frame 19 is nineteen frames into a run of twenty.
  Walk w(file);
  w.decode(19);
  const long before = w.cost();
  w.decode(20);
  const long step = w.cost() - before;
  std::printf("[cost] inter per_frame=%ld step=%ld\n", per_frame, step);
  EXPECT_LE(step, 2 * per_frame);
}

namespace {

/** 256 x 256 animation of a key frame and ten inter frames of @p table. */
Bytes memory_fixture(const InterSpec * table, size_t table_size) {
  const uint32_t w = 256, h = 256;
  const Bytes k = chunk_payload(
      encode_still(w, h, picture(w, h, 7, false), true), "VP8 ");
  std::vector<AnimFrame> frames;
  frames.push_back({"VP8 ", k, 2});
  for (size_t j = 0; j < 10; ++j) {
    frames.push_back(
        {"VP8 ", inter_frame(16 * 16, table[j % table_size]), 2});
  }
  return build_anim(w, h, frames);
}

}  // namespace

TEST(WebpInterRun, TheCacheHoldsTheReferencesNotCopiesOfThem) {
  const size_t pixels = 256u * 256u;
  const size_t picture_bytes = pixels * 3u / 2u; // y plane plus two quarter planes
  // Every frame refreshes all three references: they are one picture, so the
  // cache is the canvas and that picture.  Copies would be three.
  const InterSpec all[1] = {{0, 1, 1, 1, 0, 0, 20}};
  {
    Walk walk(memory_fixture(all, 1));
    for (size_t i = 0; i < walk.frames(); ++i) {
      walk.decode(i);
      EXPECT_LE(walk.held_above_load(), pixels * 4u + picture_bytes + 8192u) << i;
    }
    std::printf("[mem] inter, one picture: held=%zu peak=%zu\n",
        walk.held_above_load(), walk.peak_above_load());
  }
  // Frames that leave golden and the alternate where they were: as many
  // pictures as references that differ, and never more than three.
  const InterSpec mixed[6] = {spec_for(0), spec_for(1), spec_for(2),
      spec_for(3), spec_for(4), spec_for(5)};
  {
    Walk walk(memory_fixture(mixed, 6));
    size_t most = 0;
    for (size_t i = 0; i < walk.frames(); ++i) {
      walk.decode(i);
      most = std::max(most, walk.held_above_load());
      EXPECT_LE(walk.held_above_load(), pixels * 4u + 3u * picture_bytes + 8192u)
          << i;
    }
    std::printf("[mem] inter, mixed: most held=%zu peak=%zu\n", most,
        walk.peak_above_load());
  }
}

TEST(WebpInterRun, KeyFramesOnlyMakeNoReferencePictures) {
  const uint32_t w = 256, h = 256;
  const Bytes still = encode_still(w, h, picture(w, h, 9, false), true);
  const Bytes k = chunk_payload(still, "VP8 ");
  std::vector<AnimFrame> frames;
  for (size_t j = 0; j < 8; ++j) {
    frames.push_back({"VP8 ", k, 2});
  }
  const Bytes file = build_anim(w, h, frames);
  const size_t pixels = static_cast<size_t>(w) * h;
  size_t still_peak = 0;
  {
    Walk one(still);
    one.decode(0);
    still_peak = one.peak_above_load();
  }
  Walk walk(file);
  for (size_t i = 0; i < walk.frames(); ++i) {
    walk.decode(i);
  }
  std::printf("[mem] keys-only: held=%zu peak=%zu still_peak=%zu\n",
      walk.held_above_load(), walk.peak_above_load(), still_peak);
  // No later frame reads a reference, so none is made and none is cached: what
  // the document holds is the canvas, and no more.
  EXPECT_LE(walk.held_above_load(), pixels * 4u + 8192u);
  // A frame's decode is a still decode (its raster and working set) plus the
  // canvas it returns and the canvas the cache is taking; reference pictures
  // beyond that were three more full pictures, 4.5 bytes a pixel.
  EXPECT_LE(walk.peak_above_load(), still_peak + pixels * 8u + 8192u);
}

TEST(WebpInterRun, ACapEqualToAColdDecodesPeakStillWalksTheRun) {
  const Bytes file = build_inter_animation();
  const std::vector<Bytes> cold = cold_frames(file);
  size_t worst = 0;
  for (size_t i = 0; i < cold.size(); ++i) {
    Walk fresh(file);
    fresh.decode(i);
    worst = std::max(worst, fresh.peak_above_load());
  }
  GIMG_Limits limits = {};
  limits.max_memory = worst + worst / 4 + 4096;
  GIMG_Decode_Options o = {};
  o.limits = &limits;
  Walk w(file);
  for (size_t i = 0; i < cold.size(); ++i) {
    EXPECT_EQ(w.decode(i, &o), cold[i]) << i;
  }
}

TEST(WebpInterRun, ConcurrentDecodesOfOneDocumentGiveTheColdDecode) {
  const Bytes file = build_inter_animation();
  const std::vector<Bytes> cold = cold_frames(file);
  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  std::vector<std::thread> threads;
  std::vector<int> bad(4, 0);
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&, t]() {
      uint32_t s = 100u + static_cast<uint32_t>(t);
      for (size_t k = 0; k < 120; ++k) {
        const size_t i = t == 0 ? k % cold.size() : next_rand(s) % cold.size();
        GIMG_Raster * r = nullptr;
        if (gimg_item_decode(gimg_doc_item(doc, i), nullptr, &r) != GIMG_OK ||
            Walk::pixels(r) != cold[i]) {
          bad[static_cast<size_t>(t)]++;
        }
        gimg_raster_destroy(r);
      }
    });
  }
  for (std::thread & th : threads) {
    th.join();
  }
  for (int t = 0; t < 4; ++t) {
    EXPECT_EQ(bad[static_cast<size_t>(t)], 0) << "thread " << t;
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
}

namespace {

struct Loaded {
  GIMG_Stream * in = nullptr;
  GIMG_Doc * doc = nullptr;
  explicit Loaded(const Bytes & file) {
    EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in), GIMG_OK);
    EXPECT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  }
  ~Loaded() {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
  }
  void decode_all() {
    for (size_t i = 0; i < gimg_doc_item_count(doc); ++i) {
      EXPECT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, i), nullptr), GIMG_OK);
    }
  }
};

/** The profile on a raster, or empty. */
Bytes profile_of(const GIMG_Raster * r) {
  const GCOL_Color_Info * ci = r ? gimg_raster_color_info_const(r) : nullptr;
  if (!ci || !ci->icc_bytes || ci->icc_size == 0) {
    return Bytes();
  }
  const uint8_t * p = static_cast<const uint8_t *>(ci->icc_bytes);
  return Bytes(p, p + ci->icc_size);
}

}  // namespace

TEST(WebpInterRun, EveryCanvasOfAnAnimationWithInterFramesCarriesTheProfile) {
  const Bytes icc = profile_bytes();
  const Bytes k = chunk_payload(
      encode_still(kW, kH, picture(kW, kH, 1, false), true), "VP8 ");
  std::vector<AnimFrame> frames;
  frames.push_back({"VP8 ", k, 2});
  for (size_t j = 0; j < 5; ++j) {
    frames.push_back({"VP8 ", inter_frame(kMbs, spec_for(j)), 0});
  }
  const Bytes file = build_anim(kW, kH, frames, &icc);
  Loaded l(file);
  for (size_t i = 0; i < gimg_doc_item_count(l.doc); ++i) {
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(l.doc, i), nullptr, &r), GIMG_OK);
    EXPECT_EQ(profile_of(r), icc) << i;
    gimg_raster_destroy(r);
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
