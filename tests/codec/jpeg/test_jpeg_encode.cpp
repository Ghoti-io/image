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
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

#include "jpeg_test_utils.h"

namespace {

TEST(JpegEncode, SaveGrayscaleThenLoadDecode) {
  // Create synthetic doc with 16x16 grayscale raster.
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_create(&doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);

  GIMG_Raster * raster = nullptr;
  r = gimg_raster_create(
      16, 16, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster);
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

  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
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

  std::vector<uint8_t> jpeg_copy(p, p + jpeg_size);
  gimg_stream_destroy(out_stream);
  out_stream = nullptr;

  GIMG_Stream * in_stream = nullptr;
  r = gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream);
  ASSERT_EQ(r, GIMG_OK);

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
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster),
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
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  const unsigned char * jpeg_bytes = (const unsigned char *)jpeg_data;
  std::vector<uint8_t> jpeg_copy(jpeg_bytes, jpeg_bytes + jpeg_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    ASSERT_EQ(r, GIMG_OK) << "Encoded JPEG should load";
  }
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
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(item, raster);

  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report1 = {};
  GIMG_Save_Report report2 = {};

  GIMG_Stream * out1 = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out1), GIMG_OK);
  ASSERT_EQ(gimg_doc_save(doc, out1, "jpeg", &save_opts, &report1), GIMG_OK);

  GIMG_Stream * out2 = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out2), GIMG_OK);
  // Save again (same doc, same options) to second stream.
  ASSERT_EQ(gimg_doc_save(doc, out2, "jpeg", &save_opts, &report2), GIMG_OK);

  const void *d1 = nullptr, *d2 = nullptr;
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

TEST(JpegEncode, QualityOptionUsedWhenNonZero) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(item, raster);

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL, .quality = 50};
  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      (const uint8_t *)jpeg_data, (const uint8_t *)jpeg_data + jpeg_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  item = gimg_doc_item(doc, 0);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &decoded), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/** Minimal TIFF/Exif payload (orientation 6). APP1 EXIF payload = "Exif\0\0" +
 * this. */
static std::vector<uint8_t> make_minimal_exif_payload() {
  return {
      0x49, 0x49, 0x2A, 0x00, 0x08, 0x00, 0x00, 0x00, // II, 42, IFD0@8
      0x01, 0x00,                                     // 1 entry
      0x12, 0x01, 0x03, 0x00, 0x01, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00,
      0x00,                  // Orientation=6
      0x00, 0x00, 0x00, 0x00 // next IFD
  };
}

static const uint32_t kJpegRawApp1Exif = 0xE100u;
static const uint32_t kJpegRawCom = 0xFEu;

/** Create a synthetic doc with 8x8 grayscale raster (no metadata). */
static GIMG_Doc * create_doc_with_raster_only() {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK || !doc)
    return nullptr;
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
          &raster) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return nullptr;
  }
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  return doc;
}

/** Create a synthetic doc with 8x8 grayscale raster and APP1 EXIF in meta_raw.
 */
static GIMG_Doc * create_doc_with_raster_and_exif() {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK || !doc)
    return nullptr;
  GIMG_Meta_Raw * raw = nullptr;
  if (gimg_doc_ensure_meta_raw(doc, &raw) != GIMG_OK)
    return nullptr;
  std::vector<uint8_t> exif = make_minimal_exif_payload();
  std::vector<uint8_t> app1_payload = {'E', 'x', 'i', 'f', 0, 0};
  app1_payload.insert(app1_payload.end(), exif.begin(), exif.end());
  if (gimg_meta_raw_attach(raw, "jpeg", kJpegRawApp1Exif, app1_payload.data(),
          app1_payload.size()) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return nullptr;
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
          &raster) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return nullptr;
  }
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  return doc;
}

