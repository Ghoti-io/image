/**
 * @file
 *
 * JPEG encode tests: save raster to JPEG, re-load and decode; deterministic.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <ghoti.io/image/bitdepth.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

#include "jpeg_test_utils.h"

extern "C" {
#include "jpeg_huffman_tables_internal.h"
#include "jpeg_internal.h"
}

namespace {

/** RAII: frees doc then stream on scope exit so load+decode tests don't leak on ASSERT. */
struct DocStreamGuard {
  GIMG_Doc * d = nullptr;
  GIMG_Stream * s = nullptr;
  ~DocStreamGuard() {
    if (d) gimg_doc_destroy(d);
    if (s) gimg_stream_destroy(s);
  }
};

/** RAII: frees raster on scope exit so tests don't leak on ASSERT after decode. */
struct RasterGuard {
  GIMG_Raster * r = nullptr;
  ~RasterGuard() {
    if (r) gimg_raster_destroy(r);
  }
};

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

  jpeg_test::write_jpeg_output(
      "baseline_grayscale.jpg", jpeg_copy.data(), jpeg_copy.size());

  std::string jpeg_path =
      jpeg_test::jpeg_output_dir() + "/baseline_grayscale.jpg";
  std::string raw_path =
      jpeg_test::jpeg_output_dir() + "/libjpeg_enc_grayscale.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path.c_str(), raw_path.c_str(), libjpeg_pixels, &oracle_w,
          &oracle_h, &oracle_mode)) {
    EXPECT_EQ(oracle_w, 16u);
    EXPECT_EQ(oracle_h, 16u);
    EXPECT_EQ(oracle_mode, 0) << "grayscale => L mode";
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_pixels.data(), oracle_w, oracle_h, oracle_mode, 0))
        << "our decode must match libjpeg oracle";
  }

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

  jpeg_test::write_jpeg_output(
      "baseline_rgb.jpg", jpeg_copy.data(), jpeg_copy.size());

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
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  item = gimg_doc_item(doc, 0);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &decoded), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);

  std::string jpeg_path_rgb =
      jpeg_test::jpeg_output_dir() + "/baseline_rgb.jpg";
  std::string raw_path_rgb =
      jpeg_test::jpeg_output_dir() + "/libjpeg_enc_rgb.raw";
  std::vector<uint8_t> libjpeg_rgb;
  uint32_t oracle_w_rgb = 0, oracle_h_rgb = 0;
  int oracle_mode_rgb = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path_rgb.c_str(), raw_path_rgb.c_str(), libjpeg_rgb,
          &oracle_w_rgb, &oracle_h_rgb, &oracle_mode_rgb)) {
    EXPECT_EQ(oracle_w_rgb, 8u);
    EXPECT_EQ(oracle_h_rgb, 8u);
    EXPECT_EQ(oracle_mode_rgb, 1) << "RGB => mode 1";
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_rgb.data(), oracle_w_rgb, oracle_h_rgb,
        oracle_mode_rgb, 0))
        << "our decode must match libjpeg oracle exactly (same spec, same bitstream)";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/** Minimal 1x1 RGB: 3 blocks (Y,Cb,Cr), each DC+EOB only. Use 4:4:4 so
 * blocks_per_mcu=3 and component index matches block index. */
TEST(JpegEncode, SaveRgb1x1ThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                1, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
  px[0] = px[1] = px[2] = 128;
  px[3] = 255;
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report),
      GIMG_OK);
  gimg_doc_destroy(doc);

  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out_stream, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      static_cast<const uint8_t *>(jpeg_data),
      static_cast<const uint8_t *>(jpeg_data) + jpeg_size);
  gimg_stream_destroy(out_stream);
  jpeg_test::write_jpeg_output(
      "baseline_1x1.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  GIMG_Result r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  item = gimg_doc_item(doc, 0);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(item, nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK) << "1x1 encode→decode round-trip (see baseline_1x1.jpg)";
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded), 1u);
  EXPECT_EQ(gimg_raster_height(decoded), 1u);
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

  jpeg_test::write_jpeg_output(
      "quality_low.jpg", jpeg_copy.data(), jpeg_copy.size());

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

/** Task 4.2.4: 16-bit JPEG with APP segments: load populates meta_raw; round-trip
 * preserves metadata (APPn segments per T.81 Annex B). Confirms APP parsing is
 * applied for 12/16-bit SOF. */
TEST(JpegEncode, SaveGray16WithExifThenLoadMeta) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Meta_Raw * raw = nullptr;
  ASSERT_EQ(gimg_doc_ensure_meta_raw(doc, &raw), GIMG_OK);
  std::vector<uint8_t> exif = make_minimal_exif_payload();
  std::vector<uint8_t> app1_payload = {'E', 'x', 'i', 'f', 0, 0};
  app1_payload.insert(app1_payload.end(), exif.begin(), exif.end());
  ASSERT_EQ(gimg_meta_raw_attach(raw, "jpeg", kJpegRawApp1Exif,
                app1_payload.data(), app1_payload.size()),
      GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED, NULL,
                0, &raster),
      GIMG_OK);
  uint16_t * px = (uint16_t *)gimg_raster_pixels(raster);
  for (size_t i = 0; i < 8 * 8; i++)
    px[i] = 0x8000;
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
  ASSERT_NE(raw, nullptr)
      << "16-bit JPEG with APP1: re-load must populate meta_raw";
  size_t exif_size = 0;
  ASSERT_EQ(
      gimg_meta_raw_get(raw, "jpeg", kJpegRawApp1Exif, nullptr, &exif_size),
      GIMG_OK);
  EXPECT_GE(exif_size, 6u + 14u) << "APP1 EXIF preserved in 16-bit round-trip";
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

// DHT consistency (task 2.3.2.2): saved DHT payloads must match shared table header.

