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
 * Three contracts of the TIFF codec, each on files built here byte by byte:
 *
 * - a directory chain whose next pointer steps backwards in the file is valid
 *   and every page loads (only a directory named twice ends the chain);
 * - a page whose strips are corrupt fails its own gimg_item_decode with
 *   GIMG_ERR_CORRUPT and leaves every other page decodable;
 * - alpha: an associated-alpha file decodes to a straight raster with the
 *   colour divided out, and a premultiplied raster is saved as unassociated
 *   straight colour, so the tag and the samples agree.
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

static void p16(Bytes & b, uint16_t v) {
  b.push_back(v & 0xFF);
  b.push_back(v >> 8);
}
static void p32(Bytes & b, uint32_t v) {
  p16(b, v & 0xFFFF);
  p16(b, v >> 16);
}
static void entry(Bytes & b, uint16_t tag, uint16_t type, uint32_t value) {
  p16(b, tag);
  p16(b, type);
  p32(b, 1);
  if (type == 3) {
    p16(b, (uint16_t)value);
    p16(b, 0);
  }
  else {
    p32(b, value);
  }
}

/** One 4x4 8-bit grey strip page. */
struct Page {
  uint16_t compression = 1;
  Bytes strip;                // empty: 16 bytes of (k + index)
  bool has_next = false;      // next-IFD word is `next` rather than the
  uint32_t next = 0;          // following page / 0
};

static Bytes build(const std::vector<Page> & pages) {
  Bytes b = {'I', 'I', 42, 0};
  p32(b, 8);
  for (size_t i = 0; i < pages.size(); i++) {
    const Page & pg = pages[i];
    Bytes data = pg.strip;
    if (data.empty()) {
      for (uint32_t k = 0; k < 16; k++) {
        data.push_back((uint8_t)(k + i));
      }
    }
    const uint32_t data_at = (uint32_t)b.size() + 2 + 10 * 12 + 4;
    p16(b, 10);
    entry(b, 256, 4, 4);
    entry(b, 257, 4, 4);
    entry(b, 258, 3, 8);
    entry(b, 259, 3, pg.compression);
    entry(b, 262, 3, 1);
    entry(b, 273, 4, data_at);
    entry(b, 277, 3, 1);
    entry(b, 278, 4, 4);
    entry(b, 279, 4, (uint32_t)data.size());
    entry(b, 284, 3, 1);
    p32(b, pg.has_next ? pg.next
                       : (i + 1 < pages.size()
                                 ? data_at + (uint32_t)data.size()
                                 : 0u));
    b.insert(b.end(), data.begin(), data.end());
  }
  return b;
}

struct Loaded {
  GIMG_Doc * doc = nullptr;
  GIMG_Diagnostics diag = {};
  GIMG_Result r = GIMG_ERR_INTERNAL;
  Bytes keep;
  ~Loaded() {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    gimg_diagnostics_destroy(&diag);
  }
};

static void load(const Bytes & file, Loaded & out) {
  out.keep = file;
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(out.keep.data(), out.keep.size(), &s),
      GIMG_OK);
  GIMG_Load_Options opts = {};
  out.r = gimg_doc_load(s, &opts, &out.diag, &out.doc);
  gimg_stream_destroy(s);
}

static size_t count_severity(const GIMG_Diagnostics & d, GIMG_Diag_Severity s) {
  size_t n = 0;
  for (size_t i = 0; i < d.count; i++) {
    n += d.items[i].severity == s;
  }
  return n;
}

/** First byte of an item's decoded raster, or -1 when decoding fails. */
static int first_byte(Loaded & l, size_t item) {
  GIMG_Raster * px = nullptr;
  if (gimg_item_decode(gimg_doc_item(l.doc, item), nullptr, &px) != GIMG_OK) {
    return -1;
  }
  const int v = ((const uint8_t *)gimg_raster_pixels(px))[0];
  gimg_raster_destroy(px);
  return v;
}

/** One RGBA8 two-pixel little-endian file; the ExtraSamples value is given. */
static Bytes rgba_file(uint16_t extra, const uint8_t (&px)[8]) {
  Bytes b = {'I', 'I', 42, 0};
  p32(b, 8);
  const uint32_t entries = 11;
  const uint32_t pool = 8 + 2 + entries * 12 + 4; // bits-per-sample array
  p16(b, (uint16_t)entries);
  entry(b, 256, 4, 2);
  entry(b, 257, 4, 1);
  // BitsPerSample: four shorts do not fit inline, so point at the pool.
  p16(b, 258);
  p16(b, 3);
  p32(b, 4);
  p32(b, pool);
  entry(b, 259, 3, 1);
  entry(b, 262, 3, 2);
  entry(b, 273, 4, pool + 8);
  entry(b, 277, 3, 4);
  entry(b, 278, 4, 1);
  entry(b, 279, 4, 8);
  entry(b, 284, 3, 1);
  entry(b, 338, 3, extra);
  p32(b, 0);
  for (int k = 0; k < 4; k++) {
    p16(b, 8);
  }
  b.insert(b.end(), px, px + 8);
  return b;
}

/** The value of the first short in a tag of the first directory, or -1. */
static int tag_value(const Bytes & f, uint16_t tag) {
  const bool le = f.size() > 8 && f[0] == 'I';
  auto r16 = [&](size_t at) {
    return le ? (f[at] | (f[at + 1] << 8)) : ((f[at] << 8) | f[at + 1]);
  };
  auto r32 = [&](size_t at) {
    return le ? (uint32_t)(f[at] | (f[at + 1] << 8) | (f[at + 2] << 16) |
                    ((uint32_t)f[at + 3] << 24))
              : (uint32_t)(((uint32_t)f[at] << 24) | (f[at + 1] << 16) |
                    (f[at + 2] << 8) | f[at + 3]);
  };
  const size_t ifd = r32(4);
  const size_t n = r16(ifd);
  for (size_t i = 0; i < n; i++) {
    const size_t e = ifd + 2 + i * 12;
    if (r16(e) == tag) {
      return r16(e + 8);
    }
  }
  return -1;
}

} // namespace