TEST(JpegEncode, MetadataPreserveAllRoundTrip) {
  GIMG_Doc * doc = create_doc_with_raster_and_exif();
  ASSERT_NE(doc, nullptr);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  ASSERT_EQ(
      gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_stream, &out_data, &out_size);
  std::vector<uint8_t> saved(out_size, 0);
  if (out_size)
    memcpy(saved.data(), out_data, out_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr)
      << "Re-loaded JPEG with PRESERVE_ALL should have meta_raw";
  size_t exif_size = 0;
  ASSERT_EQ(
      gimg_meta_raw_get(raw, "jpeg", kJpegRawApp1Exif, nullptr, &exif_size),
      GIMG_OK);
  EXPECT_GE(exif_size, 6u + 14u) << "APP1 EXIF should be preserved";
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, ComRoundTrip) {
  // Doc with 8x8 raster and COM in meta_raw (combined format: 2-byte BE length
  // + payload). One COM "Hello" = 00 05 48 65 6C 6C 6F.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Meta_Raw * raw = nullptr;
  ASSERT_EQ(gimg_doc_ensure_meta_raw(doc, &raw), GIMG_OK);
  const uint8_t com_combined[] = {0x00, 0x05, 'H', 'e', 'l', 'l', 'o'};
  ASSERT_EQ(gimg_meta_raw_attach(
                raw, "jpeg", kJpegRawCom, com_combined, sizeof(com_combined)),
      GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  ASSERT_EQ(
      gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_stream, &out_data, &out_size);
  std::vector<uint8_t> saved(out_size, 0);
  if (out_size)
    memcpy(saved.data(), out_data, out_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t com_size = 0;
  ASSERT_EQ(
      gimg_meta_raw_get(raw, "jpeg", kJpegRawCom, nullptr, &com_size), GIMG_OK);
  EXPECT_EQ(com_size, 7u);
  std::vector<uint8_t> com_data(com_size);
  ASSERT_EQ(
      gimg_meta_raw_get(raw, "jpeg", kJpegRawCom, com_data.data(), &com_size),
      GIMG_OK);
  EXPECT_EQ(com_data[0], 0x00);
  EXPECT_EQ(com_data[1], 0x05);
  EXPECT_EQ(memcmp(com_data.data() + 2, "Hello", 5), 0);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, MetaCommonDescriptionWrittenAsCom) {
  // Programmatic doc: set meta_common description, save as JPEG, re-load and
  // verify description (written as COM when no COM in meta_raw).
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Meta_Common * meta = nullptr;
  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &meta), GIMG_OK);
  ASSERT_EQ(gimg_meta_common_set_description(meta, "My caption"), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  ASSERT_EQ(
      gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_stream, &out_data, &out_size);
  std::vector<uint8_t> saved(out_size, 0);
  if (out_size)
    memcpy(saved.data(), out_data, out_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  meta = gimg_doc_meta_common(doc);
  ASSERT_NE(meta, nullptr);
  const char * desc = gimg_meta_common_description(meta);
  ASSERT_NE(desc, nullptr);
  EXPECT_STREQ(desc, "My caption");
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, MetadataDropAllStripsExif) {
  // Doc with 16x16 raster (same as SaveGrayscale); save with DROP_ALL.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                16, 16, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 16 * 16);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_DROP_ALL};
  GIMG_Save_Report report = {};
  ASSERT_EQ(
      gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_stream, &out_data, &out_size);
  std::vector<uint8_t> saved(out_size, 0);
  if (out_size)
    memcpy(saved.data(), out_data, out_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  if (raw) {
    size_t exif_size = 0;
    GIMG_Result r =
        gimg_meta_raw_get(raw, "jpeg", kJpegRawApp1Exif, nullptr, &exif_size);
    EXPECT_NE(r, GIMG_OK)
        << "DROP_ALL: APP1 EXIF should not be present after re-load";
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, MetadataKeepCommonOnlyNoExif) {
  // Doc with raster only; save with KEEP_COMMON_ONLY, re-load has no EXIF.
  GIMG_Doc * doc = create_doc_with_raster_only();
  ASSERT_NE(doc, nullptr);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_KEEP_COMMON_ONLY};
  GIMG_Save_Report report = {};
  ASSERT_EQ(
      gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_stream, &out_data, &out_size);
  std::vector<uint8_t> saved(out_size, 0);
  if (out_size)
    memcpy(saved.data(), out_data, out_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  if (raw) {
    size_t exif_size = 0;
    GIMG_Result r =
        gimg_meta_raw_get(raw, "jpeg", kJpegRawApp1Exif, nullptr, &exif_size);
    EXPECT_NE(r, GIMG_OK)
        << "KEEP_COMMON_ONLY: APP1 EXIF should not be present";
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

// Save failure paths: NULL doc, invalid/unsupported format, unsupported raster.

TEST(JpegEncode, SaveNullDocReturnsError) {
  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r =
      gimg_doc_save(nullptr, out_stream, "jpeg", &save_opts, &report);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(r, GIMG_ERR_INTERNAL);
  gimg_stream_destroy(out_stream);
}

TEST(JpegEncode, SaveUnsupportedFormatReturnsError) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r =
      gimg_doc_save(doc, out_stream, "nosuchformat", &save_opts, &report);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out_stream);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
}

TEST(JpegEncode, SaveCmykRasterReturnsUnsupported) {
  // JPEG encoder supports only grayscale and RGB/RGBA; CMYK returns
  // GIMG_ERR_UNSUPPORTED.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out_stream);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
}

TEST(JpegEncode, SaveTwoItemsExifThumbnailFormat6) {
  // Doc with main image and thumbnail. Save with exif_thumbnail_format=6;
  // re-load and verify we get 2 items, main (item 0) has correct size, item 1
  // decodes.
  constexpr uint32_t kMainW = 16u, kMainH = 16u, kThumbW = 8u, kThumbH = 8u;
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);

  GIMG_Raster * main_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kMainW, kMainH, &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, NULL, 0, &main_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), main_raster);

  GIMG_Raster * thumb_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kThumbW, kThumbH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &thumb_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_JPEG,
      .exif_thumbnail_quality = 85,
  };
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(static_cast<const uint8_t *>(jpeg_data),
      static_cast<const uint8_t *>(jpeg_data) + jpeg_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "Saved two-item doc should load with two items";

  GIMG_Raster * decoded_main = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded_main);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded_main, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded_main), kMainW);
  EXPECT_EQ(gimg_raster_height(decoded_main), kMainH);
  gimg_raster_destroy(decoded_main);

  GIMG_Raster * decoded_thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &decoded_thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded_thumb, nullptr);
  EXPECT_GT(gimg_raster_width(decoded_thumb), 0u);
  EXPECT_GT(gimg_raster_height(decoded_thumb), 0u);
  gimg_raster_destroy(decoded_thumb);
  gimg_doc_destroy(doc);
}