TEST(JpegEncode, SavedDhtMatchesSharedTableHeader) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  GIMG_Save_Options save_opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  ASSERT_EQ(r, GIMG_OK);

  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out_stream, &data, &size);
  ASSERT_GE(size, 2u);
  const uint8_t * p = (const uint8_t *)data;
  if (p[0] != 0xFF || p[1] != 0xD8) {
    gimg_stream_destroy(out_stream);
    FAIL() << "expected SOI";
  }
  size_t off = 2;
  const uint8_t * dht_payload = nullptr;
  size_t dht_payload_len = 0;
  while (off + 2 <= size) {
    if (p[off] != 0xFF) {
      gimg_stream_destroy(out_stream);
      FAIL() << "expected marker at " << off;
    }
    uint8_t marker = p[off + 1];
    if (marker == 0xD9)
      break;
    if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7)) {
      off += 2;
      continue;
    }
    if (off + 4 > size) {
      gimg_stream_destroy(out_stream);
      FAIL() << "truncated segment at " << off;
    }
    uint16_t seg_len = (uint16_t)((p[off + 2] << 8) | p[off + 3]);
    if (marker == 0xC4 && seg_len >= 2) {
      dht_payload_len = (size_t)seg_len - 2;
      dht_payload = p + off + 4;
      break;
    }
    off += 2 + seg_len;
  }
  ASSERT_NE(dht_payload, nullptr);
  const size_t kDcLumPayloadLen =
      1u + 16u + (size_t)GIMG_JPEG_STD_DC_VALS;  // TcTh + bits + vals
  ASSERT_GE(dht_payload_len, kDcLumPayloadLen)
      << "first DHT table (DC luma) is 1+16+12=29 bytes";

  uint8_t expected_dc_lum[1 + 16 + GIMG_JPEG_STD_DC_VALS];
  expected_dc_lum[0] = 0x00;
  memcpy(expected_dc_lum + 1, gimg_jpeg_std_dc_lum_bits, 16);
  memcpy(expected_dc_lum + 17, gimg_jpeg_std_dc_lum_vals,
         (size_t)GIMG_JPEG_STD_DC_VALS);
  EXPECT_EQ(0, memcmp(dht_payload, expected_dc_lum, kDcLumPayloadLen))
      << "first DHT table (DC luma TcTh=0x00) must match jpeg_huffman_tables_internal.h";

  gimg_stream_destroy(out_stream);  // after all uses of stream buffer (data/dht_payload)
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
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = nullptr;
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
  guard.d = nullptr;
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
  DocStreamGuard guard1;
  guard1.d = doc;
  guard1.s = nullptr;
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "Saved two-item doc with format 1 should load with two items";

  GIMG_Raster * decoded_thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &decoded_thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded_thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded_thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(decoded_thumb), kThumbH);
  gimg_raster_destroy(decoded_thumb);
  guard1.d = nullptr;
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
  DocStreamGuard guard7;
  guard7.d = doc;
  guard7.s = nullptr;
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "Saved two-item doc with format 7 should load with two items";

  GIMG_Raster * decoded_thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &decoded_thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded_thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded_thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(decoded_thumb), kThumbH);
  gimg_raster_destroy(decoded_thumb);
  guard7.d = nullptr;
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
  DocStreamGuard guard_rt1_first;
  guard_rt1_first.d = doc;
  guard_rt1_first.s = nullptr;
  ASSERT_GE(gimg_doc_item_count(doc), 2u);

  out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  guard_rt1_first.s = out_stream;
  r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  guard_rt1_first.d = nullptr;
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);
  const void * buf2 = nullptr;
  size_t size2 = 0;
  gimg_stream_output_buffer(out_stream, &buf2, &size2);
  std::vector<uint8_t> jpeg2(static_cast<const uint8_t *>(buf2),
      static_cast<const uint8_t *>(buf2) + size2);
  guard_rt1_first.s = nullptr;
  gimg_stream_destroy(out_stream);

  ASSERT_EQ(gimg_stream_create_memory(jpeg2.data(), jpeg2.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  DocStreamGuard guard_rt1;
  guard_rt1.d = doc;
  guard_rt1.s = nullptr;
  EXPECT_EQ(gimg_doc_item_count(doc), 2u);
  GIMG_Raster * thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(thumb), kThumbH);
  gimg_raster_destroy(thumb);
  guard_rt1.d = nullptr;
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
  DocStreamGuard guard_rt7_first;
  guard_rt7_first.d = doc;
  guard_rt7_first.s = nullptr;
  ASSERT_GE(gimg_doc_item_count(doc), 2u);

  out_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
  guard_rt7_first.s = out_stream;
  r = gimg_doc_save(doc, out_stream, "jpeg", &save_opts, &report);
  guard_rt7_first.d = nullptr;
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_EQ(r, GIMG_OK);
  const void * buf2 = nullptr;
  size_t size2 = 0;
  gimg_stream_output_buffer(out_stream, &buf2, &size2);
  std::vector<uint8_t> jpeg2(static_cast<const uint8_t *>(buf2),
      static_cast<const uint8_t *>(buf2) + size2);
  guard_rt7_first.s = nullptr;
  gimg_stream_destroy(out_stream);

  ASSERT_EQ(gimg_stream_create_memory(jpeg2.data(), jpeg2.size(), &in_stream),
      GIMG_OK);
  r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  gimg_stream_destroy(in_stream);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  DocStreamGuard guard_rt7;
  guard_rt7.d = doc;
  guard_rt7.s = nullptr;
  EXPECT_EQ(gimg_doc_item_count(doc), 2u);
  GIMG_Raster * thumb = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &thumb);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(thumb, nullptr);
  EXPECT_EQ(gimg_raster_width(thumb), kThumbW);
  EXPECT_EQ(gimg_raster_height(thumb), kThumbH);
  gimg_raster_destroy(thumb);
  guard_rt7.d = nullptr;
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
  DocStreamGuard guard_preserved;
  guard_preserved.d = doc;
  guard_preserved.s = nullptr;
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
  guard_preserved.d = nullptr;
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

  jpeg_test::write_jpeg_output(
      "chroma_420.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard_chroma;
  guard_chroma.d = doc;
  guard_chroma.s = in_stream;
  ASSERT_EQ(gimg_doc_item_count(doc), 1u);
  item = gimg_doc_item(doc, 0);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &decoded), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 32u);
  EXPECT_EQ(gimg_raster_height(decoded), 32u);

  std::string jpeg_path_420 =
      jpeg_test::jpeg_output_dir() + "/chroma_420.jpg";
  std::string raw_path_420 =
      jpeg_test::jpeg_output_dir() + "/libjpeg_enc_chroma420.raw";
  std::vector<uint8_t> libjpeg_420;
  uint32_t oracle_w_420 = 0, oracle_h_420 = 0;
  int oracle_mode_420 = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path_420.c_str(), raw_path_420.c_str(), libjpeg_420,
          &oracle_w_420, &oracle_h_420, &oracle_mode_420)) {
    EXPECT_EQ(oracle_w_420, 32u);
    EXPECT_EQ(oracle_h_420, 32u);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_420.data(), oracle_w_420, oracle_h_420,
        oracle_mode_420, 32))
        << "our decode must match libjpeg (tolerance 32 for 4:2:0)";
  }

  gimg_raster_destroy(decoded);
  guard_chroma.d = nullptr;
  guard_chroma.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, EncodeRestartIntervalThenLoadDecodeAndLibjpegOracle) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                32, 32, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < 32; y++) {
    for (uint32_t x = 0; x < 32; x++) {
      pixels[y * stride + x] = (unsigned char)((x + y) & 0xFF);
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_restart_interval = 4,
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

  jpeg_test::write_jpeg_output(
      "restart_interval.jpg", jpeg_copy.data(), jpeg_copy.size());

  std::string jpeg_path_ri =
      jpeg_test::jpeg_output_dir() + "/restart_interval.jpg";
  std::string raw_path_ri =
      jpeg_test::jpeg_output_dir() + "/libjpeg_enc_restart.raw";
  std::vector<uint8_t> libjpeg_ri;
  uint32_t oracle_w_ri = 0, oracle_h_ri = 0;
  int oracle_mode_ri = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path_ri.c_str(), raw_path_ri.c_str(), libjpeg_ri, &oracle_w_ri,
          &oracle_h_ri, &oracle_mode_ri)) {
    GTEST_SKIP() << "Run make jpeg-oracle-tools (see tests/data/jpeg/README.md)";
  }
  EXPECT_EQ(oracle_w_ri, 32u);
  EXPECT_EQ(oracle_h_ri, 32u)
      << "libjpeg must decode our DRI-encoded JPEG with correct dimensions";

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed";
  }
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r == GIMG_OK && decoded) {
    EXPECT_EQ(gimg_raster_width(decoded), 32u);
    EXPECT_EQ(gimg_raster_height(decoded), 32u);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_ri.data(), oracle_w_ri, oracle_h_ri, oracle_mode_ri, 0))
        << "our decode must match libjpeg oracle (DRI/RST)";
    gimg_raster_destroy(decoded);
  }
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, Chroma422RoundTripAndLibjpegOracle) {
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

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_422,
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

  jpeg_test::write_jpeg_output(
      "chroma_422.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 32u);
  EXPECT_EQ(gimg_raster_height(decoded), 32u);

  std::string jpeg_path_422 =
      jpeg_test::jpeg_output_dir() + "/chroma_422.jpg";
  std::string raw_path_422 =
      jpeg_test::jpeg_output_dir() + "/libjpeg_enc_chroma_422.raw";
  std::vector<uint8_t> libjpeg_422;
  uint32_t oracle_w_422 = 0, oracle_h_422 = 0;
  int oracle_mode_422 = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path_422.c_str(), raw_path_422.c_str(), libjpeg_422,
          &oracle_w_422, &oracle_h_422, &oracle_mode_422)) {
    EXPECT_EQ(oracle_w_422, 32u);
    EXPECT_EQ(oracle_h_422, 32u);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_422.data(), oracle_w_422, oracle_h_422,
        oracle_mode_422, 32))
        << "our decode must match libjpeg (tolerance 32 for 4:2:2)";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, Chroma444RoundTripAndLibjpegOracle) {
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

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
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

  jpeg_test::write_jpeg_output(
      "chroma_444.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 32u);
  EXPECT_EQ(gimg_raster_height(decoded), 32u);

  std::string jpeg_path_444 =
      jpeg_test::jpeg_output_dir() + "/chroma_444.jpg";
  std::string raw_path_444 =
      jpeg_test::jpeg_output_dir() + "/libjpeg_enc_chroma444.raw";
  std::vector<uint8_t> libjpeg_444;
  uint32_t oracle_w_444 = 0, oracle_h_444 = 0;
  int oracle_mode_444 = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path_444.c_str(), raw_path_444.c_str(), libjpeg_444,
          &oracle_w_444, &oracle_h_444, &oracle_mode_444)) {
    EXPECT_EQ(oracle_w_444, 32u);
    EXPECT_EQ(oracle_h_444, 32u);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_444.data(), oracle_w_444, oracle_h_444,
        oracle_mode_444, 32))
        << "our decode must match libjpeg oracle (tolerance 32 for 4:4:4)";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
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
  jpeg_test::write_jpeg_output(
      "baseline_default.jpg", baseline_copy.data(), baseline_copy.size());

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
  DocStreamGuard guard_base;
  guard_base.d = doc_baseline;
  guard_base.s = in_baseline;
  GIMG_Raster * decoded_baseline = nullptr;
  ASSERT_EQ(gimg_item_decode(
                gimg_doc_item(doc_baseline, 0), nullptr, &decoded_baseline),
      GIMG_OK);
  RasterGuard guard_decoded_base;
  guard_decoded_base.r = decoded_baseline;
  uint64_t hash_baseline = jpeg_test::raster_pixel_hash(decoded_baseline);

  GIMG_Stream * in_prog = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(prog_copy.data(), prog_copy.size(), &in_prog),
      GIMG_OK);
  GIMG_Doc * doc_prog = nullptr;
  ASSERT_EQ(gimg_doc_load(in_prog, nullptr, nullptr, &doc_prog), GIMG_OK);
  DocStreamGuard guard_prog;
  guard_prog.d = doc_prog;
  guard_prog.s = in_prog;
  GIMG_Raster * decoded_prog = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(doc_prog, 0), nullptr, &decoded_prog),
      GIMG_OK);
  uint64_t hash_prog = jpeg_test::raster_pixel_hash(decoded_prog);

  std::string diff_msg = jpeg_test::raster_first_diff(
      decoded_baseline, decoded_prog);
  EXPECT_EQ(hash_prog, hash_baseline)
      << "Progressive decode must match baseline exactly (same image, same "
         "quantized coefficients; only scan order differs). "
      << (diff_msg.empty() ? "" : diff_msg);

  jpeg_test::write_jpeg_output(
      "progressive_default.jpg", prog_copy.data(), prog_copy.size());

  std::string jpeg_path_prog =
      jpeg_test::jpeg_output_dir() + "/progressive_default.jpg";
  std::string raw_path_prog =
      jpeg_test::jpeg_output_dir() + "/libjpeg_enc_progressive.raw";
  std::vector<uint8_t> libjpeg_prog;
  uint32_t oracle_w_prog = 0, oracle_h_prog = 0;
  int oracle_mode_prog = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path_prog.c_str(), raw_path_prog.c_str(), libjpeg_prog,
          &oracle_w_prog, &oracle_h_prog, &oracle_mode_prog)) {
    EXPECT_EQ(oracle_w_prog, 16u);
    EXPECT_EQ(oracle_h_prog, 16u);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded_prog, libjpeg_prog.data(), oracle_w_prog, oracle_h_prog,
        oracle_mode_prog, 32))
        << "our progressive decode must match libjpeg oracle";
  }

  guard_decoded_base.r = nullptr;
  gimg_raster_destroy(decoded_baseline);
  guard_base.d = nullptr;
  guard_base.s = nullptr;
  gimg_doc_destroy(doc_baseline);
  gimg_stream_destroy(in_baseline);
  gimg_raster_destroy(decoded_prog);
  guard_prog.d = nullptr;
  guard_prog.s = nullptr;
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

  jpeg_test::write_jpeg_output(
      "progressive_custom.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard_custom;
  guard_custom.d = doc;
  guard_custom.s = in_stream;
  item = gimg_doc_item(doc, 0);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &decoded), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  gimg_raster_destroy(decoded);
  guard_custom.d = nullptr;
  guard_custom.s = nullptr;
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

TEST(JpegEncode, ProgressiveInvalidScriptSsSeOutOfRangeReturnsError) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  gimg_item_set_raster(item, raster);

  GIMG_JPEG_Progressive_Scan bad_scan = {64, 63, 0, 0};  // Ss=64 > 63
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
  EXPECT_NE(r, GIMG_OK) << "Ss>63 should be rejected";
  EXPECT_TRUE(r == GIMG_ERR_UNSUPPORTED || r == GIMG_ERR_FORMAT);
}

TEST(JpegEncode, ProgressiveInvalidScriptAhAlOutOfRangeReturnsError) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  gimg_item_set_raster(item, raster);

  GIMG_JPEG_Progressive_Scan bad_scan = {1, 63, 16, 0};  // Ah=16 > 15
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
  EXPECT_NE(r, GIMG_OK) << "Ah>15 should be rejected";
  EXPECT_TRUE(r == GIMG_ERR_UNSUPPORTED || r == GIMG_ERR_FORMAT);
}

/* T.81 Annex G: spectral selection bands must not overlap (successive bands). */
TEST(JpegEncode, ProgressiveInvalidScriptOverlappingSpectralBandsReturnsError) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  gimg_item_set_raster(item, raster);

  /* Two initial AC scans with overlapping [Ss,Se]: [1,30] and [25,63]. */
  static const GIMG_JPEG_Progressive_Scan overlap_scans[] = {
      {0, 0, 0, 0},
      {1, 30, 0, 0},
      {25, 63, 0, 0},
  };
  GIMG_JPEG_Progressive_Config bad_config = {
      .scan_count = (unsigned)sizeof(overlap_scans) / sizeof(overlap_scans[0]),
      .scans = overlap_scans,
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
  EXPECT_NE(r, GIMG_OK) << "Overlapping spectral bands [1,30] and [25,63] should be rejected (T.81 Annex G)";
  EXPECT_TRUE(r == GIMG_ERR_UNSUPPORTED || r == GIMG_ERR_FORMAT);
}

/* Task 2.3.2: progressive encode with all chroma options (4:2:0, 4:2:2, 4:4:4). */
TEST(JpegEncode, ProgressiveChromaSubsampling420_422_444) {
  constexpr uint32_t kW = 32u, kH = 32u;
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                (int)kW, (int)kH, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
                &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < kH; y++) {
    for (uint32_t x = 0; x < kW; x++) {
      pixels[y * stride + x * 4 + 0] = (unsigned char)((x * 7) & 0xFF);
      pixels[y * stride + x * 4 + 1] = (unsigned char)((y * 11) & 0xFF);
      pixels[y * stride + x * 4 + 2] = 128;
      pixels[y * stride + x * 4 + 3] = 255;
    }
  }
  gimg_item_set_raster(item, raster);

  for (uint8_t chroma : {GIMG_JPEG_CHROMA_420, GIMG_JPEG_CHROMA_422,
                          GIMG_JPEG_CHROMA_444}) {
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {
        .metadata_policy = GIMG_META_PRESERVE_ALL,
        .quality = 85,
        .jpeg_chroma_subsampling = chroma,
        .jpeg_progressive = 1,
    };
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK)
        << "progressive save chroma " << (int)chroma;
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    std::vector<uint8_t> copy((const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(out);

    GIMG_Stream * in_stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(copy.data(), copy.size(), &in_stream),
        GIMG_OK);
    GIMG_Doc * loaded = nullptr;
    ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &loaded), GIMG_OK);
    GIMG_Raster * decoded = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded), GIMG_OK);
    EXPECT_EQ(gimg_raster_width(decoded), kW) << "chroma " << (int)chroma;
    EXPECT_EQ(gimg_raster_height(decoded), kH);
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(loaded);
    gimg_stream_destroy(in_stream);
  }
  gimg_doc_destroy(doc);
}

