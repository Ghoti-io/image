/**
 * @file
 *
 * What the TIFF writer refuses, and the shapes it has to get right.
 *
 * The comparison sweep in test_tiff_oracle.cpp is the writer's main evidence:
 * everything it writes, libtiff reads and agrees with. What that sweep cannot
 * reach is the writer's *refusals* - it only ever asks for files that can be
 * written - and the geometry that only an unusual raster produces: a row
 * wider than a whole strip, more pages than the layout keeps on the stack.
 *
 * Copyright 2026 by Corey Pennycuff
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
#include <string>
#include <vector>

namespace {

/** A document of @p pages items, each a small raster of @p format. */
GIMG_Doc * make_doc(uint32_t w, uint32_t h, const GIMG_Pixel_Format * format,
    size_t pages) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK) { return nullptr; }
  if (pages > 1u && gimg_doc_set_item_count(doc, pages) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return nullptr;
  }
  for (size_t i = 0; i < pages; i++) {
    GIMG_Raster * raster = nullptr;
    if (gimg_raster_create(w, h, format, GIMG_RASTER_OWNED, nullptr, 0,
            &raster) != GIMG_OK) {
      gimg_doc_destroy(doc);
      return nullptr;
    }
    uint8_t * p = (uint8_t *)gimg_raster_pixels(raster);
    const size_t stride = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < h; y++) {
      for (size_t k = 0; k < stride; k++) {
        p[(y * stride) + k] = (uint8_t)((y * 31u) + k + i);
      }
    }
    gimg_item_set_raster(gimg_doc_item(doc, i), raster);
  }
  return doc;
}

/** Save @p doc as TIFF with @p opts; returns the result and the bytes. */
GIMG_Result save(GIMG_Doc * doc, const GIMG_Save_Options * opts,
    std::vector<uint8_t> * out) {
  GIMG_Stream * stream = nullptr;
  if (gimg_stream_create_memory_output(&stream) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Save_Report report = {};
  const GIMG_Result r = gimg_doc_save(doc, stream, "tiff", opts, &report);
  if (r == GIMG_OK && out) {
    const void * bytes = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &bytes, &size);
    out->assign((const uint8_t *)bytes, (const uint8_t *)bytes + size);
    EXPECT_EQ(report.bytes_written, size);
  }
  gimg_stream_destroy(stream);
  return r;
}

/** Load bytes back and return item 0's pixels without the stride. */
std::vector<uint8_t> reload(const std::vector<uint8_t> & bytes,
    uint32_t * out_w, uint32_t * out_h, size_t * out_items) {
  std::vector<uint8_t> flat;
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
    return flat;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) == GIMG_OK && doc) {
    if (out_items) { *out_items = gimg_doc_item_count(doc); }
    GIMG_Raster * raster = nullptr;
    if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) == GIMG_OK &&
        raster) {
      const uint32_t w = gimg_raster_width(raster);
      const uint32_t h = gimg_raster_height(raster);
      const size_t bpp =
          gimg_raster_bytes_per_pixel(gimg_raster_format(raster));
      const size_t stride = gimg_raster_stride_bytes(raster);
      const uint8_t * p = (const uint8_t *)gimg_raster_pixels(raster);
      if (out_w) { *out_w = w; }
      if (out_h) { *out_h = h; }
      for (uint32_t y = 0; y < h; y++) {
        flat.insert(flat.end(), p + (y * stride),
            p + (y * stride) + (w * bpp));
      }
      gimg_raster_destroy(raster);
    }
  }
  if (doc) { gimg_doc_destroy(doc); }
  gimg_stream_destroy(s);
  return flat;
}

} // namespace

