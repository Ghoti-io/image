/**
 * @file
 *
 * JPEG encode tests: save raster to JPEG, re-load and decode; deterministic.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
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
#include "../../exif_test_utils.h"
#include "../../failing_allocator.h"
#include "../../../src/codec/codec_internal.h"

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

namespace jfif_aspect {

/** Load a fixture into a document, decoded and ready to re-save. */
GIMG_Doc * load(const char * name, GIMG_Stream ** keep,
    std::vector<uint8_t> & bytes) {
  if (!jpeg_test::load_jpeg_file(name, bytes)) {
    return nullptr;
  }
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), keep) != GIMG_OK) {
    return nullptr;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(*keep, nullptr, nullptr, &doc) != GIMG_OK) {
    return nullptr;
  }
  gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr);
  return doc;
}

/** Save as JPEG and load the result back. */
GIMG_Doc * round_trip(GIMG_Doc * doc, GIMG_Stream ** keep,
    std::vector<uint8_t> & saved) {
  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    return nullptr;
  }
  GIMG_Save_Report report = {};
  if (gimg_doc_save(doc, out_s, "jpeg", nullptr, &report) != GIMG_OK) {
    gimg_stream_destroy(out_s);
    return nullptr;
  }
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out_s, &data, &size);
  saved.assign(static_cast<const uint8_t *>(data),
      static_cast<const uint8_t *>(data) + size);
  gimg_stream_destroy(out_s);
  if (gimg_stream_create_memory(saved.data(), saved.size(), keep) != GIMG_OK) {
    return nullptr;
  }
  GIMG_Doc * back = nullptr;
  if (gimg_doc_load(*keep, nullptr, nullptr, &back) != GIMG_OK) {
    return nullptr;
  }
  return back;
}

} // namespace jfif_aspect

TEST(JpegEncode, AJfifDensityWithUnitsZeroIsAPixelAspectRatio) {
  // JFIF 1.02: units 0 means the density fields are the pixel's aspect ratio
  // and the file states no size at all - the same thing PNG says with a unit 0
  // pHYs and GIF with its Pixel Aspect Ratio byte.  The loader used to handle
  // units 1 only, so a JPEG declaring a non-square pixel declared nothing here.
  GIMG_Stream * keep = nullptr;
  std::vector<uint8_t> bytes;
  GIMG_Doc * doc = jfif_aspect::load("jfif_aspect_2_1.jpg", &keep, bytes);
  ASSERT_NE(doc, nullptr);
  uint32_t num = 0, den = 0;
  const int said = gimg_doc_pixel_aspect_ratio(doc, &num, &den);
  EXPECT_EQ(said, 1);
  EXPECT_EQ(num, 2u);
  EXPECT_EQ(den, 1u);
  // It is not a resolution, and must not be reported as one.
  GIMG_Meta_Common * meta = gimg_doc_meta_common(doc);
  if (meta) {
    uint32_t x_dpi = 0, y_dpi = 0;
    gimg_meta_common_dpi(meta, &x_dpi, &y_dpi);
    EXPECT_EQ(x_dpi, 0u) << "units 0 says nothing about physical size";
    EXPECT_EQ(y_dpi, 0u);
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(keep);
}

TEST(JpegEncode, EqualJfifDensitiesAreNotAClaimAboutShape) {
  // Every encoder writes units 0 with density 1:1 whether it knows anything or
  // not - it is the JFIF way of writing "no information".  Recording it would
  // turn boilerplate into a declaration and put a pHYs in every PNG converted
  // from a JPEG.
  GIMG_Stream * keep = nullptr;
  std::vector<uint8_t> bytes;
  GIMG_Doc * doc = jfif_aspect::load("baseline_16x16_ycbcr.jpg", &keep, bytes);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(doc, nullptr, nullptr), 0);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(keep);
}

TEST(JpegEncode, APixelAspectRatioSurvivesASaveAndCanBeChangedAndCleared) {
  // The whole of CRUD through the file.  The update half is the one that was
  // silently broken everywhere: the APP0 the file arrived with was preserved
  // verbatim, so setting a ratio changed what the accessor reported and not
  // what was written, and a caller saw success and got the old value back.
  GIMG_Stream * keep = nullptr;
  std::vector<uint8_t> bytes;
  GIMG_Doc * doc = jfif_aspect::load("jfif_aspect_2_1.jpg", &keep, bytes);
  ASSERT_NE(doc, nullptr);

  // Read, then save unchanged: it comes back.
  {
    GIMG_Stream * k2 = nullptr;
    std::vector<uint8_t> saved;
    GIMG_Doc * back = jfif_aspect::round_trip(doc, &k2, saved);
    ASSERT_NE(back, nullptr);
    uint32_t n = 0, d = 0;
    EXPECT_EQ(gimg_doc_pixel_aspect_ratio(back, &n, &d), 1);
    EXPECT_EQ(n, 2u);
    EXPECT_EQ(d, 1u);
    gimg_doc_destroy(back);
    gimg_stream_destroy(k2);
  }
  // Update.
  gimg_doc_set_pixel_aspect_ratio(doc, 5u, 4u);
  {
    GIMG_Stream * k2 = nullptr;
    std::vector<uint8_t> saved;
    GIMG_Doc * back = jfif_aspect::round_trip(doc, &k2, saved);
    ASSERT_NE(back, nullptr);
    uint32_t n = 0, d = 0;
    EXPECT_EQ(gimg_doc_pixel_aspect_ratio(back, &n, &d), 1);
    EXPECT_EQ(n, 5u) << "the preserved APP0 won and the new ratio was lost";
    EXPECT_EQ(d, 4u);
    gimg_doc_destroy(back);
    gimg_stream_destroy(k2);
  }
  // Delete.
  gimg_doc_clear_pixel_aspect_ratio(doc);
  {
    GIMG_Stream * k2 = nullptr;
    std::vector<uint8_t> saved;
    GIMG_Doc * back = jfif_aspect::round_trip(doc, &k2, saved);
    ASSERT_NE(back, nullptr);
    EXPECT_EQ(gimg_doc_pixel_aspect_ratio(back, nullptr, nullptr), 0);
    gimg_doc_destroy(back);
    gimg_stream_destroy(k2);
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(keep);
}

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

// The picture the four-component fixtures were made from, and that the encode
// tests below build again.  tests/data/jpeg/mk_cmyk.c fills the same pattern,
// so libjpeg's files and this library's files carry the same image.
static void fill_cmyk_pattern(GIMG_Raster * raster) {
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      px[y * stride + x * 4 + 0] = (unsigned char)((x * 7 + y * 3) & 0xFF);
      px[y * stride + x * 4 + 1] = (unsigned char)((x * 3 + y * 11) & 0xFF);
      px[y * stride + x * 4 + 2] = (unsigned char)((x * 13 + y * 5) & 0xFF);
      px[y * stride + x * 4 + 3] = (unsigned char)((x + y * 2) & 0xFF);
    }
  }
}

// Save that pattern as JPEG with the given options; returns the file bytes.
static bool save_cmyk_pattern(uint32_t w, uint32_t h, uint8_t transform,
    uint8_t subsampling, uint8_t progressive, uint8_t arithmetic,
    uint8_t non_interleaved, unsigned quality, std::vector<uint8_t> & out,
    GIMG_Result * out_result) {
  out.clear();
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK) {
    return false;
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(
          w, h, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0, &raster) !=
      GIMG_OK) {
    gimg_doc_destroy(doc);
    return false;
  }
  fill_cmyk_pattern(raster);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * out_stream = nullptr;
  if (gimg_stream_create_memory_output(&out_stream) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return false;
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  opts.quality = quality;
  opts.jpeg_cmyk_transform = transform;
  opts.jpeg_chroma_subsampling = subsampling;
  opts.jpeg_progressive = progressive;
  opts.jpeg_arithmetic = arithmetic;
  opts.jpeg_non_interleaved = non_interleaved;
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_stream, "jpeg", &opts, &report);
  if (out_result) {
    *out_result = r;
  }
  if (r == GIMG_OK) {
    const void * buf = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out_stream, &buf, &n);
    out.assign((const uint8_t *)buf, (const uint8_t *)buf + n);
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out_stream);
  return r == GIMG_OK;
}

// A four-component raster used to be refused outright, so a CMYK JPEG could be
// read and never written back: load-and-save lost the picture.  T.81 B.2.2
// counts components from 1 to 255 and says nothing about what they mean, so
// there was never a reason in the standard for the encoder to stop at three;
// the reason was that the encoder carried its components in comp_y, comp_cb
// and comp_cr, and three names is where that ends.
//
// The oracle is libjpeg-turbo 3.0.4: each .jpg here is this encoder's own
// output and each .raw beside it is libjpeg's decode of that exact file
// (tests/data/jpeg/cmyk_ref.c).  So the test pins two things at once - that
// re-encoding the pattern still produces the committed bytes, and that those
// bytes mean to libjpeg what they mean here.
TEST(JpegEncode, FourComponentFramesAreWrittenAndLibjpegTurboReadsThem) {
  struct Case {
    const char * base;
    uint8_t transform;
    uint8_t subsampling;
    uint8_t progressive;
    uint8_t arithmetic;
    uint8_t non_interleaved;
    const char * what;
  };
  const Case cases[] = {
      {"cmyk_ours_seq", 0, GIMG_JPEG_CHROMA_444, 0, 0, 0,
          "CMYK, sequential (A.2.2)"},
      {"cmyk_ours_prog", 0, GIMG_JPEG_CHROMA_444, 1, 0, 0,
          "CMYK, progressive (Annex G with Nf=4)"},
      {"cmyk_ours_arith", 0, GIMG_JPEG_CHROMA_444, 0, 1, 0,
          "CMYK, arithmetic (SOF9, Annex D)"},
      {"cmyk_ours_ni", 0, GIMG_JPEG_CHROMA_444, 0, 0, 1,
          "CMYK, one scan per component (A.2.3)"},
      {"ycck_ours_444", 2, GIMG_JPEG_CHROMA_444, 0, 0, 0, "YCCK, 4:4:4"},
      {"ycck_ours_420", 2, GIMG_JPEG_CHROMA_420, 0, 0, 0, "YCCK, 4:2:0"},
      {"ycck_ours_422", 2, GIMG_JPEG_CHROMA_422, 0, 0, 0, "YCCK, 4:2:2"},
      {"ycck_ours_prog420", 2, GIMG_JPEG_CHROMA_420, 1, 0, 0,
          "YCCK, 4:2:0, progressive"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.base) + ": " + c.what);
    std::vector<uint8_t> written;
    ASSERT_TRUE(save_cmyk_pattern(33, 17, c.transform, c.subsampling,
        c.progressive, c.arithmetic, c.non_interleaved, 90, written, nullptr))
        << "a four-component raster must be writable";

    std::vector<uint8_t> committed;
    std::string name = std::string(c.base) + ".jpg";
    ASSERT_TRUE(jpeg_test::load_jpeg_file(name.c_str(), committed))
        << "missing fixture " << name;
    ASSERT_EQ(written.size(), committed.size())
        << "the encoder no longer produces the file libjpeg was shown";
    EXPECT_TRUE(written == committed)
        << "the encoder no longer produces the file libjpeg was shown";

    std::vector<uint8_t> oracle;
    uint32_t ow = 0, oh = 0;
    int omode = -1;
    ASSERT_TRUE(
        jpeg_test::load_jpeg_oracle_raw(c.base, oracle, &ow, &oh, &omode))
        << "missing oracle " << c.base << ".raw";
    ASSERT_EQ(omode, 2) << "the oracle must be a CMYK .raw";

    DocStreamGuard in;
    ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(), &in.s),
        GIMG_OK);
    ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
    RasterGuard got;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
    ASSERT_NE(got.r, nullptr);
    const GIMG_Pixel_Format * fmt = gimg_raster_format(got.r);
    ASSERT_NE(fmt, nullptr);
    EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK);
    ASSERT_EQ(gimg_raster_width(got.r), ow);
    ASSERT_EQ(gimg_raster_height(got.r), oh);
    const unsigned char * gp = (const unsigned char *)gimg_raster_pixels(got.r);
    size_t gs = gimg_raster_stride_bytes(got.r);
    for (uint32_t y = 0; y < oh; y++) {
      for (uint32_t x = 0; x < ow; x++) {
        for (int ch = 0; ch < 4; ch++) {
          int a = (int)gp[y * gs + x * 4 + (size_t)ch];
          int b = (int)oracle[((size_t)y * ow + x) * 4 + (size_t)ch];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
        }
      }
    }
  }
}

// Transform 0 writes the four components exactly as they arrived, which is
// what libjpeg's JCS_CMYK does (jdcolor.c null_convert on the way back).  A
// CMYK raster must therefore survive a save and a load unchanged apart from
// the quantizer, and at quality 100 with no subsampling the only remaining
// loss is the DCT rounding - small, bounded, and the same in both directions.
//
// This is the property the gap list called out: a CMYK JPEG that decodes but
// cannot be saved does not round-trip at all.
TEST(JpegEncode, CmykRasterSurvivesASaveAndLoad) {
  std::vector<uint8_t> written;
  ASSERT_TRUE(save_cmyk_pattern(
      33, 17, 0, GIMG_JPEG_CHROMA_444, 0, 0, 0, 100, written, nullptr));
  DocStreamGuard in;
  ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(), &in.s),
      GIMG_OK);
  ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
  RasterGuard got;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
  ASSERT_NE(got.r, nullptr);
  ASSERT_EQ(gimg_raster_width(got.r), 33u);
  ASSERT_EQ(gimg_raster_height(got.r), 17u);
  RasterGuard want;
  ASSERT_EQ(gimg_raster_create(33, 17, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED,
                NULL, 0, &want.r),
      GIMG_OK);
  fill_cmyk_pattern(want.r);
  const unsigned char * gp = (const unsigned char *)gimg_raster_pixels(got.r);
  const unsigned char * wp = (const unsigned char *)gimg_raster_pixels(want.r);
  size_t gs = gimg_raster_stride_bytes(got.r);
  size_t ws = gimg_raster_stride_bytes(want.r);
  int worst = 0;
  for (uint32_t y = 0; y < 17u; y++) {
    for (uint32_t x = 0; x < 33u; x++) {
      for (int ch = 0; ch < 4; ch++) {
        int d = (int)gp[y * gs + x * 4 + (size_t)ch] -
            (int)wp[y * ws + x * 4 + (size_t)ch];
        if (d < 0) {
          d = -d;
        }
        if (d > worst) {
          worst = d;
        }
      }
    }
  }
  EXPECT_LE(worst, 2) << "the components are meant to pass through unchanged "
                         "apart from the quantizer";
}

// T.81 has no color space, so the Adobe APP14 marker is the only thing that
// says whether the first three components of a four-component frame are C, M
// and Y or Y, Cb and Cr.  Writing the frame without it would leave a YCCK file
// that every decoder reads as CMYK, so the marker goes in whatever the
// metadata policy says - it is not metadata.
TEST(JpegEncode, FourComponentFrameAlwaysCarriesItsAdobeMarker) {
  for (uint8_t transform : {uint8_t(0), uint8_t(2)}) {
    for (GIMG_Meta_Policy policy :
        {GIMG_META_PRESERVE_ALL, GIMG_META_DROP_ALL}) {
      SCOPED_TRACE("transform " + std::to_string((int)transform) + ", policy " +
          std::to_string((int)policy));
      GIMG_Doc * doc = nullptr;
      ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
      GIMG_Raster * raster = nullptr;
      ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_CMYK8,
                    GIMG_RASTER_OWNED, NULL, 0, &raster),
          GIMG_OK);
      fill_cmyk_pattern(raster);
      gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
      GIMG_Stream * out_stream = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
      GIMG_Save_Options opts = {};
      opts.metadata_policy = policy;
      opts.quality = 90;
      opts.jpeg_cmyk_transform = transform;
      GIMG_Save_Report report = {};
      ASSERT_EQ(gimg_doc_save(doc, out_stream, "jpeg", &opts, &report),
          GIMG_OK);
      const void * buf = nullptr;
      size_t n = 0;
      gimg_stream_output_buffer(out_stream, &buf, &n);
      const uint8_t * b = (const uint8_t *)buf;
      // Find APP14 and read its transform byte (payload index 11).
      int found_transform = -1;
      int saw_jfif = 0;
      size_t i = 2;
      while (i + 4 <= n && b[i] == 0xFF) {
        uint8_t m = b[i + 1];
        if (m == 0xD9 || m == 0xDA) {
          break;
        }
        size_t len = (size_t)((b[i + 2] << 8) | b[i + 3]);
        const uint8_t * pay = b + i + 4;
        size_t paylen = len >= 2 ? len - 2 : 0;
        if (m == 0xEE && paylen >= 12 && memcmp(pay, "Adobe\0", 6) == 0) {
          found_transform = pay[11];
        }
        if (m == 0xE0 && paylen >= 5 && memcmp(pay, "JFIF\0", 5) == 0) {
          saw_jfif = 1;
        }
        i += 2 + len;
      }
      EXPECT_EQ(found_transform, (int)transform)
          << "the Adobe marker must say which transform was used";
      EXPECT_EQ(saw_jfif, 0) << "JFIF describes grayscale or YCbCr data and "
                                "says nothing about four components; libjpeg "
                                "writes none for a CMYK or YCCK file";
      gimg_doc_destroy(doc);
      gimg_stream_destroy(out_stream);
    }
  }
}

// A restart interval changes where the entropy coder resets and nothing else:
// the coefficients either side of an RST marker are the same coefficients
// (T.81 F.1.1 and the note in B.2.4.4), so the same image written with any
// interval must decode to the same pixels as one written with none.  An
// interval of 1 is the extreme of that - a marker after every MCU - and the
// edge-case table used to say it was not tested.
//
// The equality needs no oracle, and it covers what an oracle would not: the
// interval is counted in MCUs, and an MCU is a different thing in an
// interleaved scan, a non-interleaved one (A.2.3, where it is a single block)
// and each band of a progressive one.
//
// Precision is on the list because it decides which entropy decoder reads the
// file, and the twelve-bit one has a restart branch of its own - including
// the DC predictor reset, which is the whole reason a restart marker is a
// thing that can go wrong. This swept four axes at eight bits only, so that
// branch had never run: every twelve-bit fixture was written without an
// interval and every restart fixture at eight bits. Varying four things and
// not the one that selects the code under test is how a sweep reports a
// result about somewhere else.
TEST(JpegEncode, SmallRestartIntervalsChangeNothingButWhereTheCoderResets) {
  const uint32_t w = 37, h = 21;
  long combinations = 0;
  for (int precision : {8, 12}) {
  for (int subsampling = 0; subsampling <= 2; subsampling++) {
    for (int progressive = 0; progressive <= 1; progressive++) {
      for (int arithmetic = 0; arithmetic <= 1; arithmetic++) {
        for (int non_interleaved = 0; non_interleaved <= 1;
            non_interleaved++) {
          if (progressive && non_interleaved) {
            continue; // Annex G owns the scan script; the encoder refuses it
          }
          SCOPED_TRACE("precision " + std::to_string(precision) +
              ", subsampling " + std::to_string(subsampling) +
              ", progressive " + std::to_string(progressive) +
              ", arithmetic " + std::to_string(arithmetic) +
              ", non-interleaved " + std::to_string(non_interleaved));
          std::vector<uint8_t> decoded[4];
          GIMG_Result save_result[4] = {
              GIMG_OK, GIMG_OK, GIMG_OK, GIMG_OK};
          const uint16_t intervals[4] = {0, 1, 2, 3};
          for (int k = 0; k < 4; k++) {
            GIMG_Doc * doc = nullptr;
            ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
            GIMG_Raster * raster = nullptr;
            const GIMG_Pixel_Format * fmt =
                (precision == 12) ? &GIMG_PIXEL_RGBA12 : &GIMG_PIXEL_RGBA8;
            ASSERT_EQ(gimg_raster_create(
                          w, h, fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
                GIMG_OK);
            unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
            size_t stride = gimg_raster_stride_bytes(raster);
            for (uint32_t y = 0; y < h; y++) {
              for (uint32_t x = 0; x < w; x++) {
                if (precision == 12) {
                  uint16_t * p =
                      (uint16_t *)(void *)(px + y * stride + x * 8);
                  p[0] = (uint16_t)((x * 111 + y * 79) & 0xFFF);
                  p[1] = (uint16_t)((x * 47 + y * 173) & 0xFFF);
                  p[2] = (uint16_t)((x * 209 + y * 31) & 0xFFF);
                  p[3] = 4095;
                }
                else {
                  unsigned char * p = px + y * stride + x * 4;
                  p[0] = (unsigned char)((x * 7 + y * 5) & 0xFF);
                  p[1] = (unsigned char)((x * 3 + y * 11) & 0xFF);
                  p[2] = (unsigned char)((x * 13 + y * 2) & 0xFF);
                  p[3] = 255;
                }
              }
            }
            gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
            GIMG_Stream * os = nullptr;
            ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
            GIMG_Save_Options so = {};
            so.metadata_policy = GIMG_META_DROP_ALL;
            so.quality = 85;
            so.jpeg_restart_interval = intervals[k];
            so.jpeg_chroma_subsampling = (uint8_t)subsampling;
            so.jpeg_progressive = (uint8_t)progressive;
            so.jpeg_arithmetic = (uint8_t)arithmetic;
            so.jpeg_non_interleaved = (uint8_t)non_interleaved;
            so.jpeg_precision = (uint8_t)precision;
            GIMG_Save_Report rep = {};
            save_result[k] = gimg_doc_save(doc, os, "jpeg", &so, &rep);
            if (save_result[k] != GIMG_OK) {
              // A combination the writer does not offer is a refusal, not a
              // failure - but it must not depend on the restart interval,
              // which changes nothing about what the frame is.
              EXPECT_EQ(save_result[k], save_result[0])
                  << "the restart interval decided whether this could be "
                     "written at all";
              gimg_doc_destroy(doc);
              gimg_stream_destroy(os);
              continue;
            }
            ASSERT_EQ(save_result[0], GIMG_OK)
                << "interval 0 was refused and interval " << intervals[k]
                << " was not";
            const void * buf = nullptr;
            size_t bn = 0;
            gimg_stream_output_buffer(os, &buf, &bn);
            std::vector<uint8_t> written(
                (const uint8_t *)buf, (const uint8_t *)buf + bn);
            gimg_doc_destroy(doc);
            gimg_stream_destroy(os);

            // An interval must actually put RST markers in the file, or the
            // equality below would hold for the dullest reason.
            if (intervals[k] != 0) {
              bool saw_rst = false;
              for (size_t i = 0; i + 1 < written.size(); i++) {
                if (written[i] == 0xFF && written[i + 1] >= 0xD0 &&
                    written[i + 1] <= 0xD7) {
                  saw_rst = true;
                  break;
                }
              }
              EXPECT_TRUE(saw_rst) << "no RST marker for interval "
                                   << intervals[k];
            }

            DocStreamGuard in;
            ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(),
                          &in.s),
                GIMG_OK);
            ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
            RasterGuard got;
            ASSERT_EQ(gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
                GIMG_OK);
            ASSERT_NE(got.r, nullptr);
            const unsigned char * gp =
                (const unsigned char *)gimg_raster_pixels(got.r);
            size_t gs = gimg_raster_stride_bytes(got.r);
            const size_t row_bytes = (size_t)w *
                gimg_raster_bytes_per_pixel(gimg_raster_format(got.r));
            decoded[k].resize(row_bytes * h);
            for (uint32_t y = 0; y < h; y++) {
              memcpy(decoded[k].data() + (size_t)y * row_bytes, gp + y * gs,
                  row_bytes);
            }
            combinations++;
          }
          for (int k = 1; k < 4; k++) {
            if (save_result[k] != GIMG_OK) { continue; }
            EXPECT_TRUE(decoded[k] == decoded[0])
                << "restart interval " << intervals[k]
                << " changed the picture, and it only changes where the "
                   "entropy coder resets";
          }
        }
      }
    }
  }
  }
  // The alarm on the sweep: if the writer starts refusing a whole precision
  // this would otherwise pass by comparing nothing at all.
  ASSERT_GT(combinations, 40)
      << "only " << combinations << " combinations were written, so most of "
         "this sweep refused rather than ran";
}