TEST(JpegEncode, SaveTwoItemsExifThumbnailFormat1) {
  // Doc with main image and thumbnail. Save with exif_thumbnail_format=1
  // (uncompressed); re-load and verify second item decodes and dimensions
  // match.
  constexpr uint32_t kMainW = 16u, kMainH = 16u, kThumbW = 8u, kThumbH = 8u;
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);

  GIMG_Raster * main_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kMainW, kMainH, &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, NULL, 0, &main_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), main_raster);

  GIMG_Raster * thumb_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kThumbW, kThumbH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &thumb_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED,
      .exif_thumbnail_quality = 85,
  };
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(static_cast<const uint8_t *>(jpeg_data),
      static_cast<const uint8_t *>(jpeg_data) + jpeg_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "Saved two-item doc with format 1 should load with two items";

  GIMG_Raster * decoded_thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &decoded_thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded_thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded_thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(decoded_thumb), kThumbH);
  gimg_raster_destroy(decoded_thumb);
  gimg_doc_destroy(doc);
}

TEST(JpegEncode, SaveTwoItemsExifThumbnailFormat7) {
  // Doc with main image and thumbnail. Save with exif_thumbnail_format=7
  // (TIFF TechNote 2 JPEG); re-load and verify second item decodes and
  // dimensions match.
  constexpr uint32_t kMainW = 16u, kMainH = 16u, kThumbW = 8u, kThumbH = 8u;
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);

  GIMG_Raster * main_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kMainW, kMainH, &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, NULL, 0, &main_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), main_raster);

  GIMG_Raster * thumb_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kThumbW, kThumbH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &thumb_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG,
      .exif_thumbnail_quality = 85,
  };
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(static_cast<const uint8_t *>(jpeg_data),
      static_cast<const uint8_t *>(jpeg_data) + jpeg_size);
  gimg_stream_destroy(out_stream);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "Saved two-item doc with format 7 should load with two items";

  GIMG_Raster * decoded_thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &decoded_thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded_thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded_thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(decoded_thumb), kThumbH);
  gimg_raster_destroy(decoded_thumb);
  gimg_doc_destroy(doc);
}