/* Task 2.3.3: progressive encode with DRI (restart interval); load+decode succeeds. */
TEST(JpegEncode, ProgressiveWithRestartIntervalRoundTrip) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                32, 32, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 32 * 32);
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
      .jpeg_progressive = 1,
      .jpeg_restart_interval = 8,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> copy((const uint8_t *)data, (const uint8_t *)data + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(copy.data(), copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Doc * loaded = nullptr;
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &loaded), GIMG_OK);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 32u);
  EXPECT_EQ(gimg_raster_height(decoded), 32u);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(loaded);
  gimg_stream_destroy(in_stream);
}

/* Task 2.3.1: progressive encode at minimal dimensions (8×8, 16×16). */
TEST(JpegEncode, ProgressiveMinimalDimensions8x8And16x16) {
  for (uint32_t w : {8u, 16u}) {
    uint32_t h = w;
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Item * item = gimg_doc_item(doc, 0);
    ASSERT_NE(item, nullptr);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(
                  (int)w, (int)h, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0,
                  &raster),
        GIMG_OK);
    memset(gimg_raster_pixels(raster), 128, (size_t)(w * h));
    gimg_item_set_raster(item, raster);

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {
        .metadata_policy = GIMG_META_PRESERVE_ALL,
        .quality = 85,
        .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
        .jpeg_progressive = 1,
    };
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK)
        << "progressive save " << w << "x" << h;
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    std::vector<uint8_t> copy((const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);

    GIMG_Stream * in_stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(copy.data(), copy.size(), &in_stream),
        GIMG_OK);
    GIMG_Doc * loaded = nullptr;
    ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &loaded), GIMG_OK);
    GIMG_Raster * decoded = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded), GIMG_OK);
    EXPECT_EQ(gimg_raster_width(decoded), w) << "decoded width " << w << "x" << h;
    EXPECT_EQ(gimg_raster_height(decoded), h);
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(loaded);
    gimg_stream_destroy(in_stream);
  }
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
      {0, 0, 0, 0},  // DC initial
      {1, 63, 0, 0}, // AC initial Ss=1..63
      {1, 63, 1, 0}, // AC refinement same band
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

  jpeg_test::write_jpeg_output(
      "progressive_refinement.jpg", refine_copy.data(), refine_copy.size());

  GIMG_Stream * in_baseline = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(
                baseline_copy.data(), baseline_copy.size(), &in_baseline),
      GIMG_OK);
  GIMG_Doc * doc_baseline = nullptr;
  ASSERT_EQ(
      gimg_doc_load(in_baseline, nullptr, nullptr, &doc_baseline), GIMG_OK);
  DocStreamGuard guard_ref_base;
  guard_ref_base.d = doc_baseline;
  guard_ref_base.s = in_baseline;
  GIMG_Raster * decoded_baseline = nullptr;
  ASSERT_EQ(gimg_item_decode(
                gimg_doc_item(doc_baseline, 0), nullptr, &decoded_baseline),
      GIMG_OK);
  RasterGuard guard_decoded_base;
  guard_decoded_base.r = decoded_baseline;
  uint64_t hash_baseline = jpeg_test::raster_pixel_hash(decoded_baseline);

  GIMG_Stream * in_refine = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(
                refine_copy.data(), refine_copy.size(), &in_refine),
      GIMG_OK);
  GIMG_Doc * doc_refine = nullptr;
  ASSERT_EQ(gimg_doc_load(in_refine, nullptr, nullptr, &doc_refine), GIMG_OK);
  DocStreamGuard guard_ref_refine;
  guard_ref_refine.d = doc_refine;
  guard_ref_refine.s = in_refine;
  GIMG_Raster * decoded_refine = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(doc_refine, 0), nullptr, &decoded_refine),
      GIMG_OK);
  uint64_t hash_refine = jpeg_test::raster_pixel_hash(decoded_refine);

  EXPECT_EQ(hash_refine, hash_baseline) << "Progressive with refinement decode "
                                           "should match baseline (same image)";

  guard_decoded_base.r = nullptr;
  gimg_raster_destroy(decoded_baseline);
  guard_ref_base.d = nullptr;
  guard_ref_base.s = nullptr;
  gimg_doc_destroy(doc_baseline);
  gimg_stream_destroy(in_baseline);
  gimg_raster_destroy(decoded_refine);
  guard_ref_refine.d = nullptr;
  guard_ref_refine.s = nullptr;
  gimg_doc_destroy(doc_refine);
  gimg_stream_destroy(in_refine);
}

TEST(JpegEncode, SaveGray16ThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      pixels[y * stride_el + x] = (uint16_t)((x + y * 16) * 256);
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);
  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      (const uint8_t *)jpeg_data, (const uint8_t *)jpeg_data + jpeg_size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  doc = nullptr;

  jpeg_test::write_jpeg_output(
      "baseline_gray16.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 16u);
  EXPECT_EQ(gimg_raster_height(decoded), 16u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(decoded);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, SaveRgb16ThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      pixels[y * stride_el + x * 4 + 0] = (uint16_t)(x * 8192);
      pixels[y * stride_el + x * 4 + 1] = (uint16_t)(y * 8192);
      pixels[y * stride_el + x * 4 + 2] = (uint16_t)((x + y) * 4096);
      pixels[y * stride_el + x * 4 + 3] = 65535;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 90,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);
  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      (const uint8_t *)jpeg_data, (const uint8_t *)jpeg_data + jpeg_size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  doc = nullptr;

  jpeg_test::write_jpeg_output(
      "baseline_rgb16.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(decoded);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, SaveRgb16ProgressiveThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      pixels[y * stride_el + x * 4 + 0] = (uint16_t)(x * 8192);
      pixels[y * stride_el + x * 4 + 1] = (uint16_t)(y * 8192);
      pixels[y * stride_el + x * 4 + 2] = (uint16_t)((x + y) * 4096);
      pixels[y * stride_el + x * 4 + 3] = 65535;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 90,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
      .jpeg_progressive = 1,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);
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
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(decoded);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16);
  jpeg_test::write_jpeg_output(
      "progressive_rgb16.jpg", jpeg_copy.data(), jpeg_copy.size());
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, SaveGray16ProgressiveThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      pixels[y * stride_el + x] = (uint16_t)((x * 17 + y * 31) & 0xFFFF);
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_progressive = 1,
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

  jpeg_test::write_jpeg_output(
      "progressive_gray16.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 16u);
  EXPECT_EQ(gimg_raster_height(decoded), 16u);
  EXPECT_EQ(gimg_raster_format(decoded)->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/* Task 3.2.1: 16-bit quality applies to quant; vary quality, decode, check dimensions and file size. */
TEST(JpegEncode, SaveGray16QualityVariation) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      pixels[y * stride_el + x] = (uint16_t)((x + y * 16) * 256);
    }
  }
  gimg_item_set_raster(item, raster);

  // A GRAY16 raster is written at 12-bit (T.81 has no 16-bit DCT frame), so the
  // 12-bit quality range applies here.  This test used to run only qualities 50
  // and 85: quality 100 was refused at save, and 88-90 and 94-99 produced files
  // our own decoder rejected as corrupt, because the extended Huffman tables
  // over-subscribed the code space.  With the tables fixed and the refusal
  // removed, every quality works, so the test covers the ends of the range too.
  size_t size_50 = 0, size_85 = 0, size_100 = 0;
  for (unsigned q : {1u, 50u, 85u, 95u, 100u}) {
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {
        .metadata_policy = GIMG_META_PRESERVE_ALL,
        .quality = q,
        .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
    };
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
    const void * data = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out, &data, &n);
    if (q == 50) size_50 = n;
    else if (q == 85) size_85 = n;
    else if (q == 100) size_100 = n;
    GIMG_Stream * in_stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory((const uint8_t *)data, n, &in_stream),
        GIMG_OK);
    GIMG_Doc * loaded = nullptr;
    ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &loaded), GIMG_OK);
    GIMG_Raster * decoded = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded), GIMG_OK);
    EXPECT_EQ(gimg_raster_width(decoded), 16u) << "quality " << q;
    EXPECT_EQ(gimg_raster_height(decoded), 16u);
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(loaded);
    gimg_stream_destroy(in_stream);
    gimg_stream_destroy(out);
  }
  gimg_doc_destroy(doc);
  /* Same image: higher quality should yield larger or similar size (no strict order). */
  EXPECT_GT(size_50, 0u);
  EXPECT_GT(size_85, 0u);
  EXPECT_GT(size_100, 0u) << "quality 100 must produce a file, not an error";
  // Finer quantisation, more bits.
  EXPECT_GT(size_85, size_50);
  EXPECT_GT(size_100, size_85);
}

/**
 * A flat 12-bit image must come back at the value it went in at, spread across
 * the whole 16-bit output range - not squeezed into a band around mid-grey.
 *
 * This checks sample values, which nothing did before: every 12-bit test
 * asserted dimensions, format and "decode succeeded", so a decoder whose
 * inverse DCT was scaled wrong by a factor of 64 passed them all.  T.81 A.3.3
 * fixes the transform exactly; there is no latitude in it beyond rounding.
 */
/** A flat 12-bit colour field must survive a 12-bit round trip.
 *
 * The encoder's 12-bit RGB->YCbCr had coefficients scaled for 8-bit data but a
 * shift of 12 rather than 8, so luminance came out sixteen times too small and
 * every 12-bit colour image we wrote was ruined.  Neither this library's own
 * decoder nor libjpeg could reveal that on its own - both read the file back
 * faithfully, and what they read back was faithfully wrong - so the check has
 * to be against the sample that went in.
 *
 * A flat field is used so that chroma subsampling and the DCT are both exact,
 * leaving nothing between the input and the output but the colour transform.
 * The tolerance covers the round trip through YCbCr, which is not lossless. */
TEST(JpegEncode, Save12BitColourFlatFieldsKeepTheirValue) {
  struct Case {
    uint16_t r, g, b;
  };
  static const Case cases[] = {
      {3000, 1000, 2000},
      {4095, 4095, 4095},
      {0, 0, 0},
      {4095, 0, 0},
      {0, 4095, 0},
      {0, 0, 4095},
      {2048, 2048, 2048},
  };
  for (const Case & c : cases) {
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_RGBA12, GIMG_RASTER_OWNED,
                  NULL, 0, &raster),
        GIMG_OK);
    uint16_t * px = (uint16_t *)gimg_raster_pixels(raster);
    size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
    for (uint32_t y = 0; y < 16; y++) {
      for (uint32_t x = 0; x < 16; x++) {
        px[y * stride_el + x * 4 + 0] = c.r;
        px[y * stride_el + x * 4 + 1] = c.g;
        px[y * stride_el + x * 4 + 2] = c.b;
        px[y * stride_el + x * 4 + 3] = 4095;
      }
    }
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.metadata_policy = GIMG_META_PRESERVE_ALL;
    opts.quality = 95;
    opts.jpeg_precision = 12;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK)
        << "rgb (" << c.r << ", " << c.g << ", " << c.b << ")";
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    std::vector<uint8_t> jpeg((const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);

    GIMG_Stream * in = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &in), GIMG_OK);
    GIMG_Doc * back = nullptr;
    ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &back), GIMG_OK);
    gimg_stream_destroy(in);
    GIMG_Raster * decoded = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(back, 0), nullptr, &decoded),
        GIMG_OK);
    ASSERT_NE(decoded, nullptr);
    const uint16_t * out_px =
        (const uint16_t *)gimg_raster_pixels_const(decoded);
    size_t out_stride = gimg_raster_stride_bytes(decoded) / sizeof(uint16_t);
    // The decoded raster is 16-bit, widened from 12 by replication; compare in
    // the frame's own precision.
    const uint16_t want[3] = {c.r, c.g, c.b};
    for (uint32_t y = 0; y < 16; y++) {
      for (uint32_t x = 0; x < 16; x++) {
        for (int ch = 0; ch < 3; ch++) {
          int got = (int)gimg_bitdepth_16_to_12(
              out_px[y * out_stride + x * 4 + (uint32_t)ch]);
          EXPECT_NEAR(got, (int)want[ch], 8)
              << "rgb (" << c.r << ", " << c.g << ", " << c.b << ") channel "
              << ch << " at (" << x << ", " << y << ")";
        }
      }
    }
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(back);
  }
}

