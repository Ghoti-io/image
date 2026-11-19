/**
 * @file
 *
 * How much work a JPEG decode does, against what the file spends to ask for
 * it.  The work is counted, not timed: the entropy decoders add the number of
 * 8x8 blocks they visited to a per-thread counter (gimg_jpeg_work_blocks), so
 * a result here is a fact about the file and the code, not about the host.
 *
 * Three ways a small file can be made to cost a great deal are measured, each
 * with a file assembled by hand:
 *
 *  - an arithmetic-coded scan with no entropy data at all, which the decoder
 *    must read as an endless run of zero bytes (T.81 D.2.9);
 *  - a hierarchical sequence of the full image size repeated frame after
 *    frame;
 *  - a progressive frame whose every scan is one long end-of-band run, legal,
 *    and as cheap per scan as a scan can be.
 *
 * Set GIMG_TEST_WORK_REPORT=1 to print the measured figures.  Frames are
 * kept small here; the cost per block was measured at the largest size the
 * default limits admit and is stated in documentation/formats/jpeg.md.
 *
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <initializer_list>
#include <vector>

#include "../../src/codec/jpeg/jpeg_internal.h"

namespace {

using Bytes = std::vector<uint8_t>;

static bool report() {
  const char * e = std::getenv("GIMG_TEST_WORK_REPORT");
  return e && e[0] == '1';
}

static void put(Bytes & b, std::initializer_list<int> v) {
  for (int x : v) {
    b.push_back((uint8_t)x);
  }
}

static void put16(Bytes & b, unsigned v) {
  b.push_back((uint8_t)(v >> 8));
  b.push_back((uint8_t)v);
}

static void dqt(Bytes & b) {
  put(b, {0xFF, 0xDB, 0x00, 0x43, 0x00});
  b.insert(b.end(), 64, 1);
}

/** A frame header: one component, 8 bits, 1x1 sampling, quantizer 0. */
static void frame(Bytes & b, int marker, unsigned w, unsigned h, int ncomp = 1) {
  put(b, {0xFF, marker});
  put16(b, 8 + 3 * ncomp);
  put(b, {8});
  put16(b, h);
  put16(b, w);
  put(b, {ncomp});
  for (int c = 1; c <= ncomp; c++) {
    put(b, {c, 0x11, 0});
  }
}

/** A scan of one component (@p comp) with its entropy data. */
static void scan(Bytes & b, int comp, int ss, int se, int ahal,
    const Bytes & data = Bytes()) {
  put(b, {0xFF, 0xDA, 0x00, 0x08, 0x01, comp, 0x00, ss, se, ahal});
  b.insert(b.end(), data.begin(), data.end());
}

struct Result {
  GIMG_Result load = GIMG_OK;
  GIMG_Result decode = GIMG_OK;
  uint64_t blocks = 0;
  double ms = 0;
};

static Result run(const Bytes & jpeg, const GIMG_Limits * limits = nullptr) {
  Result res;
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s) != GIMG_OK) {
    res.load = GIMG_ERR_INTERNAL;
    return res;
  }
  GIMG_Load_Options lo;
  std::memset(&lo, 0, sizeof(lo));
  lo.limits = limits;
  GIMG_Doc * doc = nullptr;
  res.load = gimg_doc_load(s, &lo, nullptr, &doc);
  if (res.load == GIMG_OK) {
    GIMG_Decode_Options dopt;
    std::memset(&dopt, 0, sizeof(dopt));
    dopt.limits = limits;
    GIMG_Raster * ras = nullptr;
    gimg_jpeg_work_reset();
    auto t0 = std::chrono::steady_clock::now();
    res.decode = gimg_item_decode(gimg_doc_item(doc, 0), &dopt, &ras);
    res.ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0)
                 .count();
    res.blocks = gimg_jpeg_work_blocks();
    if (ras) {
      gimg_raster_destroy(ras);
    }
  }
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return res;
}

static uint64_t blocks_of(unsigned w, unsigned h) {
  return (uint64_t)((w + 7) / 8) * ((h + 7) / 8);
}

