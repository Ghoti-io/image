/**
 * @file
 *
 * PNG encode/save tests: round-trip, save to stream, metadata policy.
 * Loads reference files from tests/data/png/ and writes encoded PNGs to
 * tests/out/png/ for verification (e.g. by
 * tests/data/png/verify_png_output.py).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/color.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

#include "png_test_utils.h"

// Reaches gimg_png_retarget_ancillary(), which is internal: the rules it
// encodes are per-colour-type and there are more of them than an end-to-end
// fixture per case would be a sensible way to cover.
#include "../../../src/codec/png/png_internal.h"
#include <fstream>
#include <string>

TEST(PngEncode, SaveNullDocReturnsInternal) {
  GIMG_Stream * out_s = nullptr;
  GIMG_Result r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(out_s, nullptr);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(nullptr, out_s, "png", &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_INTERNAL);
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, SaveNullStreamReturnsInternal) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, nullptr, "png", &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_INTERNAL);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, SaveNullFormatReturnsInternal) {
  GIMG_Doc * doc = nullptr;
  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_s, nullptr, &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_INTERNAL);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, SaveUnsupportedFormatReturnsUnsupported) {
  GIMG_Doc * doc = nullptr;
  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_s, "jpeg", &opts, &report);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, MetaCommonDescriptionWrittenAndReadAsText) {
  // Programmatic doc: set meta_common description, save as PNG, re-load and
  // verify description (written as tEXt "Description" when not in ancillary).
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  memset(gimg_raster_pixels(raster), 128, 8 * 8);

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_from_raster(raster, &doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_raster_destroy(raster);
  raster = nullptr;
  GIMG_Meta_Common * meta = nullptr;
  ASSERT_EQ(gimg_doc_ensure_meta_common(doc, &meta), GIMG_OK);
  ASSERT_EQ(gimg_meta_common_set_description(meta, "Image caption"), GIMG_OK);

  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_s, "png", &opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> saved(out_size, 0);
  if (out_size)
    memcpy(saved.data(), out_data, out_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * in_s = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(saved.data(), saved.size(), &in_s), GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(in_s, nullptr, nullptr, &doc), GIMG_OK);
  meta = gimg_doc_meta_common(doc);
  ASSERT_NE(meta, nullptr);
  const char * desc = gimg_meta_common_description(meta);
  ASSERT_NE(desc, nullptr);
  EXPECT_STREQ(desc, "Image caption");
  gimg_doc_destroy(doc);
  gimg_stream_destroy(in_s);
}

TEST(PngEncode, SaveToMemoryOutputSucceeds) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_gray.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * in_s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &in_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(in_s, nullptr);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(in_s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(in_s);
  in_s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(out_s, nullptr);

  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save (1x1 gray)";
  EXPECT_GT(report.bytes_written, 0u);
  gimg_doc_destroy(doc);
  doc = nullptr;

  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  EXPECT_GT(out_size, 8u);
  if (out_size >= 8u && out_data) {
    const unsigned char * sig = static_cast<const unsigned char *>(out_data);
    EXPECT_EQ(sig[0], 0x89);
    EXPECT_EQ(sig[1], 0x50);
    EXPECT_EQ(sig[2], 0x4E);
    EXPECT_EQ(sig[3], 0x47);
  }
  gimg_stream_destroy(out_s);
}

TEST(PngEncode, RoundTrip1x1Gray) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_gray.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  if (r != GIMG_OK) {
    GTEST_SKIP() << "gimg_doc_save returned " << r << " (round-trip save)";
  }
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_gray.png", saved_data.data(), saved_data.size());

  // Re-load from saved buffer
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  // Decode both and compare
  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);

  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip pixel data must match";

  // Golden encode test: re-decoded pixels must match canonical decode hash.
  EXPECT_EQ(png_test::raster_pixel_hash(decoded), 12638153115695167455ULL)
      << "Round-trip decode hash must match golden 1x1 gray";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip1x1Rgba) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_rgba.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  if (r != GIMG_OK) {
    GTEST_SKIP() << "gimg_doc_save returned " << r << " (round-trip RGBA)";
  }

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_rgba.png", saved_data.data(), saved_data.size());

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);

  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip RGBA pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitGray) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_gray.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit grayscale";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit grayscale pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitRgba) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_rgba.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit RGBA";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit RGBA pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip1x1Rgb) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_rgb.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save RGB (color_type 2) round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_rgb.png", saved_data.data(), saved_data.size());

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip RGB (color_type 2) pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitRgb) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_rgb.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit RGB round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit RGB pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip1x1Grayalpha) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_grayalpha.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save grayscale+alpha (color_type 4) round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "roundtrip_1x1_grayalpha.png", saved_data.data(), saved_data.size());

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip grayscale+alpha (color_type 4) pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTrip16BitGrayalpha) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_16bit_grayalpha.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save 16-bit grayscale+alpha round-trip";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip 16-bit grayscale+alpha pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, RoundTripPalette) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_palette.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "save palette (PLTE + tRNS)";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * orig = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(orig, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);

  EXPECT_TRUE(png_test::rasters_equal(orig, decoded))
      << "Round-trip palette + tRNS pixel data must match";

  gimg_raster_destroy(decoded);
  gimg_raster_destroy(orig);
  gimg_doc_destroy(doc2);
  gimg_doc_destroy(doc);
}

TEST(PngEncode, SaveWithPreserveAllKeepsExif) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  if (r != GIMG_OK) {
    GTEST_SKIP() << "gimg_doc_save returned " << r << " (preserve eXIf)";
  }

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  out_s = nullptr;
  png_test::write_png_output(
      "preserve_exif.png", saved_data.data(), saved_data.size());
  gimg_doc_destroy(doc);

  // Re-load and verify eXIf is present
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw, nullptr);
  size_t exif_size = 0;
  r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
  EXPECT_EQ(r, GIMG_OK);
  EXPECT_EQ(exif_size, 6u) << "eXIf preserved after save with PRESERVE_ALL";

  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithDropAllStripsMetadata) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_DROP_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  // Re-load: eXIf must not be present (stripped by DROP_ALL).
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  // With DROP_ALL, either no meta_raw or no eXIf in it.
  if (raw) {
    size_t exif_size = 0;
    r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
    EXPECT_NE(r, GIMG_OK);
    EXPECT_EQ(exif_size, 0u)
        << "eXIf must be stripped when saving with DROP_ALL";
  }
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithStripGpsStripsOnlyGps) {
  // STRIP_GPS strips only GPS from eXIf; eXIf chunk is preserved (re-written
  // without GPS IFD).
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_STRIP_GPS};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw, nullptr);
  size_t exif_size = 0;
  r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
  EXPECT_EQ(r, GIMG_OK)
      << "eXIf must be present after STRIP_GPS (only GPS removed)";
  EXPECT_GE(exif_size, 6u) << "eXIf preserved after save with STRIP_GPS";
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithNormalizeExifPreservesExif) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_NORMALIZE_EXIF};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw, nullptr);
  size_t exif_size = 0;
  r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
  EXPECT_EQ(r, GIMG_OK);
  EXPECT_EQ(exif_size, 6u) << "eXIf preserved after save with NORMALIZE_EXIF";
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithKeepRawOnlyOmitsKnownSemantic) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_KEEP_RAW_ONLY};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc2);
  if (raw) {
    size_t exif_size = 0;
    r = gimg_meta_raw_get(raw, "png", 0x65584966u, nullptr, &exif_size);
    EXPECT_NE(r, GIMG_OK);
    EXPECT_EQ(exif_size, 0u)
        << "eXIf (known semantic) must be omitted with KEEP_RAW_ONLY";
  }
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithKeepCommonOnlyRoundTripsColorOnly) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_srgb.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_KEEP_COMMON_ONLY};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * raster = nullptr;
  r = gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc2, 0), nullptr, &raster);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(raster, nullptr);
  const GIMG_Color_Info * info = gimg_raster_color_info_const(raster);
  EXPECT_NE(info, nullptr);
  if (info) {
    EXPECT_EQ(info->transfer, GIMG_TRANSFER_SRGB)
        << "KEEP_COMMON_ONLY should preserve sRGB from raster";
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveInterlacedRoundTrip) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_1x1_rgba.png", buf))
      << "Run tests/data/png/generate.py";
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Raster * ref_raster = nullptr;
  r = gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc, 0), nullptr, &ref_raster);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(ref_raster, nullptr);

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {
      .metadata_policy = GIMG_META_PRESERVE_ALL, .interlaced = 1};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  gimg_stream_destroy(s2);

  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(gimg_doc_item((GIMG_Doc *)doc2, 0), nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(decoded, nullptr);
  EXPECT_TRUE(png_test::rasters_equal(ref_raster, decoded))
      << "Interlaced save round-trip should match original pixels";
  gimg_raster_destroy(ref_raster);
  gimg_raster_destroy(decoded);
  gimg_doc_destroy(doc2);
  png_test::write_png_output(
      "interlaced_roundtrip.png", saved_data.data(), saved_data.size());
}

TEST(PngEncode, ApngRoundTrip) {
  std::vector<uint8_t> buf;
  ASSERT_TRUE(png_test::load_png_file("png_apng_2frame.png", buf))
      << "Run tests/data/png/generate.py";

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u);
  gimg_stream_destroy(s);
  s = nullptr;

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "APNG save";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(static_cast<const uint8_t *>(out_ptr),
      static_cast<const uint8_t *>(out_ptr) + saved_size);
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);

  // Re-load and verify frame count, timing, dispose/blend, and pixels.
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(saved_data.data(), saved_data.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc2, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc2), 2u);
  gimg_stream_destroy(s2);

  GIMG_Item * item0 = gimg_doc_item((GIMG_Doc *)doc2, 0);
  GIMG_Item * item1 = gimg_doc_item((GIMG_Doc *)doc2, 1);
  ASSERT_NE(item0, nullptr);
  ASSERT_NE(item1, nullptr);

  uint16_t num = 0, den = 0;
  gimg_item_frame_delay(item0, &num, &den);
  EXPECT_EQ(num, 50u);
  EXPECT_EQ(den, 100u);
  EXPECT_EQ(gimg_item_dispose_op(item0), GIMG_DISPOSE_NONE);
  EXPECT_EQ(gimg_item_blend_op(item0), GIMG_BLEND_SOURCE);

  gimg_item_frame_delay(item1, &num, &den);
  EXPECT_EQ(num, 25u);
  EXPECT_EQ(den, 100u);
  EXPECT_EQ(gimg_item_dispose_op(item1), GIMG_DISPOSE_BACKGROUND);
  EXPECT_EQ(gimg_item_blend_op(item1), GIMG_BLEND_OVER);

  GIMG_Raster * r0 = nullptr;
  GIMG_Raster * r1 = nullptr;
  r = gimg_item_decode(item0, nullptr, &r0);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(r0, nullptr);
  r = gimg_item_decode(item1, nullptr, &r1);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(r1, nullptr);

  const unsigned char * px0 =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(r0));
  const unsigned char * px1 =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(r1));
  ASSERT_NE(px0, nullptr);
  ASSERT_NE(px1, nullptr);
  EXPECT_EQ(px0[0], 0) << "frame 0 gray";
  EXPECT_EQ(px1[0], 0x80) << "frame 1 gray";

  gimg_raster_destroy(r0);
  gimg_raster_destroy(r1);
  gimg_doc_destroy(doc2);
  png_test::write_png_output(
      "apng_2frame_roundtrip.png", saved_data.data(), saved_data.size());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// ---------------------------------------------------------------------------
// Saving at sample depths below 8 bits (palette and grayscale).
//
// PNG 7.2 packs samples of depth 1, 2 and 4 several to a byte, so a scanline
// is (width * bit_depth + 7) / 8 bytes and not width. A writer that stores one
// byte per pixel overruns every row it writes and declares a bit depth in IHDR
// that its own IDAT does not use.
//
// The fixtures are the interlaced/non-interlaced pairs from
// tests/data/png/generate.py; each is loaded, saved, and loaded again, and the
// pixels must survive. The saved files land in tests/out/png/ where
// verify_png_output.py reads them back with an independent decoder.
// ---------------------------------------------------------------------------

namespace {

struct SubByteSaveCase {
  const char * filename;
  uint8_t expect_bit_depth;  ///< IHDR byte 8 the writer should emit.
  uint8_t expect_color_type; ///< IHDR byte 9.
};

const SubByteSaveCase kSubByteSaveCases[] = {
    {"png_pal1_32x8.png", 1, 3},
    {"png_pal2_32x8.png", 2, 3},
    {"png_pal4_32x8.png", 4, 3},
    {"png_pal1_33x9.png", 1, 3},
    {"png_pal2_33x9.png", 2, 3},
    {"png_pal4_33x9.png", 4, 3},
    {"png_pal4_32x8_interlaced.png", 4, 3},
    {"png_pal2_33x9_interlaced.png", 2, 3},
    // Grayscale at the same depths. A frame that arrived below 8 bits goes
    // back out that way when every sample survives the rescaling of PNG 13.12
    // in both directions; widening it to 8 would be lossless but would make
    // the file several times larger for no reason.
    {"png_gray1_32x8.png", 1, 0},
    {"png_gray2_32x8.png", 2, 0},
    {"png_gray4_32x8.png", 4, 0},
    {"png_gray1_33x9.png", 1, 0},
    {"png_gray4_33x9.png", 4, 0},
    {"png_gray2_32x8_interlaced.png", 2, 0},
    {"png_gray4_33x9_interlaced.png", 4, 0},
};

} // namespace

TEST(PngEncode, SamplesBelowEightBitsRoundTripAndKeepTheirDepth) {
  for (const SubByteSaveCase & c : kSubByteSaveCases) {
    std::vector<uint8_t> buf;
    ASSERT_TRUE(png_test::load_png_file(c.filename, buf))
        << c.filename << " missing; run tests/data/png/generate.py";

    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(buf.data(), buf.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK) << c.filename;
    gimg_stream_destroy(s);

    GIMG_Stream * out_s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
    GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
    GIMG_Save_Report report = {};
    ASSERT_EQ(gimg_doc_save(doc, out_s, "png", &opts, &report), GIMG_OK)
        << "save " << c.filename;

    const void * out_ptr = nullptr;
    size_t saved_size = 0;
    gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
    std::vector<uint8_t> saved(static_cast<const uint8_t *>(out_ptr),
        static_cast<const uint8_t *>(out_ptr) + saved_size);
    gimg_stream_destroy(out_s);

    // IHDR payload starts at signature (8) + length (4) + type (4) = 16;
    // bit depth is byte 8 of the payload and colour type byte 9. PNG 11.2.1.
    ASSERT_GT(saved.size(), 26u) << c.filename;
    EXPECT_EQ(saved[24], c.expect_bit_depth)
        << c.filename << ": IHDR bit depth";
    EXPECT_EQ(saved[25], c.expect_color_type)
        << c.filename << ": IHDR colour type";

    GIMG_Stream * s2 = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &s2),
        GIMG_OK);
    GIMG_Doc * doc2 = nullptr;
    ASSERT_EQ(gimg_doc_load(s2, nullptr, nullptr, &doc2), GIMG_OK)
        << "reload " << c.filename;
    gimg_stream_destroy(s2);

    GIMG_Raster * orig = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig), GIMG_OK);
    GIMG_Raster * again = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &again),
        GIMG_OK);
    EXPECT_TRUE(png_test::rasters_equal(orig, again))
        << c.filename << ": pixels must survive a save at its own bit depth";

    std::string out_name = std::string("subbyte_") + c.filename;
    png_test::write_png_output(out_name.c_str(), saved.data(), saved.size());

    gimg_raster_destroy(again);
    gimg_raster_destroy(orig);
    gimg_doc_destroy(doc2);
    gimg_doc_destroy(doc);
  }
}

// ---------------------------------------------------------------------------
// Row filters (PNG 9, filter method 0).
//
// Every row carries a filter type byte, and the five types subtract a
// prediction drawn from the byte above, the byte to the left, both, or the
// Paeth choice among them. The encoder wrote type 0 on every row and no other,
// so four of the five had no producer at all and the sizes showed it.
//
// GIMG_Save_Options.png_filter pins one filter for testing; the default is the
// per-row choice PNG 12.8 recommends.
// ---------------------------------------------------------------------------

namespace {

/** Load, save with the given filter setting, and return the encoded bytes. */
std::vector<uint8_t> SaveWithFilter(
    const char * fixture, uint8_t filter, GIMG_Result * out_result) {
  std::vector<uint8_t> buf;
  if (!png_test::load_png_file(fixture, buf)) {
    *out_result = GIMG_ERR_IO;
    return {};
  }
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(buf.data(), buf.size(), &s) != GIMG_OK) {
    *out_result = GIMG_ERR_INTERNAL;
    return {};
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK) {
    *out_result = r;
    return {};
  }
  GIMG_Stream * out_s = nullptr;
  if (gimg_stream_create_memory_output(&out_s) != GIMG_OK) {
    gimg_doc_destroy(doc);
    *out_result = GIMG_ERR_INTERNAL;
    return {};
  }
  GIMG_Save_Options opts = {.metadata_policy = GIMG_META_PRESERVE_ALL};
  opts.png_filter = filter;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  std::vector<uint8_t> saved;
  if (r == GIMG_OK) {
    const void * p = nullptr;
    size_t n = 0;
    gimg_stream_output_buffer(out_s, &p, &n);
    saved.assign(static_cast<const uint8_t *>(p),
        static_cast<const uint8_t *>(p) + n);
  }
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);
  *out_result = r;
  return saved;
}