/** Quality 100 at 12 bits must encode, and must read back.
 *
 * It used to be refused outright: the saver returned GIMG_ERR_UNSUPPORTED for
 * any quality of 100 or more at P=12, because the file it produced could not be
 * decoded.  The cause was in the extended Huffman tables - they over-subscribed
 * the code space, so 79 symbols shared codes with other symbols - and not in
 * the quantiser, as the comment on the guard had assumed.  With the tables
 * fixed the guard was refusing files that are perfectly good, so it is gone.
 *
 * The content here is deliberately hostile to quality 100: a one-pixel
 * checkerboard puts as much energy as an 8x8 block can hold into its highest
 * AC coefficients, which is where a coefficient too large for any category
 * (T.81 F.1.2.2 allows SSSS up to 14 at P=12) would first appear. */
TEST(JpegEncode, Save12BitQuality100RoundTrips) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 32, &GIMG_PIXEL_GRAY12, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * px = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 32; y++) {
    for (uint32_t x = 0; x < 32; x++) {
      px[y * stride_el + x] = ((x + y) & 1u) ? 4095u : 0u;
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  opts.quality = 100;
  opts.jpeg_precision = 12;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> jpeg((const uint8_t *)data, (const uint8_t *)data + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &in), GIMG_OK);
  GIMG_Doc * back = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &back), GIMG_OK);
  gimg_stream_destroy(in);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(back, 0), nullptr, &decoded), GIMG_OK);
  ASSERT_NE(decoded, nullptr);
  ASSERT_EQ(gimg_raster_width(decoded), 32u);
  ASSERT_EQ(gimg_raster_height(decoded), 32u);
  const uint16_t * out_px = (const uint16_t *)gimg_raster_pixels_const(decoded);
  size_t out_stride = gimg_raster_stride_bytes(decoded) / sizeof(uint16_t);
  // At quality 100 the quantisation values are all 1, so the checkerboard comes
  // back essentially intact; the tolerance is for the DCT round trip alone.
  for (uint32_t y = 0; y < 32; y++) {
    for (uint32_t x = 0; x < 32; x++) {
      int want = ((x + y) & 1u) ? 4095 : 0;
      int got =
          (int)gimg_bitdepth_16_to_12(out_px[y * out_stride + x]);
      EXPECT_NEAR(got, want, 24) << "at (" << x << ", " << y << ")";
    }
  }
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(back);
}

/** A lossless frame round-trips exactly, at every precision and predictor.
 *
 * "Lossless" is a claim that can be checked directly rather than approximated:
 * decode what was encoded and the samples must be identical, not close.  T.81
 * Annex H is also the only place a JPEG may carry 16-bit samples (Table B.2),
 * so this is where a 16-bit raster survives a JPEG round trip - the DCT-based
 * writers narrow one to 12 bits, because a 16-bit DCT frame does not exist. */
TEST(JpegEncode, LosslessRoundTripsExactly) {
  struct Case {
    const GIMG_Pixel_Format * fmt;
    int channels;
    int bits;
    const char * what;
  };
  static const Case formats[] = {
      {&GIMG_PIXEL_GRAY8, 1, 8, "GRAY8"},
      {&GIMG_PIXEL_RGBA8, 4, 8, "RGBA8"},
      {&GIMG_PIXEL_GRAY12, 1, 12, "GRAY12"},
      {&GIMG_PIXEL_RGBA12, 4, 12, "RGBA12"},
      {&GIMG_PIXEL_GRAY16, 1, 16, "GRAY16"},
      {&GIMG_PIXEL_RGBA16, 4, 16, "RGBA16"},
  };
  const uint32_t kW = 23, kH = 11; // deliberately not a multiple of anything
  for (const Case & f : formats) {
    for (int psv = 1; psv <= 7; psv++) {
      for (uint16_t ri : {(uint16_t)0, (uint16_t)kW}) {
      // T.81 Table B.1: SOF3 and SOF11 are the same predictive process with a
      // different entropy coder, so everything below is shared and only the
      // marker and the table segment differ.
      for (int arith = 0; arith <= 1; arith++) {
        uint32_t maxv = (f.bits >= 32) ? 0xFFFFFFFFu : ((1u << f.bits) - 1u);
        std::vector<uint32_t> want((size_t)kW * kH * 3);
        GIMG_Doc * doc = nullptr;
        ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
        GIMG_Raster * raster = nullptr;
        ASSERT_EQ(gimg_raster_create(kW, kH, f.fmt, GIMG_RASTER_OWNED, NULL, 0,
                      &raster),
            GIMG_OK);
        size_t stride = gimg_raster_stride_bytes(raster);
        void * px = gimg_raster_pixels(raster);
        uint32_t seed = 4242u;
        for (uint32_t y = 0; y < kH; y++) {
          for (uint32_t x = 0; x < kW; x++) {
            for (int c = 0; c < 3; c++) {
              seed = seed * 1103515245u + 12345u;
              // Mix noise with a gradient: noise reaches the large difference
              // categories, the gradient the small ones.
              uint32_t v = ((seed >> 13) ^ (x * 37u + y * 11u)) & maxv;
              want[((size_t)y * kW + x) * 3 + (size_t)c] = v;
              if (f.channels == 1) {
                if (f.bits == 8) {
                  ((unsigned char *)px)[y * stride + x] =
                      (unsigned char)want[((size_t)y * kW + x) * 3];
                } else {
                  ((uint16_t *)((unsigned char *)px + y * stride))[x] =
                      (uint16_t)want[((size_t)y * kW + x) * 3];
                }
              } else if (f.bits == 8) {
                unsigned char * p = (unsigned char *)px + y * stride + x * 4;
                p[c] = (unsigned char)v;
                p[3] = 255;
              } else {
                uint16_t * p =
                    (uint16_t *)((unsigned char *)px + y * stride) + x * 4;
                p[c] = (uint16_t)v;
                p[3] = (uint16_t)maxv;
              }
            }
          }
        }
        gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

        GIMG_Stream * st = nullptr;
        ASSERT_EQ(gimg_stream_create_memory_output(&st), GIMG_OK);
        GIMG_Save_Options opts = {};
        opts.metadata_policy = GIMG_META_PRESERVE_ALL;
        opts.jpeg_lossless_predictor = (uint8_t)psv;
        opts.jpeg_restart_interval = ri;
        opts.jpeg_arithmetic = (uint8_t)arith;
        GIMG_Save_Report report = {};
        ASSERT_EQ(gimg_doc_save(doc, st, "jpeg", &opts, &report), GIMG_OK)
            << f.what << " psv " << psv;
        const void * data = nullptr;
        size_t size = 0;
        gimg_stream_output_buffer(st, &data, &size);
        std::vector<uint8_t> jpeg(
            (const uint8_t *)data, (const uint8_t *)data + size);
        gimg_stream_destroy(st);
        gimg_doc_destroy(doc);

        // The frame must announce itself as lossless at the raster's own
        // precision, and must carry no quantisation table - there is nothing
        // to quantise.
        bool saw_sof = false, saw_dqt = false, saw_dht = false, saw_dac = false;
        int got_precision = 0;
        const uint8_t want_sof = arith ? 0xCB : 0xC3;
        for (size_t i = 0; i + 3 < jpeg.size(); i++) {
          if (jpeg[i] != 0xFF) continue;
          if (jpeg[i + 1] == want_sof) {
            saw_sof = true;
            got_precision = jpeg[i + 4];
          }
          if (jpeg[i + 1] == 0xDB) saw_dqt = true;
          if (jpeg[i + 1] == 0xC4) saw_dht = true;
          if (jpeg[i + 1] == 0xCC) saw_dac = true;
          if (jpeg[i + 1] == 0xDA) break;
        }
        EXPECT_TRUE(saw_sof) << f.what << (arith ? " SOF11" : " SOF3");
        EXPECT_FALSE(saw_dqt) << f.what << ": a lossless frame has no DQT";
        // B.2.4.3: DAC replaces DHT in an arithmetic frame, and a Huffman one
        // carries no conditioning.
        EXPECT_EQ(saw_dac, arith != 0) << f.what;
        EXPECT_EQ(saw_dht, arith == 0) << f.what;
        EXPECT_EQ(got_precision, f.bits) << f.what;

        GIMG_Stream * in = nullptr;
        ASSERT_EQ(
            gimg_stream_create_memory(jpeg.data(), jpeg.size(), &in), GIMG_OK);
        GIMG_Doc * back = nullptr;
        ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &back), GIMG_OK)
            << f.what << " psv " << psv;
        gimg_stream_destroy(in);
        GIMG_Raster * decoded = nullptr;
        ASSERT_EQ(gimg_item_decode(gimg_doc_item(back, 0), nullptr, &decoded),
            GIMG_OK)
            << f.what << " psv " << psv;
        ASSERT_NE(decoded, nullptr);
        ASSERT_EQ(gimg_raster_width(decoded), kW);
        ASSERT_EQ(gimg_raster_height(decoded), kH);
        const GIMG_Pixel_Format * dfmt = gimg_raster_format(decoded);
        int out_bits = dfmt->bits_per_channel[0];
        size_t dstride = gimg_raster_stride_bytes(decoded);
        const void * dpx = gimg_raster_pixels_const(decoded);
        int nch = (f.channels == 1) ? 1 : 3;
        for (uint32_t y = 0; y < kH && !HasFailure(); y++) {
          for (uint32_t x = 0; x < kW && !HasFailure(); x++) {
            for (int c = 0; c < nch; c++) {
              uint32_t got;
              if (out_bits == 8) {
                const unsigned char * p =
                    (const unsigned char *)dpx + y * dstride;
                got = (nch == 1) ? p[x] : p[x * 4 + (uint32_t)c];
              } else {
                const uint16_t * p =
                    (const uint16_t *)((const unsigned char *)dpx + y * dstride);
                got = (nch == 1) ? p[x] : p[x * 4 + (uint32_t)c];
              }
              // The decoder widens to the raster depth; undo that to compare
              // the samples that were actually coded.
              uint32_t narrowed = (out_bits == f.bits)
                  ? got
                  : (uint32_t)(((uint64_t)got * maxv +
                                   ((1ull << out_bits) - 1ull) / 2ull) /
                      ((1ull << out_bits) - 1ull));
              ASSERT_EQ(narrowed, want[((size_t)y * kW + x) * 3 + (size_t)c])
                  << f.what << (arith ? " SOF11" : " SOF3") << " psv " << psv
                  << " ri " << ri << " at (" << x << ", " << y << ") channel "
                  << c;
            }
          }
        }
        gimg_raster_destroy(decoded);
        gimg_doc_destroy(back);
      }
      }
    }
  }
}

/** Arithmetic and Huffman encoding of the same image must agree.
 *
 * T.81 defines two entropy coders, and they are exactly that: two ways of
 * writing the same quantised coefficients.  Both encoders here are fed one
 * coefficient buffer, so whatever a decoder reconstructs from one file it must
 * reconstruct from the other, to the sample.  That makes each a check on the
 * other, which is how the Huffman encoder's zero-run bug was found: it had been
 * dropping the remainder of any run of sixteen or more zeros, so a coefficient
 * after a long run was written up to fifteen positions too early.  No decoder
 * could notice - the file faithfully said what the encoder meant - and a
 * round trip through our own decoder agreed with libjpeg on the wrong answer.
 *
 * The qualities are not arbitrary: with this content the two encoders diverge
 * at 60, 75 and 80 and agree at 70 and 85, so a test that picked one quality
 * had every chance of picking one that hides the defect. */