TEST(TiffEncode, EveryOptionThatCannotBeHonouredIsRefused) {
  GIMG_Doc * doc = make_doc(4u, 4u, &GIMG_PIXEL_GRAY8, 1u);
  ASSERT_NE(doc, nullptr);

  struct Bad {
    const char * name;
    uint8_t compression;
    uint8_t predictor;
  };
  const Bad bad[] = {
      {"a compression number this writer has no method for", 9u, 0u},
      {"a predictor the format does not define", 0u, 7u},
      // Not an oversight: differencing with nothing behind it makes a file
      // larger and harder to read for nothing at all, so asking for it can
      // only be a mistake and is answered as one.
      {"horizontal differencing with no compressor behind it", 0u, 2u},
  };
  for (const Bad & b : bad) {
    SCOPED_TRACE(b.name);
    GIMG_Save_Options opts = {};
    opts.tiff_compression = b.compression;
    opts.tiff_predictor = b.predictor;
    EXPECT_EQ(save(doc, &opts, nullptr), GIMG_ERR_UNSUPPORTED);
  }

  // And the one that is allowed, so the refusals above are not simply "this
  // writer refuses everything".
  GIMG_Save_Options fine = {};
  fine.tiff_compression = 3u;
  fine.tiff_predictor = 2u;
  EXPECT_EQ(save(doc, &fine, nullptr), GIMG_OK);
  gimg_doc_destroy(doc);
}

TEST(TiffEncode, ARasterWithNoTiffShapeIsRefusedRatherThanGuessedAt) {
  // A twelve-bit raster has no TIFF this writer produces: the format allows
  // it, and writing one would mean deciding how to pack samples that do not
  // fill a byte. Refused by name rather than narrowed silently, because a
  // caller who asked for 12-bit and got 8 has been given a different picture.
  GIMG_Doc * doc = make_doc(4u, 4u, &GIMG_PIXEL_GRAY12, 1u);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(save(doc, nullptr, nullptr), GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(TiffEncode, ARowWiderThanAStripStillGetsAStrip) {
  // Strips are sized to hold about eight kilobytes, and a row wider than
  // that leaves the division at zero. One row per strip is the answer; zero
  // would be a file with no strips and an infinite loop computing them.
  GIMG_Doc * doc = make_doc(4096u, 3u, &GIMG_PIXEL_RGBA8, 1u);
  ASSERT_NE(doc, nullptr);
  std::vector<uint8_t> bytes;
  ASSERT_EQ(save(doc, nullptr, &bytes), GIMG_OK);
  uint32_t w = 0, h = 0;
  const std::vector<uint8_t> back = reload(bytes, &w, &h, nullptr);
  EXPECT_EQ(w, 4096u);
  EXPECT_EQ(h, 3u);
  ASSERT_EQ(back.size(), (size_t)4096u * 3u * 4u);
  gimg_doc_destroy(doc);
}

TEST(TiffEncode, MorePagesThanTheLayoutKeepsOnTheStack) {
  // The layout keeps the directories' offsets in a fixed array up to
  // sixty-four pages and allocates beyond that. Both arms are files, so both
  // are written here: a boundary that only one side of is ever exercised is
  // a boundary nobody has tested.
  for (size_t pages : {3u, 64u, 100u}) {
    SCOPED_TRACE(pages);
    GIMG_Doc * doc = make_doc(2u, 2u, &GIMG_PIXEL_GRAY8, pages);
    ASSERT_NE(doc, nullptr);
    std::vector<uint8_t> bytes;
    ASSERT_EQ(save(doc, nullptr, &bytes), GIMG_OK);
    size_t items = 0;
    const std::vector<uint8_t> back = reload(bytes, nullptr, nullptr, &items);
    EXPECT_EQ(items, pages) << "every item is a page and every page came back";
    EXPECT_EQ(back.size(), 4u);
    gimg_doc_destroy(doc);
  }
}

TEST(TiffEncode, AnEmptyDocumentIsNotAFile) {
  // A raster of no area, which gimg_raster_create will not make - so the
  // document carries an item with nothing attached, and the writer has
  // nothing to decode either. That is a document this format cannot hold.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(save(doc, nullptr, nullptr), GIMG_ERR_FORMAT);
  gimg_doc_destroy(doc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
