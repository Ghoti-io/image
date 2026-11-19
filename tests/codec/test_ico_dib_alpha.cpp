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
 * ICO DIB entries whose rows run top to bottom, and the interplay of a 32-bpp
 * alpha channel with the AND mask.  Every file is assembled here by hand, as a
 * 2x2 image whose pixels are named A B (top row) and C D (bottom row).
 */

#include <cstdint>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

void u16(Bytes & b, unsigned v) {
  b.push_back(v & 255u);
  b.push_back((v >> 8) & 255u);
}

void u32(Bytes & b, uint32_t v) {
  u16(b, v & 0xFFFFu);
  u16(b, v >> 16);
}

void append(Bytes & b, const Bytes & more) {
  b.insert(b.end(), more.begin(), more.end());
}

/** One-entry icon around @p dib. */
Bytes wrap(const Bytes & dib) {
  Bytes f;
  u16(f, 0);
  u16(f, 1);
  u16(f, 1);
  f.push_back(2);
  f.push_back(2);
  f.push_back(0);
  f.push_back(0);
  u16(f, 1);
  u16(f, 32);
  u32(f, (uint32_t)dib.size());
  u32(f, 22);
  append(f, dib);
  return f;
}

/** BITMAPINFOHEADER (or V4 when @p v4) for a 2x2 image; height is XOR+AND. */
Bytes header(unsigned bpp, bool top_down, uint32_t compression, bool v4,
    uint32_t clr_used = 0) {
  Bytes h;
  u32(h, v4 ? 108u : 40u);
  u32(h, 2);
  u32(h, top_down ? (uint32_t)-4 : 4u);
  u16(h, 1);
  u16(h, bpp);
  u32(h, compression);
  u32(h, 0);
  u32(h, 0);
  u32(h, 0);
  u32(h, clr_used);
  u32(h, 0);
  if (v4) {
    u32(h, 0x00FF0000u);
    u32(h, 0x0000FF00u);
    u32(h, 0x000000FFu);
    u32(h, 0xFF000000u);
    h.resize(108, 0);
  }
  return h;
}

/** A two-pixel 32-bpp row of BGRA pixels. */
Bytes row32(const uint8_t (&p0)[4], const uint8_t (&p1)[4]) {
  Bytes r(p0, p0 + 4);
  r.insert(r.end(), p1, p1 + 4);
  return r;
}


/** Pixels A B C D with the given alpha bytes. */
void four(uint8_t a, uint8_t b, uint8_t c, uint8_t d, Bytes & top,
    Bytes & bottom) {
  uint8_t A[4] = {0, 0, 200, a}, B[4] = {0, 200, 0, b};
  uint8_t C[4] = {200, 0, 0, c}, D[4] = {200, 200, 0, d};
  top = row32(A, B);
  bottom = row32(C, D);
}

/** Two AND rows: @p first_bits then @p second_bits as the leftmost byte. */
Bytes mask_rows(uint8_t first_bits, uint8_t second_bits) {
  Bytes m = {first_bits, 0, 0, 0, second_bits, 0, 0, 0};
  return m;
}

/** AND mask in which only the pixels in the "transparent" set are 1. */
struct Mask {
  bool a, b, c, d;
};

Bytes mask_for(const Mask & t, bool top_down) {
  uint8_t top = (uint8_t)((t.a ? 0x80 : 0) | (t.b ? 0x40 : 0));
  uint8_t bottom = (uint8_t)((t.c ? 0x80 : 0) | (t.d ? 0x40 : 0));
  return top_down ? mask_rows(top, bottom) : mask_rows(bottom, top);
}

Bytes xor32(const Bytes & top, const Bytes & bottom, bool top_down) {
  Bytes x;
  append(x, top_down ? top : bottom);
  append(x, top_down ? bottom : top);
  return x;
}

GIMG_Result decode(const Bytes & file, GIMG_Raster ** out) {
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(file.data(), file.size(), &s);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, out);
  gimg_doc_destroy(doc);
  return r;
}

/** Alpha of the pixel at (x, y) (top-down raster). */
uint8_t alpha(const GIMG_Raster * r, unsigned x, unsigned y) {
  const uint8_t * p = static_cast<const uint8_t *>(gimg_raster_pixels_const(r));
  return p[y * gimg_raster_stride_bytes(r) + x * 4u + 3u];
}

uint8_t red(const GIMG_Raster * r, unsigned x, unsigned y) {
  const uint8_t * p = static_cast<const uint8_t *>(gimg_raster_pixels_const(r));
  return p[y * gimg_raster_stride_bytes(r) + x * 4u];
}

void expect_alpha(const GIMG_Raster * r, int a, int b, int c, int d) {
  EXPECT_EQ(alpha(r, 0, 0), a);
  EXPECT_EQ(alpha(r, 1, 0), b);
  EXPECT_EQ(alpha(r, 0, 1), c);
  EXPECT_EQ(alpha(r, 1, 1), d);
}

Bytes dib32(bool top_down, const Bytes & top, const Bytes & bottom,
    const Bytes & mask) {
  Bytes d = header(32, top_down, 0, false);
  append(d, xor32(top, bottom, top_down));
  append(d, mask);
  return d;
}

}  // namespace

TEST(IcoDibAlpha, BottomUpAlphaIsKept) {
  Bytes top, bottom;
  four(10, 20, 30, 40, top, bottom);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(dib32(false, top, bottom, mask_rows(0, 0))), &r),
      GIMG_OK);
  expect_alpha(r, 10, 20, 30, 40);
  EXPECT_EQ(red(r, 0, 0), 200);  // A is red; the row order is right
  gimg_raster_destroy(r);
}