TEST(JpegEncode, RoundTripExifThumbnailFormat1) {
  // Save doc with two items and format 1 → load → save again with format 1 →
  // load; thumbnail decodes and dimensions unchanged.
  constexpr uint32_t kThumbW = 6u, kThumbH = 4u;
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  GIMG_Raster * main_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL,
                0, &main_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), main_raster);
  GIMG_Raster * thumb_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kThumbW, kThumbH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &thumb_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED,
      .exif_thumbnail_quality = 0,
  };
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  const void * buf1 = nullptr;
  size_t size1 = 0;
  gimg_stream_output_buffer(out_stream, &buf1, &size1);
  std::vector<uint8_t> jpeg1(static_cast<const uint8_t *>(buf1),
      static_cast<const uint8_t *>(buf1) + size1);
  gimg_stream_destroy(out_stream);
  gimg_doc_destroy(doc);
  doc = nullptr;

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg1.data(), jpeg1.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  ASSERT_GE(gimg_doc_item_count(doc), 2u);

  out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);
  const void * buf2 = nullptr;
  size_t size2 = 0;
  gimg_stream_output_buffer(out_stream, &buf2, &size2);
  std::vector<uint8_t> jpeg2(static_cast<const uint8_t *>(buf2),
      static_cast<const uint8_t *>(buf2) + size2);
  gimg_stream_destroy(out_stream);

  ASSERT_EQ(gimg_stream_create_memory(jpeg2.data(), jpeg2.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u);
  GIMG_Raster * thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(thumb), kThumbH);
  gimg_raster_destroy(thumb);
  gimg_doc_destroy(doc);
}

TEST(JpegEncode, RoundTripExifThumbnailFormat7) {
  // Save doc with two items and format 7 → load → save again with format 7 →
  // load; thumbnail decodes and dimensions unchanged.
  constexpr uint32_t kThumbW = 6u, kThumbH = 4u;
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  GIMG_Raster * main_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL,
                0, &main_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), main_raster);
  GIMG_Raster * thumb_raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kThumbW, kThumbH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &thumb_raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG,
      .exif_thumbnail_quality = 85,
  };
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  const void * buf1 = nullptr;
  size_t size1 = 0;
  gimg_stream_output_buffer(out_stream, &buf1, &size1);
  std::vector<uint8_t> jpeg1(static_cast<const uint8_t *>(buf1),
      static_cast<const uint8_t *>(buf1) + size1);
  gimg_stream_destroy(out_stream);
  gimg_doc_destroy(doc);
  doc = nullptr;

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg1.data(), jpeg1.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  ASSERT_GE(gimg_doc_item_count(doc), 2u);

  out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);
  const void * buf2 = nullptr;
  size_t size2 = 0;
  gimg_stream_output_buffer(out_stream, &buf2, &size2);
  std::vector<uint8_t> jpeg2(static_cast<const uint8_t *>(buf2),
      static_cast<const uint8_t *>(buf2) + size2);
  gimg_stream_destroy(out_stream);

  ASSERT_EQ(gimg_stream_create_memory(jpeg2.data(), jpeg2.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u);
  GIMG_Raster * thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(thumb), kThumbH);
  gimg_raster_destroy(thumb);
  gimg_doc_destroy(doc);
}

