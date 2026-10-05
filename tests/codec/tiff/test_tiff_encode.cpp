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

  GIMG_Save_Options odd = {};
  odd.tiff_bigtiff = 9u;
  EXPECT_EQ(save(doc, &odd, nullptr), GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(TiffEncode, ASmallFileStaysClassicUnlessAsked) {
  // The default measures the offsets and keeps version 42 when they fit.
  // Forcing version 43 is the same picture in the wider directory.
  GIMG_Doc * doc = make_doc(4u, 4u, &GIMG_PIXEL_GRAY8, 1u);
  ASSERT_NE(doc, nullptr);
  std::vector<uint8_t> classic;
  ASSERT_EQ(save(doc, nullptr, &classic), GIMG_OK);
  ASSERT_GE(classic.size(), 8u);
  EXPECT_EQ(classic[0], (uint8_t)'I');
  EXPECT_EQ(classic[2] | (classic[3] << 8), 42);
  const size_t ifd = (size_t)classic[4] | ((size_t)classic[5] << 8) |
      ((size_t)classic[6] << 16) | ((size_t)classic[7] << 24);
  ASSERT_LT(ifd + 14u, classic.size());
  // Twelve bytes from the first entry to the second: the classic width.
  const uint16_t first =
      (uint16_t)(classic[ifd + 2] | (classic[ifd + 3] << 8));
  const uint16_t second =
      (uint16_t)(classic[ifd + 14] | (classic[ifd + 15] << 8));
  EXPECT_LT(first, second);

  GIMG_Save_Options force = {};
  force.tiff_bigtiff = GIMG_TIFF_BIG_FORCE;
  std::vector<uint8_t> big;
  ASSERT_EQ(save(doc, &force, &big), GIMG_OK);
  ASSERT_GE(big.size(), 16u);
  EXPECT_EQ(big[2] | (big[3] << 8), 43);
  EXPECT_EQ(big[4] | (big[5] << 8), 8);
  const uint64_t big_ifd = (uint64_t)big[8] | ((uint64_t)big[9] << 8) |
      ((uint64_t)big[10] << 16) | ((uint64_t)big[11] << 24) |
      ((uint64_t)big[12] << 32) | ((uint64_t)big[13] << 40) |
      ((uint64_t)big[14] << 48) | ((uint64_t)big[15] << 56);
  ASSERT_LT(big_ifd + 28u, big.size());
  const uint16_t big_first =
      (uint16_t)(big[big_ifd + 8] | (big[big_ifd + 9] << 8));
  const uint16_t big_second =
      (uint16_t)(big[big_ifd + 28] | (big[big_ifd + 29] << 8));
  EXPECT_LT(big_first, big_second);

  uint32_t w = 0, h = 0;
  const std::vector<uint8_t> back = reload(big, &w, &h, nullptr);
  EXPECT_EQ(w, 4u);
  EXPECT_EQ(h, 4u);
  const std::vector<uint8_t> classic_pixels =
      reload(classic, nullptr, nullptr, nullptr);
  EXPECT_EQ(back, classic_pixels);

  // Big-endian version 43. The 64-bit stores are a different byte order
  // from the little-endian file above.
  GIMG_Save_Options be = {};
  be.tiff_bigtiff = GIMG_TIFF_BIG_FORCE;
  be.tiff_big_endian = 1u;
  std::vector<uint8_t> big_be;
  ASSERT_EQ(save(doc, &be, &big_be), GIMG_OK);
  ASSERT_GE(big_be.size(), 4u);
  EXPECT_EQ(big_be[0], (uint8_t)'M');
  EXPECT_EQ(big_be[1], (uint8_t)'M');
  EXPECT_EQ(big_be[3], 43u);
  EXPECT_EQ(reload(big_be, nullptr, nullptr, nullptr), classic_pixels);

  // Forcing classic on a file that fits is version 42, the same as the default.
  GIMG_Save_Options stay = {};
  stay.tiff_bigtiff = GIMG_TIFF_BIG_CLASSIC;
  std::vector<uint8_t> forced_classic;
  ASSERT_EQ(save(doc, &stay, &forced_classic), GIMG_OK);
  ASSERT_GE(forced_classic.size(), 4u);
  EXPECT_EQ(forced_classic[2] | (forced_classic[3] << 8), 42);

  // Two pages, so the 8-byte next-IFD pointer has somewhere to point. A
  // writer that stored a zero there would come back as the first page only.
  GIMG_Doc * pages = make_doc(2u, 2u, &GIMG_PIXEL_GRAY8, 2u);
  ASSERT_NE(pages, nullptr);
  GIMG_Save_Options force_pages = {};
  force_pages.tiff_bigtiff = GIMG_TIFF_BIG_FORCE;
  std::vector<uint8_t> multi;
  ASSERT_EQ(save(pages, &force_pages, &multi), GIMG_OK);
  size_t items = 0;
  (void)reload(multi, nullptr, nullptr, &items);
  EXPECT_EQ(items, 2u);
  gimg_doc_destroy(pages);
  gimg_doc_destroy(doc);
}

/**
 * A page whose second strip starts past 4 GiB.
 *
 * Sixteen bytes a row and 2^28 rows in the first strip is exactly 4 GiB, so
 * the next strip's offset does not fit in a classic TIFF. The rows are
 * borrowed from the raster, and the bytes are inspected in the output buffer
 * rather than copied: a second 4 GiB buffer is what this machine does not
 * have. The matching read is AStripOffsetPastFourGigabytesIsRead.
 */
TEST(TiffEncode, AnOffsetPastFourGigabytesIsWrittenAsBigTiff) {
  const uint32_t width = 16u;
  const uint32_t rows = 0x10000000u;
  const uint32_t height = rows + 1u;
  // Created empty on purpose. Filling every byte would dirty 4 GiB before
  // the file is even written, and the loop that does it is one store a byte.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(width, height, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, nullptr, 0, &raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  uint8_t * pixels = (uint8_t *)gimg_raster_pixels(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);
  ASSERT_EQ(stride, (size_t)width);
  pixels[0] = 0x11;
  pixels[(size_t)rows * stride] = 0x22;

  GIMG_Save_Options classic = {};
  classic.tiff_rows_per_strip = rows;
  classic.tiff_bigtiff = GIMG_TIFF_BIG_CLASSIC;
  EXPECT_EQ(save(doc, &classic, nullptr), GIMG_ERR_LIMIT);

  GIMG_Save_Options opts = {};
  opts.tiff_rows_per_strip = rows;
  GIMG_Stream * stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&stream), GIMG_OK);
  GIMG_Save_Report report = {};
  const GIMG_Result r = gimg_doc_save(doc, stream, "tiff", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(stream, &bytes, &size);
  const auto * file = static_cast<const uint8_t *>(bytes);
  ASSERT_GE(size, 16u);
  EXPECT_EQ(file[2] | (file[3] << 8), 43);
  const uint64_t ifd = (uint64_t)file[8] | ((uint64_t)file[9] << 8) |
      ((uint64_t)file[10] << 16) | ((uint64_t)file[11] << 24) |
      ((uint64_t)file[12] << 32) | ((uint64_t)file[13] << 40) |
      ((uint64_t)file[14] << 48) | ((uint64_t)file[15] << 56);
  EXPECT_GT(ifd, 0xFFFFFFFFull);
  const uint64_t second = 16ull + (uint64_t)rows * width;
  ASSERT_LT(second, size);
  EXPECT_EQ(file[16], 0x11);
  EXPECT_EQ(file[second], 0x22);
  EXPECT_GT(second, 0xFFFFFFFFull);

  // The two LONG8 StripOffsets in the pool, not the bytes the cursor
  // happened to land on. A directory that stored only the low 32 bits
  // would still have those payload bytes in the right place.
  const uint64_t entries = (uint64_t)file[ifd] | ((uint64_t)file[ifd + 1] << 8) |
      ((uint64_t)file[ifd + 2] << 16) | ((uint64_t)file[ifd + 3] << 24);
  bool saw_offsets = false;
  for (uint64_t i = 0; i < entries; i++) {
    const size_t ent = (size_t)ifd + 8u + (size_t)i * 20u;
    ASSERT_LE(ent + 20u, size);
    const uint16_t tag = (uint16_t)(file[ent] | (file[ent + 1] << 8));
    if (tag != 273u) {
      continue;
    }
    EXPECT_EQ(file[ent + 2] | (file[ent + 3] << 8), 16); // LONG8
    const uint64_t count = (uint64_t)file[ent + 4] |
        ((uint64_t)file[ent + 5] << 8) | ((uint64_t)file[ent + 6] << 16) |
        ((uint64_t)file[ent + 7] << 24);
    EXPECT_EQ(count, 2u);
    uint64_t pool = 0;
    for (int b = 0; b < 8; b++) {
      pool |= (uint64_t)file[ent + 12u + (size_t)b] << (8 * b);
    }
    ASSERT_LE(pool + 16u, size);
    uint64_t first_off = 0;
    uint64_t second_off = 0;
    for (int b = 0; b < 8; b++) {
      first_off |= (uint64_t)file[pool + (size_t)b] << (8 * b);
      second_off |= (uint64_t)file[pool + 8u + (size_t)b] << (8 * b);
    }
    EXPECT_EQ(first_off, 16u);
    EXPECT_EQ(second_off, 16ull + (uint64_t)rows * width);
    saw_offsets = true;
  }
  EXPECT_TRUE(saw_offsets);

  gimg_stream_destroy(stream);
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
