/**
 * @file
 *
 * PNG decode tests (Phase 1.2.1): DEFLATE, filters, raster output.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

const unsigned char kPngSignature[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

// IHDR: 1x1 grayscale 8-bit.
const unsigned char kIhdr1x1[] = {0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44,
    0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x3A, 0x7E, 0x9B, 0x55};

const unsigned char kIendChunk[] = {
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

void append(std::vector<uint8_t> & out, const unsigned char * p, size_t n) {
  out.insert(out.end(), p, p + n);
}

} // namespace

TEST(PngDecode, DecodeNoIdatReturnsFormat) {
  // Minimal PNG (signature + IHDR + IEND, no IDAT). Decode should fail.
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdr1x1, sizeof(kIhdr1x1));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);

  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);

  GIMG_Raster * raster = nullptr;
  r = gimg_item_decode(item, nullptr, &raster);
  EXPECT_EQ(r, GIMG_ERR_FORMAT) << "decode with no IDAT should return FORMAT";
  EXPECT_EQ(raster, nullptr);

  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngDecode, DecodeEmptyIdatReturnsCorrupt) {
  // Minimal PNG with IDAT that decompresses to 0 bytes (empty stored block).
  // Expected raw size for 1x1 gray is 2; decompress yields 0 -> corrupt.
  const unsigned char kIdatEmpty[] = {0x00, 0x00, 0x00, 0x05, 0x49, 0x44, 0x41,
      0x54, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xCF, 0xCA, 0x62, 0x86};
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdr1x1, sizeof(kIhdr1x1));
  append(buf, kIdatEmpty, sizeof(kIdatEmpty));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);

  GIMG_Item * item = gimg_doc_item(doc, 0);
  GIMG_Raster * raster = nullptr;
  r = gimg_item_decode(item, nullptr, &raster);
  EXPECT_EQ(r, GIMG_ERR_CORRUPT) << "empty IDAT decompress should be corrupt";
  EXPECT_EQ(raster, nullptr);

  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