TEST(JpegEncode, ArithmeticAndHuffmanEncodeTheSameImage) {
  static const uint32_t kW = 17u, kH = 9u;
  auto encode = [](bool arithmetic, bool progressive, unsigned quality,
                    std::vector<uint8_t> & out) {
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(kW, kH, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                  NULL, 0, &raster),
        GIMG_OK);
    unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    // Noise in one channel and gradients in the others.  Noise is what produces
    // blocks with a few scattered high-frequency coefficients separated by long
    // runs of zeros, which is the case the zero-run bug fell over; smooth
    // content does not reach it.  The generator is a plain congruential one so
    // that the image is the same on every run and every platform.
    uint32_t seed = 12345u;
    for (uint32_t y = 0; y < kH; y++) {
      for (uint32_t x = 0; x < kW; x++) {
        seed = seed * 1103515245u + 12345u;
        unsigned char * p = px + y * stride + x * 4;
        p[0] = (unsigned char)((seed >> 16) & 0xFFu);
        p[1] = (unsigned char)((y * 255u) / (kH - 1u));
        p[2] = (unsigned char)(((x + y) * 255u) / (kW + kH - 2u));
        p[3] = 255;
      }
    }
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
    GIMG_Stream * st = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&st), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.metadata_policy = GIMG_META_PRESERVE_ALL;
    opts.quality = quality;
    opts.jpeg_arithmetic = arithmetic ? 1 : 0;
    opts.jpeg_progressive = progressive ? 1 : 0;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, st, "jpeg", &opts, &report), GIMG_OK);
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(st, &data, &size);
    out.assign((const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(st);
    gimg_doc_destroy(doc);
  };
  auto decode = [](const std::vector<uint8_t> & jpeg,
                    std::vector<uint8_t> & out) {
    GIMG_Stream * st = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &st), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(st, nullptr, nullptr, &doc), GIMG_OK);
    gimg_stream_destroy(st);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    uint32_t w = gimg_raster_width(raster), h = gimg_raster_height(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    size_t bpp = gimg_raster_bytes_per_pixel(gimg_raster_format(raster));
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels_const(raster);
    out.clear();
    for (uint32_t y = 0; y < h; y++) {
      out.insert(out.end(), px + y * stride, px + y * stride + (size_t)w * bpp);
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
  };

  for (unsigned q : {60u, 70u, 75u, 80u, 85u, 95u}) {
   for (bool progressive : {false, true}) {
    std::vector<uint8_t> ar, hu, ar_px, hu_px;
    encode(true, progressive, q, ar);
    encode(false, progressive, q, hu);
    ASSERT_FALSE(ar.empty()) << "quality " << q;
    ASSERT_FALSE(hu.empty()) << "quality " << q;
    // The arithmetic file must announce itself as one: SOF9, and a DAC segment
    // where the Huffman file has DHT (T.81 Table B.1 and B.2.4.3).
    bool saw_sof9 = false, saw_dac = false, saw_dht = false;
    for (size_t i = 0; i + 1 < ar.size(); i++) {
      if (ar[i] != 0xFF) continue;
      // SOF9 sequential, SOF10 progressive (T.81 Table B.1).
      if (ar[i + 1] == (progressive ? 0xCA : 0xC9)) saw_sof9 = true;
      if (ar[i + 1] == 0xCC) saw_dac = true;
      if (ar[i + 1] == 0xC4) saw_dht = true;
      if (ar[i + 1] == 0xDA) break; // stop before the entropy-coded data
    }
    EXPECT_TRUE(saw_sof9) << "quality " << q;
    EXPECT_TRUE(saw_dac) << "quality " << q;
    EXPECT_FALSE(saw_dht) << "an arithmetic frame carries no Huffman tables";

    decode(ar, ar_px);
    decode(hu, hu_px);
    ASSERT_EQ(ar_px.size(), hu_px.size()) << "quality " << q;
    EXPECT_EQ(ar_px, hu_px)
        << "the two entropy coders disagree about the same coefficients, at "
           "quality "
        << q << (progressive ? " (progressive)" : " (sequential)");
   }
  }
}

/** A progressive arithmetic frame (SOF10) is written, and says what it is.
 *
 * Progressive and arithmetic are independent choices in T.81: Table B.1 has a
 * marker for each of the four combinations.  This checks the one that needs
 * both the G.2 encoding procedures and the SOF10 frame header. */
TEST(JpegEncode, ProgressiveArithmeticWritesSof10) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 32, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < 32; y++) {
    for (uint32_t x = 0; x < 32; x++) {
      px[y * stride + x] = (unsigned char)((x * 8 + y * 3) & 0xFF);
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * st = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&st), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  opts.quality = 80;
  opts.jpeg_progressive = 1;
  opts.jpeg_arithmetic = 1;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, st, "jpeg", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(st, &data, &size);
  std::vector<uint8_t> jpeg((const uint8_t *)data, (const uint8_t *)data + size);
  gimg_stream_destroy(st);
  gimg_doc_destroy(doc);

  bool saw_sof10 = false, saw_dac = false, saw_dht = false;
  for (size_t i = 0; i + 1 < jpeg.size(); i++) {
    if (jpeg[i] != 0xFF) continue;
    if (jpeg[i + 1] == 0xCA) saw_sof10 = true;
    if (jpeg[i + 1] == 0xCC) saw_dac = true;
    if (jpeg[i + 1] == 0xC4) saw_dht = true;
    if (jpeg[i + 1] == 0xDA) break;
  }
  EXPECT_TRUE(saw_sof10);
  EXPECT_TRUE(saw_dac);
  EXPECT_FALSE(saw_dht);

  // And it reads back.
  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &in), GIMG_OK);
  GIMG_Doc * back = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &back), GIMG_OK);
  gimg_stream_destroy(in);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(back, 0), nullptr, &decoded), GIMG_OK);
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded), 32u);
  EXPECT_EQ(gimg_raster_height(decoded), 32u);
  const unsigned char * out =
      (const unsigned char *)gimg_raster_pixels_const(decoded);
  size_t out_stride = gimg_raster_stride_bytes(decoded);
  for (uint32_t y = 0; y < 32u; y++) {
    for (uint32_t x = 0; x < 32u; x++) {
      EXPECT_NEAR((int)out[y * out_stride + x], (int)((x * 8 + y * 3) & 0xFF), 24)
          << "at (" << x << ", " << y << ")";
    }
  }
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(back);
}

TEST(JpegEncode, Save12BitFlatFieldsKeepTheirValue) {
  struct Case {
    uint16_t sample;   // 12-bit input, 0..4095
    unsigned quality;
  };
  static const Case cases[] = {
      {0, 85}, {1024, 85}, {2048, 85}, {3072, 85}, {4095, 85},
      {0, 50}, {2048, 50}, {4095, 50},
  };
  for (const Case & c : cases) {
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY12, GIMG_RASTER_OWNED,
                  NULL, 0, &raster),
        GIMG_OK);
    uint16_t * px = (uint16_t *)gimg_raster_pixels(raster);
    size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
    for (uint32_t y = 0; y < 16; y++) {
      for (uint32_t x = 0; x < 16; x++) {
        px[y * stride_el + x] = c.sample;
      }
    }
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {
        .metadata_policy = GIMG_META_PRESERVE_ALL,
        .quality = c.quality,
    };
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK)
        << "sample " << c.sample << " quality " << c.quality;
    const void * data = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out, &data, &n);

    GIMG_Stream * in = nullptr;
    ASSERT_EQ(gimg_stream_create_memory((const uint8_t *)data, n, &in), GIMG_OK);
    GIMG_Doc * loaded = nullptr;
    ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &loaded), GIMG_OK);
    GIMG_Raster * decoded = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded),
        GIMG_OK)
        << "sample " << c.sample << " quality " << c.quality;
    const uint16_t * dp = (const uint16_t *)gimg_raster_pixels_const(decoded);
    size_t dstride = gimg_raster_stride_bytes(decoded) / 2;

    // A flat field is carried entirely by the DC coefficient, so the only loss
    // is one quantisation step.  Allow 2 steps of the 12-bit quantiser, widened.
    const int expect = (int)gimg_bitdepth_12_to_16(c.sample);
    const int tolerance = 16 * 24;
    for (uint32_t y = 0; y < 16; y += 5) {
      for (uint32_t x = 0; x < 16; x += 5) {
        int got = (int)dp[y * dstride + x];
        EXPECT_NEAR(got, expect, tolerance)
            << "sample " << c.sample << " quality " << c.quality << " at (" << x
            << "," << y << ")";
      }
    }
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(loaded);
    gimg_stream_destroy(in);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
  }
}

/** A 12-bit ramp must stay a ramp: the decoded range has to span most of the
 * output range, not collapse toward the middle. */