// A hierarchical sequence's components are counted by B.2.2 like any other
// frame's, so a CMYK pyramid is legal in all three processes.  The encoder
// built one and three; four fell out of the raster's channel count and then
// failed at the differential coefficient walk, which was still capped at
// three components of its own.
//
// The lossless form is the one that can be checked without an oracle: it must
// return the original bit for bit.
TEST(JpegEncode, FourComponentHierarchicalSequences) {
  const uint32_t w = 41, h = 27;
  for (int levels = 1; levels <= 3; levels++) {
    for (int progressive = 0; progressive <= 1; progressive++) {
      for (int arithmetic = 0; arithmetic <= 1; arithmetic++) {
        for (int psv : {0, 1}) {
          if (psv && progressive) {
            continue; // one process per sequence (B.3.1)
          }
          SCOPED_TRACE("levels " + std::to_string(levels) + ", progressive " +
              std::to_string(progressive) + ", arithmetic " +
              std::to_string(arithmetic) + ", predictor " +
              std::to_string(psv));
          GIMG_Doc * doc = nullptr;
          ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
          GIMG_Raster * raster = nullptr;
          ASSERT_EQ(gimg_raster_create(w, h, &GIMG_PIXEL_CMYK8,
                        GIMG_RASTER_OWNED, NULL, 0, &raster),
              GIMG_OK);
          unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
          size_t stride = gimg_raster_stride_bytes(raster);
          for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
              for (int c = 0; c < 4; c++) {
                px[y * stride + x * 4 + (size_t)c] = (unsigned char)(
                    (x * (5u + (unsigned)c * 7u) +
                        y * (3u + (unsigned)c * 11u) + (unsigned)c * 23u) &
                    0xFFu);
              }
            }
          }
          gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
          GIMG_Stream * os = nullptr;
          ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
          GIMG_Save_Options so = {};
          so.quality = 90;
          so.jpeg_hierarchical_levels = (uint8_t)levels;
          so.jpeg_progressive = (uint8_t)progressive;
          so.jpeg_arithmetic = (uint8_t)arithmetic;
          so.jpeg_lossless_predictor = (uint8_t)psv;
          GIMG_Save_Report rep = {};
          ASSERT_EQ(gimg_doc_save(doc, os, "jpeg", &so, &rep), GIMG_OK)
              << "a four-component sequence is legal in every process";
          const void * buf = nullptr;
          size_t bn = 0;
          gimg_stream_output_buffer(os, &buf, &bn);
          std::vector<uint8_t> written(
              (const uint8_t *)buf, (const uint8_t *)buf + bn);
          gimg_doc_destroy(doc);
          gimg_stream_destroy(os);

          DocStreamGuard in;
          ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(),
                        &in.s),
              GIMG_OK);
          ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
          RasterGuard got;
          ASSERT_EQ(
              gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
              GIMG_OK);
          ASSERT_NE(got.r, nullptr);
          const GIMG_Pixel_Format * gf = gimg_raster_format(got.r);
          EXPECT_EQ(gf->channel_model, GIMG_CHANNEL_CMYK);
          ASSERT_EQ(gimg_raster_width(got.r), w);
          ASSERT_EQ(gimg_raster_height(got.r), h);
          const unsigned char * gp =
              (const unsigned char *)gimg_raster_pixels(got.r);
          size_t gs = gimg_raster_stride_bytes(got.r);
          int worst = 0;
          for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
              for (int c = 0; c < 4; c++) {
                int want = (int)((x * (5u + (unsigned)c * 7u) +
                                     y * (3u + (unsigned)c * 11u) +
                                     (unsigned)c * 23u) &
                    0xFFu);
                int d = (int)gp[y * gs + x * 4 + (size_t)c] - want;
                if (d < 0) {
                  d = -d;
                }
                if (d > worst) {
                  worst = d;
                }
              }
            }
          }
          if (psv) {
            EXPECT_EQ(worst, 0) << "a lossless pyramid returns the original";
          }
          else {
            EXPECT_LE(worst, 60) << "the pyramid's own loss, and no more";
          }
        }
      }
    }
  }
}

// A sequence wider than one scan can name is refused rather than half-written:
// every frame of it would have to be split, in three entropy coders and two
// processes, and there is no other implementation of Annex J here to check a
// wide sequence against - libjpeg has no hierarchical mode and the reference
// codec does not take one this wide.  A round trip through this library alone
// cannot tell a private misreading from a correct one.
// The writer and the reader must agree on which component counts exist.  They
// did not: gimg_jpeg_encode_hierarchical offers every count up to
// GIMG_JPEG_MAX_SCAN_COMPONENTS, but hier_emit_raster admitted one, three and
// four and refused everything else, so a two-component raster - which has no
// color model and is written as plain Nf=2, legal under B.2.2 with nothing in
// Annex J to forbid it - was saved successfully and then refused by this same
// library at decode.
//
// The assertion is deliberately not "two components work".  It is that every
// count the writer accepts, the reader reads back, which is the property that
// was broken and which stays meaningful if either end's limit moves.  Counts
// the writer refuses are skipped rather than demanded: what they are is
// HierarchicalRefusesASequenceWiderThanAScan's question, not this one.
TEST(JpegEncode, EveryHierarchicalWidthTheWriterAcceptsIsReadBack) {
  const uint32_t w = 16, h = 9;
  int wrote = 0, exact = 0;
  for (int nc = 1; nc <= 6; nc++) {
    for (int levels : {1, 2}) {
      for (int lossless : {0, 1}) {
        SCOPED_TRACE("components " + std::to_string(nc) + ", levels " +
            std::to_string(levels) +
            (lossless ? ", lossless" : ", DCT"));
        GIMG_Pixel_Format fmt;
        ASSERT_EQ(gimg_pixel_format_multichannel((uint8_t)nc, 8, &fmt), GIMG_OK);
        GIMG_Doc * doc = nullptr;
        ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
        GIMG_Raster * raster = nullptr;
        ASSERT_EQ(gimg_raster_create(
                      w, h, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
            GIMG_OK);
        const size_t bpp = gimg_raster_bytes_per_pixel(&fmt);
        const size_t stride = gimg_raster_stride_bytes(raster);
        auto * px = (unsigned char *)gimg_raster_pixels(raster);
        // A separate gradient per component, so a component recovered from
        // another one's plane is a different number and not a coincidence.
        std::vector<int> want((size_t)w * h * (size_t)nc);
        for (uint32_t y = 0; y < h; y++) {
          for (uint32_t x = 0; x < w; x++) {
            for (int c = 0; c < nc; c++) {
              const int v = (int)((x * 23u + y * 47u + (unsigned)c * 91u) & 0xFFu);
              want[((size_t)y * w + x) * (size_t)nc + (size_t)c] = v;
              px[y * stride + x * bpp + (size_t)c] = (unsigned char)v;
            }
          }
        }
        gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

        GIMG_Stream * os = nullptr;
        ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
        GIMG_Save_Options so = {};
        so.metadata_policy = GIMG_META_DROP_ALL;
        so.quality = 95;
        so.jpeg_precision = 8;
        so.jpeg_hierarchical_levels = (uint8_t)levels;
        so.jpeg_lossless_predictor = (uint8_t)(lossless ? 1 : 0);
        GIMG_Save_Report rep = {};
        const GIMG_Result sr = gimg_doc_save(doc, os, "jpeg", &so, &rep);
        const void * buf = nullptr;
        size_t bn = 0;
        gimg_stream_output_buffer(os, &buf, &bn);
        const std::vector<uint8_t> file(
            (const uint8_t *)buf, (const uint8_t *)buf + bn);
        gimg_doc_destroy(doc);
        gimg_stream_destroy(os);
        if (sr != GIMG_OK) {
          // Wider than a scan: refused on purpose, and not this test's subject.
          ASSERT_EQ(sr, GIMG_ERR_UNSUPPORTED);
          ASSERT_GT(nc, (int)GIMG_JPEG_MAX_SCAN_COMPONENTS)
              << "the writer refused a width it is supposed to offer";
          continue;
        }
        wrote++;

        DocStreamGuard in;
        ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in.s),
            GIMG_OK);
        ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK)
            << "the library would not load a file it had just written";
        RasterGuard got;
        ASSERT_EQ(gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
            GIMG_OK)
            << "the library would not decode a file it had just written";
        ASSERT_NE(got.r, nullptr);
        EXPECT_EQ(gimg_raster_width(got.r), w);
        EXPECT_EQ(gimg_raster_height(got.r), h);

        // Three components come back as RGBA and four as CMYK; the rest carry
        // no convention and keep their count.  Either way the first nc
        // channels are the ones that went in.
        const GIMG_Pixel_Format * gf = gimg_raster_format(got.r);
        ASSERT_GE((int)gf->channel_count, nc);
        const auto * gp = (const unsigned char *)gimg_raster_pixels_const(got.r);
        const size_t gs = gimg_raster_stride_bytes(got.r);
        const size_t gbpp = gimg_raster_bytes_per_pixel(gf);
        int worst = 0;
        for (uint32_t y = 0; y < h; y++) {
          for (uint32_t x = 0; x < w; x++) {
            for (int c = 0; c < nc; c++) {
              const int v = (int)gp[y * gs + x * gbpp + (size_t)c];
              const int e =
                  v - want[((size_t)y * w + x) * (size_t)nc + (size_t)c];
              worst = std::max(worst, e < 0 ? -e : e);
            }
          }
        }
        if (lossless) {
          // Annex H reconstructs exactly, and a hierarchical sequence of
          // lossless frames sums differentials that are themselves exact.
          EXPECT_EQ(worst, 0) << "a lossless sequence lost a sample";
          exact++;
        }
        else {
          EXPECT_LE(worst, 24) << "worst channel difference " << worst;
        }
      }
    }
  }
  // 4 widths x 2 depths x 2 processes written; 5 and 6 refused at the writer.
  EXPECT_EQ(wrote, 16);
  EXPECT_EQ(exact, 8);
}

TEST(JpegEncode, HierarchicalRefusesASequenceWiderThanAScan) {
  for (int n : {5, 8}) {
    SCOPED_TRACE("channels " + std::to_string(n));
    GIMG_Pixel_Format fmt;
    ASSERT_EQ(gimg_pixel_format_multichannel((uint8_t)n, 8, &fmt), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_raster_create(16, 16, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
        GIMG_OK);
    memset(gimg_raster_pixels(raster), 0x40,
        gimg_raster_stride_bytes(raster) * 16);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
    GIMG_Stream * os = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
    GIMG_Save_Options so = {};
    so.quality = 85;
    so.jpeg_hierarchical_levels = 1;
    GIMG_Save_Report rep = {};
    EXPECT_EQ(gimg_doc_save(doc, os, "jpeg", &so, &rep), GIMG_ERR_UNSUPPORTED);
    gimg_stream_destroy(os);
    gimg_doc_destroy(doc);
  }
}

// T.81 Annex H has no color concept of its own and B.2.2 counts components
// from 1 to 255, so a lossless frame of four is as legal as one of three and a
// lossless CMYK file is an ordinary thing in prepress.  This codec refused
// anything but one or three, on both sides: "CMYK lossless is not handled
// here".
//
// Lossless is a claim about the pixels, so the test is equality and needs no
// oracle.  B.2.3 Table B.3 still caps Ns at 4, so a frame wider than that is
// written as one scan per component, each with the Huffman table its own
// differences generated - which is legal because B.2.4.2 lets a table at the
// same destination stand until redefined.
TEST(JpegEncode, LosslessFramesOfAnyComponentCountAreExact) {
  const int counts[] = {1, 2, 3, 4, 5, 8, 16};
  const uint32_t w = 29, h = 19;
  for (int n : counts) {
    for (int bits : {8, 16}) {
      for (int psv : {1, 4, 7}) {
        for (int arithmetic = 0; arithmetic <= 1; arithmetic++) {
          SCOPED_TRACE("channels " + std::to_string(n) + ", bits " +
              std::to_string(bits) + ", predictor " + std::to_string(psv) +
              ", arithmetic " + std::to_string(arithmetic));
          GIMG_Pixel_Format fmt;
          if (n == 4 && bits == 8) {
            fmt = GIMG_PIXEL_CMYK8;
          }
          else if (n == 4) {
            fmt = GIMG_PIXEL_CMYK16;
          }
          else {
            ASSERT_EQ(gimg_pixel_format_multichannel(
                          (uint8_t)n, (uint8_t)bits, &fmt),
                GIMG_OK);
          }
          GIMG_Doc * doc = nullptr;
          ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
          GIMG_Raster * raster = nullptr;
          ASSERT_EQ(gimg_raster_create(
                        w, h, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
              GIMG_OK);
          unsigned char * p8 = (unsigned char *)gimg_raster_pixels(raster);
          uint16_t * p16 = (uint16_t *)p8;
          size_t st = gimg_raster_stride_bytes(raster);
          size_t st16 = st / sizeof(uint16_t);
          for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
              for (int c = 0; c < n; c++) {
                unsigned v = x * (7u + (unsigned)c * 13u) +
                    y * (3u + (unsigned)c * 29u) + (unsigned)c * 41u;
                if (bits == 8) {
                  p8[y * st + (size_t)x * (size_t)n + (size_t)c] =
                      (unsigned char)(v & 0xFFu);
                }
                else {
                  p16[y * st16 + (size_t)x * (size_t)n + (size_t)c] =
                      (uint16_t)(v & 0xFFFFu);
                }
              }
            }
          }
          gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
          GIMG_Stream * os = nullptr;
          ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
          GIMG_Save_Options so = {};
          so.metadata_policy = GIMG_META_PRESERVE_ALL;
          so.jpeg_lossless_predictor = (uint8_t)psv;
          so.jpeg_arithmetic = (uint8_t)arithmetic;
          GIMG_Save_Report rep = {};
          ASSERT_EQ(gimg_doc_save(doc, os, "jpeg", &so, &rep), GIMG_OK)
              << "a lossless frame of " << n << " components is legal";
          const void * buf = nullptr;
          size_t bn = 0;
          gimg_stream_output_buffer(os, &buf, &bn);
          std::vector<uint8_t> written(
              (const uint8_t *)buf, (const uint8_t *)buf + bn);

          // No scan may name more than four components, whatever Nf is.
          size_t i = 2;
          int widest_ns = 0;
          while (i + 4 <= written.size() && written[i] == 0xFF) {
            uint8_t m = written[i + 1];
            if (m == 0xD9) {
              break;
            }
            size_t len = (size_t)((written[i + 2] << 8) | written[i + 3]);
            if (m == 0xDA && written[i + 4] > widest_ns) {
              widest_ns = written[i + 4];
            }
            i += 2 + len;
            if (m == 0xDA) {
              size_t j = i;
              while (j + 1 < written.size()) {
                if (written[j] == 0xFF && written[j + 1] != 0 &&
                    !(written[j + 1] >= 0xD0 && written[j + 1] <= 0xD7)) {
                  break;
                }
                j++;
              }
              i = j;
            }
          }
          EXPECT_LE(widest_ns, 4) << "T.81 B.2.3 Table B.3 caps Ns at 4";

          DocStreamGuard in;
          ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(),
                        &in.s),
              GIMG_OK);
          ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
          RasterGuard got;
          ASSERT_EQ(
              gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
              GIMG_OK);
          ASSERT_NE(got.r, nullptr);
          const GIMG_Pixel_Format * gf = gimg_raster_format(got.r);
          int oc = (int)gf->channel_count;
          int ob = (int)gf->bits_per_channel[0];
          const unsigned char * q8 =
              (const unsigned char *)gimg_raster_pixels(got.r);
          const uint16_t * q16 = (const uint16_t *)q8;
          size_t gs = gimg_raster_stride_bytes(got.r);
          size_t gs16 = gs / sizeof(uint16_t);
          for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
              for (int c = 0; c < n && c < oc; c++) {
                long want = (bits == 8)
                    ? (long)p8[y * st + (size_t)x * (size_t)n + (size_t)c]
                    : (long)p16[y * st16 + (size_t)x * (size_t)n + (size_t)c];
                long gotv = (ob == 8)
                    ? (long)q8[y * gs + (size_t)x * (size_t)oc + (size_t)c]
                    : (long)q16[y * gs16 + (size_t)x * (size_t)oc + (size_t)c];
                ASSERT_EQ(gotv, want)
                    << "lossless must be lossless: pixel (" << x << "," << y
                    << ") channel " << c;
              }
            }
          }
          gimg_doc_destroy(doc);
          gimg_stream_destroy(os);
        }
      }
    }
  }
}

// A twelve-bit four-component frame decodes to GIMG_PIXEL_CMYK16, so it has to
// be writable from one: a picture that loads and cannot be saved back is the
// same gap this work set out to close, and adding the decode would otherwise
// have opened it again one precision along.  The same goes for any other
// component count, since T.81 Table B.2 and B.2.2 are independent.
//
// At quality 100 with no subsampling the DCT rounding is all that is left, and
// on samples this smooth it is nothing: the round trip is exact, which is a
// stronger statement than a tolerance and needs no oracle.
TEST(JpegEncode, TwelveBitFramesOfAnyComponentCount) {
  const int counts[] = {1, 3, 4, 5, 8};
  for (int n : counts) {
    for (int progressive = 0; progressive <= 1; progressive++) {
      SCOPED_TRACE(
          "channels " + std::to_string(n) + ", progressive " +
          std::to_string(progressive));
      GIMG_Pixel_Format fmt;
      if (n == 4) {
        fmt = GIMG_PIXEL_CMYK16;
      }
      else {
        ASSERT_EQ(gimg_pixel_format_multichannel((uint8_t)n, 16, &fmt),
            GIMG_OK);
      }
      GIMG_Doc * doc = nullptr;
      ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
      GIMG_Raster * raster = nullptr;
      ASSERT_EQ(gimg_raster_create(
                    33, 17, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
          GIMG_OK);
      uint16_t * px = (uint16_t *)gimg_raster_pixels(raster);
      size_t st = gimg_raster_stride_bytes(raster) / sizeof(uint16_t);
      for (uint32_t y = 0; y < 17u; y++) {
        for (uint32_t x = 0; x < 33u; x++) {
          for (int c = 0; c < n; c++) {
            int v = (int)((x * 3u + y * 5u) * 8u) + (c * 37) % 512 + 256;
            if (v > 4095) {
              v = 4095;
            }
            // Twelve bits left-justified into sixteen, as the decoder returns
            // them (gimg_bitdepth_12_to_16).
            px[y * st + (size_t)x * (size_t)n + (size_t)c] =
                (uint16_t)((v << 4) | (v >> 8));
          }
        }
      }
      gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
      GIMG_Stream * os = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
      GIMG_Save_Options so = {};
      so.quality = 100;
      so.jpeg_precision = 12;
      so.jpeg_progressive = (uint8_t)progressive;
      GIMG_Save_Report rep = {};
      ASSERT_EQ(gimg_doc_save(doc, os, "jpeg", &so, &rep), GIMG_OK)
          << "a twelve-bit frame of " << n << " components is legal";
      const void * buf = nullptr;
      size_t bn = 0;
      gimg_stream_output_buffer(os, &buf, &bn);
      std::vector<uint8_t> written(
          (const uint8_t *)buf, (const uint8_t *)buf + bn);

      DocStreamGuard in;
      ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(),
                    &in.s),
          GIMG_OK);
      ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
      RasterGuard got;
      ASSERT_EQ(
          gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
      ASSERT_NE(got.r, nullptr);
      const GIMG_Pixel_Format * gf = gimg_raster_format(got.r);
      EXPECT_EQ((int)gf->bits_per_channel[0], 16)
          << "a twelve-bit frame decodes to sixteen, left-justified";
      const uint16_t * gp = (const uint16_t *)gimg_raster_pixels(got.r);
      size_t gs = gimg_raster_stride_bytes(got.r) / sizeof(uint16_t);
      int oc = (int)gf->channel_count;
      int worst = 0;
      for (uint32_t y = 0; y < 17u; y++) {
        for (uint32_t x = 0; x < 33u; x++) {
          for (int c = 0; c < n && c < oc; c++) {
            int want = (int)px[y * st + (size_t)x * (size_t)n + (size_t)c];
            int d = (int)gp[y * gs + (size_t)x * (size_t)oc + (size_t)c] - want;
            if (d < 0) {
              d = -d;
            }
            if (d > worst) {
              worst = d;
            }
          }
        }
      }
      EXPECT_EQ(worst, 0) << "quality 100, 4:4:4, smooth samples: nothing to "
                             "lose, so nothing may be lost";
      gimg_doc_destroy(doc);
      gimg_stream_destroy(os);
    }
  }
}