TEST(JpegEncode, RoundTripExifThumbnailPreserved) {
  // Load JPEG with EXIF thumbnail, save (preserve), load again; thumbnail
  // still present.
  std::vector<uint8_t> jpeg;
  if (!jpeg_test::load_jpeg_file("jpeg_exif_orientation.jpg", jpeg)) {
    GTEST_SKIP() << "Need tests/data/jpeg/jpeg_exif_orientation.jpg";
  }
  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg.data(), jpeg.size(), &in_stream), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  size_t item_count_before = gimg_doc_item_count(doc);
  uint64_t thumb_hash_before = 0;
  if (item_count_before >= 2) {
    GIMG_Raster * thumb = nullptr;
    if (gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &thumb) == GIMG_OK &&
        thumb) {
      thumb_hash_before = jpeg_test::raster_pixel_hash(thumb);
      gimg_raster_destroy(thumb);
    }
  }

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);

  const void * out_buf = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_stream, &out_buf, &out_size);
  std::vector<uint8_t> saved(out_size);
  memcpy(saved.data(), out_buf, out_size);
  gimg_stream_destroy(out_stream);

  in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_GE(gimg_doc_item_count(doc), item_count_before);
  if (item_count_before >= 2 && gimg_doc_item_count(doc) >= 2) {
    GIMG_Raster * thumb = nullptr;
    r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &thumb);
    ASSERT_EQ(r, GIMG_OK);
    ASSERT_NE(thumb, nullptr);
    EXPECT_EQ(jpeg_test::raster_pixel_hash(thumb), thumb_hash_before)
        << "Thumbnail pixels should match after round-trip";
    gimg_raster_destroy(thumb);
  }
  gimg_doc_destroy(doc);
}

