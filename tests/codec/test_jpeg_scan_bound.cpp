/**
 * @file
 *
 * Progressive JPEG scan work: a progressive frame is checked scan by scan
 * against what earlier scans already sent (T.81 G.1.1.1.2), and the number of
 * scans is bounded per component.  Each scan is a full pass over the frame, so
 * without these a file of a few kilobytes could demand a thousand passes.
 *
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

struct Scan {
  uint8_t ss, se, ah, al;
};

static void put(std::vector<uint8_t> & b, std::initializer_list<int> v) {
  for (int x : v) {
    b.push_back((uint8_t)x);
  }
}

/**
 * Progressive 8x8 JPEG with @p ncomp components, 1x1 each, followed by one
 * single-component SOS per entry of @p scans (component 1, no entropy data).
 */
static std::vector<uint8_t> make(const std::vector<Scan> & scans, int ncomp = 1) {
  std::vector<uint8_t> b;
  put(b, {0xFF, 0xD8, 0xFF, 0xC2, 0x00, 8 + 3 * ncomp, 8, 0, 8, 0, 8, ncomp});
  for (int c = 1; c <= ncomp; c++) {
    put(b, {c, 0x11, 0});
  }
  put(b, {0xFF, 0xDB, 0x00, 0x43, 0x00});
  for (int i = 0; i < 64; i++) {
    b.push_back(1);
  }
  put(b, {0xFF, 0xC4, 0x00, 0x13, 0x00});
  for (int i = 0; i < 16; i++) {
    b.push_back(0);
  }
  for (const Scan & s : scans) {
    put(b, {0xFF, 0xDA, 0x00, 0x08, 0x01, 0x01, 0x00, s.ss, s.se,
        (s.ah << 4) | s.al});
  }
  put(b, {0xFF, 0xD9});
  return b;
}

static GIMG_Result load(const std::vector<uint8_t> & jpeg) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return r;
}

} // namespace

// The amplification file: one band sent again and again.  Before the check
// it was accepted up to the 1024-scan ceiling; now the second copy is refused.
TEST(JpegScanBound, RepeatedBandRefused) {
  std::vector<Scan> v = {{0, 0, 0, 0}};
  for (int i = 0; i < 1000; i++) {
    v.push_back({1, 63, 0, 0});
  }
  EXPECT_EQ(load(make({{0, 0, 0, 0}, {1, 63, 0, 0}})), GIMG_OK);
  EXPECT_EQ(load(make({{0, 0, 0, 0}, {1, 63, 0, 0}, {1, 63, 0, 0}})),
      GIMG_ERR_FORMAT);
  EXPECT_EQ(load(make(v)), GIMG_ERR_FORMAT);
}

TEST(JpegScanBound, RepeatedDcRefusedAndRefinementOrder) {
  EXPECT_EQ(load(make({{0, 0, 0, 1}, {0, 0, 1, 0}})), GIMG_OK);
  EXPECT_EQ(load(make({{0, 0, 0, 1}, {0, 0, 0, 1}})), GIMG_ERR_FORMAT);
  // A refinement of a band nothing has sent.
  EXPECT_EQ(load(make({{0, 0, 1, 0}})), GIMG_ERR_FORMAT);
  EXPECT_EQ(load(make({{0, 0, 0, 0}, {1, 5, 1, 0}})), GIMG_ERR_FORMAT);
  // Refinement whose Ah is not the Al the band was left at.
  EXPECT_EQ(load(make({{0, 0, 0, 2}, {0, 0, 2, 1}})), GIMG_OK);
  EXPECT_EQ(load(make({{0, 0, 0, 2}, {0, 0, 3, 2}})), GIMG_ERR_FORMAT);
  // The same refinement twice: the second finds Al, not Ah.
  EXPECT_EQ(load(make({{0, 0, 0, 1}, {0, 0, 1, 0}, {0, 0, 1, 0}})),
      GIMG_ERR_FORMAT);
}

TEST(JpegScanBound, OverlappingBandsRefused) {
  EXPECT_EQ(load(make({{0, 0, 0, 0}, {1, 5, 0, 0}, {6, 63, 0, 0}})), GIMG_OK);
  EXPECT_EQ(load(make({{0, 0, 0, 0}, {1, 5, 0, 0}, {5, 63, 0, 0}})),
      GIMG_ERR_FORMAT);
  // Refining part of a band that was sent whole at a different Al.
  EXPECT_EQ(load(make({{0, 0, 0, 0}, {1, 5, 0, 1}, {1, 3, 1, 0}, {4, 6, 1, 0}})),
      GIMG_ERR_FORMAT);
}

// A legal script, successive approximation on every band, as libjpeg writes.
TEST(JpegScanBound, LegalScriptStillLoads) {
  std::vector<Scan> v = {{0, 0, 0, 1}, {1, 5, 0, 2}, {6, 63, 0, 2},
      {1, 63, 2, 1}, {0, 0, 1, 0}, {1, 63, 1, 0}};
  EXPECT_EQ(load(make(v)), GIMG_OK);
}

// Every scan below is legal on its own; only their number is the problem.
// One component: 64 scans pass, the 65th is GIMG_ERR_LIMIT.  Three
// components: the ceiling is three times that.
TEST(JpegScanBound, ScanCountBoundedPerComponent) {
  auto script = [](int n) {
    std::vector<Scan> v = {{0, 0, 0, 0}};
    for (int k = 1; (int)v.size() < n; k++) {
      v.push_back({(uint8_t)k, (uint8_t)k, 0, 0});
    }
    return v;
  };
  EXPECT_EQ(load(make(script(64))), GIMG_OK);
  // 64 coefficients make 64 scans; reach 65 through DC successive approximation.
  std::vector<Scan> over = {{0, 0, 0, 1}, {0, 0, 1, 0}};
  for (int k = 1; k < 64; k++) {
    over.push_back({(uint8_t)k, (uint8_t)k, 0, 0});
  }
  EXPECT_EQ(over.size(), 65u);
  EXPECT_EQ(load(make(over)), GIMG_ERR_LIMIT);
  // The bound scales with Nf: the same 65 scans are within it for three.
  EXPECT_EQ(load(make(over, 3)), GIMG_OK);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