/** The encoder's own file for a flat or near-flat gray image. */
static Bytes encode_gray(unsigned w, unsigned h, int mode, bool arith) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(w, h, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  auto * p = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  uint32_t lcg = 12345;
  for (unsigned y = 0; y < h; y++) {
    for (unsigned x = 0; x < w; x++) {
      uint8_t v = 128;
      if (mode == 1) {
        v = (uint8_t)(128 + ((x + y) * 8 / (w + h)));
      }
      else if (mode == 2) {
        lcg = lcg * 1664525u + 1013904223u;
        v = (uint8_t)(128 + ((lcg >> 24) & 1));
      }
      else if (mode == 3) {
        v = (x == w / 2 && y == h / 2) ? 255 : 128;
      }
      p[y * stride + x] = v;
    }
  }
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_from_raster(r, &doc), GIMG_OK);
  gimg_raster_destroy(r);
  GIMG_Save_Options o;
  std::memset(&o, 0, sizeof(o));
  o.jpeg_arithmetic = arith ? 1 : 0;
  o.jpeg_quality = 90;
  Bytes out;
  GIMG_Stream_Callbacks cb = {};
  cb.write = [](void * ctx, const void * buf, size_t n, size_t * wr) {
    auto * v = static_cast<Bytes *>(ctx);
    auto * q = static_cast<const uint8_t *>(buf);
    v->insert(v->end(), q, q + n);
    *wr = n;
    return GIMG_OK;
  };
  cb.ctx = &out;
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_callbacks(&cb, &s), GIMG_OK);
  GIMG_Save_Report rep;
  std::memset(&rep, 0, sizeof(rep));
  EXPECT_EQ(gimg_doc_save(doc, s, "jpeg", &o, &rep), GIMG_OK);
  gimg_stream_destroy(s);
  gimg_doc_destroy(doc);
  return out;
}

/** Bytes between the end of the last SOS header and the EOI. */
static size_t entropy_bytes(const Bytes & f) {
  size_t last_sos = 0;
  for (size_t i = 2; i + 3 < f.size();) {
    if (f[i] != 0xFF) {
      break;
    }
    int m = f[i + 1];
    size_t len = ((size_t)f[i + 2] << 8) | f[i + 3];
    if (m == 0xDA) {
      last_sos = i + 2 + len;
      break;
    }
    i += 2 + len;
  }
  return f.size() - 2 - last_sos;
}

} // namespace

// A progressive Huffman frame whose AC scans are each a run of EOBRUN symbols.
static Bytes eobrun_file(unsigned n, int ncomp, unsigned nscans) {
  Bytes f;
  put(f, {0xFF, 0xD8});
  dqt(f);
  frame(f, 0xC2, n, n, ncomp);
  // AC table 0: one symbol, 0xE0 (EOB14: a run of 2^14 plus 14 more bits),
  // with the one-bit code 0.
  put(f, {0xFF, 0xC4, 0x00, 0x14, 0x10, 1});
  f.insert(f.end(), 15, 0);
  put(f, {0xE0});
  // DC table 0: one symbol, 0, one-bit code 0.
  put(f, {0xFF, 0xC4, 0x00, 0x14, 0x00, 1});
  f.insert(f.end(), 15, 0);
  put(f, {0x00});
  uint64_t blocks = blocks_of(n, n);
  // One symbol covers a run of 2^14 + extra; extra all ones: 32767 blocks.
  uint64_t symbols = (blocks + 32766) / 32767;
  Bytes data;
  uint32_t acc = 0;
  int nb = 0;
  for (uint64_t i = 0; i < symbols; i++) {
    // code '0' then 14 one-bits
    acc = (acc << 15) | 0x3FFFu;
    nb += 15;
    while (nb >= 8) {
      uint8_t byte = (uint8_t)(acc >> (nb - 8));
      data.push_back(byte);
      if (byte == 0xFF) {
        data.push_back(0);
      }
      nb -= 8;
    }
  }
  if (nb) {
    uint8_t byte = (uint8_t)((acc << (8 - nb)) | ((1u << (8 - nb)) - 1));
    data.push_back(byte);
    if (byte == 0xFF) {
      data.push_back(0);
    }
  }
  for (unsigned i = 0; i < nscans; i++) {
    int comp = 1 + (int)(i % (unsigned)ncomp);
    unsigned k = 1 + i / (unsigned)ncomp;
    scan(f, comp, (int)k, (int)k, 0x00, data);
  }
  put(f, {0xFF, 0xD9});
  return f;
}