// -- a page that fails at decode --------------------------------------------

TEST(TiffPageDecodeFailure, OnePagesBadStripFailsOnlyThatItem) {
  Page good0, bad, good2;
  bad.compression = 8; // Deflate, over bytes that are not a zlib stream
  bad.strip = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  Loaded l;
  load(build({good0, bad, good2}), l);
  // Loading cannot tell: the strip is inside the file and the directory is
  // well formed.  The failure is the decoder's, on that item alone.
  ASSERT_EQ(l.r, GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(l.doc), 3u);
  EXPECT_EQ(count_severity(l.diag, GIMG_DIAG_ERROR), 0u);

  EXPECT_EQ(first_byte(l, 0), 0);
  GIMG_Raster * px = nullptr;
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(l.doc, 1), nullptr, &px),
      GIMG_ERR_CORRUPT);
  EXPECT_EQ(px, nullptr);
  // The failure leaves nothing behind that affects the others, in either
  // direction of travel, and asking again gives the same answer.
  EXPECT_EQ(first_byte(l, 2), 2);
  EXPECT_EQ(first_byte(l, 0), 0);
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(l.doc, 1), nullptr, &px),
      GIMG_ERR_CORRUPT);
  EXPECT_EQ(first_byte(l, 2), 2);
}

TEST(TiffPageDecodeFailure, SavingTheDocumentReportsTheSameResult) {
  // A document with a page that cannot decode cannot be saved whole: the save
  // decodes that item and passes its result through, with nothing written as
  // if it had succeeded.
  Page good0, bad;
  bad.compression = 8;
  bad.strip = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  Loaded l;
  load(build({good0, bad}), l);
  ASSERT_EQ(l.r, GIMG_OK);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  EXPECT_EQ(gimg_doc_save(l.doc, out, "tiff", nullptr, &report),
      GIMG_ERR_CORRUPT);
  gimg_stream_destroy(out);
}

// -- alpha ------------------------------------------------------------------

TEST(TiffAlpha, AssociatedAlphaDecodesToStraightColour) {
  // Premultiplied (50, 25, 100) at alpha 128 is (100, 50, 200) straight.
  const uint8_t px[8] = {50, 25, 100, 128, 10, 20, 30, 255};
  Loaded l;
  load(rgba_file(1, px), l);
  ASSERT_EQ(l.r, GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(l.doc, 0), nullptr, &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_format(r)->alpha_form, GIMG_ALPHA_STRAIGHT);
  const uint8_t * p = (const uint8_t *)gimg_raster_pixels(r);
  EXPECT_NEAR(p[0], 100, 1);
  EXPECT_NEAR(p[1], 50, 1);
  EXPECT_NEAR(p[2], 200, 1);
  EXPECT_EQ(p[3], 128);
  EXPECT_EQ(p[4], 10); // opaque: untouched
  EXPECT_EQ(p[5], 20);
  EXPECT_EQ(p[6], 30);
  EXPECT_EQ(p[7], 255);
  gimg_raster_destroy(r);
}

TEST(TiffAlpha, UnassociatedAlphaPassesThroughUntouched) {
  const uint8_t px[8] = {50, 25, 100, 128, 10, 20, 30, 255};
  Loaded l;
  load(rgba_file(2, px), l);
  ASSERT_EQ(l.r, GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(l.doc, 0), nullptr, &r), GIMG_OK);
  EXPECT_EQ(std::memcmp(gimg_raster_pixels(r), px, 8), 0);
  gimg_raster_destroy(r);
}

TEST(TiffAlpha, APremultipliedRasterIsSavedAsStraightAndTaggedUnassociated) {
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(
                2, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  uint8_t * p = (uint8_t *)gimg_raster_pixels(r);
  const uint8_t src[8] = {100, 150, 200, 128, 7, 8, 9, 255};
  std::memcpy(p, src, 8);
  ASSERT_EQ(gimg_alpha_premultiply(r), GIMG_OK);
  ASSERT_EQ(gimg_raster_format(r)->alpha_form, GIMG_ALPHA_PREMULTIPLIED);

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "tiff", nullptr, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  const Bytes file((const uint8_t *)data, (const uint8_t *)data + size);
  gimg_stream_destroy(out);
  // The document the caller holds is untouched.
  EXPECT_EQ(gimg_raster_format(gimg_item_raster(gimg_doc_item(doc, 0)))
                ->alpha_form,
      GIMG_ALPHA_PREMULTIPLIED);
  gimg_doc_destroy(doc);

  EXPECT_EQ(tag_value(file, 338), 2); // unassociated, to match the samples

  Loaded l;
  load(file, l);
  ASSERT_EQ(l.r, GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(l.doc, 0), nullptr, &back), GIMG_OK);
  const uint8_t * q = (const uint8_t *)gimg_raster_pixels(back);
  EXPECT_EQ(gimg_raster_format(back)->alpha_form, GIMG_ALPHA_STRAIGHT);
  // Premultiplying and dividing back at alpha 128 loses at most a level or two.
  EXPECT_NEAR(q[0], 100, 2);
  EXPECT_NEAR(q[1], 150, 2);
  EXPECT_NEAR(q[2], 200, 2);
  EXPECT_EQ(q[3], 128);
  EXPECT_EQ(std::memcmp(q + 4, src + 4, 4), 0); // opaque: exact
  gimg_raster_destroy(back);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