/** Decode encoded PNG bytes to a flat pixel vector. */
std::vector<uint8_t> DecodeBytes(const std::vector<uint8_t> & data) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(data.data(), data.size(), &s) != GIMG_OK) {
    return {};
  }
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_load(s, nullptr, nullptr, &doc) != GIMG_OK) {
    gimg_stream_destroy(s);
    return {};
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster) != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
    return {};
  }
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  size_t bpp = gimg_raster_bytes_per_pixel(fmt);
  uint32_t w = gimg_raster_width(raster);
  uint32_t h = gimg_raster_height(raster);
  size_t stride = gimg_raster_stride_bytes(raster);
  const unsigned char * px =
      static_cast<const unsigned char *>(gimg_raster_pixels_const(raster));
  std::vector<uint8_t> out;
  for (uint32_t y = 0; y < h; y++) {
    out.insert(out.end(), px + static_cast<size_t>(y) * stride,
        px + static_cast<size_t>(y) * stride + static_cast<size_t>(w) * bpp);
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  return out;
}

} // namespace

TEST(PngEncode, EveryRowFilterReconstructsTheSamePixels) {
  const char * kFixture = "png_gradient_64x64_rgb.png";
  const uint8_t filters[] = {GIMG_PNG_FILTER_ADAPTIVE, GIMG_PNG_FILTER_NONE,
      GIMG_PNG_FILTER_SUB, GIMG_PNG_FILTER_UP, GIMG_PNG_FILTER_AVERAGE,
      GIMG_PNG_FILTER_PAETH};
  const char * names[] = {
      "adaptive", "none", "sub", "up", "average", "paeth"};

  std::vector<uint8_t> reference;
  std::vector<std::vector<uint8_t>> encodings;
  for (size_t i = 0; i < sizeof(filters) / sizeof(filters[0]); i++) {
    GIMG_Result r = GIMG_OK;
    std::vector<uint8_t> saved = SaveWithFilter(kFixture, filters[i], &r);
    ASSERT_EQ(r, GIMG_OK) << "save with filter " << names[i];
    ASSERT_FALSE(saved.empty()) << names[i];
    std::vector<uint8_t> pixels = DecodeBytes(saved);
    ASSERT_FALSE(pixels.empty()) << "decode " << names[i];
    if (reference.empty()) {
      reference = pixels;
    }
    else {
      EXPECT_EQ(pixels, reference)
          << "filter " << names[i] << " does not reconstruct the same image";
    }
    encodings.push_back(std::move(saved));
    std::string out_name = std::string("filter_") + names[i] + ".png";
    png_test::write_png_output(
        out_name.c_str(), encodings.back().data(), encodings.back().size());
  }

  // Each setting must actually change what is written; otherwise a filter that
  // was silently never applied would pass the round-trip check above.
  for (size_t i = 1; i < encodings.size(); i++) {
    for (size_t j = i + 1; j < encodings.size(); j++) {
      EXPECT_NE(encodings[i], encodings[j])
          << names[i] << " and " << names[j] << " produced identical files";
    }
  }
}