static void say(const char * what, const Bytes & f, const Result & r) {
  if (report()) {
    std::printf("%s: file=%zu load=%d decode=%d blocks=%llu ms=%.1f\n", what,
        f.size(), (int)r.load, (int)r.decode, (unsigned long long)r.blocks,
        r.ms);
  }
}

// An arithmetic scan with no entropy data is read as zeros (T.81 D.2.9), and
// that is exactly what a real encoder writes for a flat image: our own writer
// puts three bytes of entropy data behind a flat 8192 x 8192 frame.  So no
// floor on entropy bytes per MCU can be enforced without refusing the writer's
// own output.  What bounds the cost is that a scan visits each block once.
TEST(JpegDecodeWork, ArithmeticEmptyScanIsOnePass) {
  const unsigned n = 2048;
  Bytes f;
  put(f, {0xFF, 0xD8});
  dqt(f);
  frame(f, 0xC9, n, n);
  scan(f, 1, 0, 63, 0x00);
  put(f, {0xFF, 0xD9});
  Result r = run(f);
  say("arith-empty", f, r);
  EXPECT_EQ(r.load, GIMG_OK);
  EXPECT_EQ(r.decode, GIMG_OK);
  EXPECT_EQ(r.blocks, blocks_of(n, n));
}

TEST(JpegDecodeWork, FlatImageNeedsAlmostNoEntropyData) {
  for (unsigned n : {1024u, 4096u}) {
    Bytes f = encode_gray(n, n, 0, true);
    Result r = run(f);
    say("arith-flat", f, r);
    EXPECT_EQ(r.decode, GIMG_OK);
    EXPECT_EQ(r.blocks, blocks_of(n, n));
    // Independent of the size: the floor the rule would need is below this.
    EXPECT_LE(entropy_bytes(f), 8u) << n;
  }
}

// Frames of a hierarchical sequence, each as large as the image DHP declares.
static Bytes hier_file(unsigned n, unsigned nframes) {
  Bytes f;
  put(f, {0xFF, 0xD8});
  dqt(f);
  frame(f, 0xDE, n, n);
  for (unsigned k = 0; k < nframes; k++) {
    frame(f, k == 0 ? 0xC9 : 0xCD, n, n);
    scan(f, 1, 0, 63, 0x00);
  }
  put(f, {0xFF, 0xD9});
  return f;
}

// Eight full-size frames are within the budget and cost eight passes; a ninth,
// or the thirty-two the format counts allow, are refused at load.  Before the
// bound all of them loaded and 32 cost 32 passes from 822 bytes.
TEST(JpegDecodeWork, HierarchicalFullSizeFramesBounded) {
  const unsigned n = 1024;
  Bytes ok = hier_file(n, 8);
  Result r = run(ok);
  say("hier-8", ok, r);
  EXPECT_EQ(r.load, GIMG_OK);
  EXPECT_EQ(r.decode, GIMG_OK);
  EXPECT_EQ(r.blocks, 8u * blocks_of(n, n));
  for (unsigned frames : {9u, 32u}) {
    Bytes bad = hier_file(n, frames);
    Result q = run(bad);
    say("hier-over", bad, q);
    EXPECT_EQ(q.load, GIMG_ERR_LIMIT) << frames;
    EXPECT_EQ(q.blocks, 0u);
  }
}