TEST(JpegEncode, Save12BitRampKeepsItsContrast) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(64, 8, &GIMG_PIXEL_GRAY12, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * px = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 64; x++) {
      px[y * stride_el + x] = (uint16_t)(x * 65u);  // 0 .. 4095
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 90,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &data, &n);
  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory((const uint8_t *)data, n, &in), GIMG_OK);
  GIMG_Doc * loaded = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &loaded), GIMG_OK);
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded),
      GIMG_OK);
  const uint16_t * dp = (const uint16_t *)gimg_raster_pixels_const(decoded);
  size_t dstride = gimg_raster_stride_bytes(decoded) / 2;
  int lo = 65535, hi = 0;
  for (uint32_t x = 0; x < 64; x++) {
    int v = (int)dp[4 * dstride + x];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  EXPECT_GT(hi - lo, 60000)
      << "decoded ramp spans " << (hi - lo) << " of 65535; a wrongly scaled "
         "inverse DCT collapses it toward mid-grey";
  EXPECT_LT(lo, 2000) << "dark end should stay dark";
  EXPECT_GT(hi, 63000) << "bright end should stay bright";
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(loaded);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

/* Task 3.3.5: Native 12-bit format (GRAY12) → 12-bit JPEG (SOF1 baseline), round-trip. */
TEST(JpegEncode, SaveGray12ThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY12, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      /* 12-bit range 0..4095 (clamped); no left-shift in raster. */
      pixels[y * stride_el + x] = (uint16_t)((x + y * 16) * 16);
      if (pixels[y * stride_el + x] > 4095) pixels[y * stride_el + x] = 4095;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);
  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      (const uint8_t *)jpeg_data, (const uint8_t *)jpeg_data + jpeg_size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  doc = nullptr;

  jpeg_test::write_jpeg_output(
      "baseline_gray12.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 16u);
  EXPECT_EQ(gimg_raster_height(decoded), 16u);
  /* 12-bit decode outputs GRAY16 with 12-bit left-justified. */
  const GIMG_Pixel_Format * fmt = gimg_raster_format(decoded);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/* Task 3.3.5: Native 12-bit format (RGBA12) → 12-bit JPEG, round-trip. */
TEST(JpegEncode, SaveRgb12ThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA12, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      pixels[y * stride_el + x * 4 + 0] = (uint16_t)((x * 512) > 4095 ? 4095 : (x * 512));
      pixels[y * stride_el + x * 4 + 1] = (uint16_t)((y * 512) > 4095 ? 4095 : (y * 512));
      pixels[y * stride_el + x * 4 + 2] = (uint16_t)(((x + y) * 256) > 4095 ? 4095 : ((x + y) * 256));
      pixels[y * stride_el + x * 4 + 3] = 4095;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 90,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);
  const void * jpeg_data = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(out, &jpeg_data, &jpeg_size);
  std::vector<uint8_t> jpeg_copy(
      (const uint8_t *)jpeg_data, (const uint8_t *)jpeg_data + jpeg_size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  doc = nullptr;

  jpeg_test::write_jpeg_output(
      "baseline_rgb12.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(decoded);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/* Task 3.3.5: GRAY16 raster with jpeg_precision=12 → 12-bit JPEG (library conversion), round-trip. */
TEST(JpegEncode, SaveGray16WithPrecision12ThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      pixels[y * stride_el + x] = (uint16_t)((x + y * 16) * 256);
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_precision = 12,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);
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
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 16u);
  EXPECT_EQ(gimg_raster_height(decoded), 16u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(decoded);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/* Task 3.3.5: 12-bit progressive (SOF2), round-trip. */
TEST(JpegEncode, SaveGray12ProgressiveThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY12, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      uint16_t v = (uint16_t)((x * 17 + y * 31) & 0xFFF);
      if (v > 4095) v = 4095;
      pixels[y * stride_el + x] = v;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_progressive = 1,
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

  jpeg_test::write_jpeg_output(
      "progressive_gray12.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 16u);
  EXPECT_EQ(gimg_raster_height(decoded), 16u);
  EXPECT_EQ(gimg_raster_format(decoded)->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/* Task 3.3.5: 12-bit progressive RGB, round-trip. */
TEST(JpegEncode, SaveRgb12ProgressiveThenLoadDecode) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA12, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 8; y++) {
    for (uint32_t x = 0; x < 8; x++) {
      pixels[y * stride_el + x * 4 + 0] = (uint16_t)((x * 512) > 4095 ? 4095 : (x * 512));
      pixels[y * stride_el + x * 4 + 1] = (uint16_t)((y * 512) > 4095 ? 4095 : (y * 512));
      pixels[y * stride_el + x * 4 + 2] = (uint16_t)(((x + y) * 256) > 4095 ? 4095 : ((x + y) * 256));
      pixels[y * stride_el + x * 4 + 3] = 4095;
    }
  }
  gimg_item_set_raster(item, raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 90,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
      .jpeg_progressive = 1,
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

  jpeg_test::write_jpeg_output(
      "progressive_rgb12.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  GIMG_Result load_r = gimg_doc_load(in_stream, nullptr, nullptr, &doc);
  if (load_r != GIMG_OK) {
    gimg_stream_destroy(in_stream);
    FAIL() << "load failed with " << load_r;
    return;
  }
  GIMG_Raster * decoded = nullptr;
  GIMG_Result decode_r =
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded);
  if (decode_r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
    FAIL() << "decode failed with " << decode_r;
    return;
  }
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  EXPECT_EQ(gimg_raster_format(decoded)->bits_per_channel[0], 16);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/* Task 3.3.5b: 12-bit quality variation (quality 50, 85, 100); decode dimensions and file size. */
TEST(JpegEncode, SaveGray12QualityVariation) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_GRAY12, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 16; y++) {
    for (uint32_t x = 0; x < 16; x++) {
      uint16_t v = (uint16_t)((x + y * 16) * 16);
      if (v > 4095) v = 4095;
      pixels[y * stride_el + x] = v;
    }
  }
  gimg_item_set_raster(item, raster);

  /* Quality 100 used to be rejected at P=12 (GIMG_ERR_UNSUPPORTED) because the
   * file it produced could not be decoded; the fault was in the extended
   * Huffman tables and is fixed, so the whole range is exercised here. */
  size_t size_50 = 0, size_85 = 0, size_100 = 0;
  for (unsigned q : {50u, 85u, 100u}) {
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {
        .metadata_policy = GIMG_META_PRESERVE_ALL,
        .quality = q,
        .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444,
    };
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
    const void * data = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out, &data, &n);
    if (q == 50) size_50 = n;
    else if (q == 85) size_85 = n;
    else if (q == 100) size_100 = n;
    std::vector<uint8_t> jpeg_copy(
        (const uint8_t *)data, (const uint8_t *)data + n);
    gimg_stream_destroy(out);
    out = nullptr;

    GIMG_Stream * in_stream = nullptr;
    ASSERT_EQ(
        gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
        GIMG_OK);
    GIMG_Doc * loaded = nullptr;
    ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &loaded), GIMG_OK);
    GIMG_Raster * decoded = nullptr;
    GIMG_Result decode_r =
        gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded);
    if (decode_r != GIMG_OK) {
      gimg_doc_destroy(loaded);
      gimg_stream_destroy(in_stream);
      FAIL() << "decode failed with " << decode_r << " at quality " << q;
    }
    EXPECT_EQ(gimg_raster_width(decoded), 16u) << "quality " << q;
    EXPECT_EQ(gimg_raster_height(decoded), 16u);
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(loaded);
    gimg_stream_destroy(in_stream);
  }
  gimg_doc_destroy(doc);
  EXPECT_GT(size_50, 0u);
  EXPECT_GT(size_85, 0u);
  EXPECT_GT(size_100, 0u) << "quality 100 must produce a file, not an error";
  // Finer quantisation, more bits.
  EXPECT_GT(size_85, size_50);
  EXPECT_GT(size_100, size_85);
}

/* Task 3.2.2: 16-bit YCbCr encode with 4:2:0, 4:2:2, 4:4:4; round-trip decode. */
TEST(JpegEncode, SaveRgb16Chroma420_422_444) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 32, &GIMG_PIXEL_RGBA16, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  uint16_t * pixels = (uint16_t *)gimg_raster_pixels(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / 2;
  for (uint32_t y = 0; y < 32; y++) {
    for (uint32_t x = 0; x < 32; x++) {
      pixels[y * stride_el + x * 4 + 0] = (uint16_t)((x * 200) & 0xFFFF);
      pixels[y * stride_el + x * 4 + 1] = (uint16_t)((y * 200) & 0xFFFF);
      pixels[y * stride_el + x * 4 + 2] = 0x8000;
      pixels[y * stride_el + x * 4 + 3] = 0xFFFF;
    }
  }
  gimg_item_set_raster(item, raster);

  for (uint8_t chroma : {GIMG_JPEG_CHROMA_420, GIMG_JPEG_CHROMA_422,
                          GIMG_JPEG_CHROMA_444}) {
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {
        .metadata_policy = GIMG_META_PRESERVE_ALL,
        .quality = 85,
        .jpeg_chroma_subsampling = chroma,
    };
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK)
        << "16-bit RGB chroma " << (int)chroma;
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    std::vector<uint8_t> copy((const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(out);

    GIMG_Stream * in_stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(copy.data(), copy.size(), &in_stream),
        GIMG_OK);
    GIMG_Doc * loaded = nullptr;
    ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &loaded), GIMG_OK);
    GIMG_Raster * decoded = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(loaded, 0), nullptr, &decoded), GIMG_OK);
    EXPECT_EQ(gimg_raster_width(decoded), 32u) << "16-bit chroma " << (int)chroma;
    EXPECT_EQ(gimg_raster_height(decoded), 32u);
    const GIMG_Pixel_Format * fmt = gimg_raster_format(decoded);
    ASSERT_NE(fmt, nullptr);
    EXPECT_EQ(fmt->bits_per_channel[0], 16);
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(loaded);
    gimg_stream_destroy(in_stream);
  }
  gimg_doc_destroy(doc);
}

/* Task 3.2.3: 16-bit encode uses SOF2 only (T.81; no SOF1 for 16-bit). */
/** A 16-bit raster has no matching JPEG precision: T.81 Table B.2 allows 8 and
 * 12 in a DCT frame, and nothing above that outside lossless (SOF3).  So a
 * GRAY16 raster is written at 12-bit, which for a sequential frame is SOF1.
 * This test previously asserted SOF2 with precision 16 "per T.81"; no such
 * frame exists. */
TEST(JpegEncode, SaveGray16WritesTwelveBitFrame) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 0, 8 * 8 * 2);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
  };
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> copy((const uint8_t *)data, (const uint8_t *)data + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  const uint8_t * p = copy.data();
  int sof_marker = 0;
  size_t sof_at = 0;
  for (size_t i = 0; i + 1 < copy.size(); i++) {
    if (p[i] == 0xFF &&
        (p[i + 1] == 0xC0 || p[i + 1] == 0xC1 || p[i + 1] == 0xC2)) {
      sof_marker = p[i + 1];
      sof_at = i;
      break;
    }
  }
  ASSERT_NE(sof_marker, 0) << "no SOF written";
  EXPECT_EQ(sof_marker, 0xC1)
      << "a 12-bit sequential frame is SOF1 (extended sequential)";
  ASSERT_LT(sof_at + 4, copy.size());
  EXPECT_EQ(p[sof_at + 4], 12)
      << "sample precision byte must be 12, never 16";
}

/** Asking for precision 16 is refused rather than quietly downgraded, so a
 * caller that wants it learns it does not exist. */
TEST(JpegEncode, SavePrecision16Unsupported) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY16, GIMG_RASTER_OWNED,
                NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 0, 8 * 8 * 2);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
  };
  opts.jpeg_precision = 16;
  GIMG_Save_Report report = {};
  EXPECT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report),
      GIMG_ERR_UNSUPPORTED)
      << "jpeg_precision = 16 has no representation in T.81";
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

static constexpr uint32_t kLargeW = 640u;
static constexpr uint32_t kLargeH = 480u;

TEST(JpegEncode, Large640x480BaselineGrayscale) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kLargeW, kLargeH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < kLargeH; y++) {
    for (uint32_t x = 0; x < kLargeW; x++) {
      pixels[y * stride + x] = (unsigned char)((x + y) & 0xFF);
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
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

  jpeg_test::write_jpeg_output(
      "large_640x480_baseline_grayscale.jpg",
      jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), kLargeW);
  EXPECT_EQ(gimg_raster_height(decoded), kLargeH);

  std::string jpeg_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_baseline_grayscale.jpg";
  std::string raw_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_baseline_grayscale.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path.c_str(), raw_path.c_str(), libjpeg_pixels, &oracle_w,
          &oracle_h, &oracle_mode)) {
    EXPECT_EQ(oracle_w, kLargeW);
    EXPECT_EQ(oracle_h, kLargeH);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_pixels.data(), oracle_w, oracle_h, oracle_mode, 0))
        << "our decode must match libjpeg oracle";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, Large640x480BaselineRgb420) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kLargeW, kLargeH, &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < kLargeH; y++) {
    for (uint32_t x = 0; x < kLargeW; x++) {
      pixels[y * stride + x * 4 + 0] = (unsigned char)((x * 17) & 0xFF);
      pixels[y * stride + x * 4 + 1] = (unsigned char)((y * 13) & 0xFF);
      pixels[y * stride + x * 4 + 2] = 128;
      pixels[y * stride + x * 4 + 3] = 255;
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

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

  jpeg_test::write_jpeg_output(
      "large_640x480_baseline_rgb420.jpg",
      jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), kLargeW);
  EXPECT_EQ(gimg_raster_height(decoded), kLargeH);

  std::string jpeg_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_baseline_rgb420.jpg";
  std::string raw_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_baseline_rgb420.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path.c_str(), raw_path.c_str(), libjpeg_pixels, &oracle_w,
          &oracle_h, &oracle_mode)) {
    EXPECT_EQ(oracle_w, kLargeW);
    EXPECT_EQ(oracle_h, kLargeH);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_pixels.data(), oracle_w, oracle_h, oracle_mode, 2))
        << "our decode must match libjpeg oracle";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, Large640x480ProgressiveGrayscale) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kLargeW, kLargeH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < kLargeH; y++) {
    for (uint32_t x = 0; x < kLargeW; x++) {
      pixels[y * stride + x] = (unsigned char)((x + y) & 0xFF);
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_progressive = 1,
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

  jpeg_test::write_jpeg_output(
      "large_640x480_progressive_grayscale.jpg",
      jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), kLargeW);
  EXPECT_EQ(gimg_raster_height(decoded), kLargeH);

  std::string jpeg_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_progressive_grayscale.jpg";
  std::string raw_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_progressive_grayscale.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path.c_str(), raw_path.c_str(), libjpeg_pixels, &oracle_w,
          &oracle_h, &oracle_mode)) {
    EXPECT_EQ(oracle_w, kLargeW);
    EXPECT_EQ(oracle_h, kLargeH);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_pixels.data(), oracle_w, oracle_h, oracle_mode, 0))
        << "our decode must match libjpeg oracle";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, Large640x480ProgressiveRgb420) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kLargeW, kLargeH, &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < kLargeH; y++) {
    for (uint32_t x = 0; x < kLargeW; x++) {
      pixels[y * stride + x * 4 + 0] = (unsigned char)((x * 17) & 0xFF);
      pixels[y * stride + x * 4 + 1] = (unsigned char)((y * 13) & 0xFF);
      pixels[y * stride + x * 4 + 2] = 128;
      pixels[y * stride + x * 4 + 3] = 255;
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_420,
      .jpeg_progressive = 1,
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

  jpeg_test::write_jpeg_output(
      "large_640x480_progressive_rgb420.jpg",
      jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), kLargeW);
  EXPECT_EQ(gimg_raster_height(decoded), kLargeH);

  std::string jpeg_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_progressive_rgb420.jpg";
  std::string raw_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_progressive_rgb420.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path.c_str(), raw_path.c_str(), libjpeg_pixels, &oracle_w,
          &oracle_h, &oracle_mode)) {
    EXPECT_EQ(oracle_w, kLargeW);
    EXPECT_EQ(oracle_h, kLargeH);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_pixels.data(), oracle_w, oracle_h, oracle_mode, 2))
        << "our decode must match libjpeg oracle";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, Large640x480BaselineWithRestart) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(kLargeW, kLargeH, &GIMG_PIXEL_GRAY8,
                GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  size_t stride = gimg_raster_stride_bytes(raster);
  unsigned char * pixels = (unsigned char *)gimg_raster_pixels(raster);
  for (uint32_t y = 0; y < kLargeH; y++) {
    for (uint32_t x = 0; x < kLargeW; x++) {
      pixels[y * stride + x] = (unsigned char)((x + y) & 0xFF);
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 85,
      .jpeg_restart_interval = 16,
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

  jpeg_test::write_jpeg_output(
      "large_640x480_restart.jpg", jpeg_copy.data(), jpeg_copy.size());

  GIMG_Stream * in_stream = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(jpeg_copy.data(), jpeg_copy.size(), &in_stream),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
  DocStreamGuard guard;
  guard.d = doc;
  guard.s = in_stream;
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), kLargeW);
  EXPECT_EQ(gimg_raster_height(decoded), kLargeH);

  std::string jpeg_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_restart.jpg";
  std::string raw_path =
      jpeg_test::jpeg_output_dir() + "/large_640x480_restart.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (jpeg_test::libjpeg_decode_to_oracle_raw(
          jpeg_path.c_str(), raw_path.c_str(), libjpeg_pixels, &oracle_w,
          &oracle_h, &oracle_mode)) {
    EXPECT_EQ(oracle_w, kLargeW);
    EXPECT_EQ(oracle_h, kLargeH);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
        decoded, libjpeg_pixels.data(), oracle_w, oracle_h, oracle_mode, 0))
        << "our decode must match libjpeg oracle";
  }

  gimg_raster_destroy(decoded);
  guard.d = nullptr;
  guard.s = nullptr;
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