TEST(JpegEncode, ChromaSubsamplingOption) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                32, 32, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < 32; y++) {
    for (uint32_t x = 0; x < 32; x++) {
      pixels[y * stride + x * 4 + 0] = (unsigned char)(x * 8);
      pixels[y * stride + x * 4 + 1] = (unsigned char)(y * 8);
      pixels[y * stride + x * 4 + 2] = 128;
      pixels[y * stride + x * 4 + 3] = 255;
    }
  }
  gimg_item_set_raster(item, raster);

  auto save_and_size = [doc](unsigned chroma) {
    GIMG_Save_Options opts = {
        .metadata_policy = GIMG_META_PRESERVE_ALL,
        .quality = 85,
        .jpeg_chroma_subsampling = (uint8_t)chroma,
    };
    GIMG_Stream * out = nullptr;
    if (gimg_stream_create_memory_output(&out) != GIMG_OK)
      return (size_t)0;
    GIMG_Save_Report report = {};
    GIMG_Result r = gimg_doc_save(doc, out, "jpeg", &opts, &report);
    size_t n = report.bytes_written;
    gimg_stream_destroy(out);
    return (r == GIMG_OK) ? n : (size_t)0;
  };

  size_t size_420 = save_and_size(GIMG_JPEG_CHROMA_420);
  size_t size_422 = save_and_size(GIMG_JPEG_CHROMA_422);
  size_t size_444 = save_and_size(GIMG_JPEG_CHROMA_444);
  EXPECT_GT(size_420, 0u);
  EXPECT_GT(size_422, 0u);
  EXPECT_GT(size_444, 0u);
  EXPECT_LE(size_420, size_422) << "4:2:0 should be <= 4:2:2 for same quality";
  EXPECT_LE(size_422, size_444) << "4:2:2 should be <= 4:4:4 for same quality";

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_420,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      (const uint8_t *)jpeg_data, (const uint8_t *)jpeg_data + jpeg_size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  doc = nullptr;

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(doc), 1u);
  item = gimg_doc_item(doc, 0);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &decoded), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 32u);
  EXPECT_EQ(gimg_raster_height(decoded), 32u);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, ProgressiveDefaultConfigDecodeMatchesBaseline) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      pixels[y * stride + x * 4 + 0] = (unsigned char)((x * 17) & 0xFF);
      pixels[y * stride + x * 4 + 1] = (unsigned char)((y * 13) & 0xFF);
      pixels[y * stride + x * 4 + 2] = 128;
      pixels[y * stride + x * 4 + 3] = 255;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out_baseline = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_baseline), GIMG_OK);
  GIMG_Save_Options opts_baseline = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_420,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_baseline, "jpeg", &opts_baseline, &report),
      GIMG_OK);
  const void * baseline_data = nullptr;
  size_t baseline_size = 0;
  gimg_stream_output_buffer(out_baseline, &baseline_data, &baseline_size);
  std::vector<uint8_t> baseline_copy((const uint8_t *)baseline_data,
      (const uint8_t *)baseline_data + baseline_size);
  gimg_stream_destroy(out_baseline);

  GIMG_Stream * out_prog = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_prog), GIMG_OK);
  GIMG_Save_Options opts_prog = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_420,
      .jpeg_progressive = 1,
  };
  ASSERT_EQ(gimg_doc_save(doc, out_prog, "jpeg", &opts_prog, &report), GIMG_OK);
  const void * prog_data = nullptr;
  size_t prog_size = 0;
  gimg_stream_output_buffer(out_prog, &prog_data, &prog_size);
  std::vector<uint8_t> prog_copy(
      (const uint8_t *)prog_data, (const uint8_t *)prog_data + prog_size);
  gimg_stream_destroy(out_prog);
  gimg_doc_destroy(doc);
  doc = nullptr;

  GIMG_Stream * in_baseline = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(
                baseline_copy.data(), baseline_copy.size(), &in_baseline),
      GIMG_OK);
  GIMG_Doc * doc_baseline = nullptr;
  ASSERT_EQ(
      gimg_doc_load(in_baseline, nullptr, nullptr, &doc_baseline), GIMG_OK);
  GIMG_Raster * decoded_baseline = nullptr;
  ASSERT_EQ(gimg_item_decode(
                gimg_doc_item(doc_baseline, 0), nullptr, &decoded_baseline),
      GIMG_OK);
  uint64_t hash_baseline = jpeg_test::raster_pixel_hash(decoded_baseline);

  GIMG_Stream * in_prog = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(prog_copy.data(), prog_copy.size(), &in_prog),
      GIMG_OK);
  GIMG_Doc * doc_prog = nullptr;
  ASSERT_EQ(gimg_doc_load(in_prog, nullptr, nullptr, &doc_prog), GIMG_OK);
  GIMG_Raster * decoded_prog = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(doc_prog, 0), nullptr, &decoded_prog),
      GIMG_OK);
  uint64_t hash_prog = jpeg_test::raster_pixel_hash(decoded_prog);

  EXPECT_EQ(hash_prog, hash_baseline)
      << "Progressive decode should match baseline decode (same image)";

  gimg_raster_destroy(decoded_baseline);
  gimg_doc_destroy(doc_baseline);
  gimg_stream_destroy(in_baseline);
  gimg_raster_destroy(decoded_prog);
  gimg_doc_destroy(doc_prog);
  gimg_stream_destroy(in_prog);
}

TEST(JpegEncode, ProgressiveCustomScanScriptDecodeMatches) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      pixels[y * stride + x] = (unsigned char)((x + y * 8) & 0xFF);
    }
  }
  gimg_item_set_raster(item, raster);

  static const GIMG_JPEG_Progressive_Scan custom_scans[] = {
      {0, 0, 0, 0},
      {1, 63, 0, 0},
  };
  GIMG_JPEG_Progressive_Config custom_config = {
      .scan_count = 2,
      .scans = custom_scans,
  };
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 90,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
      .jpeg_progressive = 1,
      .jpeg_progressive_config = &custom_config,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      (const uint8_t *)jpeg_data, (const uint8_t *)jpeg_data + jpeg_size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  doc = nullptr;

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
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