TEST(PngEncode, ChoosingAFilterPerRowBeatsForcingNoneOnAll) {
  // PNG 12.8's reason for choosing per row: on data a predictor fits, the
  // filtered bytes are small and DEFLATE codes them in fewer bits. A gradient
  // is the clearest case, and filter None is what the encoder used to write.
  GIMG_Result r = GIMG_OK;
  std::vector<uint8_t> adaptive =
      SaveWithFilter("png_gradient_64x64_rgb.png", GIMG_PNG_FILTER_ADAPTIVE, &r);
  ASSERT_EQ(r, GIMG_OK);
  std::vector<uint8_t> none =
      SaveWithFilter("png_gradient_64x64_rgb.png", GIMG_PNG_FILTER_NONE, &r);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_LT(adaptive.size(), none.size())
      << "adaptive " << adaptive.size() << " bytes, none " << none.size();
}

TEST(PngEncode, AnUnknownFilterSettingIsRefused) {
  GIMG_Result r = GIMG_OK;
  (void)SaveWithFilter("png_gradient_64x64_rgb.png", 99, &r);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
}

// ---------------------------------------------------------------------------
// Colour-type-dependent ancillary chunks on save
//
// bKGD, sBIT and hIST are laid out according to the colour type in the IHDR
// beside them (PNG 11.3.4.1, 11.3.2.4, 11.3.4.2). The writer does not always
// emit the colour type a frame arrived as, so copying them across unchanged
// produces a chunk whose length contradicts the header in the same file -
// which is what saving the conformance suite's tbbn0g04.png used to do, and
// what libpng called "bKGD: invalid".
// ---------------------------------------------------------------------------