/**
 * Decode a libjpeg-produced JPEG that carries restart markers, and compare
 * every sample to what libjpeg itself decoded from it.
 *
 * This used to look for a file someone might have generated into tests/out and
 * skip when it was absent, which it always was, so it asserted nothing - and
 * what it asserted when it did run was only that the decode returned OK, not
 * that the pixels were right.  The fixture and libjpeg's own decode of it are
 * committed instead, so it always runs and compares actual samples.
 *
 * The colour case is exact rather than approximate because a NULL
 * GIMG_Decode_Options now selects fancy chroma upsampling, which is also
 * libjpeg's default; before that the two defaults disagreed and a comparison
 * like this reported a difference on every subsampled file.
 */
TEST(JpegEncode, DecodeLibjpegRestartOracle) {
  struct Case {
    const char * jpg;
    const char * decoded;
    const char * what;
  };
  static const Case cases[] = {
      {"libjpeg_restart_gray.jpg", "libjpeg_restart_gray.pgm", "grey, DRI 34"},
      {"libjpeg_restart_rgb.jpg", "libjpeg_restart_rgb.ppm", "4:2:0, DRI 18"},
  };
  for (const Case & c : cases) {
    uint32_t rw = 0, rh = 0;
    int channels = 0, bits = 0;
    std::vector<uint32_t> want;
    ASSERT_TRUE(
        jpeg_test::load_pnm_file(c.decoded, &rw, &rh, &channels, &bits, want))
        << c.decoded;
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg)) << c.jpg;

    // The point of the fixture is the restart markers; if a regenerated one
    // ever loses them the test would still pass while covering nothing.
    int rst = 0;
    for (size_t i = 0; i + 1 < jpeg.size(); i++) {
      if (jpeg[i] == 0xFF && jpeg[i + 1] >= 0xD0 && jpeg[i + 1] <= 0xD7) {
        rst++;
      }
    }
    EXPECT_GT(rst, 0) << c.what << ": fixture carries no RST markers";

    GIMG_Stream * in_stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &in_stream),
        GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(in_stream, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * decoded = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded), GIMG_OK)
        << c.what;
    ASSERT_NE(decoded, nullptr);
    ASSERT_EQ(gimg_raster_width(decoded), rw) << c.what;
    ASSERT_EQ(gimg_raster_height(decoded), rh) << c.what;
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels_const(decoded);
    size_t stride = gimg_raster_stride_bytes(decoded);
    int rchan = (int)gimg_raster_format(decoded)->channel_count;
    int bad = 0;
    for (uint32_t y = 0; y < rh && bad == 0; y++) {
      for (uint32_t x = 0; x < rw && bad == 0; x++) {
        for (int k = 0; k < channels; k++) {
          uint32_t got = px[(size_t)y * stride + (size_t)x * rchan + k];
          uint32_t expect = want[((size_t)y * rw + x) * (size_t)channels + k];
          if (got != expect) {
            ADD_FAILURE() << c.what << ": first diff at (" << x << "," << y
                          << ") channel " << k << ": libjpeg " << expect
                          << ", ours " << got;
            bad = 1;
            break;
          }
        }
      }
    }
    gimg_raster_destroy(decoded);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in_stream);
  }
}

TEST(JpegEncode, QualityZeroUsesDefault) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 0,
  };
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
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, Quality100ProducesValidJpeg) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 100,
  };
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
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

TEST(JpegEncode, QualityOver100Clamped) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Save_Options save_opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL,
      .quality = 150,
  };
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
  GIMG_Raster * decoded = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &decoded),
      GIMG_OK);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_stream);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// An Adobe APP14 describes the frame it accompanies, so it cannot be copied
// across a re-encode unexamined.
//
// Its transform byte says which colour space the components are in.  A source
// that carried RGB says transform 0; this encoder writes YCbCr for three
// components, and preserving the marker unchanged left the file asserting both
// at once, alongside a JFIF APP0 that asserts YCbCr a third time.  Nothing here
// decodes it wrongly - JFIF outranks Adobe, for this library and for libjpeg -
// but a decoder that reads Adobe first gets a picture in the wrong colours out
// of a file this library wrote.
//
// The lossless path has the opposite problem.  It keeps RGB, and writes its own
// Adobe marker saying so, so a preserved one is not contradictory but
// duplicated: two Adobe segments in one file.
TEST(JpegEncode, AdobeMarkerDescribesTheFrameThatWasWritten) {
  struct Segment {
    uint8_t marker;
    size_t offset;
    size_t payload_len;
  };
  auto segments = [](const std::vector<uint8_t> & d) {
    std::vector<Segment> out;
    size_t i = 2;
    while (i + 1 < d.size()) {
      if (d[i] != 0xFF) {
        i++;
        continue;
      }
      uint8_t m = d[i + 1];
      if (m == 0xD8 || m == 0xD9 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
        i += 2;
        continue;
      }
      if (i + 3 >= d.size()) break;
      size_t len = (size_t)((d[i + 2] << 8) | d[i + 3]);
      if (len < 2) break;
      out.push_back({m, i + 4, len - 2});
      i += 2 + len;
      if (m == 0xDA) break; // Entropy data follows; the headers are all above.
    }
    return out;
  };

  // The fixture's metadata carries an Adobe APP14 with transform 0 (RGB).
  std::vector<uint8_t> src;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("rgb_adobe0.jpg", src));
  {
    int found = 0;
    for (const Segment & seg : segments(src)) {
      if (seg.marker == 0xEE && seg.payload_len >= 12 &&
          memcmp(&src[seg.offset], "Adobe\0", 6) == 0) {
        found++;
        EXPECT_EQ((int)src[seg.offset + 11], 0) << "fixture must say RGB";
      }
    }
    ASSERT_EQ(found, 1) << "fixture must carry exactly one Adobe APP14";
  }

  struct Case {
    uint8_t lossless_predictor;
    int want_adobe_count;
    int want_transform;   // -1 when no Adobe segment is expected
    bool want_jfif;
    const char * what;
  };
  const Case cases[] = {
      {0, 1, 1, true,
          "baseline: one Adobe marker, saying the YCbCr that was written"},
      {4, 1, 0, false,
          "lossless: one Adobe marker, the body's, still saying RGB"},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    DocStreamGuard in;
    ASSERT_EQ(gimg_stream_create_memory(src.data(), src.size(), &in.s),
        GIMG_OK);
    ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options so = {};
    so.metadata_policy = GIMG_META_PRESERVE_ALL;
    so.jpeg_lossless_predictor = c.lossless_predictor;
    GIMG_Save_Report rep = {};
    ASSERT_EQ(gimg_doc_save(in.d, out, "jpeg", &so, &rep), GIMG_OK);
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    std::vector<uint8_t> enc(
        (const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(out);

    int adobe = 0, jfif = 0, transform = -1;
    for (const Segment & seg : segments(enc)) {
      if (seg.marker == 0xEE && seg.payload_len >= 12 &&
          memcmp(&enc[seg.offset], "Adobe\0", 6) == 0) {
        adobe++;
        transform = (int)enc[seg.offset + 11];
      }
      if (seg.marker == 0xE0 && seg.payload_len >= 5 &&
          memcmp(&enc[seg.offset], "JFIF\0", 5) == 0) {
        jfif++;
      }
    }
    EXPECT_EQ(adobe, c.want_adobe_count);
    EXPECT_EQ(transform, c.want_transform);
    EXPECT_EQ(jfif > 0, c.want_jfif);
  }
}

// Writing a hierarchical sequence (T.81 Annex J, J.1).
//
// The encoder has to contain a decoder to work at all: every differential
// frame codes the difference between the picture and what has been
// reconstructed so far, so at each step the encoder must reconstruct exactly
// what a decoder will, quantisation loss included.  Get that reconstruction
// wrong and the file still decodes - it just decodes to the wrong picture,
// consistently, in every decoder, which is why the round trip below compares
// against the source rather than against another decode.
//
// The first version of this encoder handed its quantization table to the
// decoder's dequantiser, which reads the table in the zigzag order a DQT
// segment stores it in, while the encoder keeps it in natural order.  Every
// coefficient was multiplied by the wrong element, the reference bore no
// relation to the frame, and the differential frames spent their bits coding
// nonsense: the file grew by 70% and the picture came out visibly wrong while
// two independent decoders agreed with each other about it.
TEST(JpegEncode, HierarchicalRoundTrip) {
  const uint32_t kW = 129, kH = 77; // odd both ways: doubling overshoots
  std::vector<uint8_t> src((size_t)kW * kH * 3);
  for (uint32_t y = 0; y < kH; y++) {
    for (uint32_t x = 0; x < kW; x++) {
      // Smooth, so that quantisation error is the only thing being measured.
      size_t k = ((size_t)y * kW + x) * 3;
      src[k + 0] = (uint8_t)(128 + 100 * std::sin(x / 11.0) * std::cos(y / 9.0));
      src[k + 1] = (uint8_t)(128 + 90 * std::sin((x + y) / 17.0));
      src[k + 2] = (uint8_t)(128 + 80 * std::cos(x / 23.0));
    }
  }

  for (int levels = 1; levels <= 3; levels++) {
    for (int arith = 0; arith <= 1; arith++) {
      // A restart interval applies to every frame of the sequence, each
      // counting it in its own MCUs (B.2.4.4).
      for (uint16_t ri : {(uint16_t)0, (uint16_t)3}) {
      SCOPED_TRACE("levels=" + std::to_string(levels) +
          " arithmetic=" + std::to_string(arith) +
          " restart_interval=" + std::to_string(ri));
      GIMG_Doc * doc = nullptr;
      ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
      GIMG_Raster * raster = nullptr;
      ASSERT_EQ(gimg_raster_create(kW, kH, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                    NULL, 0, &raster),
          GIMG_OK);
      unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
      size_t stride = gimg_raster_stride_bytes(raster);
      for (uint32_t y = 0; y < kH; y++) {
        for (uint32_t x = 0; x < kW; x++) {
          size_t k = ((size_t)y * kW + x) * 3;
          px[y * stride + x * 4 + 0] = src[k + 0];
          px[y * stride + x * 4 + 1] = src[k + 1];
          px[y * stride + x * 4 + 2] = src[k + 2];
          px[y * stride + x * 4 + 3] = 255;
        }
      }
      gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

      GIMG_Stream * out = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
      GIMG_Save_Options so = {};
      so.metadata_policy = GIMG_META_DROP_ALL;
      so.quality = 85;
      so.jpeg_hierarchical_levels = (uint8_t)levels;
      so.jpeg_arithmetic = (uint8_t)arith;
      so.jpeg_restart_interval = ri;
      GIMG_Save_Report rep = {};
      ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &so, &rep), GIMG_OK);
      const void * data = nullptr;
      size_t size = 0;
      gimg_stream_output_buffer(out, &data, &size);
      std::vector<uint8_t> enc(
          (const uint8_t *)data, (const uint8_t *)data + size);
      gimg_stream_destroy(out);
      gimg_doc_destroy(doc);

      // The markers say it is a pyramid: a DHP, then one non-differential
      // frame and `levels` differential ones, each with its own EXP.
      int dhp = 0, non_diff = 0, diff = 0, exp_seg = 0;
      std::vector<uint16_t> frame_w;
      for (size_t i = 0; i + 3 < enc.size();) {
        if (enc[i] != 0xFF) {
          i++;
          continue;
        }
        uint8_t m = enc[i + 1];
        if (m == 0xD8 || m == 0xD9 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
          i += 2;
          continue;
        }
        size_t len = (size_t)((enc[i + 2] << 8) | enc[i + 3]);
        if (m == 0xDE) {
          dhp++;
        }
        if (m == 0xDF) {
          exp_seg++;
        }
        if (m == 0xC1 || m == 0xC9) {
          non_diff++;
          frame_w.push_back((uint16_t)((enc[i + 7] << 8) | enc[i + 8]));
        }
        if (m == 0xC5 || m == 0xCD) {
          diff++;
          frame_w.push_back((uint16_t)((enc[i + 7] << 8) | enc[i + 8]));
        }
        i += 2 + len;
        if (m == 0xDA) {
          while (i + 1 < enc.size()) {
            if (enc[i] == 0xFF && enc[i + 1] != 0 &&
                !(enc[i + 1] >= 0xD0 && enc[i + 1] <= 0xD7)) {
              break;
            }
            i++;
          }
        }
      }
      EXPECT_EQ(dhp, 1);
      EXPECT_EQ(non_diff, 1);
      EXPECT_EQ(diff, levels);
      EXPECT_EQ(exp_seg, levels) << "each differential frame needs its EXP";
      ASSERT_EQ((int)frame_w.size(), levels + 1);
      EXPECT_EQ(frame_w.back(), kW) << "the last frame is the full image";
      // Going down, each level is ceil(w / 2) - K.5: "If the image being
      // downsampled has an odd width or length, the odd dimension is increased
      // by 1 by sample replication ... before downsampling."  Going back up,
      // J.1.1.2's expansion "always doubles the line length" and the odd
      // column that overshoots is dropped, so the relation to assert is the
      // halving, not an exact doubling.
      for (size_t f = 1; f < frame_w.size(); f++) {
        EXPECT_EQ(frame_w[f - 1], (uint16_t)((frame_w[f] + 1u) / 2u))
            << "frame " << f << " must be the level above frame " << (f - 1);
      }

      // Decode it back and measure against the source.  A hierarchical encode
      // at this quality should land where an ordinary one does; the bound is
      // loose enough not to be a tripwire for a changed rounding and tight
      // enough that a reference computed even slightly wrongly fails it - the
      // zigzag-order bug above put the worst channel past 90.
      DocStreamGuard in;
      ASSERT_EQ(gimg_stream_create_memory(enc.data(), enc.size(), &in.s),
          GIMG_OK);
      ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
      RasterGuard got;
      ASSERT_EQ(gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
          GIMG_OK);
      ASSERT_NE(got.r, nullptr);
      ASSERT_EQ(gimg_raster_width(got.r), kW);
      ASSERT_EQ(gimg_raster_height(got.r), kH);
      const unsigned char * gp =
          (const unsigned char *)gimg_raster_pixels(got.r);
      size_t gs = gimg_raster_stride_bytes(got.r);
      int worst = 0;
      double se = 0;
      for (uint32_t y = 0; y < kH; y++) {
        for (uint32_t x = 0; x < kW; x++) {
          for (int c = 0; c < 3; c++) {
            int a = (int)gp[y * gs + x * 4 + c];
            int b = (int)src[((size_t)y * kW + x) * 3 + c];
            int d = a - b;
            se += (double)d * d;
            if (d < 0) {
              d = -d;
            }
            if (d > worst) {
              worst = d;
            }
          }
        }
      }
      double mse = se / ((double)kW * kH * 3);
      double psnr = 10.0 * std::log10(255.0 * 255.0 / mse);
      EXPECT_LE(worst, 25) << "worst channel error";
      EXPECT_GE(psnr, 40.0) << "PSNR " << psnr << " dB";
      }
    }
  }
}