// The other half of T.81 B.4: writing the pair.  A tables stream and an
// abbreviated image saved with the same options belong together, and reading
// them together must give exactly what the complete file gives - not nearly,
// exactly, because the same tables and the same coefficients are involved
// either way and nothing is requantized between them.
//
// The two halves are also checked apart: neither is a JPEG on its own.
TEST(JpegEncode, AbbreviatedStreamsOfB4) {
  for (int progressive = 0; progressive <= 1; progressive++) {
    for (int arithmetic = 0; arithmetic <= 1; arithmetic++) {
      SCOPED_TRACE("progressive " + std::to_string(progressive) +
          ", arithmetic " + std::to_string(arithmetic));
      std::vector<uint8_t> out[3];
      for (int mode = 0; mode < 3; mode++) {
        GIMG_Doc * doc = nullptr;
        ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
        GIMG_Raster * raster = nullptr;
        ASSERT_EQ(gimg_raster_create(33, 17, &GIMG_PIXEL_RGBA8,
                      GIMG_RASTER_OWNED, NULL, 0, &raster),
            GIMG_OK);
        unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
        size_t stride = gimg_raster_stride_bytes(raster);
        for (uint32_t y = 0; y < 17u; y++) {
          for (uint32_t x = 0; x < 33u; x++) {
            unsigned char * p = px + y * stride + x * 4;
            p[0] = (unsigned char)((x * 5 + y * 3) & 0xFF);
            p[1] = (unsigned char)((x * 2 + y * 7) & 0xFF);
            p[2] = (unsigned char)((x * 9 + y * 11) & 0xFF);
            p[3] = 255;
          }
        }
        gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
        GIMG_Stream * os = nullptr;
        ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
        GIMG_Save_Options so = {};
        so.metadata_policy = GIMG_META_DROP_ALL;
        so.quality = 88;
        so.jpeg_abbreviated = (uint8_t)mode;
        so.jpeg_progressive = (uint8_t)progressive;
        so.jpeg_arithmetic = (uint8_t)arithmetic;
        GIMG_Save_Report rep = {};
        ASSERT_EQ(gimg_doc_save(doc, os, "jpeg", &so, &rep), GIMG_OK);
        const void * buf = nullptr;
        size_t bn = 0;
        gimg_stream_output_buffer(os, &buf, &bn);
        out[mode].assign((const uint8_t *)buf, (const uint8_t *)buf + bn);
        gimg_doc_destroy(doc);
        gimg_stream_destroy(os);
      }
      const std::vector<uint8_t> & full = out[0];
      const std::vector<uint8_t> & image = out[1];
      const std::vector<uint8_t> & tabs = out[2];
      EXPECT_LT(image.size(), full.size())
          << "the abbreviated image must be the smaller of the two";
      // A table-specification stream has no frame header and no scan.
      for (size_t i = 2; i + 4 <= tabs.size() && tabs[i] == 0xFF;) {
        uint8_t m = tabs[i + 1];
        if (m == 0xD9) {
          break;
        }
        EXPECT_FALSE(m == 0xDA || (m >= 0xC0 && m <= 0xCF && m != 0xC4 &&
                                      m != 0xC8 && m != 0xCC))
            << "a table-specification stream carries no frame (T.81 B.4)";
        i += 2 + (size_t)((tabs[i + 2] << 8) | tabs[i + 3]);
      }

      // Neither half on its own.
      {
        GIMG_Stream * s = nullptr;
        ASSERT_EQ(gimg_stream_create_memory(tabs.data(), tabs.size(), &s),
            GIMG_OK);
        GIMG_Doc * d = nullptr;
        EXPECT_NE(gimg_doc_load(s, nullptr, nullptr, &d), GIMG_OK);
        gimg_stream_destroy(s);
      }

      GIMG_Stream * ts = nullptr;
      ASSERT_EQ(
          gimg_stream_create_memory(tabs.data(), tabs.size(), &ts), GIMG_OK);
      GIMG_JPEG_Tables * tables = nullptr;
      ASSERT_EQ(gimg_jpeg_tables_load(ts, &tables), GIMG_OK);
      gimg_stream_destroy(ts);

      GIMG_Load_Options lo = {};
      lo.jpeg_tables = tables;
      DocStreamGuard ab;
      ASSERT_EQ(
          gimg_stream_create_memory(image.data(), image.size(), &ab.s),
          GIMG_OK);
      ASSERT_EQ(gimg_doc_load(ab.s, &lo, nullptr, &ab.d), GIMG_OK);
      RasterGuard got;
      ASSERT_EQ(
          gimg_item_decode(gimg_doc_item(ab.d, 0), nullptr, &got.r), GIMG_OK);
      ASSERT_NE(got.r, nullptr);

      DocStreamGuard fl;
      ASSERT_EQ(
          gimg_stream_create_memory(full.data(), full.size(), &fl.s), GIMG_OK);
      ASSERT_EQ(gimg_doc_load(fl.s, nullptr, nullptr, &fl.d), GIMG_OK);
      RasterGuard want;
      ASSERT_EQ(
          gimg_item_decode(gimg_doc_item(fl.d, 0), nullptr, &want.r), GIMG_OK);
      ASSERT_NE(want.r, nullptr);
      EXPECT_TRUE(jpeg_test::rasters_equal(got.r, want.r))
          << "the abbreviated pair and the complete file are the same image: "
          << jpeg_test::raster_first_diff(got.r, want.r);
      gimg_jpeg_tables_destroy(tables);
    }
  }
}

// B.4's formats need tables that can be written before the frame.  A
// hierarchical sequence's frames carry their own (B.3.1), and a lossless
// frame's Huffman table is generated from the very coefficients it codes, so
// neither can be split that way; both are refused rather than half-written.
TEST(JpegEncode, AbbreviatedRefusesWhatCannotBeSplit) {
  struct Case {
    uint8_t abbreviated;
    uint8_t hierarchical;
    uint8_t lossless;
    const char * what;
  };
  const Case cases[] = {
      {1, 1, 0, "abbreviated image of a hierarchical sequence"},
      {2, 1, 0, "tables of a hierarchical sequence"},
      {1, 0, 3, "abbreviated image of a lossless frame"},
      {2, 0, 3, "tables of a lossless frame"},
      {3, 0, 0, "an abbreviation that is neither of B.4's two"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                  NULL, 0, &raster),
        GIMG_OK);
    memset(gimg_raster_pixels(raster), 0x40,
        gimg_raster_stride_bytes(raster) * 16);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
    GIMG_Stream * os = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
    GIMG_Save_Options so = {};
    so.quality = 85;
    so.jpeg_abbreviated = c.abbreviated;
    so.jpeg_hierarchical_levels = c.hierarchical;
    so.jpeg_lossless_predictor = c.lossless;
    GIMG_Save_Report rep = {};
    EXPECT_EQ(gimg_doc_save(doc, os, "jpeg", &so, &rep), GIMG_ERR_UNSUPPORTED);
    gimg_stream_destroy(os);
    gimg_doc_destroy(doc);
  }
}

// T.81 B.2.2 lets a frame carry from 1 to 255 components; B.2.3 caps one scan
// at 4, so a frame wider than that has exactly one legal arrangement - several
// non-interleaved scans (A.2.3) - and the encoder writes it that way whether
// or not the caller asked, because there is no other way to honor the
// request.
//
// There is no oracle for the whole file: libjpeg's decoder matches a scan's Cs
// against only the first four components of the frame (jdmarker.c get_sos), so
// it cannot read one back.  It can read each scan on its own, though, and that
// is what JpegLoad.FramesWiderThanOneScanCanName rests on; here the check is
// the round trip, which is what a caller of this library actually gets.  The
// error bound is the quantizer's: at quality 100 the DCT rounding is all that
// is left.
TEST(JpegEncode, FramesWiderThanOneScanCanNameAreWritten) {
  const int counts[] = {2, 5, 8, 10, 32, 255};
  for (int n : counts) {
    SCOPED_TRACE("Nf = " + std::to_string(n));
    GIMG_Pixel_Format fmt;
    ASSERT_EQ(gimg_pixel_format_multichannel((uint8_t)n, 8, &fmt), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(
                  17, 9, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
        GIMG_OK);
    unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < 9u; y++) {
      for (uint32_t x = 0; x < 17u; x++) {
        for (int c = 0; c < n; c++) {
          // Smooth in x and y so the quantizer has little to do, and offset
          // per component so a mix-up between two of them shows.
          int v = (int)(x * 3u + y * 5u) + (c * 37) % 96 + 32;
          px[y * stride + (size_t)x * (size_t)n + (size_t)c] =
              (unsigned char)(v > 255 ? 255 : v);
        }
      }
    }
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
    GIMG_Stream * out_stream = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.metadata_policy = GIMG_META_PRESERVE_ALL;
    opts.quality = 100;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out_stream, "jpeg", &opts, &report), GIMG_OK)
        << "a frame of " << n << " components is legal and must be writable";
    const void * buf = nullptr;
    size_t bn = 0;
    gimg_stream_output_buffer(out_stream, &buf, &bn);
    std::vector<uint8_t> written(
        (const uint8_t *)buf, (const uint8_t *)buf + bn);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(out_stream);

    // Every scan names one component when the frame is wider than B.2.3's Ns.
    int scans = 0;
    int sof_components = 0;
    int widest_ns = 0;
    size_t i = 2;
    while (i + 4 <= written.size() && written[i] == 0xFF) {
      uint8_t m = written[i + 1];
      if (m == 0xD9) {
        break;
      }
      size_t len = (size_t)((written[i + 2] << 8) | written[i + 3]);
      const uint8_t * pay = written.data() + i + 4;
      if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
        sof_components = pay[5];
      }
      if (m == 0xDA) {
        scans++;
        if (pay[0] > widest_ns) {
          widest_ns = pay[0];
        }
      }
      i += 2 + len;
      if (m == 0xDA) {
        size_t j = i;
        while (j + 1 < written.size()) {
          if (written[j] == 0xFF && written[j + 1] != 0 &&
              !(written[j + 1] >= 0xD0 && written[j + 1] <= 0xD7)) {
            break;
          }
          j++;
        }
        i = j;
      }
    }
    EXPECT_EQ(sof_components, n);
    EXPECT_LE(widest_ns, 4) << "T.81 B.2.3 Table B.3 caps Ns at 4";
    if (n > 4) {
      EXPECT_EQ(scans, n) << "a wide frame is one scan per component (A.2.3)";
    }

    DocStreamGuard in;
    ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(), &in.s),
        GIMG_OK);
    ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
    RasterGuard got;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
    ASSERT_NE(got.r, nullptr);
    const GIMG_Pixel_Format * gf = gimg_raster_format(got.r);
    ASSERT_EQ((int)gf->channel_count, n);
    ASSERT_EQ(gimg_raster_width(got.r), 17u);
    ASSERT_EQ(gimg_raster_height(got.r), 9u);
    const unsigned char * gp = (const unsigned char *)gimg_raster_pixels(got.r);
    size_t gs = gimg_raster_stride_bytes(got.r);
    int worst = 0;
    for (uint32_t y = 0; y < 9u; y++) {
      for (uint32_t x = 0; x < 17u; x++) {
        for (int c = 0; c < n; c++) {
          int want = (int)(x * 3u + y * 5u) + (c * 37) % 96 + 32;
          if (want > 255) {
            want = 255;
          }
          int d = (int)gp[y * gs + (size_t)x * (size_t)n + (size_t)c] - want;
          if (d < 0) {
            d = -d;
          }
          if (d > worst) {
            worst = d;
          }
        }
      }
    }
    EXPECT_LE(worst, 3) << "components must not be crossed or mis-shaped";
  }
}