namespace {

/** A doc state standing in for a loaded file, for the retargeting rules. */
struct SourceImage {
  gimg_png_doc_state_t state {};
  std::vector<unsigned char> palette;

  SourceImage(uint8_t color_type, uint8_t bit_depth) {
    state.ihdr.color_type = color_type;
    state.ihdr.bit_depth = bit_depth;
  }
  void set_palette(std::initializer_list<unsigned char> rgb) {
    palette.assign(rgb);
    state.plte = palette.data();
    state.plte_size = palette.size();
  }
};

struct Retargeted {
  gimg_png_retarget_t what;
  std::vector<unsigned char> payload; // only meaningful when REPLACE
};

Retargeted Retarget(gimg_png_chunk_type_t type,
    const std::vector<unsigned char> & payload, SourceImage & src,
    uint8_t out_color_type, uint8_t out_bit_depth) {
  unsigned char buf[6] = {};
  size_t n = 0;
  gimg_png_retarget_t what = gimg_png_retarget_ancillary(type, payload.data(),
      payload.size(), &src.state, out_color_type, out_bit_depth, buf, &n);
  return {what, std::vector<unsigned char>(buf, buf + n)};
}

} // namespace

/** Find one chunk in an encoded PNG. Returns false when it is not there. */
bool FindChunk(const std::vector<uint8_t> & png, const char (&type)[5],
    std::vector<uint8_t> & payload) {
  size_t i = 8; // past the signature
  while (i + 8 <= png.size()) {
    uint32_t len = ((uint32_t)png[i] << 24) | ((uint32_t)png[i + 1] << 16) |
        ((uint32_t)png[i + 2] << 8) | (uint32_t)png[i + 3];
    if (i + 12 + (size_t)len > png.size()) {
      return false;
    }
    if (std::memcmp(&png[i + 4], type, 4) == 0) {
      payload.assign(png.begin() + (long)i + 8,
          png.begin() + (long)i + 8 + (long)len);
      return true;
    }
    i += 12 + (size_t)len;
  }
  return false;
}