// A hierarchical sequence is built out of one coding process, so asking for
// another alongside it is refused rather than quietly downgraded.
//
// The frames this encoder writes are 8-bit sequential DCT (and their
// arithmetic counterparts); Annex J allows progressive and lossless ones too,
// and this library reads them, but writing one means a different
// reconstruction at every step - the part of a hierarchical encoder that
// cannot be approximated - so the option combination has no meaning yet.
TEST(JpegEncode, HierarchicalRefusesACombinationItCannotWrite) {
  struct Case {
    uint8_t progressive;
    uint8_t lossless_predictor;
    uint8_t precision;
    const char * what;
  };
  const Case cases[] = {
      {1, 0, 0, "progressive frames in a sequence (SOF6, SOF14)"},
      {0, 4, 0, "lossless frames in a sequence (SOF7, SOF15)"},
      {0, 0, 12, "12-bit frames"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(
                  16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster),
        GIMG_OK);
    memset(gimg_raster_pixels(raster), 0x40,
        gimg_raster_stride_bytes(raster) * 16);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options so = {};
    so.quality = 85;
    so.jpeg_hierarchical_levels = 1;
    so.jpeg_progressive = c.progressive;
    so.jpeg_lossless_predictor = c.lossless_predictor;
    so.jpeg_precision = c.precision;
    GIMG_Save_Report rep = {};
    EXPECT_EQ(gimg_doc_save(doc, out, "jpeg", &so, &rep), GIMG_ERR_UNSUPPORTED);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
  }
}

// The ISO reference codec reads what this encoder writes.
//
// The fixtures are this library's own hierarchical output and that codec's
// decode of it, so the comparison is against an independent implementation of
// Annex J reading our file - the half of interoperability that a round trip
// through our own decoder cannot test, since a private misreading of the spec
// would round-trip perfectly.  The tolerance is the usual one: that codec's
// IDCT is not libjpeg's and this library matches libjpeg.
//
// See tests/data/jpeg/README.md for how to regenerate them.
TEST(JpegEncode, HierarchicalOutputIsReadByTheReferenceCodec) {
  struct Case {
    const char * jpg;
    const char * ref;
    int tolerance;
    const char * what;
  };
  const Case cases[] = {
      {"hier_ours_l1.jpg", "hier_ours_l1_thor.ppm", 4, "one level, Huffman"},
      {"hier_ours_l2.jpg", "hier_ours_l2_thor.ppm", 5, "two levels, Huffman"},
      {"hier_ours_l1_arith.jpg", "hier_ours_l1_arith_thor.ppm", 4,
          "one level, arithmetic"},
      {"hier_ours_l3_arith.jpg", "hier_ours_l3_arith_thor.ppm", 6,
          "three levels, arithmetic"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    uint32_t rw = 0, rh = 0;
    int rchan = 0, rbits = 0;
    std::vector<uint32_t> ref;
    ASSERT_TRUE(jpeg_test::load_pnm_file(c.ref, &rw, &rh, &rchan, &rbits, ref))
        << "missing reference " << c.ref;
    ASSERT_EQ(rchan, 3);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    DocStreamGuard in;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &in.s),
        GIMG_OK);
    ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
    RasterGuard got;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
    ASSERT_NE(got.r, nullptr);
    ASSERT_EQ(gimg_raster_width(got.r), rw);
    ASSERT_EQ(gimg_raster_height(got.r), rh);
    const unsigned char * gp = (const unsigned char *)gimg_raster_pixels(got.r);
    size_t gs = gimg_raster_stride_bytes(got.r);
    int worst = 0;
    for (uint32_t y = 0; y < rh; y++) {
      for (uint32_t x = 0; x < rw; x++) {
        for (int c2 = 0; c2 < 3; c2++) {
          int a = (int)gp[y * gs + x * 4 + c2];
          int b = (int)ref[((size_t)y * rw + x) * 3 + c2];
          int d = a > b ? a - b : b - a;
          if (d > worst) {
            worst = d;
          }
        }
      }
    }
    EXPECT_LE(worst, c.tolerance) << "worst channel difference " << worst;
  }
}


// Entropy data must not be reallocated once per byte.
//
// T.81 B.2.2's stuffing has to be read a byte at a time - a 0xFF in the
// entropy stream is followed by a 0x00 that is not data - so the loader
// appends one or two bytes at a time, and the buffer used to be resized to fit
// on every call.  That makes reading a scan quadratic in its length and leaves
// one dead allocation per byte behind it.  The fuzzer is what noticed: a
// 224-byte input describing a 64x9280 frame drove the address sanitizer's
// quarantine to 3.5 GB, because it holds every freed block, and there were
// half a million of them.  A megabyte of entropy data is an ordinary amount
// for a photograph.
//
// Counting the allocator's calls rather than timing anything, and rather than
// watching the pointer: realloc usually extends a growing buffer in place, so
// the address stays put through a million resizes and says nothing at all.
// What matters is that the buffer grows geometrically, which is a statement
// about how often it is asked to grow.
namespace {
struct CountingAllocator {
  GIMG_Allocator vt;
  size_t reallocs = 0;
  size_t mallocs = 0;
};
void * counting_malloc(void * ctx, size_t size) {
  ((CountingAllocator *)ctx)->mallocs++;
  return malloc(size ? size : 1);
}
void * counting_calloc(void * ctx, size_t n, size_t size) {
  ((CountingAllocator *)ctx)->mallocs++;
  return calloc(n ? n : 1, size ? size : 1);
}
void * counting_realloc(void * ctx, void * ptr, size_t size) {
  ((CountingAllocator *)ctx)->reallocs++;
  return realloc(ptr, size ? size : 1);
}
void counting_free(void *, void * ptr) {
  free(ptr);
}
} // namespace

TEST(JpegEncode, ScanDataGrowsGeometrically) {
  CountingAllocator counter;
  counter.vt.ctx = &counter;
  counter.vt.malloc_fn = counting_malloc;
  counter.vt.calloc_fn = counting_calloc;
  counter.vt.realloc_fn = counting_realloc;
  counter.vt.free_fn = counting_free;

  gimg_jpeg_doc_state_t * state =
      (gimg_jpeg_doc_state_t *)calloc(1, sizeof(gimg_jpeg_doc_state_t));
  ASSERT_NE(state, nullptr);
  state->allocator = &counter.vt;
  state->num_scans = 1;
  state->cur_scan = &state->scans[0];

  const size_t kBytes = 1u << 20; // a megabyte, one or two bytes at a time
  const unsigned char pair[2] = {0xFF, 0x00};
  const unsigned char one = 0x5A;
  size_t written = 0;
  while (written < kBytes) {
    // Alternate, so that both shapes of append are exercised.
    bool two = (written & 1u) != 0;
    ASSERT_EQ(jpeg_append_scan_data(state, two ? pair : &one, two ? 2u : 1u),
        GIMG_OK);
    written += two ? 2u : 1u;
  }
  EXPECT_EQ(state->scans[0].data_size, written);
  // Doubling from 4 KiB reaches a megabyte in eight steps, so two dozen is
  // generous; resizing to fit would be about seven hundred thousand.
  EXPECT_LE(counter.reallocs, 24u)
      << "buffer resized " << counter.reallocs << " times for " << written
      << " bytes appended";

  counting_free(nullptr, state->scans[0].data);
  free(state);
}
