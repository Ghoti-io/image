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

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
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

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
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
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 50,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_DROP_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.bytes_written = 0;
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_KEEP_COMMON_ONLY,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.diagnostics = nullptr;
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
      .interlaced = 0,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_JPEG,
      .exif_thumbnail_quality = 85,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.bytes_written = 0;
  report.diagnostics = nullptr;
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
      .interlaced = 0,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED,
      .exif_thumbnail_quality = 85,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.bytes_written = 0;
  report.diagnostics = nullptr;
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
      .interlaced = 0,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG,
      .exif_thumbnail_quality = 85,
      ._reserved = {0, 0},
  };
  GIMG_Save_Report report = {};
  report.bytes_written = 0;
  report.diagnostics = nullptr;
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
      .interlaced = 0,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_UNCOMPRESSED,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
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
      .interlaced = 0,
      .quality = 85,
      .exif_thumbnail_format = GIMG_EXIF_THUMB_FORMAT_TIFF_JPEG,
      .exif_thumbnail_quality = 85,
      ._reserved = {0, 0},
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
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .interlaced = 0,
      .quality = 0,
      .exif_thumbnail_format = 0,
      .exif_thumbnail_quality = 0,
      ._reserved = {0, 0},
  };
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

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