// What the writer produces is a pyramid, which the bound never approaches.
TEST(JpegDecodeWork, HierarchicalPyramidStillLoads) {
  for (int arith = 0; arith < 2; arith++) {
    for (unsigned levels : {1u, 3u, 6u}) {
      GIMG_Raster * ras = nullptr;
      ASSERT_EQ(gimg_raster_create(64, 64, &GIMG_PIXEL_GRAY8,
                    GIMG_RASTER_OWNED, nullptr, 0, &ras),
          GIMG_OK);
      auto * p = static_cast<uint8_t *>(gimg_raster_pixels(ras));
      for (unsigned i = 0; i < 64 * 64; i++) {
        p[i] = (uint8_t)(i * 7);
      }
      GIMG_Doc * doc = nullptr;
      ASSERT_EQ(gimg_doc_from_raster(ras, &doc), GIMG_OK);
      gimg_raster_destroy(ras);
      GIMG_Save_Options o;
      std::memset(&o, 0, sizeof(o));
      o.jpeg_hierarchical_levels = (uint8_t)levels;
      o.jpeg_arithmetic = (uint8_t)arith;
      Bytes out;
      GIMG_Stream_Callbacks cb = {};
      cb.write = [](void * ctx, const void * buf, size_t len, size_t * wr) {
        auto * v = static_cast<Bytes *>(ctx);
        auto * q = static_cast<const uint8_t *>(buf);
        v->insert(v->end(), q, q + len);
        *wr = len;
        return GIMG_OK;
      };
      cb.ctx = &out;
      GIMG_Stream * st = nullptr;
      ASSERT_EQ(gimg_stream_create_callbacks(&cb, &st), GIMG_OK);
      GIMG_Save_Report rep;
      std::memset(&rep, 0, sizeof(rep));
      ASSERT_EQ(gimg_doc_save(doc, st, "jpeg", &o, &rep), GIMG_OK);
      gimg_stream_destroy(st);
      gimg_doc_destroy(doc);
      Result r = run(out);
      EXPECT_EQ(r.load, GIMG_OK) << levels << " " << arith;
      EXPECT_EQ(r.decode, GIMG_OK) << levels << " " << arith;
    }
  }
}

// A legal progressive frame cannot take more than 64 scans per component, and
// each scan is a pass over that component's blocks, so the work it can ask for
// is at most 64 visits per block (whatever the coder, however cheap the scan).
// Measured at 4096 x 4096, one component: 63 Huffman scans of one EOB run each,
// 16.5 million visits in 200 ms from 2.3 kB; 64 arithmetic scans of no data,
// 16.8 million in 130 ms.  At the limit the default limits admit (2^28 pixels)
// that is 2^28 visits, a few seconds, against half a second for the one pass a
// sequential frame of the same size costs.  That is the stated ceiling.
TEST(JpegDecodeWork, ProgressiveWorstCaseIsSixtyFourVisitsPerBlock) {
  const unsigned n = 1024;
  {
    Bytes f = eobrun_file(n, 1, 63);
    Result r = run(f);
    say("eobrun-1", f, r);
    EXPECT_EQ(r.decode, GIMG_OK);
    EXPECT_EQ(r.blocks, 63u * blocks_of(n, n));
  }
  {
    Bytes f = eobrun_file(n, 3, 63 * 3);
    Result r = run(f);
    say("eobrun-3", f, r);
    EXPECT_EQ(r.decode, GIMG_OK);
    EXPECT_EQ(r.blocks, 189u * blocks_of(n, n));
    EXPECT_LE(r.blocks, 64u * 3u * blocks_of(n, n));
  }
  {
    // 16 components reach the 1024-scan ceiling, not the per-component one.
    Bytes f = eobrun_file(n / 4, 16, 63 * 16);
    Result r = run(f);
    say("eobrun-16", f, r);
    EXPECT_EQ(r.decode, GIMG_OK);
    EXPECT_EQ(r.blocks, 1008u * blocks_of(n / 4, n / 4));
  }
  {
    Bytes f;
    put(f, {0xFF, 0xD8});
    dqt(f);
    frame(f, 0xCA, n, n);
    scan(f, 1, 0, 0, 0x00);
    for (int k = 1; k <= 63; k++) {
      scan(f, 1, k, k, 0x00);
    }
    put(f, {0xFF, 0xD9});
    Result r = run(f);
    say("arith-prog-64", f, r);
    EXPECT_EQ(r.decode, GIMG_OK);
    EXPECT_EQ(r.blocks, 64u * blocks_of(n, n));
  }
  {
    // One more scan than the bound is refused before any of it runs.
    Bytes f;
    put(f, {0xFF, 0xD8});
    dqt(f);
    frame(f, 0xCA, n, n);
    scan(f, 1, 0, 0, 0x01);
    scan(f, 1, 0, 0, 0x10);
    for (int k = 1; k <= 63; k++) {
      scan(f, 1, k, k, 0x00);
    }
    put(f, {0xFF, 0xD9});
    Result r = run(f);
    EXPECT_EQ(r.load, GIMG_ERR_LIMIT);
    EXPECT_EQ(r.blocks, 0u);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