TEST(IcoDibAlpha, TopDownAlphaFollowsItsColourRows) {
  Bytes top, bottom;
  four(10, 20, 30, 40, top, bottom);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(dib32(true, top, bottom, mask_rows(0, 0))), &r),
      GIMG_OK);
  expect_alpha(r, 10, 20, 30, 40);
  EXPECT_EQ(red(r, 0, 0), 200);
  EXPECT_EQ(red(r, 0, 1), 0);
  gimg_raster_destroy(r);
}

TEST(IcoDibAlpha, RealAlphaIgnoresAndMaskInBothOrders) {
  Bytes top, bottom;
  four(10, 20, 30, 40, top, bottom);
  for (bool td : {false, true}) {
    GIMG_Raster * r = nullptr;
    Bytes mask = mask_for({true, true, true, true}, td);
    ASSERT_EQ(decode(wrap(dib32(td, top, bottom, mask)), &r), GIMG_OK);
    expect_alpha(r, 10, 20, 30, 40);
    gimg_raster_destroy(r);
  }
}

TEST(IcoDibAlpha, ZeroAlphaUsesMaskInTheDibsRowOrder) {
  Bytes top, bottom;
  four(0, 0, 0, 0, top, bottom);
  for (bool td : {false, true}) {
    GIMG_Raster * r = nullptr;
    Bytes mask = mask_for({true, false, false, true}, td);
    ASSERT_EQ(decode(wrap(dib32(td, top, bottom, mask)), &r), GIMG_OK);
    expect_alpha(r, 0, 255, 255, 0);
    gimg_raster_destroy(r);
  }
}

TEST(IcoDibAlpha, ZeroAlphaWithNoMaskIsOpaque) {
  Bytes top, bottom;
  four(0, 0, 0, 0, top, bottom);
  for (bool td : {false, true}) {
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(decode(wrap(dib32(td, top, bottom, Bytes())), &r), GIMG_OK);
    expect_alpha(r, 255, 255, 255, 255);
    gimg_raster_destroy(r);
  }
}

TEST(IcoDibAlpha, ZeroAlphaWithShortMaskIsOpaque) {
  Bytes top, bottom;
  four(0, 0, 0, 0, top, bottom);
  GIMG_Raster * r = nullptr;
  Bytes mask = {0xFF, 0, 0, 0};  // one row of two
  ASSERT_EQ(decode(wrap(dib32(false, top, bottom, mask)), &r), GIMG_OK);
  expect_alpha(r, 255, 255, 255, 255);
  gimg_raster_destroy(r);
}

TEST(IcoDibAlpha, RealAlphaWithShortMaskDecodes) {
  Bytes top, bottom;
  four(10, 20, 30, 40, top, bottom);
  GIMG_Raster * r = nullptr;
  Bytes mask = {0xFF, 0, 0, 0};
  ASSERT_EQ(decode(wrap(dib32(true, top, bottom, mask)), &r), GIMG_OK);
  expect_alpha(r, 10, 20, 30, 40);
  gimg_raster_destroy(r);
}

TEST(IcoDibAlpha, PaletteWithMaskInBothOrders) {
  for (bool td : {false, true}) {
    Bytes d = header(8, td, 0, false, 2);
    u32(d, 0x000000C8u);  // index 0: blue
    u32(d, 0x00C80000u);
    // Rows of two indices, padded to four bytes: top A B, bottom C D.
    Bytes top = {0, 1, 0, 0}, bottom = {1, 0, 0, 0};
    append(d, td ? top : bottom);
    append(d, td ? bottom : top);
    append(d, mask_for({false, true, true, false}, td));
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
    expect_alpha(r, 255, 0, 0, 255);
    EXPECT_NE(red(r, 0, 0), red(r, 1, 0));  // A and B use different entries
    EXPECT_EQ(red(r, 0, 0), red(r, 1, 1));  // A and D the same
    gimg_raster_destroy(r);
  }
}

TEST(IcoDibAlpha, PaletteWithShortMaskIsCorrupt) {
  Bytes d = header(8, false, 0, false, 2);
  u32(d, 0);
  u32(d, 0x00FFFFFFu);
  append(d, Bytes{0, 1, 0, 0, 1, 0, 0, 0});
  append(d, Bytes{0xFF, 0, 0, 0});
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(decode(wrap(d), &r), GIMG_ERR_CORRUPT);
}

TEST(IcoDibAlpha, BitfieldsWithoutAlphaMaskTakesTransparencyFromAndMask) {
  for (bool td : {false, true}) {
    Bytes d = header(32, td, 3, false);
    u32(d, 0x00FF0000u);
    u32(d, 0x0000FF00u);
    u32(d, 0x000000FFu);
    Bytes top, bottom;
    four(77, 77, 77, 77, top, bottom);  // fourth byte is padding here
    append(d, xor32(top, bottom, td));
    append(d, mask_for({false, true, false, false}, td));
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
    expect_alpha(r, 255, 0, 255, 255);
    gimg_raster_destroy(r);
  }
}

TEST(IcoDibAlpha, V4AlphaMaskIsTheDeclaredChannel) {
  for (bool td : {false, true}) {
    Bytes d = header(32, td, 3, true);
    Bytes top, bottom;
    four(10, 20, 30, 40, top, bottom);
    append(d, xor32(top, bottom, td));
    append(d, mask_for({true, true, true, true}, td));
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
    expect_alpha(r, 10, 20, 30, 40);
    gimg_raster_destroy(r);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
