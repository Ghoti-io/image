/**
 * @file
 *
 * JPEG encode tests: save raster to JPEG, re-load and decode; deterministic.
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
#include <vector>

namespace {

TEST(JpegEncode, SaveGrayscaleThenLoadDecode) {
  /* Create synthetic doc with 16x16 grayscale raster. */
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_create(&doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);

  GIMG_Raster * raster = nullptr;
  r = gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
      &raster);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(raster, nullptr);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      pixels[y * stride + x] = (unsigned char)((x + y) & 0xFF);
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out_stream = nullptr;
  r = gimg_stream_create_memory_output(&out_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(out_stream, nullptr);

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      ._reserved = {0},
  };
  GIMG_Save_Report report = {};
  report.bytes_written = 0;
  report.diagnostics = nullptr;
  r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = nullptr;

  ASSERT_EQ(r, GIMG_OK) << "JPEG save should succeed";
  EXPECT_GT(report.bytes_written, 0u);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  ASSERT_NE(jpeg_data, nullptr);
  EXPECT_GT(jpeg_size, 0u);
  EXPECT_GE(jpeg_size, 2u);
  const unsigned char * p = (const unsigned char *)jpeg_data;
  EXPECT_EQ(p[0], 0xFF);
  EXPECT_EQ(p[1], 0xD8);

  GIMG_Stream * in_stream = nullptr;
  r = gimg_stream_create_memory(jpeg_data, jpeg_size, &in_stream);
  ASSERT_EQ(r, GIMG_OK);
  gimg_stream_destroy(out_stream);
  out_stream = nullptr;

  doc = nullptr;
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "Encoded JPEG should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);

  item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(item, nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK) << "Encoded JPEG should decode";
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded), 16u);
  EXPECT_EQ(gimg_raster_height(decoded), 16u);

  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, SaveRgbThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);

  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL,
            0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      pixels[y * stride + x * 4 + 0] = (unsigned char)(x * 32);
      pixels[y * stride + x * 4 + 1] = (unsigned char)(y * 32);
      pixels[y * stride + x * 4 + 2] = 128;
      pixels[y * stride + x * 4 + 3] = 255;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      ._reserved = {0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg_data, jpeg_size, &in_stream),
      GIMG_OK);
  gimg_stream_destroy(out_stream);

  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  item = gimg_doc_item(doc, 0);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &decoded), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, SameInputSameOutputDeterministic) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL,
            0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(item, raster);

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      ._reserved = {0},
  };
  GIMG_Save_Report report1 = {};
  GIMG_Save_Report report2 = {};
  report1.diagnostics = nullptr;
  report2.diagnostics = nullptr;

  GIMG_Stream * out1 = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out1), GIMG_OK);
  ASSERT_EQ(gimg_doc_save(doc, out1, "jpeg", &save_opts, &report1), GIMG_OK);

  GIMG_Stream * out2 = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out2), GIMG_OK);
  /* Save again (same doc, same options) to second stream. */
  ASSERT_EQ(gimg_doc_save(doc, out2, "jpeg", &save_opts, &report2), GIMG_OK);

  const void * d1 = nullptr, * d2 = nullptr;
  size_t s1 = 0, s2 = 0;
  gimg_stream_output_buffer(out1, &d1, &s1);
  gimg_stream_output_buffer(out2, &d2, &s2);
  EXPECT_EQ(s1, s2) << "Same input and options should produce same size";
  if (s1 == s2 && d1 && d2) {
    EXPECT_EQ(memcmp(d1, d2, s1), 0)
        << "Same input and options should produce identical bytes";
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out1);
  gimg_stream_destroy(out2);
}

}  // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
