/**
 * @file
 *
 * Limits a caller did not set, and the memory cap under every codec.
 *
 * Two things were true before this file.  Passing no limits meant no limits,
 * so the documented way to call the library was the unbounded one and a
 * header naming a huge picture was simply obeyed.  And max_memory was read
 * by BMP alone, so a caller who set it had bounded one format in seven.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

typedef std::vector<uint8_t> Bytes;

void le16(Bytes & b, uint16_t v) {
  b.push_back((uint8_t)(v & 0xFFu));
  b.push_back((uint8_t)(v >> 8));
}
void le32(Bytes & b, uint32_t v) {
  le16(b, (uint16_t)(v & 0xFFFFu));
  le16(b, (uint16_t)(v >> 16));
}
void entry(Bytes & b, uint16_t tag, uint16_t type, uint32_t count,
    uint32_t value) {
  le16(b, tag);
  le16(b, type);
  le32(b, count);
  if (type == 3 && count == 1) {
    le16(b, (uint16_t)value);
    le16(b, 0);
  }
  else {
    le32(b, value);
  }
}

/** An uncompressed 8-bit gray TIFF header for @p w x @p h, one short strip. */
Bytes gray_tiff(uint32_t w, uint32_t h) {
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  const uint32_t strip_at = 8u + 2u + 7u * 12u + 4u;
  le16(b, 7);
  entry(b, 256, 4, 1, w);
  entry(b, 257, 4, 1, h);
  entry(b, 258, 3, 1, 8);
  entry(b, 259, 3, 1, 1);
  entry(b, 262, 3, 1, 1);
  entry(b, 273, 4, 1, strip_at);
  entry(b, 279, 4, 1, 16);
  le32(b, 0);
  b.resize(b.size() + 16, 0x7F);
  return b;
}

/** Load and decode item 0 with @p limits given at load (NULL for none). */
GIMG_Result open_image(const Bytes & file, const GIMG_Limits * limits,
    uint32_t * out_w = nullptr) {
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Load_Options lo;
  memset(&lo, 0, sizeof lo);
  lo.limits = limits;
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, limits ? &lo : nullptr, nullptr, &doc);
  if (r == GIMG_OK && doc) {
    GIMG_Raster * raster = nullptr;
    r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
    if (raster) {
      if (out_w) {
        *out_w = gimg_raster_width(raster);
      }
      gimg_raster_destroy(raster);
    }
  }
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return r;
}

TEST(DefaultLimits, NoOptionsIsABoundedLoad) {
  uint32_t w = 0;
  ASSERT_EQ(open_image(gray_tiff(64, 64), nullptr, &w), GIMG_OK) << "control";
  EXPECT_EQ(w, 64u);

  // 20000 x 20000 is 4 x 10^8 pixels, past the default of 2^28, in a file of
  // 114 bytes.  With no options this used to be a 400 MB allocation.
  EXPECT_EQ(open_image(gray_tiff(20000, 20000), nullptr), GIMG_ERR_LIMIT);

  // A zeroed struct is the same as none: zero is "the default", not "none".
  GIMG_Limits zero;
  gimg_limits_default(&zero);
  EXPECT_EQ(zero.max_decoded_pixels, 0u);
  EXPECT_EQ(open_image(gray_tiff(20000, 20000), &zero), GIMG_ERR_LIMIT);

  // Exactly the default passes the pixel check and one more does not; the
  // 256 MB raster itself is not asked for, because the memory cap is set
  // below it and answers first.
  GIMG_Limits small_memory;
  gimg_limits_default(&small_memory);
  small_memory.max_memory = (size_t)1 << 20;
  EXPECT_EQ(open_image(gray_tiff(16384, 16384), &small_memory), GIMG_ERR_LIMIT);
}

TEST(DefaultLimits, AskingForNoLimitIsSayingSo) {
  // GIMG_LIMIT_NONE on the pixel count lets the picture through to the
  // memory cap, which is what refuses it here - so the pixel limit really
  // was lifted, and nothing of that size is allocated to show it.
  GIMG_Limits lim;
  gimg_limits_default(&lim);
  lim.max_decoded_pixels = GIMG_LIMIT_NONE;
  lim.max_memory = (size_t)8 << 20;
  EXPECT_EQ(open_image(gray_tiff(20000, 20000), &lim), GIMG_ERR_LIMIT);
  // 2048 x 2048 gray is 4 MiB, inside 8.
  EXPECT_EQ(open_image(gray_tiff(2048, 2048), &lim), GIMG_OK);
  // And the same picture against a cap of 1 MiB.
  lim.max_memory = (size_t)1 << 20;
  EXPECT_EQ(open_image(gray_tiff(2048, 2048), &lim), GIMG_ERR_LIMIT);
  lim.max_memory = GIMG_LIMIT_NONE;
  EXPECT_EQ(open_image(gray_tiff(2048, 2048), &lim), GIMG_OK);
}

/** A 256 x 256 picture of four flat colours, written as @p format. */
Bytes encode(const char * format) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                256, 256, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  uint8_t * px = (uint8_t *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < 256; y++) {
    for (uint32_t x = 0; x < 256; x++) {
      uint8_t * p = px + y * stride + x * 4u;
      p[0] = (x < 128) ? 255 : 0;
      p[1] = (y < 128) ? 255 : 0;
      p[2] = 64;
      p[3] = 255;
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  EXPECT_EQ(gimg_doc_save(doc, out, format, nullptr, &report), GIMG_OK)
      << format;
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  Bytes file((const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return file;
}

TEST(DefaultLimits, TheMemoryCapHoldsUnderEveryCodec) {
  // The decoded raster alone is 256 KiB or more in every format, so a cap of
  // 64 KiB cannot be met by any of them, and one of 64 MiB by all.
  const char * formats[] = {"bmp", "gif", "ico", "jpeg", "png", "tiff",
      "webp"};
  for (const char * fmt : formats) {
    SCOPED_TRACE(fmt);
    const Bytes file = encode(fmt);
    ASSERT_FALSE(file.empty());
    GIMG_Limits lim;
    gimg_limits_default(&lim);
    lim.max_memory = (size_t)64 << 20;
    uint32_t w = 0;
    EXPECT_EQ(open_image(file, &lim, &w), GIMG_OK) << "control";
    EXPECT_EQ(w, 256u);
    lim.max_memory = (size_t)64 << 10;
    EXPECT_EQ(open_image(file, &lim), GIMG_ERR_LIMIT);
  }
}

TEST(DefaultLimits, ALimitGivenAtDecodeIsHeldToo) {
  const Bytes file = encode("png");
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Limits lim;
  gimg_limits_default(&lim);
  lim.max_memory = (size_t)64 << 10;
  GIMG_Decode_Options o;
  memset(&o, 0, sizeof o);
  o.limits = &lim;
  GIMG_Raster * raster = nullptr;
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), &o, &raster),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(raster, nullptr);
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK)
      << "and the document is still good for a decode within the default";
  if (raster) {
    gimg_raster_destroy(raster);
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
