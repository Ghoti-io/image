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
 * ICO entries that stray from the usual DIB layout (a colour table the header
 * states above 8 bpp or beyond 2^bpp, a height that is not doubled) and the
 * writer's choice of payload for rasters a 32-bpp DIB cannot hold.  Every
 * file is assembled by hand.
 */

#include <cstdint>
#include <cstring>
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

/** One-entry icon or cursor around @p dib, directory size @p dir x @p dir. */
Bytes wrap(const Bytes & dib, unsigned dir = 2, unsigned type = 1,
    unsigned hx = 1, unsigned hy = 32) {
  Bytes f;
  u16(f, 0);
  u16(f, type);
  u16(f, 1);
  f.push_back(dir);
  f.push_back(dir);
  f.push_back(0);
  f.push_back(0);
  u16(f, hx);
  u16(f, hy);
  u32(f, (uint32_t)dib.size());
  u32(f, 22);
  append(f, dib);
  return f;
}

/** BITMAPINFOHEADER for a 2x2 image with the given biHeight. */
Bytes header(unsigned bpp, uint32_t bi_height, uint32_t clr_used) {
  Bytes h;
  u32(h, 40u);
  u32(h, 2);
  u32(h, bi_height);
  u16(h, 1);
  u16(h, bpp);
  u32(h, 0);
  u32(h, 0);
  u32(h, 0);
  u32(h, 0);
  u32(h, clr_used);
  u32(h, 0);
  return h;
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

const uint8_t * px(const GIMG_Raster * r, unsigned x, unsigned y) {
  return static_cast<const uint8_t *>(gimg_raster_pixels_const(r)) +
      y * gimg_raster_stride_bytes(r) + x * 4u;
}

/** Red channel at (x, y) and alpha, of an RGBA8 raster. */
void expect_px(const GIMG_Raster * r, unsigned x, unsigned y, int red,
    int green, int blue, int a) {
  const uint8_t * p = px(r, x, y);
  EXPECT_EQ(p[0], red) << x << "," << y;
  EXPECT_EQ(p[1], green) << x << "," << y;
  EXPECT_EQ(p[2], blue) << x << "," << y;
  EXPECT_EQ(p[3], a) << x << "," << y;
}

/** 24-bpp rows, bottom-up: bottom row C D, then top row A B (BGR, padded). */
Bytes xor24() {
  Bytes x = {200, 0, 0, 200, 200, 0, 0, 0,   // C blue, D cyan-ish
      0, 0, 200, 0, 200, 0, 0, 0};           // A red, B green
  return x;
}

Bytes mask_none() {
  return Bytes{0, 0, 0, 0, 0, 0, 0, 0};
}

void expect_24bpp_picture(const GIMG_Raster * r) {
  ASSERT_EQ(gimg_raster_width(r), 2u);
  ASSERT_EQ(gimg_raster_height(r), 2u);
  expect_px(r, 0, 0, 200, 0, 0, 255);    // A
  expect_px(r, 1, 0, 0, 200, 0, 255);    // B
  expect_px(r, 0, 1, 0, 0, 200, 255);    // C
  expect_px(r, 1, 1, 0, 200, 200, 255);  // D
}

}  // namespace

TEST(IcoDibShape, ControlWithoutColourTable) {
  Bytes d = header(24, 4, 0);
  append(d, xor24());
  append(d, mask_none());
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
  expect_24bpp_picture(r);
  gimg_raster_destroy(r);
}

TEST(IcoDibShape, ColourTableAbove8BppIsSkipped) {
  // biClrUsed = 3 at 24 bpp: three 4-byte entries sit between the header and
  // the pixels.  They used to be read as the first rows of the image.
  Bytes d = header(24, 4, 3);
  for (int i = 0; i < 12; i++) {
    d.push_back(0xEE);
  }
  append(d, xor24());
  append(d, mask_none());
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
  expect_24bpp_picture(r);
  gimg_raster_destroy(r);
}

TEST(IcoDibShape, ColourTableBeyondTwoToTheBppIsSkipped) {
  // A 1-bpp DIB states five entries; the first two are the palette and the
  // other three are unreachable but still occupy the file.
  Bytes d = header(1, 4, 5);
  const uint8_t palette[8] = {0, 0, 255, 0, 255, 0, 0, 0};  // red, blue
  d.insert(d.end(), palette, palette + 8);
  for (int i = 0; i < 12; i++) {
    d.push_back(0xEE);
  }
  // Rows bottom-up: C D = 1 0, then A B = 0 1 (palette index per pixel).
  Bytes rows = {0x80, 0, 0, 0, 0x40, 0, 0, 0};
  append(d, rows);
  append(d, mask_none());
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
  expect_px(r, 0, 0, 255, 0, 0, 255);  // A: index 0
  expect_px(r, 1, 0, 0, 0, 255, 255);  // B: index 1
  expect_px(r, 0, 1, 0, 0, 255, 255);  // C: index 1
  expect_px(r, 1, 1, 255, 0, 0, 255);  // D: index 0
  gimg_raster_destroy(r);
}