// The same frames again through the progressive process.  Annex G's AC scans
// are one component each already (G.1.2.2); what a wide frame adds is that its
// DC scan has to split too, because B.2.3 caps Ns at 4 in every scan and not
// only in an AC one.
TEST(JpegEncode, WideFramesProgressiveAndArithmetic) {
  const int counts[] = {5, 10, 32};
  for (int n : counts) {
    for (int progressive = 0; progressive <= 1; progressive++) {
      for (int arithmetic = 0; arithmetic <= 1; arithmetic++) {
        SCOPED_TRACE("Nf = " + std::to_string(n) + ", progressive " +
            std::to_string(progressive) + ", arithmetic " +
            std::to_string(arithmetic));
        GIMG_Pixel_Format fmt;
        ASSERT_EQ(gimg_pixel_format_multichannel((uint8_t)n, 8, &fmt), GIMG_OK);
        GIMG_Doc * doc = nullptr;
        ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
        GIMG_Raster * raster = nullptr;
        ASSERT_EQ(gimg_raster_create(
                      17, 9, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
            GIMG_OK);
        unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
        size_t stride = gimg_raster_stride_bytes(raster);
        for (uint32_t y = 0; y < 9u; y++) {
          for (uint32_t x = 0; x < 17u; x++) {
            for (int c = 0; c < n; c++) {
              int v = (int)(x * 3u + y * 5u) + (c * 37) % 96 + 32;
              px[y * stride + (size_t)x * (size_t)n + (size_t)c] =
                  (unsigned char)(v > 255 ? 255 : v);
            }
          }
        }
        gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
        GIMG_Stream * out_stream = nullptr;
        ASSERT_EQ(gimg_stream_create_memory_output(&out_stream), GIMG_OK);
        GIMG_Save_Options opts = {};
        opts.metadata_policy = GIMG_META_PRESERVE_ALL;
        opts.quality = 100;
        opts.jpeg_progressive = (uint8_t)progressive;
        opts.jpeg_arithmetic = (uint8_t)arithmetic;
        GIMG_Save_Report report = {};
        ASSERT_EQ(
            gimg_doc_save(doc, out_stream, "jpeg", &opts, &report), GIMG_OK);
        const void * buf = nullptr;
        size_t bn = 0;
        gimg_stream_output_buffer(out_stream, &buf, &bn);
        std::vector<uint8_t> written(
            (const uint8_t *)buf, (const uint8_t *)buf + bn);
        gimg_doc_destroy(doc);
        gimg_stream_destroy(out_stream);

        // The frame header must say which process was actually used - a
        // progressive request that quietly produced a sequential file is the
        // failure this pins.
        int sof = 0;
        int widest_ns = 0;
        size_t i = 2;
        while (i + 4 <= written.size() && written[i] == 0xFF) {
          uint8_t m = written[i + 1];
          if (m == 0xD9) {
            break;
          }
          size_t len = (size_t)((written[i + 2] << 8) | written[i + 3]);
          if ((m >= 0xC0 && m <= 0xCF) && m != 0xC4 && m != 0xC8 &&
              m != 0xCC) {
            sof = m;
          }
          if (m == 0xDA && written[i + 4] > widest_ns) {
            widest_ns = written[i + 4];
          }
          i += 2 + len;
          if (m == 0xDA) {
            size_t j = i;
            while (j + 1 < written.size()) {
              if (written[j] == 0xFF && written[j + 1] != 0 &&
                  !(written[j + 1] >= 0xD0 && written[j + 1] <= 0xD7)) {
                break;
              }
              j++;
            }
            i = j;
          }
        }
        int want_sof = progressive ? (arithmetic ? 0xCA : 0xC2)
                                   : (arithmetic ? 0xC9 : 0xC0);
        EXPECT_EQ(sof, want_sof);
        EXPECT_LE(widest_ns, 4) << "T.81 B.2.3 Table B.3 caps Ns at 4";

        DocStreamGuard in;
        ASSERT_EQ(
            gimg_stream_create_memory(written.data(), written.size(), &in.s),
            GIMG_OK);
        ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
        RasterGuard got;
        ASSERT_EQ(gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
            GIMG_OK);
        ASSERT_NE(got.r, nullptr);
        const unsigned char * gp =
            (const unsigned char *)gimg_raster_pixels(got.r);
        size_t gs = gimg_raster_stride_bytes(got.r);
        int worst = 0;
        for (uint32_t y = 0; y < 9u; y++) {
          for (uint32_t x = 0; x < 17u; x++) {
            for (int c = 0; c < n; c++) {
              int want = (int)(x * 3u + y * 5u) + (c * 37) % 96 + 32;
              if (want > 255) {
                want = 255;
              }
              int d =
                  (int)gp[y * gs + (size_t)x * (size_t)n + (size_t)c] - want;
              if (d < 0) {
                d = -d;
              }
              if (d > worst) {
                worst = d;
              }
            }
          }
        }
        EXPECT_LE(worst, 3);
      }
    }
  }
}

// Chroma subsampling has nothing to subsample in a raw CMYK frame.  C, M, Y
// and K are four ink amounts; none of them is a chrominance difference, and
// throwing away half the M samples throws away ink.  libjpeg agrees - its
// jpeg_set_colorspace gives all four components 1x1 for JCS_CMYK - so the
// option is ignored here, and a CMYK save produces the same file whatever it
// says.  The default is 4:2:0, so this is the path an ordinary caller takes.
//
// A YCCK frame is the other way round: components 1 and 2 really are
// chrominance, so they subsample and the files differ.
TEST(JpegEncode, SubsamplingAppliesToYcckChrominanceAndNotToCmykInk) {
  std::vector<uint8_t> cmyk444, cmyk422, cmyk420;
  ASSERT_TRUE(save_cmyk_pattern(
      33, 17, 0, GIMG_JPEG_CHROMA_444, 0, 0, 0, 90, cmyk444, nullptr));
  ASSERT_TRUE(save_cmyk_pattern(
      33, 17, 0, GIMG_JPEG_CHROMA_422, 0, 0, 0, 90, cmyk422, nullptr));
  ASSERT_TRUE(save_cmyk_pattern(
      33, 17, 0, GIMG_JPEG_CHROMA_420, 0, 0, 0, 90, cmyk420, nullptr));
  EXPECT_TRUE(cmyk444 == cmyk422)
      << "a raw CMYK frame has no chrominance to subsample";
  EXPECT_TRUE(cmyk444 == cmyk420)
      << "a raw CMYK frame has no chrominance to subsample";

  std::vector<uint8_t> ycck444, ycck422, ycck420;
  ASSERT_TRUE(save_cmyk_pattern(
      33, 17, 2, GIMG_JPEG_CHROMA_444, 0, 0, 0, 90, ycck444, nullptr));
  ASSERT_TRUE(save_cmyk_pattern(
      33, 17, 2, GIMG_JPEG_CHROMA_422, 0, 0, 0, 90, ycck422, nullptr));
  ASSERT_TRUE(save_cmyk_pattern(
      33, 17, 2, GIMG_JPEG_CHROMA_420, 0, 0, 0, 90, ycck420, nullptr));
  EXPECT_FALSE(ycck444 == ycck422) << "YCCK chrominance does subsample";
  EXPECT_FALSE(ycck444 == ycck420) << "YCCK chrominance does subsample";
}

// Only 0 and 2 are transforms this encoder can carry out: 1 is the YCbCr of a
// three-component frame and there is no fourth reading at all.  Naming one of
// those would produce a file whose marker and whose samples disagree.
TEST(JpegEncode, CmykTransformRefusesWhatItCannotMean) {
  for (uint8_t bad : {uint8_t(1), uint8_t(3), uint8_t(255)}) {
    SCOPED_TRACE("transform " + std::to_string((int)bad));
    std::vector<uint8_t> written;
    GIMG_Result r = GIMG_OK;
    EXPECT_FALSE(save_cmyk_pattern(
        16, 16, bad, GIMG_JPEG_CHROMA_444, 0, 0, 0, 90, written, &r));
    EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  }
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

/**
 * A refinement progression decodes to the same picture as a baseline file.
 *
 * The script used to be `{0,0,0,0}`, `{1,63,0,0}`, `{1,63,1,0}`: an initial
 * scan that held nothing back, and then a refinement of a bit that was never
 * withheld. T.81 G.1.1.1.2 has no such file - a scan's Ah must be the Al the
 * previous scan over those coefficients used - and the save now refuses it
 * rather than writing something no decoder can read. The script below holds
 * one bit back and refines it, which is what the old one was meant to say.
 */
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
      {0, 0, 0, 1},  // DC initial, one bit held back
      {1, 63, 0, 1}, // AC initial, one bit held back
      {0, 0, 1, 0},  // DC refinement
      {1, 63, 1, 0}, // AC refinement of the same band
  };
  GIMG_JPEG_Progressive_Config refine_config = {
      .scan_count = 4,
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
  // Finer quantization, more bits.
  EXPECT_GT(size_85, size_50);
  EXPECT_GT(size_100, size_85);
}

/**
 * A flat 12-bit image must come back at the value it went in at, spread across
 * the whole 16-bit output range - not squeezed into a band around mid-gray.
 *
 * This checks sample values, which nothing did before: every 12-bit test
 * asserted dimensions, format and "decode succeeded", so a decoder whose
 * inverse DCT was scaled wrong by a factor of 64 passed them all.  T.81 A.3.3
 * fixes the transform exactly; there is no latitude in it beyond rounding.
 */
/** A flat 12-bit color field must survive a 12-bit round trip.
 *
 * The encoder's 12-bit RGB->YCbCr had coefficients scaled for 8-bit data but a
 * shift of 12 rather than 8, so luminance came out sixteen times too small and
 * every 12-bit color image we wrote was ruined.  Neither this library's own
 * decoder nor libjpeg could reveal that on its own - both read the file back
 * faithfully, and what they read back was faithfully wrong - so the check has
 * to be against the sample that went in.
 *
 * A flat field is used so that chroma subsampling and the DCT are both exact,
 * leaving nothing between the input and the output but the color transform.
 * The tolerance covers the round trip through YCbCr, which is not lossless. */
TEST(JpegEncode, Save12BitColorFlatFieldsKeepTheirValue) {
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
 * the quantizer, as the comment on the guard had assumed.  With the tables
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
  // At quality 100 the quantization values are all 1, so the checkerboard comes
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
        // precision, and must carry no quantization table - there is nothing
        // to quantize.
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
 * writing the same quantized coefficients.  Both encoders here are fed one
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
    // is one quantization step.  Allow 2 steps of the 12-bit quantizer, widened.
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
         "inverse DCT collapses it toward mid-gray";
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
  // Finer quantization, more bits.
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
 * The color case is exact rather than approximate because a NULL
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
      {"libjpeg_restart_gray.jpg", "libjpeg_restart_gray.pgm", "gray, DRI 34"},
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
// Its transform byte says which color space the components are in.  A source
// that carried RGB says transform 0; this encoder writes YCbCr for three
// components, and preserving the marker unchanged left the file asserting both
// at once, alongside a JFIF APP0 that asserts YCbCr a third time.  Nothing here
// decodes it wrongly - JFIF outranks Adobe, for this library and for libjpeg -
// but a decoder that reads Adobe first gets a picture in the wrong colors out
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
// what a decoder will, quantization loss included.  Get that reconstruction
// wrong and the file still decodes - it just decodes to the wrong picture,
// consistently, in every decoder, which is why the round trip below compares
// against the source rather than against another decode.
//
// The first version of this encoder handed its quantization table to the
// decoder's dequantizer, which reads the table in the zigzag order a DQT
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
      // Smooth, so that quantization error is the only thing being measured.
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
// T.81 B.3.1 lets a hierarchical sequence be built out of any of the three
// coding processes, as long as every frame uses the same one.  Only the
// sequential DCT was written here; the gap list called the other two out.
//
// A lossless sequence is the one that can be checked without an oracle at all,
// because "lossless" is a claim about the pixels and nothing else: the encoder
// downsamples, codes each level's difference by Annex H, and reconstructs
// exactly, so the decoded image must be the original bit for bit at every
// level count, both predictors, and either entropy coder.  If the encoder's
// idea of the reconstruction and the decoder's ever part company, this test
// stops being exact - which is the whole failure mode a pyramid has.
TEST(JpegEncode, LosslessHierarchicalSequenceIsExactlyLossless) {
  for (int levels = 1; levels <= 3; levels++) {
    for (int psv : {1, 4, 7}) {
      for (int arithmetic = 0; arithmetic <= 1; arithmetic++) {
        for (int gray = 0; gray <= 1; gray++) {
          SCOPED_TRACE("levels " + std::to_string(levels) + ", predictor " +
              std::to_string(psv) + ", arithmetic " +
              std::to_string(arithmetic) + ", gray " + std::to_string(gray));
          const uint32_t w = 41, h = 27;
          GIMG_Doc * doc = nullptr;
          ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
          GIMG_Raster * raster = nullptr;
          ASSERT_EQ(gimg_raster_create(w, h,
                        gray ? &GIMG_PIXEL_GRAY8 : &GIMG_PIXEL_RGBA8,
                        GIMG_RASTER_OWNED, NULL, 0, &raster),
              GIMG_OK);
          unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
          size_t stride = gimg_raster_stride_bytes(raster);
          size_t bpp = gray ? 1u : 4u;
          for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
              unsigned char * p = px + y * stride + x * bpp;
              p[0] = (unsigned char)((x * 5 + y * 3) & 0xFF);
              if (!gray) {
                p[1] = (unsigned char)((x * 2 + y * 7) & 0xFF);
                p[2] = (unsigned char)((x * 9 + y * 11) & 0xFF);
                p[3] = 255;
              }
            }
          }
          gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
          GIMG_Stream * out = nullptr;
          ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
          GIMG_Save_Options so = {};
          so.jpeg_hierarchical_levels = (uint8_t)levels;
          so.jpeg_lossless_predictor = (uint8_t)psv;
          so.jpeg_arithmetic = (uint8_t)arithmetic;
          GIMG_Save_Report rep = {};
          ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &so, &rep), GIMG_OK);
          const void * buf = nullptr;
          size_t bn = 0;
          gimg_stream_output_buffer(out, &buf, &bn);
          std::vector<uint8_t> written(
              (const uint8_t *)buf, (const uint8_t *)buf + bn);
          gimg_doc_destroy(doc);
          gimg_stream_destroy(out);

          // The frames must be the lossless ones of Table B.1: SOF3 (or SOF11)
          // once, then SOF7 (or SOF15) for each differential level.
          int first_sof = 0, diff_sofs = 0;
          size_t i = 2;
          while (i + 4 <= written.size() && written[i] == 0xFF) {
            uint8_t m = written[i + 1];
            if (m == 0xD9) {
              break;
            }
            size_t len = (size_t)((written[i + 2] << 8) | written[i + 3]);
            if (m == 0xC3 || m == 0xCB) {
              if (!first_sof) {
                first_sof = m;
              }
            }
            if (m == 0xC7 || m == 0xCF) {
              diff_sofs++;
            }
            i += 2 + len;
            if (m == 0xDA) {
              size_t j = i;
              while (j + 1 < written.size()) {
                if (written[j] == 0xFF && written[j + 1] != 0 &&
                    !(written[j + 1] >= 0xD0 && written[j + 1] <= 0xD7)) {
                  break;
                }
                j++;
              }
              i = j;
            }
          }
          EXPECT_EQ(first_sof, arithmetic ? 0xCB : 0xC3);
          EXPECT_EQ(diff_sofs, levels);

          DocStreamGuard in;
          ASSERT_EQ(
              gimg_stream_create_memory(written.data(), written.size(), &in.s),
              GIMG_OK);
          ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
          RasterGuard got;
          ASSERT_EQ(gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
              GIMG_OK);
          ASSERT_NE(got.r, nullptr);
          ASSERT_EQ(gimg_raster_width(got.r), w);
          ASSERT_EQ(gimg_raster_height(got.r), h);
          const unsigned char * gp =
              (const unsigned char *)gimg_raster_pixels(got.r);
          size_t gs = gimg_raster_stride_bytes(got.r);
          size_t gbpp = gimg_raster_bytes_per_pixel(gimg_raster_format(got.r));
          for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
              int nc = gray ? 1 : 3;
              int want[3] = {(int)((x * 5 + y * 3) & 0xFF),
                  (int)((x * 2 + y * 7) & 0xFF),
                  (int)((x * 9 + y * 11) & 0xFF)};
              for (int c = 0; c < nc; c++) {
                ASSERT_EQ((int)gp[y * gs + x * gbpp + (size_t)c], want[c])
                    << "lossless must be lossless: pixel (" << x << "," << y
                    << ") channel " << c;
              }
            }
          }
        }
      }
    }
  }
}

// A progressive sequence carries the same coefficients as a sequential one and
// differs only in the order its scans leave in (Annex G over the frames of
// Annex J), so the two must decode to the *same* picture - not merely to a
// similar one.  That equality is what caught the real fault here: T.81 J.1.3.1
// says a differential frame's DC coefficient "is coded directly - without
// prediction", and the progressive scan encoder was predicting it, so the
// error grew with every level of the pyramid while the file still decoded.
TEST(JpegEncode, ProgressiveHierarchicalSequenceMatchesTheSequentialOne) {
  for (int levels = 1; levels <= 3; levels++) {
    for (int gray = 0; gray <= 1; gray++) {
      SCOPED_TRACE("levels " + std::to_string(levels) + ", gray " +
          std::to_string(gray));
      const uint32_t w = 41, h = 27;
      std::vector<uint8_t> decoded[4];
      // sequential, progressive, and the arithmetic form of each
      const int progressive[4] = {0, 1, 0, 1};
      const int arithmetic[4] = {0, 0, 1, 1};
      const int want_first_sof[4] = {0xC1, 0xC2, 0xC9, 0xCA};
      const int want_diff_sof[4] = {0xC5, 0xC6, 0xCD, 0xCE};
      for (int k = 0; k < 4; k++) {
        GIMG_Doc * doc = nullptr;
        ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
        GIMG_Raster * raster = nullptr;
        ASSERT_EQ(gimg_raster_create(w, h,
                      gray ? &GIMG_PIXEL_GRAY8 : &GIMG_PIXEL_RGBA8,
                      GIMG_RASTER_OWNED, NULL, 0, &raster),
            GIMG_OK);
        unsigned char * px = (unsigned char *)gimg_raster_pixels(raster);
        size_t stride = gimg_raster_stride_bytes(raster);
        size_t bpp = gray ? 1u : 4u;
        for (uint32_t y = 0; y < h; y++) {
          for (uint32_t x = 0; x < w; x++) {
            unsigned char * p = px + y * stride + x * bpp;
            p[0] = (unsigned char)((x * 5 + y * 3) & 0xFF);
            if (!gray) {
              p[1] = (unsigned char)((x * 2 + y * 7) & 0xFF);
              p[2] = (unsigned char)((x * 9 + y * 11) & 0xFF);
              p[3] = 255;
            }
          }
        }
        gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
        GIMG_Stream * out = nullptr;
        ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
        GIMG_Save_Options so = {};
        so.quality = 85;
        so.jpeg_hierarchical_levels = (uint8_t)levels;
        so.jpeg_progressive = (uint8_t)progressive[k];
        so.jpeg_arithmetic = (uint8_t)arithmetic[k];
        GIMG_Save_Report rep = {};
        ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &so, &rep), GIMG_OK);
        const void * buf = nullptr;
        size_t bn = 0;
        gimg_stream_output_buffer(out, &buf, &bn);
        std::vector<uint8_t> written(
            (const uint8_t *)buf, (const uint8_t *)buf + bn);
        gimg_doc_destroy(doc);
        gimg_stream_destroy(out);

        int first_sof = 0, diff_sofs = 0, scans = 0;
        size_t i = 2;
        while (i + 4 <= written.size() && written[i] == 0xFF) {
          uint8_t m = written[i + 1];
          if (m == 0xD9) {
            break;
          }
          size_t len = (size_t)((written[i + 2] << 8) | written[i + 3]);
          if (m == want_first_sof[k] && !first_sof) {
            first_sof = m;
          }
          if (m == want_diff_sof[k]) {
            diff_sofs++;
          }
          if (m == 0xDA) {
            scans++;
          }
          i += 2 + len;
          if (m == 0xDA) {
            size_t j = i;
            while (j + 1 < written.size()) {
              if (written[j] == 0xFF && written[j + 1] != 0 &&
                  !(written[j + 1] >= 0xD0 && written[j + 1] <= 0xD7)) {
                break;
              }
              j++;
            }
            i = j;
          }
        }
        EXPECT_EQ(first_sof, want_first_sof[k]) << "wrong process in the SOF";
        EXPECT_EQ(diff_sofs, levels);
        int comps = gray ? 1 : 3;
        // A progressive frame is a DC scan plus one AC scan per component
        // (G.1.2.2); a sequential frame is one scan.
        EXPECT_EQ(scans,
            progressive[k] ? (levels + 1) * (1 + comps) : (levels + 1));

        DocStreamGuard in;
        ASSERT_EQ(
            gimg_stream_create_memory(written.data(), written.size(), &in.s),
            GIMG_OK);
        ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
        RasterGuard got;
        ASSERT_EQ(gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r),
            GIMG_OK);
        ASSERT_NE(got.r, nullptr);
        ASSERT_EQ(gimg_raster_width(got.r), w);
        ASSERT_EQ(gimg_raster_height(got.r), h);
        const unsigned char * gp =
            (const unsigned char *)gimg_raster_pixels(got.r);
        size_t gs = gimg_raster_stride_bytes(got.r);
        size_t gbpp = gimg_raster_bytes_per_pixel(gimg_raster_format(got.r));
        decoded[k].resize((size_t)w * h * gbpp);
        for (uint32_t y = 0; y < h; y++) {
          memcpy(decoded[k].data() + (size_t)y * w * gbpp, gp + y * gs,
              (size_t)w * gbpp);
        }
      }
      for (int k = 1; k < 4; k++) {
        EXPECT_TRUE(decoded[k] == decoded[0])
            << "the four processes carry the same coefficients and must decode "
               "to the same pixels; only the scan order differs";
      }
    }
  }
}