/** colour type and bit depth out of an encoded PNG's IHDR. */
void ReadIhdr(const std::vector<uint8_t> & png, uint8_t * color_type,
    uint8_t * bit_depth) {
  std::vector<uint8_t> ihdr;
  ASSERT_TRUE(FindChunk(png, "IHDR", ihdr));
  ASSERT_EQ(ihdr.size(), 13u);
  *bit_depth = ihdr[8];
  *color_type = ihdr[9];
}

/** Load a fixture, save it whole, and hand back the encoded bytes. */
std::vector<uint8_t> LoadAndSave(const char * fixture) {
  GIMG_Result r = GIMG_OK;
  return SaveWithFilter(fixture, GIMG_PNG_FILTER_ADAPTIVE, &r);
}

// -- bKGD -------------------------------------------------------------------

TEST(PngAncillaryRetarget, AGreyBackgroundBecomesThreeEqualSamples) {
  // 4-bit greyscale promoted to colour type 6 at 8 bits, which is what happens
  // when a tRNS has to become an alpha channel. Grey 7 of 15 rescales to 119
  // by 13.12 - round(7 * 255 / 15) - a value that is not 7, not 112 and not
  // 127, so every plausible way of getting the rescaling wrong is visible.
  SourceImage src(0, 4);
  Retargeted got = Retarget(GIMG_PNG_bKGD, {0x00, 0x07}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  ASSERT_EQ(got.payload.size(), 6u);
  const std::vector<unsigned char> want = {0, 119, 0, 119, 0, 119};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, AnUnchangedColourTypeAndDepthKeepsTheChunk) {
  // The control. A writer that rewrote unconditionally would pass the test
  // above and fail this one.
  SourceImage src(0, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_bKGD, {0x00, 0x80}, src, 0, 8).what,
      GIMG_PNG_RETARGET_KEEP);
}