TEST(IcoDibShape, ColourTableLongerThanTheEntryIsRefused) {
  Bytes d = header(24, 4, 0xFFFFFFFFu);
  append(d, xor24());
  append(d, mask_none());
  GIMG_Raster * r = nullptr;
  GIMG_Result res = decode(wrap(d), &r);
  EXPECT_NE(res, GIMG_OK);
  if (res == GIMG_OK) {
    gimg_raster_destroy(r);
  }
}

TEST(IcoDibShape, HeightThatIsNotDoubledDecodesInFull) {
  // biHeight is the real height (2), as some writers store it, and the
  // directory agrees.  The whole picture is the XOR bitmap; there is no mask.
  Bytes d = header(24, 2, 0);
  append(d, xor24());
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
  expect_24bpp_picture(r);
  gimg_raster_destroy(r);
}

TEST(IcoDibShape, HeightThatIsNotDoubledUsesAMaskIfOneFollows) {
  Bytes d = header(24, 2, 0);
  append(d, xor24());
  Bytes mask = {0x40, 0, 0, 0, 0x00, 0, 0, 0};  // bottom row first: D transparent
  append(d, mask);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(d), &r), GIMG_OK);
  ASSERT_EQ(gimg_raster_height(r), 2u);
  expect_px(r, 0, 1, 0, 0, 200, 255);
  EXPECT_EQ(px(r, 1, 1)[3], 0);
  gimg_raster_destroy(r);
}

TEST(IcoDibShape, DoubledHeightStillHalves) {
  // Directory 2, biHeight 4: unchanged behaviour, checked with the same
  // bytes as the undoubled case above.
  Bytes d = header(24, 4, 0);
  append(d, xor24());
  append(d, mask_none());
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode(wrap(d, 2), &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_height(r), 2u);
  gimg_raster_destroy(r);
}

namespace {

GIMG_Result save_to(const GIMG_Doc * doc, const GIMG_Save_Options * opts,
    Bytes & out) {
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory_output(&s);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Save_Report report;
  memset(&report, 0, sizeof(report));
  r = gimg_doc_save(doc, s, "ico", opts, &report);
  if (r == GIMG_OK) {
    const void * data = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(s, &data, &n);
    out.assign(static_cast<const uint8_t *>(data),
        static_cast<const uint8_t *>(data) + n);
  }
  gimg_stream_destroy(s);
  return r;
}

GIMG_Doc * doc_of(const GIMG_Pixel_Format * fmt, bool opaque = false) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(2, 2, fmt, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  memset(gimg_raster_pixels(r), 0x40,
      gimg_raster_stride_bytes(r) * gimg_raster_height(r));
  if (opaque) {
    // Rows may be padded: set the fourth byte of each pixel row by row.
    uint8_t * p = static_cast<uint8_t *>(gimg_raster_pixels(r));
    for (unsigned y = 0; y < 2; y++) {
      for (unsigned x = 0; x < 2; x++) {
        p[y * gimg_raster_stride_bytes(r) + x * 4 + 3] = 255;
      }
    }
  }
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_from_raster(r, &doc), GIMG_OK);
  gimg_raster_destroy(r);
  return doc;
}

}  // namespace

TEST(IcoWriterFormats, FloatRasterIsRefusedNotConverted) {
  GIMG_Doc * doc = doc_of(&GIMG_PIXEL_GRAYF32);
  GIMG_Save_Options opts;
  memset(&opts, 0, sizeof(opts));
  opts.ico_payload = GIMG_ICO_PAYLOAD_DIB;
  Bytes out;
  EXPECT_EQ(save_to(doc, &opts, out), GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(IcoWriterFormats, PlanarRasterCannotReachTheWriter) {
  // The writer reads interleaved samples and now says so in its format check;
  // the only way a planar raster could arrive is if one could be created, and
  // the library refuses to.
  GIMG_Pixel_Format planar = GIMG_PIXEL_RGBA8;
  planar.layout = GIMG_LAYOUT_PLANAR;
  GIMG_Raster * r = nullptr;
  EXPECT_NE(gimg_raster_create(
                2, 2, &planar, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  EXPECT_EQ(r, nullptr);
}

TEST(IcoWriterFormats, OpaqueRgba8StillUsesADibUnderAuto) {
  GIMG_Doc * doc = doc_of(&GIMG_PIXEL_RGBA8, true);
  Bytes out;
  ASSERT_EQ(save_to(doc, nullptr, out), GIMG_OK);
  ASSERT_GT(out.size(), 30u);
  EXPECT_EQ(out[22], 40);  // BITMAPINFOHEADER size
  gimg_doc_destroy(doc);
}

TEST(IcoWriterFormats, CursorWithTopLeftHotspotsIsSavedAsCursor) {
  Bytes d = header(24, 4, 0);
  append(d, xor24());
  append(d, mask_none());
  Bytes file = wrap(d, 2, 2, 0, 0);
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  Bytes out;
  ASSERT_EQ(save_to(doc, nullptr, out), GIMG_OK);
  ASSERT_GT(out.size(), 6u);
  EXPECT_EQ(out[2], 2);  // type: cursor
  gimg_doc_destroy(doc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