TEST(JpegEncode, ProgressiveInvalidScriptReturnsError) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  gimg_item_set_raster(item, raster);

  GIMG_JPEG_Progressive_Scan bad_scan = {5, 3, 0, 0};
  GIMG_JPEG_Progressive_Config bad_config = {
      .scan_count = 1,
      .scans = &bad_scan,
  };
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
      .jpeg_progressive = 1,
      .jpeg_progressive_config = &bad_config,
  };
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out, "jpeg", &opts, &report);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  EXPECT_NE(r, GIMG_OK) << "Ss>Se should be rejected";
  EXPECT_TRUE(r == GIMG_ERR_UNSUPPORTED || r == GIMG_ERR_FORMAT);
}

TEST(JpegEncode, ProgressiveWithRefinementScanDecodeMatchesBaseline) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                16, 16, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      pixels[y * stride + x] = (unsigned char)((x + y * 16) & 0xFF);
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out_baseline = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_baseline), GIMG_OK);
  GIMG_Save_Options opts_baseline = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_baseline, "jpeg", &opts_baseline, &report),
      GIMG_OK);
  const void * baseline_data = nullptr;
  size_t baseline_size = 0;
  gimg_stream_output_buffer(out_baseline, &baseline_data, &baseline_size);
  std::vector<uint8_t> baseline_copy((const uint8_t *)baseline_data,
      (const uint8_t *)baseline_data + baseline_size);
  gimg_stream_destroy(out_baseline);

  static const GIMG_JPEG_Progressive_Scan refine_scans[] = {
      {0, 0, 0, 0},  /* DC initial */
      {1, 63, 0, 0}, /* AC initial Ss=1..63 */
      {1, 63, 1, 0}, /* AC refinement same band */
  };
  GIMG_JPEG_Progressive_Config refine_config = {
      .scan_count = 3,
      .scans = refine_scans,
  };
  GIMG_Stream * out_refine = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_refine), GIMG_OK);
  GIMG_Save_Options opts_refine = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
      .jpeg_progressive = 1,
      .jpeg_progressive_config = &refine_config,
  };
  ASSERT_EQ(
      gimg_doc_save(doc, out_refine, "jpeg", &opts_refine, &report), GIMG_OK);
  const void * refine_data = nullptr;
  size_t refine_size = 0;
  gimg_stream_output_buffer(out_refine, &refine_data, &refine_size);
  std::vector<uint8_t> refine_copy(
      (const uint8_t *)refine_data, (const uint8_t *)refine_data + refine_size);
  gimg_stream_destroy(out_refine);
  gimg_doc_destroy(doc);
  doc = nullptr;

  GIMG_Stream * in_baseline = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(
                baseline_copy.data(), baseline_copy.size(), &in_baseline),
      GIMG_OK);
  GIMG_Doc * doc_baseline = nullptr;
  ASSERT_EQ(
      gimg_doc_load(in_baseline, nullptr, nullptr, &doc_baseline), GIMG_OK);
  GIMG_Raster * decoded_baseline = nullptr;
  ASSERT_EQ(gimg_item_decode(
                gimg_doc_item(doc_baseline, 0), nullptr, &decoded_baseline),
      GIMG_OK);
  uint64_t hash_baseline = jpeg_test::raster_pixel_hash(decoded_baseline);

  GIMG_Stream * in_refine = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(
                refine_copy.data(), refine_copy.size(), &in_refine),
      GIMG_OK);
  GIMG_Doc * doc_refine = nullptr;
  ASSERT_EQ(gimg_doc_load(in_refine, nullptr, nullptr, &doc_refine), GIMG_OK);
  GIMG_Raster * decoded_refine = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(doc_refine, 0), nullptr, &decoded_refine),
      GIMG_OK);
  uint64_t hash_refine = jpeg_test::raster_pixel_hash(decoded_refine);

  EXPECT_EQ(hash_refine, hash_baseline) << "Progressive with refinement decode "
                                           "should match baseline (same image)";

  gimg_raster_destroy(decoded_baseline);
  gimg_doc_destroy(doc_baseline);
  gimg_stream_destroy(in_baseline);
  gimg_raster_destroy(decoded_refine);
  gimg_doc_destroy(doc_refine);
  gimg_stream_destroy(in_refine);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