TEST(JpegEncode, HierarchicalRefusesACombinationItCannotWrite) {
  struct Case {
    uint8_t progressive;
    uint8_t lossless_predictor;
    uint8_t precision;
    const char * what;
  };
  // Progressive and lossless sequences are written now (B.3.1 allows any of
  // the three processes, as long as every frame of a sequence uses the same
  // one), so the only combination left that cannot be honored is a change of
  // precision: a pyramid's reconstruction is built at 8 bits.
  const Case cases[] = {
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


// T.81 A.2.3: this library can write a sequential frame as one non-interleaved
// scan per component, and libjpeg-turbo reads the result.
//
// The oracle is that codec's decode of our file, committed alongside it, for
// the same reason the hierarchical encoder has one: a round trip through our
// own decoder would pass just as happily on a private misreading of A.2.3,
// since the same misunderstanding would be on both sides of it.
//
// The comparison is exact.  Our IDCT is libjpeg's islow and our chroma
// upsampler is its fancy one, so for a file both codecs agree is well formed
// there is nothing left to differ about - and when the first version of this
// writer named Huffman table 1 for the chroma scans while encoding them with
// table 0, this is what said so: libjpeg refused the file outright with "bad
// Huffman code".
TEST(JpegEncode, NonInterleavedOutputIsReadByLibjpegTurbo) {
  struct Case {
    const char * jpg;
    const char * ref;
    int channels;
    const char * what;
  };
  const Case cases[] = {
      {"ni_ours_444.jpg", "ni_ours_444_turbo.ppm", 3, "4:4:4"},
      {"ni_ours_420.jpg", "ni_ours_420_turbo.ppm", 3, "4:2:0"},
      {"ni_ours_422_restart.jpg", "ni_ours_422_restart_turbo.ppm", 3,
          "4:2:2 with a restart interval of 3 blocks"},
      {"ni_ours_arith_420.jpg", "ni_ours_arith_420_turbo.ppm", 3,
          "4:2:0, arithmetic (SOF9)"},
      {"ni_ours_gray.jpg", "ni_ours_gray_turbo.pgm", 1, "grayscale"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    uint32_t rw = 0, rh = 0;
    int rchan = 0, rbits = 0;
    std::vector<uint32_t> ref;
    ASSERT_TRUE(jpeg_test::load_pnm_file(c.ref, &rw, &rh, &rchan, &rbits, ref))
        << "missing reference " << c.ref;
    ASSERT_EQ(rchan, c.channels);
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
    size_t bpp = (c.channels == 3) ? 4u : 1u;
    for (uint32_t y = 0; y < rh; y++) {
      for (uint32_t x = 0; x < rw; x++) {
        for (int ch = 0; ch < c.channels; ch++) {
          int a = (int)gp[y * gs + x * bpp + (size_t)ch];
          int b = (int)ref[((size_t)y * rw + x) * (size_t)c.channels + ch];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
        }
      }
    }
  }
}

// The two scan orders of A.2.2 and A.2.3 describe the same blocks, so the same
// image written both ways must decode to the same pixels - not merely to
// similar ones.  Nothing is requantized between them; only the order the
// coefficients are written in changes.
//
// This also pins the thing the external oracle cannot see, because libjpeg
// only ever gets one of the two files: that the non-interleaved writer is
// reading the same coefficient buffer, and gathering from it correctly for a
// subsampled component, whose block grid is smaller than the MCU grid it lives
// in.
TEST(JpegEncode, NonInterleavedAndInterleavedDecodeToTheSamePixels) {
  struct Case {
    uint8_t subsampling;
    uint8_t arithmetic;
    uint16_t restart;
    const char * what;
  };
  const Case cases[] = {
      {GIMG_JPEG_CHROMA_444, 0, 0, "4:4:4"},
      {GIMG_JPEG_CHROMA_422, 0, 0, "4:2:2"},
      {GIMG_JPEG_CHROMA_420, 0, 0, "4:2:0"},
      {GIMG_JPEG_CHROMA_420, 0, 3, "4:2:0 with restarts"},
      {GIMG_JPEG_CHROMA_420, 1, 0, "4:2:0, arithmetic"},
      {GIMG_JPEG_CHROMA_422, 1, 5, "4:2:2, arithmetic, with restarts"},
  };
  // 37x23 so that neither dimension is a whole number of MCUs at any of the
  // three samplings: the edge blocks are where a wrong gather shows up.
  const uint32_t W = 37, H = 23;
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    std::vector<uint8_t> decoded[2];
    uint32_t dw[2] = {0, 0}, dh[2] = {0, 0};
    for (int pass = 0; pass < 2; pass++) {
      RasterGuard src;
      ASSERT_EQ(gimg_raster_create(W, H, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                    nullptr, 0, &src.r),
          GIMG_OK);
      size_t ss = gimg_raster_stride_bytes(src.r);
      unsigned char * sp = (unsigned char *)gimg_raster_pixels(src.r);
      for (uint32_t y = 0; y < H; y++) {
        for (uint32_t x = 0; x < W; x++) {
          unsigned char * p = sp + y * ss + x * 4;
          p[0] = (unsigned char)(x * 7 + y * 3);
          p[1] = (unsigned char)(y * 11 + 40);
          p[2] = (unsigned char)((x ^ y) * 5);
          p[3] = 0xFF;
        }
      }
      GIMG_Doc * doc = nullptr;
      ASSERT_EQ(gimg_doc_from_raster(src.r, &doc), GIMG_OK);
      GIMG_Stream * os = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
      GIMG_Save_Options o = {};
      o.quality = 88;
      o.jpeg_chroma_subsampling = c.subsampling;
      o.jpeg_arithmetic = c.arithmetic;
      o.jpeg_restart_interval = c.restart;
      o.jpeg_non_interleaved = (uint8_t)pass; // 0 = A.2.2, 1 = A.2.3
      GIMG_Save_Report rep = {};
      ASSERT_EQ(gimg_doc_save(doc, os, "jpeg", &o, &rep), GIMG_OK);
      const void * d = nullptr;
      size_t n = 0;
      gimg_stream_output_buffer(os, &d, &n);
      std::vector<uint8_t> file((const uint8_t *)d, (const uint8_t *)d + n);
      gimg_stream_destroy(os);
      gimg_doc_destroy(doc);

      DocStreamGuard in;
      ASSERT_EQ(
          gimg_stream_create_memory(file.data(), file.size(), &in.s), GIMG_OK);
      ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
      RasterGuard got;
      ASSERT_EQ(
          gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
      dw[pass] = gimg_raster_width(got.r);
      dh[pass] = gimg_raster_height(got.r);
      const unsigned char * gp =
          (const unsigned char *)gimg_raster_pixels(got.r);
      size_t gs = gimg_raster_stride_bytes(got.r);
      decoded[pass].resize((size_t)dw[pass] * dh[pass] * 3u);
      for (uint32_t y = 0; y < dh[pass]; y++) {
        for (uint32_t x = 0; x < dw[pass]; x++) {
          for (int ch = 0; ch < 3; ch++) {
            decoded[pass][((size_t)y * dw[pass] + x) * 3 + (size_t)ch] =
                gp[y * gs + x * 4 + (size_t)ch];
          }
        }
      }
    }
    ASSERT_EQ(dw[0], dw[1]);
    ASSERT_EQ(dh[0], dh[1]);
    EXPECT_EQ(decoded[0], decoded[1]);
  }
}

// A.2.3 is a sequential arrangement.  Where the standard already settles how
// the scans are laid out, or where a different writer owns the frame, asking
// for it is a contradiction rather than a request, and the encoder says so
// instead of writing something that is not what was asked for.
TEST(JpegEncode, NonInterleavedRefusesWhatItCannotMean) {
  struct Case {
    uint8_t progressive;
    uint8_t lossless_predictor;
    uint8_t hierarchical_levels;
    const char * what;
  };
  const Case cases[] = {
      {1, 0, 0, "progressive: Annex G carries its own scan script"},
      {0, 1, 0, "lossless: written by the Annex H path"},
      {0, 0, 2, "hierarchical: Annex J writes its own frames"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    RasterGuard src;
    ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                  nullptr, 0, &src.r),
        GIMG_OK);
    memset(gimg_raster_pixels(src.r), 0x40,
        gimg_raster_stride_bytes(src.r) * 16u);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_from_raster(src.r, &doc), GIMG_OK);
    GIMG_Stream * os = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
    GIMG_Save_Options o = {};
    o.jpeg_non_interleaved = 1;
    o.jpeg_progressive = c.progressive;
    o.jpeg_lossless_predictor = c.lossless_predictor;
    o.jpeg_hierarchical_levels = c.hierarchical_levels;
    GIMG_Save_Report rep = {};
    EXPECT_EQ(gimg_doc_save(doc, os, "jpeg", &o, &rep), GIMG_ERR_UNSUPPORTED);
    gimg_stream_destroy(os);
    gimg_doc_destroy(doc);
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

// The inverse DCT carries its intermediates in int64_t on purpose: the
// dequantized input alone is a 31-bit quantity for a block that is
// syntactically legal, and 32-bit sums of those values overflow.  Pass 1 used
// to compute wide and then store through (int) into the int64_t workspace,
// which handed pass 2 a wrapped value - implementation-defined rather than
// undefined, so no sanitizer ever objected.
//
// This test sits at the transform rather than at a file because the input that
// exposes it is one no encoder can emit: the forward DCT of any 12-bit block
// bounds the DC coefficient near 16376, whereas a 15-category DC difference
// (T.81 F.1.2.1) against a 16-bit quantizer value (B.2.4.1, Pq=1) dequantizes
// to 32767 x 65535.  T.81 does not say what such a block decodes to.  It does
// have to decode to the sign the transform actually computed.
TEST(JpegIdct, Pass1ResultReachesPass2WithoutBeingNarrowed) {
  const int32_t dequantized_dc = 32767 * 65535;
  // pass1_bits is 1 for 12-bit frames (jidctint.c's PASS1_BITS), so the
  // pass-1 DC is dequantized_dc << 1 == 4294770690 - past int32, and (int) of
  // it is -196606.
  ASSERT_GT((int64_t)dequantized_dc << 1, (int64_t)INT32_MAX);

  // The DC-only shortcut in pass 1.
  int32_t in[64] = {0};
  int32_t out[64];
  in[0] = dequantized_dc;
  jpeg_idct_8x8_islow(in, out, 1);
  for (int i = 0; i < 64; i++) {
    EXPECT_GT(out[i], 0) << "sample " << i << " came back negative; a positive "
                            "DC must not decode to a negative sample";
  }
  EXPECT_EQ(out[0], 268423168);

  // The general path, reached by making one AC coefficient non-zero so the
  // shortcut does not apply.  The eight pass-1 stores are the ones that used
  // to narrow.
  int32_t in2[64] = {0};
  in2[0] = dequantized_dc;
  in2[1] = 1;
  jpeg_idct_8x8_islow(in2, out, 1);
  for (int i = 0; i < 64; i++) {
    EXPECT_GT(out[i], 0) << "sample " << i << " came back negative on the "
                            "general path";
  }
}

namespace {

/**
 * Save a raster as a JPEG under one metadata policy.
 *
 * The raster becomes the document's only item, so nothing but the raster's own
 * color info can tell the writer what color space to state - which is the
 * case these tests are about.
 */
static GIMG_Result save_raster_as_jpeg(const GIMG_Color_Info & color,
    GIMG_Meta_Policy policy, std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK || !doc) {
    return GIMG_ERR_OOM;
  }
  GIMG_Raster * raster = nullptr;
  GIMG_Result r = gimg_raster_create(
      8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  memset(gimg_raster_pixels(raster), 128, 8u * 8u * 4u);
  r = gimg_raster_set_color_info(raster, &color);
  if (r != GIMG_OK) {
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    return r;
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * stream = nullptr;
  r = gimg_stream_create_memory_output(&stream);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    return r;
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, stream, "jpeg", &opts, &report);
  if (r == GIMG_OK) {
    const void * buffer = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(stream, &buffer, &size);
    const uint8_t * bytes = static_cast<const uint8_t *>(buffer);
    out.assign(bytes, bytes + size);
  }
  gimg_stream_destroy(stream);
  gimg_doc_destroy(doc);
  return r;
}

/**
 * Collect the payload of every APP2 segment introduced by "ICC_PROFILE\0",
 * in the order they appear, with the 14-byte header still on the front.
 */
static std::vector<std::vector<uint8_t>> app2_icc_segments(
    const std::vector<uint8_t> & jpeg) {
  std::vector<std::vector<uint8_t>> found;
  size_t i = 2; // past SOI
  while (i + 4 <= jpeg.size() && jpeg[i] == 0xFF) {
    uint8_t marker = jpeg[i + 1];
    if (marker == 0xD8 || marker == 0xD9 || marker == 0x01 ||
        (marker >= 0xD0 && marker <= 0xD7)) {
      i += 2;
      continue;
    }
    size_t len = (size_t)((jpeg[i + 2] << 8) | jpeg[i + 3]);
    if (len < 2 || i + 2 + len > jpeg.size()) {
      break;
    }
    const uint8_t * payload = jpeg.data() + i + 4;
    size_t payload_len = len - 2;
    if (marker == 0xE2 && payload_len >= 14 &&
        memcmp(payload, "ICC_PROFILE\0", 12) == 0) {
      found.push_back(
          std::vector<uint8_t>(payload, payload + payload_len));
    }
    if (marker == 0xDA) {
      break; // entropy-coded data follows; no more headers to walk
    }
    i += 2 + len;
  }
  return found;
}

/** Join the data of a run of ICC_PROFILE segments, headers dropped. */
static std::vector<uint8_t> icc_from_segments(
    const std::vector<std::vector<uint8_t>> & segments) {
  std::vector<uint8_t> profile;
  for (const std::vector<uint8_t> & seg : segments) {
    profile.insert(profile.end(), seg.begin() + 14, seg.end());
  }
  return profile;
}

/**
 * A syntactically plausible ICC profile of a given size: its declared length
 * in the first four bytes and the 'acsp' signature at offset 36, so a reader
 * that sanity-checks a profile before storing it has something to accept.
 */
static std::vector<uint8_t> synthetic_profile(size_t size) {
  std::vector<uint8_t> profile(size < 128 ? 128 : size, 0);
  profile[0] = (uint8_t)(profile.size() >> 24);
  profile[1] = (uint8_t)(profile.size() >> 16);
  profile[2] = (uint8_t)(profile.size() >> 8);
  profile[3] = (uint8_t)profile.size();
  memcpy(profile.data() + 36, "acsp", 4);
  for (size_t i = 40; i < profile.size(); i++) {
    profile[i] = (uint8_t)((i * 7u) & 0xFFu);
  }
  return profile;
}

} // namespace

TEST(JpegEncode, EmbeddedProfileOnTheRasterIsWrittenAsApp2) {
  // A document that did not arrive as a JPEG has no APP2 in its raw metadata
  // to preserve, so without this its profile was lost: a BMP with a V5
  // embedded profile, saved as a JPEG, came out untagged and the profile was
  // read only to be dropped.
  std::vector<uint8_t> profile = synthetic_profile(512);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  std::vector<uint8_t> jpeg;
  ASSERT_EQ(save_raster_as_jpeg(color, GIMG_META_PRESERVE_ALL, jpeg), GIMG_OK);

  std::vector<std::vector<uint8_t>> segments = app2_icc_segments(jpeg);
  ASSERT_EQ(segments.size(), 1u);
  EXPECT_EQ(segments[0][12], 1); // chunk 1
  EXPECT_EQ(segments[0][13], 1); // of 1
  EXPECT_EQ(icc_from_segments(segments), profile);

  // And the library reads back what it wrote.
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->icc_size, profile.size());
  EXPECT_EQ(memcmp(read->icc_bytes, profile.data(), profile.size()), 0);
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegEncode, AProfileTooBigForOneSegmentIsSplitAndNumbered) {
  // One APP2 holds 65519 bytes of profile, so a real one - CMYK press
  // profiles run to hundreds of kilobytes - has to be split, and a reader
  // reassembles it by the chunk numbers.  A writer that emitted a single
  // oversized segment would produce a file no decoder could read.
  const size_t per_chunk = 65533u - 14u;
  std::vector<uint8_t> profile = synthetic_profile(per_chunk * 2 + 100);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  std::vector<uint8_t> jpeg;
  ASSERT_EQ(save_raster_as_jpeg(color, GIMG_META_PRESERVE_ALL, jpeg), GIMG_OK);

  std::vector<std::vector<uint8_t>> segments = app2_icc_segments(jpeg);
  ASSERT_EQ(segments.size(), 3u);
  for (size_t i = 0; i < segments.size(); i++) {
    EXPECT_EQ(segments[i][12], (uint8_t)(i + 1)) << "chunk number";
    EXPECT_EQ(segments[i][13], 3) << "chunk count";
  }
  EXPECT_EQ(segments[0].size(), 14u + per_chunk);
  EXPECT_EQ(segments[2].size(), 14u + 100u);
  EXPECT_EQ(icc_from_segments(segments), profile);

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  ASSERT_EQ(read->icc_size, profile.size());
  EXPECT_EQ(memcmp(read->icc_bytes, profile.data(), profile.size()), 0);
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegEncode, AProfilePastTheFormatsCeilingIsLeftOutRatherThanTruncated) {
  // The chunk count is one byte, so 255 segments is all a JPEG can carry.  A
  // profile past that is written as no profile: a truncated one would be
  // worse than none, since a reader has no way to tell it is incomplete.
  const size_t per_chunk = 65533u - 14u;
  std::vector<uint8_t> profile = synthetic_profile(per_chunk * 255 + 1);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  std::vector<uint8_t> jpeg;
  ASSERT_EQ(save_raster_as_jpeg(color, GIMG_META_PRESERVE_ALL, jpeg), GIMG_OK);
  EXPECT_TRUE(app2_icc_segments(jpeg).empty());
}

TEST(JpegEncode, KeepCommonOnlyStillStatesTheColorSpace) {
  // Dropping the segments a file arrived with does not mean dropping what its
  // samples mean.  The PNG writer keeps color under this policy too.
  std::vector<uint8_t> profile = synthetic_profile(256);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  std::vector<uint8_t> jpeg;
  ASSERT_EQ(
      save_raster_as_jpeg(color, GIMG_META_KEEP_COMMON_ONLY, jpeg), GIMG_OK);
  EXPECT_EQ(icc_from_segments(app2_icc_segments(jpeg)), profile);
}

TEST(JpegEncode, DropAllAndKeepRawOnlyWriteNoProfile) {
  // DROP_ALL is asked for a file with nothing attached.  KEEP_RAW_ONLY is
  // asked for the segments the file arrived with and no others, so a profile
  // that reached the raster from somewhere else is not synthesized into one.
  std::vector<uint8_t> profile = synthetic_profile(256);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  std::vector<uint8_t> jpeg;
  ASSERT_EQ(save_raster_as_jpeg(color, GIMG_META_DROP_ALL, jpeg), GIMG_OK);
  EXPECT_TRUE(app2_icc_segments(jpeg).empty());

  jpeg.clear();
  ASSERT_EQ(save_raster_as_jpeg(color, GIMG_META_KEEP_RAW_ONLY, jpeg), GIMG_OK);
  EXPECT_TRUE(app2_icc_segments(jpeg).empty());
}

TEST(JpegEncode, ARasterWithNoProfileGetsNoApp2) {
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  std::vector<uint8_t> jpeg;
  ASSERT_EQ(save_raster_as_jpeg(color, GIMG_META_PRESERVE_ALL, jpeg), GIMG_OK);
  EXPECT_TRUE(app2_icc_segments(jpeg).empty());
}

TEST(JpegEncode, ADocumentAnotherCodecLoadedCanBeSavedAsJpeg) {
  // Converting to JPEG is the ordinary case, and it does not require the
  // caller to decode by hand first: gimg_item_decode dispatches to whichever
  // codec loaded the document, so the pixels are reachable whoever that was.
  // The PNG and BMP writers both decode unconditionally here; this one used to
  // refuse unless it had loaded the document itself, which made every
  // conversion into a JPEG fail with GIMG_ERR_UNSUPPORTED.
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.transfer = GIMG_TRANSFER_SRGB;

  // Round-trip through PNG so the document really is one another codec loaded,
  // with no raster attached to its item.
  GIMG_Doc * src = nullptr;
  ASSERT_EQ(gimg_doc_create(&src), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 96, 8u * 8u * 4u);
  ASSERT_EQ(gimg_raster_set_color_info(raster, &color), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(src, 0), raster);

  GIMG_Stream * png_out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&png_out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(src, png_out, "png", &opts, &report), GIMG_OK);
  const void * png_bytes = nullptr;
  size_t png_size = 0;
  gimg_stream_output_buffer(png_out, &png_bytes, &png_size);
  std::vector<uint8_t> png(static_cast<const uint8_t *>(png_bytes),
      static_cast<const uint8_t *>(png_bytes) + png_size);
  gimg_stream_destroy(png_out);
  gimg_doc_destroy(src);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(png.data(), png.size(), &in), GIMG_OK);
  GIMG_Doc * loaded = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &loaded), GIMG_OK);
  ASSERT_EQ(gimg_item_raster(gimg_doc_item(loaded, 0)), nullptr)
      << "the item must have no raster, or this tests nothing";

  GIMG_Stream * jpeg_out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&jpeg_out), GIMG_OK);
  EXPECT_EQ(gimg_doc_save(loaded, jpeg_out, "jpeg", &opts, &report), GIMG_OK);
  const void * jpeg_bytes = nullptr;
  size_t jpeg_size = 0;
  gimg_stream_output_buffer(jpeg_out, &jpeg_bytes, &jpeg_size);
  EXPECT_GT(jpeg_size, 0u);

  gimg_stream_destroy(jpeg_out);
  gimg_doc_destroy(loaded);
  gimg_stream_destroy(in);
}

TEST(JpegEncode, SavingFromADocumentWhoseRasterTheSaveOwnsReadsNoFreedColor) {
  // The colour the APP2 writer states has to outlive the raster it came from:
  // the encode destroys the raster as soon as the scan data exists, and the
  // APP segments are written after that.  When the save had decoded the
  // raster itself there was nothing else holding it, so reading its colour
  // read freed memory - and a garbage icc_size and icc_bytes would have
  // copied arbitrary heap into the output file.  ASan found it on a 47-byte
  // arithmetic lossless JPEG within seconds of the colour path going in; the
  // input is in tests/fuzz/corpus.
  //
  // The tests above all attach the raster to the document, which is the case
  // where it outlives the save - so none of them could have caught this.
  // Run under `make test-asan` for the assertion that matters.
  static const unsigned char lossless[] = {0xFF, 0xD8, 0x64, 0x00, 0x00, 0x00,
      0x00, 0x00, 0xFF, 0xCB, 0x00, 0x0B, 0x08, 0x01, 0x02, 0x00, 0x11, 0x01,
      0x00, 0x11, 0x00, 0xFF, 0xCC, 0x00, 0x04, 0x00, 0x10, 0xFF, 0xDA, 0x00,
      0x08, 0x01, 0x00, 0x01, 0x04, 0x00, 0x00, 0xC3, 0xE2, 0xDE, 0xDF, 0xEA,
      0xE7, 0xD8, 0xFF, 0xD9};

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(lossless, sizeof(lossless), &in),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_EQ(gimg_item_raster(gimg_doc_item(doc, 0)), nullptr)
      << "the save must decode for itself, or this tests nothing";

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);

  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &bytes, &size);
  std::vector<uint8_t> jpeg(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + size);
  // The source carried no profile, so neither may the result: an APP2 here
  // would be whatever the freed raster's bytes happened to spell.
  EXPECT_TRUE(app2_icc_segments(jpeg).empty());

  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
}

TEST(JpegEncode, AProfileSurvivesASaveThatOwnsItsRaster) {
  // The other half: when the save decodes the raster itself, the profile on
  // it still reaches the file, so the fix carries the colour rather than
  // merely dropping it.
  std::vector<uint8_t> profile = synthetic_profile(300);
  GIMG_Color_Info color;
  gimg_color_info_default(&color);
  color.icc_bytes = profile.data();
  color.icc_size = profile.size();

  // Round-trip through BMP, which stores the profile in a V5 header, so the
  // loaded document has a profile and no attached raster.
  GIMG_Doc * src = nullptr;
  ASSERT_EQ(gimg_doc_create(&src), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 200, 8u * 8u * 4u);
  ASSERT_EQ(gimg_raster_set_color_info(raster, &color), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(src, 0), raster);

  GIMG_Stream * bmp_out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&bmp_out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(src, bmp_out, "bmp", &opts, &report), GIMG_OK);
  const void * bmp_bytes = nullptr;
  size_t bmp_size = 0;
  gimg_stream_output_buffer(bmp_out, &bmp_bytes, &bmp_size);
  std::vector<uint8_t> bmp(static_cast<const uint8_t *>(bmp_bytes),
      static_cast<const uint8_t *>(bmp_bytes) + bmp_size);
  gimg_stream_destroy(bmp_out);
  gimg_doc_destroy(src);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bmp.data(), bmp.size(), &in), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_EQ(gimg_item_raster(gimg_doc_item(doc, 0)), nullptr);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &bytes, &size);
  std::vector<uint8_t> jpeg(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + size);
  EXPECT_EQ(icc_from_segments(app2_icc_segments(jpeg)), profile)
      << "a BMP's embedded profile must reach a JPEG saved from it";

  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in);
}

TEST(JpegEncode, ACmykRasterLabelledTheOtherWayRoundIsWrittenRightWayUp) {
  // A JPEG's four components are the Adobe convention - 0 is full ink - so a
  // raster that says GIMG_CMYK_POLARITY_REFLECTION holds the complement.
  // Writing those samples as they stand produced a photographic negative of
  // the picture the caller had correctly labelled.  The encoder read the
  // field nowhere, while the conversion in ops reads it as authoritative.
  //
  // The two rasters below are complements of each other and say so, and must
  // therefore encode to the same picture.
  const uint8_t values[4][4] = {
      {0, 64, 128, 255}, {255, 191, 127, 0}, {10, 20, 30, 40}, {32, 32, 32, 32}};

  std::vector<uint8_t> as_ink, as_reflection;
  for (int pass = 0; pass < 2; pass++) {
    bool reflection = (pass == 1);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_raster_create(
                  4, 1, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0, &raster),
        GIMG_OK);
    uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
    for (int x = 0; x < 4; x++) {
      for (int c = 0; c < 4; c++) {
        uint8_t v = values[x][c];
        px[(x * 4) + c] = reflection ? (uint8_t)(255u - v) : v;
      }
    }
    GIMG_Color_Info ci;
    gimg_color_info_default(&ci);
    ci.cmyk_polarity = reflection ? GIMG_CMYK_POLARITY_REFLECTION
                                  : GIMG_CMYK_POLARITY_INK;
    ASSERT_EQ(gimg_raster_set_color_info(raster, &ci), GIMG_OK);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.quality = 100;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
    const void * bytes = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &bytes, &size);
    std::vector<uint8_t> & into = reflection ? as_reflection : as_ink;
    into.assign(static_cast<const uint8_t *>(bytes),
        static_cast<const uint8_t *>(bytes) + size);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
  }

  EXPECT_EQ(as_ink, as_reflection)
      << "two rasters that say they are complements of each other describe "
         "the same picture and must encode to the same file";

  // And the one that came back says it is the file's own convention.
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(as_reflection.data(), as_reflection.size(), &s),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &back), GIMG_OK);
  const GIMG_Color_Info * read = gimg_raster_color_info_const(back);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->cmyk_polarity, GIMG_CMYK_POLARITY_INK);
  const uint8_t * got =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(back));
  for (int x = 0; x < 4; x++) {
    for (int c = 0; c < 4; c++) {
      EXPECT_NEAR(got[(x * 4) + c], values[x][c], 3)
          << "channel " << c << " of pixel " << x;
    }
  }
  gimg_raster_destroy(back);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegEncode, ACmykRasterThatStatesNoPolarityIsWrittenAsItStands) {
  // A caller building CMYK samples for a JPEG is building them the way a JPEG
  // holds them, so an unstated polarity is taken as the file's own convention
  // rather than refused.  This is the case every existing caller is in.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                4, 1, &GIMG_PIXEL_CMYK8, GIMG_RASTER_OWNED, NULL, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  for (int i = 0; i < 16; i++) {
    px[i] = (uint8_t)(i * 16);
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.quality = 100;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
  const void * bytes = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &bytes, &size);
  std::vector<uint8_t> jpeg(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * back_doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &back_doc), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(back_doc, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(back));
  for (int i = 0; i < 16; i++) {
    EXPECT_NEAR(got[i], (uint8_t)(i * 16), 3) << "sample " << i;
  }
  gimg_raster_destroy(back);
  gimg_doc_destroy(back_doc);
  gimg_stream_destroy(s);
}