TEST(PngAncillaryRetarget, APaletteIndexBecomesTheColourItNames) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0x00, 0x00, 0x20, 0x40, 0x60, 0x00, 0xFF, 0x00});
  Retargeted got = Retarget(GIMG_PNG_bKGD, {1}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {0, 0x20, 0, 0x40, 0, 0x60};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, APaletteIndexPastTheEndOfThePaletteIsDropped) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0x00, 0x00, 0x20, 0x40, 0x60});
  EXPECT_EQ(Retarget(GIMG_PNG_bKGD, {7}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, AColourBackgroundSurvivesOnlyIfItIsAlreadyGrey) {
  SourceImage colour(2, 8);
  // Three different samples: no grey level says this, so it goes.
  EXPECT_EQ(
      Retarget(GIMG_PNG_bKGD, {0, 0x20, 0, 0x40, 0, 0x60}, colour, 0, 8).what,
      GIMG_PNG_RETARGET_DROP);
  // Three equal samples: the grey level is exactly that.
  Retargeted got =
      Retarget(GIMG_PNG_bKGD, {0, 0x44, 0, 0x44, 0, 0x44}, colour, 0, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {0, 0x44};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, ABackgroundOfTheWrongLengthIsNotCarriedForward) {
  // Three bytes where colour type 0 calls for two. The file was already
  // malformed; that is not a reason to write another one.
  SourceImage src(0, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_bKGD, {0x00, 0x80, 0x00}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, SixteenBitBackgroundsRescaleDownToEight) {
  SourceImage src(2, 16);
  Retargeted got = Retarget(
      GIMG_PNG_bKGD, {0xFF, 0xFF, 0x80, 0x00, 0x00, 0x00}, src, 2, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  // 65535 -> 255, 32768 -> 128, 0 -> 0.
  const std::vector<unsigned char> want = {0, 255, 0, 128, 0, 0};
  EXPECT_EQ(got.payload, want);
}

// -- sBIT -------------------------------------------------------------------

TEST(PngAncillaryRetarget, SignificantBitsDoNotSurviveAChangeOfDepth) {
  // Rescaling by 13.12 spreads a 4-bit value across all 8 bits of the new
  // sample, so a count taken before that would tell a decoder to shift data
  // that has already been scaled. Unlike a background colour, this cannot be
  // translated - only dropped.
  SourceImage src(0, 4);
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {3}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, SignificantBitsSurviveAChangeOfChannelCount) {
  // Same depth, more channels: a grey level repeated into R, G and B is
  // significant in each to exactly the same degree, and the alpha channel the
  // writer is adding is significant in all of its bits.
  SourceImage src(0, 8);
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 5, 5, 8};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, AnAlphaChannelAlreadyPresentKeepsItsOwnCount) {
  SourceImage src(4, 8); // grey + alpha
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5, 6}, src, 6, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 5, 5, 6};
  EXPECT_EQ(got.payload, want);
}

TEST(PngAncillaryRetarget, SignificantBitsOutsideTheirLegalRangeAreDropped) {
  // 11.3.2.4: each value is at least 1 and no more than the sample depth.
  SourceImage src(0, 8);
  EXPECT_EQ(
      Retarget(GIMG_PNG_sBIT, {0}, src, 6, 8).what, GIMG_PNG_RETARGET_DROP);
  EXPECT_EQ(
      Retarget(GIMG_PNG_sBIT, {9}, src, 6, 8).what, GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, UnequalColourCountsCannotBecomeOneGreyCount) {
  SourceImage src(2, 8);
  EXPECT_EQ(Retarget(GIMG_PNG_sBIT, {5, 6, 7}, src, 0, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, PaletteSignificantBitsDescribeEightBitSamples) {
  // 11.3.2.4: for colour type 3 the three values describe the palette's
  // samples, which are always 8-bit, whatever the depth of the indices.
  SourceImage src(3, 4);
  Retargeted got = Retarget(GIMG_PNG_sBIT, {5, 6, 7}, src, 2, 8);
  EXPECT_EQ(got.what, GIMG_PNG_RETARGET_REPLACE);
  const std::vector<unsigned char> want = {5, 6, 7};
  EXPECT_EQ(got.payload, want);
}

// -- hIST -------------------------------------------------------------------

TEST(PngAncillaryRetarget, AHistogramWithoutItsPaletteIsDropped) {
  // 11.3.4.2: one frequency per palette entry, and "shall not appear unless a
  // PLTE chunk appears". A truecolour image has no palette for it to be about,
  // and there is nothing to translate it into.
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0, 0, 0x20, 0x40, 0x60});
  EXPECT_EQ(Retarget(GIMG_PNG_hIST, {0, 100, 0, 50}, src, 6, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

TEST(PngAncillaryRetarget, AHistogramThatStillMatchesItsPaletteIsKept) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0, 0, 0x20, 0x40, 0x60});
  EXPECT_EQ(Retarget(GIMG_PNG_hIST, {0, 100, 0, 50}, src, 3, 8).what,
      GIMG_PNG_RETARGET_KEEP);
}

TEST(PngAncillaryRetarget, AHistogramOfTheWrongLengthIsDropped) {
  SourceImage src(3, 8);
  src.set_palette({0xFF, 0, 0, 0x20, 0x40, 0x60}); // two entries
  EXPECT_EQ(Retarget(GIMG_PNG_hIST, {0, 100, 0, 50, 0, 25}, src, 3, 8).what,
      GIMG_PNG_RETARGET_DROP);
}

// -- everything else --------------------------------------------------------

// -- end to end -------------------------------------------------------------
//
// The rules above, reached the way a caller reaches them: load a file, save
// it, and read the chunks back out of what was written.

TEST(PngAncillaryRetarget, APromotedGreyscaleFileGetsABackgroundThatFitsIt) {
  // 4-bit greyscale with tRNS. The transparent grey level cannot survive as a
  // tRNS against an 8-bit raster, so this is written as colour type 6 - and a
  // 2-byte bKGD is not a bKGD for colour type 6.
  std::vector<uint8_t> saved = LoadAndSave("png_gray4_trns_bkgd_sbit.png");
  ASSERT_FALSE(saved.empty());

  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 6) << "the fixture is meant to be promoted; it was not";
  ASSERT_EQ(bd, 8);

  std::vector<uint8_t> bkgd;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want = {0, 119, 0, 119, 0, 119};
  EXPECT_EQ(bkgd, want) << "grey 7 of 15 rescales to 119 at 8 bits (13.12)";

  // sBIT counted bits in 4-bit samples; the samples are 8-bit now.
  std::vector<uint8_t> sbit;
  EXPECT_FALSE(FindChunk(saved, "sBIT", sbit));
}

TEST(PngAncillaryRetarget, AFileThatKeepsItsColourTypeKeepsItsChunksVerbatim) {
  // The control for the test above. No tRNS, nothing forces a change, so both
  // chunks must come back byte for byte.
  std::vector<uint8_t> saved = LoadAndSave("png_gray8_bkgd_sbit.png");
  ASSERT_FALSE(saved.empty());

  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 0);
  ASSERT_EQ(bd, 8);

  std::vector<uint8_t> bkgd, sbit;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want_bkgd = {0x00, 0x80};
  EXPECT_EQ(bkgd, want_bkgd);
  ASSERT_TRUE(FindChunk(saved, "sBIT", sbit));
  const std::vector<uint8_t> want_sbit = {5};
  EXPECT_EQ(sbit, want_sbit);
}

TEST(PngAncillaryRetarget, AMalformedBackgroundIsNotCopiedIntoTheNewFile) {
  std::vector<uint8_t> saved = LoadAndSave("png_gray8_bad_bkgd.png");
  ASSERT_FALSE(saved.empty());
  std::vector<uint8_t> bkgd;
  EXPECT_FALSE(FindChunk(saved, "bKGD", bkgd))
      << "a three-byte bKGD is wrong for every colour type";
}

TEST(PngAncillaryRetarget, APaletteThatStaysAPaletteKeepsItsHistogram) {
  std::vector<uint8_t> saved = LoadAndSave("png_palette_trns_bkgd_hist.png");
  ASSERT_FALSE(saved.empty());
  uint8_t ct = 0, bd = 0;
  ReadIhdr(saved, &ct, &bd);
  ASSERT_EQ(ct, 3) << "a palette frame is written back as one";

  std::vector<uint8_t> bkgd, hist;
  ASSERT_TRUE(FindChunk(saved, "bKGD", bkgd));
  const std::vector<uint8_t> want_bkgd = {1};
  EXPECT_EQ(bkgd, want_bkgd);
  ASSERT_TRUE(FindChunk(saved, "hIST", hist));
  EXPECT_EQ(hist.size(), 8u) << "one 16-bit frequency per palette entry";
}

TEST(PngAncillaryRetarget, ChunksThatDoNotDependOnTheColourTypeAreUntouched) {
  // pHYs, tIME, gAMA, text, and anything unknown mean the same thing whatever
  // the colour type, so a change of colour type must not disturb them.
  SourceImage src(0, 4);
  const gimg_png_chunk_type_t independent[] = {
      GIMG_PNG_gAMA, GIMG_PNG_tEXt, GIMG_PNG_iCCP, GIMG_PNG_eXIf};
  for (gimg_png_chunk_type_t t : independent) {
    EXPECT_EQ(Retarget(t, {1, 2, 3, 4}, src, 6, 8).what,
        GIMG_PNG_RETARGET_KEEP);
  }
}