TEST(JpegEncode, TheCmykPolarityFlipWorksAtTwelveBitsToo) {
  // The complement is taken against the raster's own maximum, so a twelve-bit
  // raster - what a JPEG at extended precision decodes to - has to complement
  // against 4095 and not 255.  A flip against the wrong maximum would clip
  // everything to black.
  const uint16_t values[2][4] = {{0, 1365, 2730, 4095}, {4095, 100, 2000, 0}};

  std::vector<uint8_t> as_ink, as_reflection;
  for (int pass = 0; pass < 2; pass++) {
    bool reflection = (pass == 1);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_raster_create(
            2, 1, &GIMG_PIXEL_CMYK12, GIMG_RASTER_OWNED, NULL, 0, &raster),
        GIMG_OK);
    uint16_t * px = static_cast<uint16_t *>(gimg_raster_pixels(raster));
    for (int x = 0; x < 2; x++) {
      for (int c = 0; c < 4; c++) {
        uint16_t v = values[x][c];
        px[(x * 4) + c] = reflection ? (uint16_t)(4095u - v) : v;
      }
    }
    GIMG_Color_Info ci;
    gimg_color_info_default(&ci);
    ci.cmyk_polarity = reflection ? GIMG_CMYK_POLARITY_REFLECTION
                                  : GIMG_CMYK_POLARITY_INK;
    ASSERT_EQ(gimg_raster_set_color_info(raster, &ci), GIMG_OK);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.quality = 100;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
    const void * bytes = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &bytes, &size);
    std::vector<uint8_t> & into = reflection ? as_reflection : as_ink;
    into.assign(static_cast<const uint8_t *>(bytes),
        static_cast<const uint8_t *>(bytes) + size);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
  }

  EXPECT_EQ(as_ink, as_reflection)
      << "complemented against 4095, the two describe the same picture";
  EXPECT_FALSE(as_ink.empty());
}


namespace {

/** Load a JPEG fixture, swap its APP1 Exif, save it back under `policy`. */
GIMG_Result jpeg_save_with_exif(const std::vector<uint8_t> & exif,
    GIMG_Meta_Policy policy, std::vector<uint8_t> * out_saved) {
  std::vector<uint8_t> file;
  if (!jpeg_test::load_jpeg_file("plain_gray.jpg", file)) {
    return GIMG_ERR_IO;
  }
  if (!exif_test::replace_jpeg_exif(file, exif)) {
    return GIMG_ERR_IO;
  }
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(file.data(), file.size(), &in) != GIMG_OK) {
    return GIMG_ERR_IO;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(in, nullptr, nullptr, &doc);
  gimg_stream_destroy(in);
  if (r != GIMG_OK) {
    return r;
  }
  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return GIMG_ERR_IO;
  }
  GIMG_Save_Options opts = {};
  opts.metadata_policy = policy;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out, "jpeg", &opts, &report);
  if (r == GIMG_OK && out_saved) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out, &p, &n);
    out_saved->assign((const uint8_t *)p, (const uint8_t *)p + n);
  }
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return r;
}

/** The Exif bytes of a saved JPEG's APP1 segment. Empty when there is none. */
std::vector<uint8_t> jpeg_exif_of(const std::vector<uint8_t> & jpg) {
  size_t i = 2;
  while (i + 4 < jpg.size()) {
    if (jpg[i] != 0xFF) { i++; continue; }
    const uint8_t m = jpg[i + 1];
    if (m == 0xDA || m == 0xD9) { break; }
    const size_t len = ((size_t)jpg[i + 2] << 8) | jpg[i + 3];
    if (m == 0xE1 && i + 10 <= jpg.size() &&
        std::memcmp(&jpg[i + 4], "Exif\0\0", 6) == 0) {
      return std::vector<uint8_t>(
          jpg.begin() + (long)(i + 10), jpg.begin() + (long)(i + 2 + len));
    }
    i += 2 + len;
  }
  return std::vector<uint8_t>();
}

} // namespace

/**
 * The JPEG writer applies STRIP_GPS, and reports when it cannot.
 *
 * The JPEG and PNG writers run separate copies of the metadata-policy code -
 * one writes an APP1 segment and the other an eXIf chunk - so the behaviour
 * has to be asserted on both. This is the APP1 half.
 */
TEST(JpegEncode, StripGpsRemovesTheGpsIfdFromApp1) {
  const std::vector<uint8_t> exif = exif_test::make_exif_with_gps();
  ASSERT_TRUE(exif_test::exif_has_gps_tag(exif));

  std::vector<uint8_t> saved;
  ASSERT_EQ(jpeg_save_with_exif(exif, GIMG_META_PRESERVE_ALL, &saved), GIMG_OK);
  const std::vector<uint8_t> kept = jpeg_exif_of(saved);
  ASSERT_FALSE(kept.empty()) << "the control needs the Exif to survive";
  EXPECT_TRUE(exif_test::exif_has_gps_tag(kept))
      << "PRESERVE_ALL must keep the GPS pointer";

  saved.clear();
  ASSERT_EQ(jpeg_save_with_exif(exif, GIMG_META_STRIP_GPS, &saved), GIMG_OK);
  const std::vector<uint8_t> stripped = jpeg_exif_of(saved);
  ASSERT_FALSE(stripped.empty()) << "STRIP_GPS removes GPS, not all Exif";
  EXPECT_FALSE(exif_test::exif_has_gps_tag(stripped));
  EXPECT_EQ(exif_test::exif_ifd0_entry_count(stripped), 2);
  EXPECT_LT(stripped.size(), kept.size());
}

/** Exif the library cannot parse fails a JPEG save that asks to edit it. */
TEST(JpegEncode, AnExifPolicyFailsOnApp1ItCannotParse) {
  // Valid magic, but IFD0 is past the end: parseable enough to be Exif and
  // not enough to act on. Writing it through would publish whatever it holds.
  std::vector<uint8_t> bad = {'I', 'I', 42, 0, 0xF0, 0xFF, 0, 0, 1, 2, 3, 4,
      5, 6, 7, 8};
  EXPECT_EQ(jpeg_save_with_exif(bad, GIMG_META_STRIP_GPS, nullptr),
      GIMG_ERR_CORRUPT);
  EXPECT_EQ(jpeg_save_with_exif(bad, GIMG_META_NORMALIZE_EXIF, nullptr),
      GIMG_ERR_CORRUPT);
  // The control: it saves fine when nothing is asked of the Exif.
  std::vector<uint8_t> saved;
  EXPECT_EQ(jpeg_save_with_exif(bad, GIMG_META_PRESERVE_ALL, &saved), GIMG_OK);
  EXPECT_EQ(jpeg_exif_of(saved), bad)
      << "PRESERVE_ALL must hand back the bytes it could not read";
}

namespace {

/** One option set the writer must refuse, named so a failure says which. */
struct Refusal {
  const char * why;
  GIMG_Save_Options options;
};

/**
 * Load a fixture into a document and leave it undecoded.
 *
 * The point is what is *not* done: no gimg_item_ensure_decoded. An item with
 * no raster on it makes the save decode one and own it for the duration, and
 * that ownership is the thing under test below.
 */
GIMG_Doc * load_undecoded(const char * name, GIMG_Stream ** keep,
    std::vector<uint8_t> & bytes) {
  if (!jpeg_test::load_jpeg_file(name, bytes)) { return nullptr; }
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), keep) != GIMG_OK) {
    return nullptr;
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(*keep, nullptr, nullptr, &doc) != GIMG_OK) { return nullptr; }
  return doc;
}

} // namespace

/**
 * A refused option set does not keep the raster the save decoded for itself.
 *
 * Every one of these refusals is checked after the raster exists, and the
 * raster is the save's own whenever the document arrived undecoded - which is
 * every document loaded and re-saved without being looked at, the commonest
 * shape there is. Each refusal therefore has its own `if (raster_owned)
 * gimg_raster_destroy(raster)`, eight of them, and not one was reached by any
 * test: every fixture in this suite hands the writer a raster it does not own,
 * so `raster_owned` was false in all of them and the arms were dead code that
 * looked live.
 *
 * What is asserted is the refusal and the accounting: across the load, the
 * save and the destroy, the allocator hands out and takes back the same
 * number of blocks, so a decoded raster left behind shows up as a positive
 * count. Checked rather than assumed - with the destroy removed from the
 * 16-bit refusal, that row alone reported two outstanding blocks and the
 * other eight stayed silent.
 *
 * The first attempt at it measured nothing, which is worth recording because
 * the failure was invisible: the allocator was swapped in just before
 * gimg_doc_save, and a JPEG document decodes through the allocator it was
 * *loaded* with, kept in its codec_private state. Zero allocations went
 * through the swapped one, so the count was trivially zero and the removed
 * destroy passed. `EXPECT_GT(f.attempts, 0)` is there so that a count of zero
 * can never again read as a clean result.
 */
TEST(JpegEncode, ARefusedOptionSetFreesTheRasterItDecoded) {
  auto base = [](void) {
    GIMG_Save_Options o = {};
    o.quality = 80;
    return o;
  };
  std::vector<Refusal> refusals;
  {
    GIMG_Save_Options o = base();
    o.jpeg_hierarchical_levels = 1;
    o.jpeg_precision = 12;
    refusals.push_back({"a hierarchical sequence at a precision other than 8", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_non_interleaved = 1;
    o.jpeg_progressive = 1;
    refusals.push_back({"non-interleaved scans in a progressive frame", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_hierarchical_levels = 250;
    refusals.push_back({"more hierarchical frames than the encoder holds", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_precision = 16;
    refusals.push_back({"16-bit samples in a DCT frame", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_cmyk_transform = 1;
    refusals.push_back({"an Adobe transform that is neither 0 nor 2", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_abbreviated = 3;
    refusals.push_back({"an abbreviated format T.81 B.4 does not define", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_abbreviated = 1;
    o.jpeg_lossless_predictor = 1;
    refusals.push_back({"abbreviated tables for a lossless frame", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_lossless_predictor = 8;
    refusals.push_back({"a predictor outside T.81 Table H.1", o});
  }
  {
    GIMG_Save_Options o = base();
    o.jpeg_lossless_predictor = 1;
    o.jpeg_progressive = 1;
    refusals.push_back({"a progressive lossless frame, which T.81 has no "
                        "process for", o});
  }

  GIMG_Codec * codec = gimg_codec_by_name("jpeg");
  ASSERT_NE(codec, nullptr);

  for (const Refusal & ref : refusals) {
    // The swap has to be in place for the LOAD, not just the save.  A JPEG
    // document keeps the allocator it was loaded with in its codec_private
    // state, and the decode a save triggers allocates from that rather than
    // from whatever codec->allocator says at the time - so swapping just
    // before gimg_doc_save counts nothing at all.  Measured: attempts stayed
    // at zero for every one of these, and a deliberately removed destroy went
    // unnoticed.  Held across the load, the save and the destroy, the count
    // is a closed book: everything taken must come back.
    gimg_test::Failing f;
    gimg_test::init(f);
    const GIMG_Allocator * saved = codec->allocator;
    codec->allocator = &f.a;

    GIMG_Stream * keep = nullptr;
    std::vector<uint8_t> bytes;
    GIMG_Doc * doc = load_undecoded("baseline_16x16_ycbcr.jpg", &keep, bytes);
    ASSERT_NE(doc, nullptr) << ref.why;
    ASSERT_EQ(gimg_item_raster(gimg_doc_item(doc, 0)), nullptr)
        << "the document must arrive undecoded, or this measures nothing";

    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Report report = {};
    const GIMG_Result r =
        gimg_doc_save(doc, out, "jpeg", &ref.options, &report);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(keep);
    codec->allocator = saved;

    EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED) << "should have refused " << ref.why;
    EXPECT_GT(f.attempts, 0)
        << "nothing was allocated through the swapped allocator, so the count "
           "below would hold however much leaked";
    EXPECT_EQ(f.outstanding, 0)
        << f.outstanding << " block(s) kept after refusing " << ref.why;
  }
}

namespace {

/** A 32x32 RGBA gradient with a per-pixel perturbation, so blocks have AC. */
GIMG_Raster * progression_source(void) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(32u, 32u, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
          nullptr, 0, &r) != GIMG_OK) {
    return nullptr;
  }
  unsigned char * px = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < 32u; y++) {
    for (uint32_t x = 0; x < 32u; x++) {
      px[y * stride + x * 4 + 0] = (unsigned char)(x * 8u + ((y * 13u) & 31u));
      px[y * stride + x * 4 + 1] = (unsigned char)(y * 8u + ((x * 7u) & 31u));
      px[y * stride + x * 4 + 2] = (unsigned char)(((x ^ y) * 9u) & 0xFFu);
      px[y * stride + x * 4 + 3] = 255u;
    }
  }
  return r;
}

} // namespace

/**
 * A real successive-approximation progression, checked against libjpeg.
 *
 * The encoder's default progression is two scans - `{0,0,0,0}` then
 * `{1,63,0,0}` - which hold no bits back at all: Ah and Al are zero
 * throughout, so every coefficient is sent once and complete. That is a legal
 * progressive file and it is not the one anybody writes. libjpeg's
 * `jpeg_simple_progression`, which produced very nearly every progressive JPEG
 * in circulation, sends the DC one bit short and refines it, and sends each AC
 * band two bits short and refines it twice.
 *
 * Nothing exercised that, and the encoder did not implement it. **The point
 * transform was not applied anywhere**: every scan wrote its coefficients at
 * full precision while the scan header above them declared Al, so a decoder
 * shifted them left again. Measured against the same image at Al = 0, a mean
 * absolute error of 28.7 per channel out of 255 - and libjpeg and this
 * library's decoder agreed on it, because the file really did say that. The
 * suite's one refinement test scripted `{1,63,0,0}` then `{1,63,1,0}`: an
 * initial scan that already sent every bit, followed by a refinement of a bit
 * that had not been held back. **A refinement scan only refines something if
 * the scan before it left something out.**
 *
 * Two assertions, and both matter:
 *
 *   - the picture is **identical** to the same image written in one pass.
 *     Successive approximation splits the coefficients across scans; it does
 *     not change them, so anything but an exact match is a defect. This holds
 *     without the oracle.
 *   - libjpeg reads the same pixels out of it. Refinement bits are the one
 *     part of Annex G where an encoder and a decoder written together can
 *     agree with each other and with nobody else - the correction bits mean
 *     nothing except relative to what the previous scan said, so a shared
 *     misreading round-trips perfectly. libjpeg has no such arrangement with
 *     us, and this codec's decoder is bit-exact with it elsewhere.
 */
TEST(JpegEncode, ASuccessiveApproximationProgressionSaysWhatLibjpegReads) {
  static const GIMG_JPEG_Progressive_Scan one_pass[] = {
      {0, 0, 0, 0},
      {1, 63, 0, 0},
  };
  // libjpeg's jpeg_simple_progression, in the order it emits. The DC
  // refinement between the two AC refinements is what the old encoder could
  // not write: it insisted that a refinement scan follow the initial scan of
  // its own band directly.
  static const GIMG_JPEG_Progressive_Scan simple_progression[] = {
      {0, 0, 0, 1},  // DC, one bit held back
      {1, 5, 0, 2},  // low AC band, two bits held back
      {6, 63, 0, 2}, // high AC band, two bits held back
      {1, 63, 2, 1}, // AC refinement, bit 1
      {0, 0, 1, 0},  // DC refinement, the last bit
      {1, 63, 1, 0}, // AC refinement, the last bit
  };

  struct Case {
    const char * name;
    const GIMG_JPEG_Progressive_Scan * scans;
    unsigned scan_count;
    uint16_t restart_interval;
    uint8_t arithmetic;
  };
  const Case cases[] = {
      {"progressive_successive_approximation.jpg", simple_progression, 6u, 0u,
          0u},
      // Every scan of a progression may carry restart markers, and each of the
      // four scan writers emits them from a place of its own.
      {"progressive_successive_restarts.jpg", simple_progression, 6u, 4u, 0u},
      // T.81 G.2 codes the same progression arithmetically, through an
      // entirely separate encoder, and it had a point transform bug of its
      // own: the DC first scan truncated toward zero where the clause says to
      // shift.  Worth a maximum error of 3 - small enough to read as
      // quantization noise, which is how it survived.
      {"progressive_successive_arith.jpg", simple_progression, 6u, 0u, 1u},
  };

  // The same image in one pass, as the thing every case must equal.
  std::vector<uint8_t> one_pass_pixels;
  {
    GIMG_Raster * raster = progression_source();
    ASSERT_NE(raster, nullptr);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK);
    gimg_raster_destroy(raster);
    const GIMG_JPEG_Progressive_Config cfg = {2u, one_pass};
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.quality = 90;
    opts.jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444;
    opts.jpeg_progressive = 1;
    opts.jpeg_progressive_config = &cfg;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK);
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    const std::vector<uint8_t> bytes(
        (const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    DocStreamGuard in;
    ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &in.s),
        GIMG_OK);
    ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
    RasterGuard got;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
    const unsigned char * px = (const unsigned char *)gimg_raster_pixels(got.r);
    const size_t stride = gimg_raster_stride_bytes(got.r);
    for (uint32_t y = 0; y < 32u; y++) {
      for (uint32_t x = 0; x < 32u; x++) {
        for (int ch = 0; ch < 3; ch++) {
          one_pass_pixels.push_back(px[y * stride + x * 4 + (size_t)ch]);
        }
      }
    }
  }
  ASSERT_EQ(one_pass_pixels.size(), 32u * 32u * 3u);

  bool oracle_ran = false;
  for (const Case & c : cases) {
    SCOPED_TRACE(c.name);
    GIMG_Raster * raster = progression_source();
    ASSERT_NE(raster, nullptr);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK);
    gimg_raster_destroy(raster);

    const GIMG_JPEG_Progressive_Config cfg = {c.scan_count, c.scans};
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.quality = 90;
    opts.jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444;
    opts.jpeg_progressive = 1;
    opts.jpeg_progressive_config = &cfg;
    opts.jpeg_restart_interval = c.restart_interval;
    opts.jpeg_arithmetic = c.arithmetic;
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out, "jpeg", &opts, &report), GIMG_OK)
        << "libjpeg's own progression must be writable";
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    const std::vector<uint8_t> written(
        (const uint8_t *)data, (const uint8_t *)data + size);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    ASSERT_GT(written.size(), 0u);

    jpeg_test::write_jpeg_output(c.name, written.data(), written.size());

    DocStreamGuard in;
    ASSERT_EQ(gimg_stream_create_memory(written.data(), written.size(), &in.s),
        GIMG_OK);
    ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
    RasterGuard got;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
    ASSERT_NE(got.r, nullptr);
    ASSERT_EQ(gimg_raster_width(got.r), 32u);
    ASSERT_EQ(gimg_raster_height(got.r), 32u);
    {
      const unsigned char * px = (const unsigned char *)gimg_raster_pixels(got.r);
      const size_t stride = gimg_raster_stride_bytes(got.r);
      for (uint32_t y = 0; y < 32u; y++) {
        for (uint32_t x = 0; x < 32u; x++) {
          for (int ch = 0; ch < 3; ch++) {
            const size_t i = ((size_t)y * 32u + x) * 3u + (size_t)ch;
            ASSERT_EQ((int)px[y * stride + x * 4 + (size_t)ch],
                (int)one_pass_pixels[i])
                << "successive approximation changed pixel (" << x << "," << y
                << ") channel " << ch
                << "; it reorders the bits, it does not alter them";
          }
        }
      }
    }

    const std::string jpeg_path = jpeg_test::jpeg_output_dir() + "/" + c.name;
    const std::string raw_path =
        jpeg_test::jpeg_output_dir() + "/libjpeg_" + c.name + ".raw";
    std::vector<uint8_t> libjpeg_pixels;
    uint32_t ow = 0, oh = 0;
    int omode = -1;
    if (jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(),
            raw_path.c_str(), libjpeg_pixels, &ow, &oh, &omode)) {
      oracle_ran = true;
      EXPECT_EQ(ow, 32u);
      EXPECT_EQ(oh, 32u);
      EXPECT_EQ(omode, 1) << "three components => RGB";
      EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
          got.r, libjpeg_pixels.data(), ow, oh, omode, 0))
          << "libjpeg read different pixels out of our successive-"
             "approximation scans than we did";
    }
  }
  if (!oracle_ran) {
    GTEST_SKIP() << "the libjpeg decode oracle did not run; the comparison "
                    "against an outside decoder is half of this test - build "
                    "it with `make jpeg-oracle-tools`.";
  }
}

/**
 * A scan script that describes no readable file is refused before it is one.
 *
 * T.81 G.1.1.1.2 makes successive approximation a chain: a coefficient is sent
 * once at some point transform, and each later scan over it refines exactly
 * one bit, so a scan's Ah is the previous scan's Al and its own Al is one
 * lower. A script that breaks the chain produces a file this library's decoder
 * refuses with GIMG_ERR_CORRUPT and libjpeg rejects as broken data - which is
 * the right answer arriving in the wrong place, because the caller has by then
 * been told the save worked and has the bytes.
 *
 * Each refusal is paired with the nearest script that is legal, so the test
 * says which rule is doing the refusing rather than only that something did.
 */
TEST(JpegEncode, AProgressionThatBreaksTheRefinementChainIsRefused) {
  struct Script {
    const char * why;
    std::vector<GIMG_JPEG_Progressive_Scan> scans;
    GIMG_Result expected;
  };
  const std::vector<Script> scripts = {
      {"refines a bit the initial scan did not hold back",
          {{0, 0, 0, 0}, {1, 63, 0, 0}, {1, 63, 1, 0}}, GIMG_ERR_UNSUPPORTED},
      {"refines a band no earlier scan sent at all",
          {{0, 0, 0, 1}, {0, 0, 1, 0}, {1, 63, 1, 0}}, GIMG_ERR_UNSUPPORTED},
      {"skips a bit: Al two below Ah",
          {{0, 0, 0, 2}, {1, 63, 0, 2}, {1, 63, 2, 0}}, GIMG_ERR_UNSUPPORTED},
      {"sends the same coefficients for the first time twice",
          {{0, 0, 0, 0}, {1, 63, 0, 0}, {1, 63, 0, 0}}, GIMG_ERR_UNSUPPORTED},
      // The controls: the same shapes, with the chain intact.
      {"holds one bit back and refines it",
          {{0, 0, 0, 1}, {1, 63, 0, 1}, {0, 0, 1, 0}, {1, 63, 1, 0}}, GIMG_OK},
      {"holds two bits back and refines them one at a time",
          {{0, 0, 0, 2}, {1, 63, 0, 2}, {1, 63, 2, 1}, {0, 0, 2, 1},
              {0, 0, 1, 0}, {1, 63, 1, 0}},
          GIMG_OK},
      {"two AC bands, each refined by a scan over both",
          {{0, 0, 0, 1}, {1, 5, 0, 1}, {6, 63, 0, 1}, {0, 0, 1, 0},
              {1, 63, 1, 0}},
          GIMG_OK},
  };

  for (const Script & sc : scripts) {
    SCOPED_TRACE(sc.why);
    GIMG_Raster * raster = progression_source();
    ASSERT_NE(raster, nullptr);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK);
    gimg_raster_destroy(raster);
    const GIMG_JPEG_Progressive_Config cfg = {
        (unsigned)sc.scans.size(), sc.scans.data()};
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.quality = 90;
    opts.jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444;
    opts.jpeg_progressive = 1;
    opts.jpeg_progressive_config = &cfg;
    GIMG_Save_Report report = {};
    const GIMG_Result r = gimg_doc_save(doc, out, "jpeg", &opts, &report);
    EXPECT_EQ(r, sc.expected);

    if (r == GIMG_OK) {
      // A script this accepts has to describe a file that reads back, or the
      // rule is only moving the failure rather than catching it.
      const void * data = nullptr;
      size_t size = 0;
      gimg_stream_output_buffer(out, &data, &size);
      const std::vector<uint8_t> bytes(
          (const uint8_t *)data, (const uint8_t *)data + size);
      DocStreamGuard in;
      ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &in.s),
          GIMG_OK);
      ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
      RasterGuard got;
      EXPECT_EQ(
          gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK)
          << "accepted a script whose file does not decode";
    }
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
  }
}

/**
 * Successive approximation at twelve bits is the same picture as one pass.
 *
 * The twelve-bit scan writer is a separate function from the eight-bit one -
 * different DC and AC alphabets, because a twelve-bit coefficient needs
 * categories up to 16 and sizes up to 15 - and it had the same defect plus
 * one more. It wrote `(void)Al` and ignored the point transform, exactly as
 * the eight-bit writer did, and it refused every refinement scan outright, so
 * successive approximation was unavailable at twelve bits rather than merely
 * wrong. The decoder has always handled both: the scan runner is shared
 * between the two precisions, so the gap was on the writing side only.
 *
 * There is no outside decoder to check this against - libjpeg refuses a
 * twelve-bit frame with "Unsupported JPEG data precision 12" - so the bar is
 * the internal one, and it is exact rather than approximate: splitting the
 * coefficients across six scans and refining them back must reproduce the
 * one-pass file's pixels byte for byte. A point transform applied on the way
 * out and not undone on the way back shows up immediately.
 */
TEST(JpegEncode, ASuccessiveApproximationAtTwelveBitsIsTheSameAsOnePass) {
  static const GIMG_JPEG_Progressive_Scan one_pass[] = {
      {0, 0, 0, 0},
      {1, 63, 0, 0},
  };
  static const GIMG_JPEG_Progressive_Scan simple_progression[] = {
      {0, 0, 0, 1},
      {1, 5, 0, 2},
      {6, 63, 0, 2},
      {1, 63, 2, 1},
      {0, 0, 1, 0},
      {1, 63, 1, 0},
  };

  auto decode_of = [](const GIMG_JPEG_Progressive_Scan * scans,
                       unsigned count, uint8_t arithmetic,
                       uint16_t restart_interval,
                       std::vector<uint8_t> & out_pixels) {
    GIMG_Raster * raster = progression_source();
    if (!raster) { return ::testing::AssertionFailure() << "raster"; }
    GIMG_Doc * doc = nullptr;
    if (gimg_doc_from_raster(raster, &doc) != GIMG_OK) {
      gimg_raster_destroy(raster);
      return ::testing::AssertionFailure() << "doc";
    }
    gimg_raster_destroy(raster);
    const GIMG_JPEG_Progressive_Config cfg = {count, scans};
    GIMG_Stream * out = nullptr;
    if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
      gimg_doc_destroy(doc);
      return ::testing::AssertionFailure() << "stream";
    }
    GIMG_Save_Options opts = {};
    opts.quality = 90;
    opts.jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444;
    opts.jpeg_progressive = 1;
    opts.jpeg_progressive_config = &cfg;
    opts.jpeg_precision = 12;
    opts.jpeg_arithmetic = arithmetic;
    opts.jpeg_restart_interval = restart_interval;
    GIMG_Save_Report report = {};
    const GIMG_Result sr = gimg_doc_save(doc, out, "jpeg", &opts, &report);
    gimg_doc_destroy(doc);
    if (sr != GIMG_OK) {
      gimg_stream_destroy(out);
      return ::testing::AssertionFailure() << "save: " << (int)sr;
    }
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out, &p, &n);
    const std::vector<uint8_t> bytes(
        (const uint8_t *)p, (const uint8_t *)p + n);
    gimg_stream_destroy(out);

    DocStreamGuard in;
    if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in.s)
        != GIMG_OK) {
      return ::testing::AssertionFailure() << "reload stream";
    }
    if (gimg_doc_load(in.s, nullptr, nullptr, &in.d) != GIMG_OK) {
      return ::testing::AssertionFailure() << "reload";
    }
    RasterGuard got;
    const GIMG_Result dr =
        gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r);
    if (dr != GIMG_OK || !got.r) {
      return ::testing::AssertionFailure() << "decode: " << (int)dr;
    }
    const GIMG_Pixel_Format * fmt = gimg_raster_format(got.r);
    const size_t row = (size_t)gimg_raster_width(got.r) *
        gimg_raster_bytes_per_pixel(fmt);
    const size_t stride = gimg_raster_stride_bytes(got.r);
    const auto * px = (const unsigned char *)gimg_raster_pixels_const(got.r);
    out_pixels.resize(row * gimg_raster_height(got.r));
    for (uint32_t y = 0; y < gimg_raster_height(got.r); y++) {
      memcpy(out_pixels.data() + (size_t)y * row, px + (size_t)y * stride, row);
    }
    return ::testing::AssertionSuccess();
  };

  struct Case {
    const char * what;
    uint8_t arithmetic;
    uint16_t restart_interval;
  };
  const Case cases[] = {
      {"Huffman", 0u, 0u},
      {"Huffman with restart markers", 0u, 4u},
      {"arithmetic (T.81 G.2)", 1u, 0u},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    std::vector<uint8_t> flat, woven;
    ASSERT_TRUE(decode_of(one_pass, 2u, c.arithmetic, c.restart_interval, flat));
    ASSERT_TRUE(decode_of(
        simple_progression, 6u, c.arithmetic, c.restart_interval, woven));
    ASSERT_GT(flat.size(), 0u);
    ASSERT_EQ(woven.size(), flat.size());
    for (size_t i = 0; i < flat.size(); i++) {
      ASSERT_EQ((int)woven[i], (int)flat[i])
          << "byte " << i << " of the decoded image differs; successive "
             "approximation reorders bits, it does not change them";
    }
  }
}

namespace {

/** A deterministic gradient, gray or colour, for the hierarchical matrix. */
GIMG_Raster * pyramid_source(bool colour) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(64u, 48u,
          colour ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
          nullptr, 0, &r) != GIMG_OK) {
    return nullptr;
  }
  unsigned char * px = (unsigned char *)gimg_raster_pixels(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  const unsigned ch = colour ? 4u : 1u;
  for (uint32_t y = 0; y < 48u; y++) {
    for (uint32_t x = 0; x < 64u; x++) {
      for (unsigned c = 0; c < ch; c++) {
        px[y * stride + x * ch + c] = (colour && c == 3u)
            ? 255u
            : (unsigned char)(((x * 5u) + (y * 3u) + (c * 41u)) & 0xFFu);
      }
    }
  }
  return r;
}

/** Save `pyramid_source(colour)` with these options and decode it back. */
::testing::AssertionResult pyramid_round_trip(bool colour,
    const GIMG_Save_Options & opts, std::vector<uint8_t> & out_pixels,
    size_t * out_bytes) {
  GIMG_Raster * raster = pyramid_source(colour);
  if (!raster) { return ::testing::AssertionFailure() << "raster"; }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_from_raster(raster, &doc) != GIMG_OK) {
    gimg_raster_destroy(raster);
    return ::testing::AssertionFailure() << "doc";
  }
  gimg_raster_destroy(raster);
  GIMG_Stream * out = nullptr;
  if (gimg_stream_create_memory_output(&out) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return ::testing::AssertionFailure() << "stream";
  }
  GIMG_Save_Report report = {};
  const GIMG_Result sr = gimg_doc_save(doc, out, "jpeg", &opts, &report);
  gimg_doc_destroy(doc);
  if (sr != GIMG_OK) {
    gimg_stream_destroy(out);
    return ::testing::AssertionFailure() << "save: " << (int)sr;
  }
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  const std::vector<uint8_t> bytes(
      (const uint8_t *)p, (const uint8_t *)p + n);
  gimg_stream_destroy(out);
  if (out_bytes) { *out_bytes = n; }

  DocStreamGuard in;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in.s) != GIMG_OK) {
    return ::testing::AssertionFailure() << "reload stream";
  }
  if (gimg_doc_load(in.s, nullptr, nullptr, &in.d) != GIMG_OK) {
    return ::testing::AssertionFailure() << "reload";
  }
  RasterGuard got;
  const GIMG_Result dr =
      gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r);
  if (dr != GIMG_OK || !got.r) {
    return ::testing::AssertionFailure() << "decode: " << (int)dr;
  }
  if (gimg_raster_width(got.r) != 64u || gimg_raster_height(got.r) != 48u) {
    return ::testing::AssertionFailure()
        << "decoded " << gimg_raster_width(got.r) << "x"
        << gimg_raster_height(got.r);
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(got.r);
  const size_t stride = gimg_raster_stride_bytes(got.r);
  const auto * px = (const unsigned char *)gimg_raster_pixels_const(got.r);
  const unsigned keep = colour ? 3u : 1u;
  out_pixels.clear();
  for (uint32_t y = 0; y < 48u; y++) {
    for (uint32_t x = 0; x < 64u; x++) {
      for (unsigned c = 0; c < keep; c++) {
        out_pixels.push_back(
            px[y * stride + x * fmt->channel_count + (colour ? c : 0u)]);
      }
    }
  }
  return ::testing::AssertionSuccess();
}

/** The source's own samples, in the order pyramid_round_trip returns them. */
std::vector<uint8_t> pyramid_source_pixels(bool colour) {
  GIMG_Raster * r = pyramid_source(colour);
  std::vector<uint8_t> out;
  if (!r) { return out; }
  const size_t stride = gimg_raster_stride_bytes(r);
  const auto * px = (const unsigned char *)gimg_raster_pixels_const(r);
  const unsigned ch = colour ? 4u : 1u;
  const unsigned keep = colour ? 3u : 1u;
  for (uint32_t y = 0; y < 48u; y++) {
    for (uint32_t x = 0; x < 64u; x++) {
      for (unsigned c = 0; c < keep; c++) {
        out.push_back(px[y * stride + x * ch + c]);
      }
    }
  }
  gimg_raster_destroy(r);
  return out;
}

long worst_difference(
    const std::vector<uint8_t> & a, const std::vector<uint8_t> & b) {
  long worst = 0;
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  for (size_t i = 0; i < n; i++) {
    long d = (long)a[i] - (long)b[i];
    if (d < 0) { d = -d; }
    if (d > worst) { worst = d; }
  }
  return worst;
}

} // namespace

/**
 * A hierarchical sequence is the same picture as the flat file, every way it
 * can be written.
 *
 * T.81 Annex J builds the image as a pyramid: a small frame, then
 * differential frames that each double the resolution. B.3.1 requires every
 * frame of a sequence to use the same process, and this encoder writes three
 * of them - sequential, progressive and lossless - each with the Huffman or
 * the arithmetic coder, at one or two levels, in gray or in colour. Eighteen
 * combinations, and the two fixtures in the suite cover two of them: a
 * two-level gray sequential file and a gray lossless one.
 *
 * What is asserted is what the option's own documentation promises. The
 * picture comes back the same size; a lossless sequence reproduces the source
 * **exactly**, because a lossless pyramid that lost anything would not be
 * lossless; and a DCT sequence is no further from the source than the same
 * image written flat, within one step - the pyramid adds a reconstruction,
 * and on this image it costs at most one level out of 255.
 *
 * The size claim is asserted too, and it is the one that turned out to be
 * written down wrong. A DCT sequence is larger than the flat file, as the
 * header said. A *lossless* sequence is not reliably either: on this gradient
 * the differential frames code 18% smaller than a flat lossless file, and on
 * noise 5% larger, because there the pyramid replaces the flat predictor
 * instead of adding to it. The header used to say "and is larger" without
 * qualification.
 */
TEST(JpegEncode, AHierarchicalSequenceIsTheSamePictureHoweverItIsWritten) {
  struct Case {
    const char * what;
    bool colour;
    uint8_t levels;
    uint8_t progressive;
    uint8_t arithmetic;
    uint8_t lossless;
  };
  const Case cases[] = {
      {"gray, one level, sequential", false, 1, 0, 0, 0},
      {"gray, two levels, sequential", false, 2, 0, 0, 0},
      {"gray, one level, progressive", false, 1, 1, 0, 0},
      {"gray, one level, arithmetic", false, 1, 0, 1, 0},
      {"gray, two levels, progressive + arithmetic", false, 2, 1, 1, 0},
      {"gray, one level, lossless", false, 1, 0, 0, 1},
      {"gray, two levels, lossless", false, 2, 0, 0, 1},
      {"gray, one level, lossless + arithmetic", false, 1, 0, 1, 1},
      {"colour, one level, sequential", true, 1, 0, 0, 0},
      {"colour, two levels, sequential", true, 2, 0, 0, 0},
      {"colour, one level, progressive", true, 1, 1, 0, 0},
      {"colour, one level, arithmetic", true, 1, 0, 1, 0},
      {"colour, one level, progressive + arithmetic", true, 1, 1, 1, 0},
      {"colour, one level, lossless", true, 1, 0, 0, 1},
      {"colour, two levels, lossless", true, 2, 0, 0, 1},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    GIMG_Save_Options flat = {};
    flat.quality = 95;
    flat.jpeg_chroma_subsampling = GIMG_JPEG_CHROMA_444;
    flat.jpeg_progressive = c.progressive;
    flat.jpeg_arithmetic = c.arithmetic;
    flat.jpeg_lossless_predictor = c.lossless ? 1u : 0u;
    GIMG_Save_Options pyramid = flat;
    pyramid.jpeg_hierarchical_levels = c.levels;

    std::vector<uint8_t> flat_pixels, pyramid_pixels;
    size_t flat_bytes = 0, pyramid_bytes = 0;
    ASSERT_TRUE(pyramid_round_trip(c.colour, flat, flat_pixels, &flat_bytes));
    ASSERT_TRUE(
        pyramid_round_trip(c.colour, pyramid, pyramid_pixels, &pyramid_bytes));
    const std::vector<uint8_t> source = pyramid_source_pixels(c.colour);
    ASSERT_EQ(pyramid_pixels.size(), source.size());
    ASSERT_EQ(flat_pixels.size(), source.size());

    if (c.lossless) {
      EXPECT_EQ(worst_difference(pyramid_pixels, source), 0)
          << "a lossless pyramid that lost something is not lossless";
      EXPECT_EQ(worst_difference(flat_pixels, source), 0)
          << "the control: a flat lossless file is exact too";
    }
    else {
      const long flat_worst = worst_difference(flat_pixels, source);
      const long pyramid_worst = worst_difference(pyramid_pixels, source);
      EXPECT_LE(pyramid_worst, flat_worst + 2)
          << "the pyramid is " << pyramid_worst << " from the source where "
             "the flat file is " << flat_worst
          << "; a sequence should cost one reconstruction step, not a visible "
             "amount";
      // The pyramid's lower levels are extra data in a DCT sequence.
      EXPECT_GT(pyramid_bytes, flat_bytes)
          << "a DCT sequence carries the smaller frames as well as the full "
             "one, so it cannot be smaller than the flat file";
    }
  }
}

/**
 * The colour transform's clamps never fire, and this is what says so.
 *
 * jpeg_rgb_to_ycbcr_at() clamps its three inputs into 0..max_val and its
 * three outputs the same way - twelve lines that no test could reach, which
 * is a thing to explain rather than leave on a list. The explanation is
 * arithmetic: the luma coefficients sum to exactly 65536, so Y spans the
 * range and no more; and the chroma bias is (center << 16) + 32767 rather
 * than + 32768, which is what stops Cb reaching max_val + 1 when the pixel is
 * pure blue. One unit either way in that constant and the clamp becomes
 * load-bearing.
 *
 * So the clamps stay - a numeric guard removed on the strength of today's
 * constants is a bug waiting on the next person to change them - and this
 * sweeps every 8-bit triple there is, asserting both that the transform's
 * answer is already in range and that it is exactly what the formula below
 * produces.
 *
 * The formula is written out here a second time on purpose. It means a
 * change to the encoder's constants fails this test at the equality check
 * rather than passing silently, and whoever updates this copy to match has
 * to re-run the range assertions with the new numbers - which is the
 * question that actually matters, because a clamp that starts firing does
 * not announce itself. It flattens a colour and returns success.
 *
 * Twelve bits is not swept: each output is a linear form in R, G and B with
 * fixed-sign coefficients, so its extremes over the cube are at a vertex, and
 * the eight vertices are checked here exactly.
 */
TEST(JpegEncode, TheColorTransformNeverNeedsItsClamps) {
  auto unclamped = [](int32_t r, int32_t g, int32_t b, int32_t center,
                       int32_t * y, int32_t * cb, int32_t * cr) {
    const int32_t one_half = 1 << 15;
    const int32_t bias = (center << 16) + one_half - 1;
    *y = (19595 * r + 38470 * g + 7471 * b + one_half) >> 16;
    *cb = (-11059 * r - 21709 * g + 32768 * b + bias) >> 16;
    *cr = (32768 * r - 27439 * g - 5331 * b + bias) >> 16;
  };

  long checked = 0;
  for (int32_t r = 0; r < 256; r++) {
    for (int32_t g = 0; g < 256; g++) {
      for (int32_t b = 0; b < 256; b++) {
        int32_t wy = 0, wcb = 0, wcr = 0;
        unclamped(r, g, b, 128, &wy, &wcb, &wcr);
        // The claim: already in range, so clamping cannot have changed it.
        ASSERT_GE(wy, 0);
        ASSERT_LE(wy, 255);
        ASSERT_GE(wcb, 0);
        ASSERT_LE(wcb, 255);
        ASSERT_GE(wcr, 0);
        ASSERT_LE(wcr, 255);
        uint8_t y = 0, cb = 0, cr = 0;
        jpeg_rgb_to_ycbcr((uint8_t)r, (uint8_t)g, (uint8_t)b, &y, &cb, &cr);
        ASSERT_EQ((int32_t)y, wy) << "at rgb " << r << "," << g << "," << b;
        ASSERT_EQ((int32_t)cb, wcb);
        ASSERT_EQ((int32_t)cr, wcr);
        checked++;
      }
    }
  }
  ASSERT_EQ(checked, 256L * 256L * 256L);

  // Twelve bits, at the eight vertices of the cube.
  for (int32_t r : {0, 4095}) {
    for (int32_t g : {0, 4095}) {
      for (int32_t b : {0, 4095}) {
        int32_t y = 0, cb = 0, cr = 0;
        unclamped(r, g, b, 2048, &y, &cb, &cr);
        for (int32_t v : {y, cb, cr}) {
          EXPECT_GE(v, 0) << "at rgb " << r << "," << g << "," << b;
          EXPECT_LE(v, 4095) << "at rgb " << r << "," << g << "," << b;
        }
      }
    }
  }
}

/**
 * A frame may carry any number of components from 1 to 255.
 *
 * T.81 B.2.2 says so and attaches no meaning past four, and this decoder
 * agrees: it has an arm for "two components, or five to 255" that hands the
 * samples back as they came in. The writer did not. Its twelve-bit half
 * allocates three named planes - luma and two chroma - and asked for the
 * second and third together, under `num_components >= 3`. A two-component
 * frame got one plane, and then wrote every second sample through the NULL
 * pointer where the other should have been.
 *
 * It is a segfault on a save, from a raster the library will hand you: a
 * two-channel 16-bit raster is what gimg_pixel_format_multichannel builds,
 * and the eight-bit writer accepts the same document without complaint. The
 * eight-bit path allocates one plane per component in a loop, which is why
 * it never had the bug - the same job written twice, and only one of the two
 * counting correctly.
 *
 * So this sweeps the component counts either side of the named ones at both
 * precisions. Values are held to a mean error rather than exactly, because
 * the DCT is lossy at any quality; that is still enough to catch a component
 * written into another component's plane, which is the quiet version of what
 * crashed here.
 */
TEST(JpegEncode, AFrameCarriesAsManyComponentsAsItSays) {
  const uint32_t w = 16, h = 9;
  long written = 0;
  for (int num_components = 1; num_components <= 6; num_components++) {
    for (int precision : {8, 12}) {
      SCOPED_TRACE("components " + std::to_string(num_components) +
          ", precision " + std::to_string(precision));
      GIMG_Pixel_Format fmt = {};
      ASSERT_EQ(gimg_pixel_format_multichannel((uint8_t)num_components,
                    (uint8_t)(precision == 12 ? 16 : 8), &fmt),
          GIMG_OK);

      GIMG_Doc * doc = nullptr;
      ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
      GIMG_Raster * raster = nullptr;
      ASSERT_EQ(gimg_raster_create(
                    w, h, &fmt, GIMG_RASTER_OWNED, NULL, 0, &raster),
          GIMG_OK);
      const size_t bpp = gimg_raster_bytes_per_pixel(&fmt);
      const size_t stride = gimg_raster_stride_bytes(raster);
      auto * px = (unsigned char *)gimg_raster_pixels(raster);
      // Each component gets its own gradient, so a component written into
      // another one's plane is a different picture and not a coincidence.
      std::vector<int> want((size_t)w * h * (size_t)num_components);
      for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
          for (int c = 0; c < num_components; c++) {
            const unsigned raw = (x * 23u + y * 47u + (unsigned)c * 91u);
            const int v =
                (int)(precision == 12 ? (raw & 0xFFFu) : (raw & 0xFFu));
            want[((size_t)y * w + x) * (size_t)num_components + (size_t)c] = v;
            unsigned char * p = px + y * stride + x * bpp;
            if (precision == 12) {
              // A 12-bit sample is left-justified in a 16-bit raster.
              ((uint16_t *)(void *)p)[c] = (uint16_t)(v << 4);
            }
            else {
              p[c] = (unsigned char)v;
            }
          }
        }
      }
      gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

      GIMG_Stream * os = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&os), GIMG_OK);
      GIMG_Save_Options so = {};
      so.metadata_policy = GIMG_META_DROP_ALL;
      so.quality = 95;
      so.jpeg_precision = (uint8_t)precision;
      GIMG_Save_Report rep = {};
      const GIMG_Result sr = gimg_doc_save(doc, os, "jpeg", &so, &rep);
      ASSERT_EQ(sr, GIMG_OK) << "a frame this many components wide is legal";
      const void * buf = nullptr;
      size_t bn = 0;
      gimg_stream_output_buffer(os, &buf, &bn);
      const std::vector<uint8_t> file(
          (const uint8_t *)buf, (const uint8_t *)buf + bn);
      gimg_doc_destroy(doc);
      gimg_stream_destroy(os);

      DocStreamGuard in;
      ASSERT_EQ(
          gimg_stream_create_memory(file.data(), file.size(), &in.s), GIMG_OK);
      ASSERT_EQ(gimg_doc_load(in.s, nullptr, nullptr, &in.d), GIMG_OK);
      RasterGuard got;
      ASSERT_EQ(
          gimg_item_decode(gimg_doc_item(in.d, 0), nullptr, &got.r), GIMG_OK);
      ASSERT_NE(got.r, nullptr);
      EXPECT_EQ(gimg_raster_width(got.r), w);
      EXPECT_EQ(gimg_raster_height(got.r), h);

      // And again with the box filter named explicitly.  These two arms of
      // the decoder - the eight-bit one and the extended one - are the only
      // places the upsampling option was never read: nothing but this test
      // produces a frame with two components or with five, so nothing but
      // this test can hand those arms a GIMG_Decode_Options at all, and it
      // was decoding with NULL.  A component here is written at 1x1, so
      // there is nothing to upsample and the two filters must agree exactly;
      // what that pins is that the SIMPLE branch of these arms exists and
      // emits the same picture, rather than being unreachable code.
      GIMG_Decode_Options simple = {};
      simple.jpeg_chroma_upsampling = GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE;
      RasterGuard box;
      ASSERT_EQ(
          gimg_item_decode(gimg_doc_item(in.d, 0), &simple, &box.r), GIMG_OK);
      ASSERT_NE(box.r, nullptr);
      EXPECT_EQ(jpeg_test::raster_pixel_hash(box.r),
          jpeg_test::raster_pixel_hash(got.r))
          << "nothing is subsampled here, so the box filter and the triangle "
             "filter must draw the same picture";

      // One and three components are named shapes - gray and YCbCr - and come
      // back as the library's gray and RGBA rasters. The rest have no
      // convention, so the count is carried through as it arrived.
      const GIMG_Pixel_Format * gf = gimg_raster_format(got.r);
      if (num_components != 1 && num_components != 3 && num_components != 4) {
        ASSERT_EQ(gf->channel_count, (uint8_t)num_components);
        const auto * gp = (const unsigned char *)gimg_raster_pixels_const(got.r);
        const size_t gs = gimg_raster_stride_bytes(got.r);
        const size_t gbpp = gimg_raster_bytes_per_pixel(gf);
        const int scale = (precision == 12) ? 16 : 1; // 12 bits in 16
        double err = 0.0;
        for (uint32_t y = 0; y < h; y++) {
          for (uint32_t x = 0; x < w; x++) {
            const unsigned char * p = gp + y * gs + x * gbpp;
            for (int c = 0; c < num_components; c++) {
              const int v = (gf->bits_per_channel[0] == 16)
                  ? (int)(((const uint16_t *)(const void *)p)[c] / scale)
                  : (int)p[c];
              const int expect =
                  want[((size_t)y * w + x) * (size_t)num_components +
                      (size_t)c];
              err += (v > expect) ? (v - expect) : (expect - v);
            }
          }
        }
        const double max_sample = (precision == 12) ? 4095.0 : 255.0;
        const double mean = err / (double)want.size() / max_sample;
        EXPECT_LT(mean, 0.02)
            << "mean error " << mean << " of full scale: a component came "
               "back as something other than the one that went in";
      }
      written++;
    }
  }
  ASSERT_EQ(written, 12L);
}

namespace {

/**
 * The longest codeword an unrestricted Huffman code would give these
 * frequencies.
 *
 * The test needs this to say for itself that an input requires the length
 * limiter at all. Without it, "the table came back with nothing longer than
 * sixteen bits" is true of every input, including the ones that never needed
 * limiting - which is the same as asserting nothing.
 */
int longest_unlimited_codeword(const std::vector<uint32_t> & freq) {
  struct Node {
    uint64_t w;
    int left = -1, right = -1;
  };
  std::vector<Node> nodes;
  std::vector<int> live;
  for (size_t i = 0; i < freq.size(); i++) {
    if (freq[i] != 0) {
      nodes.push_back({freq[i], -1, -1});
      live.push_back((int)nodes.size() - 1);
    }
  }
  if (live.size() < 2) { return (int)live.size(); }
  while (live.size() > 1) {
    // Two smallest, by weight.
    size_t a = 0;
    for (size_t k = 1; k < live.size(); k++) {
      if (nodes[live[k]].w < nodes[live[a]].w) { a = k; }
    }
    const int na = live[a];
    live.erase(live.begin() + (long)a);
    size_t b = 0;
    for (size_t k = 1; k < live.size(); k++) {
      if (nodes[live[k]].w < nodes[live[b]].w) { b = k; }
    }
    const int nb = live[b];
    live.erase(live.begin() + (long)b);
    nodes.push_back({nodes[na].w + nodes[nb].w, na, nb});
    live.push_back((int)nodes.size() - 1);
  }
  // Depth of the deepest leaf.
  int best = 0;
  std::vector<std::pair<int, int>> stack{{live[0], 0}};
  while (!stack.empty()) {
    const auto [id, depth] = stack.back();
    stack.pop_back();
    if (nodes[id].left < 0) {
      if (depth > best) { best = depth; }
      continue;
    }
    stack.push_back({nodes[id].left, depth + 1});
    stack.push_back({nodes[id].right, depth + 1});
  }
  return best;
}

/** A DHT payload built from what jpeg_gen_huff_table() produced. */
std::vector<unsigned char> dht_payload_from(
    const unsigned char bits[17], const unsigned char * vals, int n) {
  std::vector<unsigned char> out;
  out.push_back(0x00);  // Tc=0 Th=0
  for (int L = 1; L <= 16; L++) { out.push_back(bits[L]); }
  for (int i = 0; i < n; i++) { out.push_back(vals[i]); }
  return out;
}

} // namespace

/**
 * A frequency distribution that wants codewords longer than sixteen bits gets
 * a table the decoder will take.
 *
 * T.81 allows no codeword longer than sixteen bits, and an optimal Huffman
 * code over skewed enough statistics wants them. Figure K.3's loop trades
 * depth away until none is left - seven lines that no image in the suite had
 * ever reached, because it takes frequencies spanning a factor of thousands
 * before the optimal code runs that deep.
 *
 * Fibonacci weights are the classic shape that does it: each is the sum of the
 * two before, which is exactly the merge the algorithm performs, so every step
 * deepens the same branch. The first assertion establishes that - the input is
 * one an unrestricted code answers with more than sixteen bits - because
 * otherwise the rest holds trivially for any input at all.
 *
 * The last assertion is the one that ties this to the decoder: the table is
 * assembled into a DHT payload and handed to jpeg_build_huff_table(), which
 * refuses an over-subscribed table and, since the reserved-codeword fix, one
 * that uses the all-ones codeword as well. That the limiter's output survives
 * that check is a property of the two halves together, and neither test alone
 * would notice it breaking.
 */
/**
 * A frequency distribution that wants codewords longer than sixteen bits gets
 * a table the decoder will take.
 *
 * T.81 allows no codeword longer than sixteen bits, and an optimal Huffman
 * code over skewed enough statistics wants them. Figure K.3's loop trades
 * depth away until none is left - seven lines that no image in the suite had
 * ever reached, because it takes frequencies spanning a factor of millions
 * before the optimal code runs that deep.
 *
 * The weights are each at least the sum of all before them, which forces a
 * fully degenerate tree: 26 symbols, so an unrestricted code would reach 25
 * bits. Measured inside the generator, this input takes 37 trades to bring
 * down from a longest codeword of 26.
 *
 * Fibonacci weights were the first attempt and are the more famous worst case,
 * and they do not work here: this generator answers them with a longest
 * codeword of 13 where an optimal Huffman code uses 23, at a cost of twelve
 * bits out of 317,783. So an independent Huffman is not a predictor of what
 * this generator will do, and a test that asserted "the optimal code needs
 * more than sixteen bits" as its premise passed while the limiter never ran.
 * The premise here is the output instead: symbols piled at exactly sixteen
 * bits are what limiting leaves behind, and the flat control has none.
 *
 * The last assertion is the one that ties this to the decoder: the table is
 * assembled into a DHT payload and handed to jpeg_build_huff_table(), which
 * refuses an over-subscribed table and, since the reserved-codeword fix, one
 * that uses the all-ones codeword as well. That the limiter's output survives
 * that check is a property of the two halves together, and neither test alone
 * would notice it breaking.
 */
TEST(JpegEncode, ATableWantingLongCodewordsIsBroughtInsideSixteenBits) {
  std::vector<uint32_t> skewed(257, 0);
  skewed[0] = 1;
  for (int i = 1; i < 26; i++) { skewed[(size_t)i] = 1u << (i - 1); }

  unsigned char bits[17] = {0};
  unsigned char vals[256] = {0};
  int n = 0;
  std::vector<uint32_t> scratch = skewed;  // the generator consumes it
  jpeg_gen_huff_table(scratch.data(), 256, bits, vals, &n);

  EXPECT_EQ(n, 26) << "every symbol with a frequency keeps a codeword";
  int total = 0;
  for (int L = 1; L <= 16; L++) { total += bits[L]; }
  EXPECT_EQ(total, n)
      << "the counts and the value list must agree, or the table names "
         "symbols it gave no codeword to";
  EXPECT_GT(bits[16], 1)
      << "limiting leaves symbols piled at the longest length it allows; one "
         "or none here would mean the loop never ran";

  const std::vector<unsigned char> payload = dht_payload_from(bits, vals, n);
  gimg_jpeg_huff_table_t tbl;
  memset(&tbl, 0, sizeof tbl);
  EXPECT_EQ(jpeg_build_huff_table(payload.data(), payload.size(), &tbl), 0)
      << "the decoder refuses the table this encoder just generated";

  // Control: weights that need no limiting. Same generator, same checks, and
  // nothing piled at sixteen - which is what makes the assertion above about
  // limiting rather than about tables in general.
  std::vector<uint32_t> flat(257, 0);
  for (int i = 0; i < 16; i++) { flat[(size_t)i] = 100; }
  ASSERT_LE(longest_unlimited_codeword(flat), 16);
  unsigned char fbits[17] = {0};
  unsigned char fvals[256] = {0};
  int fn = 0;
  scratch = flat;
  jpeg_gen_huff_table(scratch.data(), 256, fbits, fvals, &fn);
  EXPECT_EQ(fbits[16], 0) << "control: this distribution needs no limiting";
  const std::vector<unsigned char> fpayload = dht_payload_from(fbits, fvals, fn);
  memset(&tbl, 0, sizeof tbl);
  EXPECT_EQ(jpeg_build_huff_table(fpayload.data(), fpayload.size(), &tbl), 0)
      << "control: an ordinary distribution builds a table the decoder takes";
}

/**
 * The table generator answers a degenerate request without inventing a table.
 *
 * Two shapes reach it and neither comes from an image. A symbol count outside
 * 1..256 is a caller error - T.81 B.2.4.2 allows no more - and frequencies
 * that are all zero are a scan that coded nothing, which a hierarchical or
 * differential frame can produce when a component's every block is empty.
 *
 * Both must come back saying so rather than half-filled: a `bits` array left
 * holding whatever was on the stack would be written into a DHT segment as if
 * it meant something.
 *
 * The control is a distribution that does define a table, so that "out_n is
 * zero" is a statement about these inputs and not about the generator.
 */
TEST(JpegEncode, AGeneratorAskedForNoTableReturnsNone) {
  unsigned char bits[17];
  unsigned char vals[256];
  int n = -1;

  // Frequencies all zero: only the generator's own reserved symbol is live,
  // and one symbol is not a code.
  std::vector<uint32_t> none(257, 0);
  memset(bits, 0xAA, sizeof bits);
  n = -1;
  jpeg_gen_huff_table(none.data(), 256, bits, vals, &n);
  EXPECT_EQ(n, 0) << "no frequency means no symbol to give a codeword to";
  for (int L = 1; L <= 16; L++) {
    EXPECT_EQ(bits[L], 0) << "length " << L << " must be cleared, not left "
                             "holding what was on the stack";
  }

  // A symbol count T.81 does not allow, either side.
  for (int bad : {0, 257, -1}) {
    std::vector<uint32_t> freq(300, 7);
    memset(bits, 0xAA, sizeof bits);
    n = -1;
    jpeg_gen_huff_table(freq.data(), bad, bits, vals, &n);
    EXPECT_EQ(n, 0) << "num_symbols " << bad << " is not a table";
    for (int L = 1; L <= 16; L++) {
      EXPECT_EQ(bits[L], 0) << "num_symbols " << bad << ", length " << L;
    }
  }

  // Control: an ordinary request still produces a table.
  std::vector<uint32_t> ok(257, 0);
  for (int i = 0; i < 8; i++) { ok[(size_t)i] = (uint32_t)(i + 1); }
  memset(bits, 0xAA, sizeof bits);
  n = -1;
  jpeg_gen_huff_table(ok.data(), 256, bits, vals, &n);
  EXPECT_EQ(n, 8) << "control: eight frequencies, eight codewords";
  int total = 0;
  for (int L = 1; L <= 16; L++) { total += bits[L]; }
  EXPECT_EQ(total, n);
}

namespace {

/** Save a small gradient as JPEG under @p options and hand back the bytes. */
::testing::AssertionResult save_gradient_jpeg(
    const GIMG_Save_Options & options, std::vector<uint8_t> & out) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK) {
    return ::testing::AssertionFailure() << "doc";
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(64, 64, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr,
          0, &raster) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return ::testing::AssertionFailure() << "raster";
  }
  auto * px = (unsigned char *)gimg_raster_pixels(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < 64; y++) {
    for (uint32_t x = 0; x < 64; x++) {
      unsigned char * p = px + y * stride + x * 4u;
      p[0] = (unsigned char)(x * 4u);
      p[1] = (unsigned char)(y * 4u);
      p[2] = (unsigned char)((x * 3u + y * 5u) & 0xFFu);
      p[3] = 255;
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * os = nullptr;
  if (gimg_stream_create_memory_output(&os) != GIMG_OK) {
    gimg_doc_destroy(doc);
    return ::testing::AssertionFailure() << "stream";
  }
  GIMG_Save_Report report = {};
  const GIMG_Result r = gimg_doc_save(doc, os, "jpeg", &options, &report);
  if (r == GIMG_OK) {
    const void * buf = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(os, &buf, &n);
    out.assign((const uint8_t *)buf, (const uint8_t *)buf + n);
  }
  gimg_stream_destroy(os);
  gimg_doc_destroy(doc);
  return r == GIMG_OK ? ::testing::AssertionSuccess()
                      : ::testing::AssertionFailure() << "save: " << (int)r;
}

} // namespace

// jpeg_fdct_method and jpeg_quant_method change nothing about the output.
//
// The header used to describe both as choices between implementations to
// compare against each other, which is true of neither:
//
//   - GIMG_JPEG_FDCT_REF names a reference float transform that does not
//     exist.  The value is accepted, carried through gimg_jpeg_save() into
//     the block encoder, and discarded there with a cast to void.
//   - GIMG_JPEG_QUANT_DIV does run - both arms of that branch are taken -
//     but the reciprocal form is an exact division by multiplication rather
//     than an approximation, so the two quantize every coefficient alike.
//
// So the four combinations write byte-identical files, and this pins that
// over the settings that change what the encoder does around them: every
// quality, baseline and progressive, and all three subsampling modes.  It is
// not an endorsement - either option could be made to mean something - but
// until then a caller reaching for one should find out here rather than by
// measuring their own output and finding no difference.
TEST(JpegEncode, TheFdctAndQuantizationMethodOptionsChangeNoOutput) {
  long compared = 0;
  for (int quality = 1; quality <= 100; quality += 9) {
    for (int progressive = 0; progressive < 2; progressive++) {
      for (uint8_t sub : {(uint8_t)GIMG_JPEG_CHROMA_420,
               (uint8_t)GIMG_JPEG_CHROMA_422, (uint8_t)GIMG_JPEG_CHROMA_444}) {
        std::vector<uint8_t> baseline_bytes;
        for (uint8_t fdct : {(uint8_t)GIMG_JPEG_FDCT_LOEFFLER,
                 (uint8_t)GIMG_JPEG_FDCT_REF}) {
          for (uint8_t quant : {(uint8_t)GIMG_JPEG_QUANT_RECIP,
                   (uint8_t)GIMG_JPEG_QUANT_DIV}) {
            SCOPED_TRACE("quality " + std::to_string(quality) +
                (progressive ? ", progressive" : ", baseline") +
                ", subsampling " + std::to_string((int)sub) + ", fdct " +
                std::to_string((int)fdct) + ", quant " +
                std::to_string((int)quant));
            GIMG_Save_Options o = {};
            o.metadata_policy = GIMG_META_DROP_ALL;
            o.quality = (uint8_t)quality;
            o.jpeg_progressive = (uint8_t)progressive;
            o.jpeg_chroma_subsampling = sub;
            o.jpeg_fdct_method = fdct;
            o.jpeg_quant_method = quant;
            std::vector<uint8_t> bytes;
            ASSERT_TRUE(save_gradient_jpeg(o, bytes));
            ASSERT_FALSE(bytes.empty());
            if (baseline_bytes.empty()) {
              baseline_bytes = bytes;
              continue;
            }
            EXPECT_EQ(bytes, baseline_bytes)
                << "this combination wrote a different file, so one of these "
                   "options has started to mean something and the header "
                   "saying it does not is now wrong";
            compared++;
          }
        }
      }
    }
  }
  EXPECT_EQ(compared, 12L * 2L * 3L * 3L)
      << "every combination must have been compared, or the sweep is not as "
         "wide as it says";
}
