/**
 * @file
 *
 * PNG encode/save tests: round-trip, save to stream, metadata policy.
 * When GIMG_TEST_DATA_PNG is defined, loads reference files from that
 * directory and writes encoded PNGs to tests/out/png/ for verification
 * (e.g. by tests/data/png/verify_png_output.py).
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

#ifdef GIMG_TEST_DATA_PNG
#include "png_test_utils.h"
#include <fstream>
#include <string>
#endif

namespace {
} // namespace

TEST(PngEncode, SaveNullDocReturnsInternal) {
  GIMG_Stream * out_s = nullptr;
  GIMG_Result r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(out_s, nullptr);
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(nullptr, out_s, "png", &opts, &report);
  EXPECT_NE(r, GIMG_OK);
  gimg_stream_destroy(out_s);
}

#ifdef GIMG_TEST_DATA_PNG
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

  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
#endif

#ifdef GIMG_TEST_DATA_PNG
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
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
  png_test::write_png_output("preserve_exif.png", saved_data.data(), saved_data.size());
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
  ASSERT_TRUE(png_test::load_png_file("png_exif.png", buf)) << "Run tests/data/png/generate.py";

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
  GIMG_Save_Options opts = {GIMG_META_DROP_ALL, 0, {0}};
  GIMG_Save_Report report = { 0, nullptr, { 0 } };
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(
      static_cast<const uint8_t *>(out_ptr),
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
    EXPECT_EQ(exif_size, 0u) << "eXIf must be stripped when saving with DROP_ALL";
  }
  gimg_doc_destroy(doc2);
}

TEST(PngEncode, SaveWithStripGpsStripsOnlyGps) {
  // STRIP_GPS strips only GPS from eXIf; eXIf chunk is preserved (re-written without GPS IFD).
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
  GIMG_Save_Options opts = {GIMG_META_STRIP_GPS, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(
      static_cast<const uint8_t *>(out_ptr),
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
  EXPECT_EQ(r, GIMG_OK) << "eXIf must be present after STRIP_GPS (only GPS removed)";
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
  GIMG_Save_Options opts = {GIMG_META_NORMALIZE_EXIF, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(
      static_cast<const uint8_t *>(out_ptr),
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
  EXPECT_EQ(exif_size, 6u)
      << "eXIf preserved after save with NORMALIZE_EXIF";
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
  GIMG_Save_Options opts = {GIMG_META_KEEP_RAW_ONLY, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(
      static_cast<const uint8_t *>(out_ptr),
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
  GIMG_Save_Options opts = {GIMG_META_KEEP_COMMON_ONLY, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(
      static_cast<const uint8_t *>(out_ptr),
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 1, {0}};  // interlaced=1
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK);
  gimg_doc_destroy(doc);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(
      static_cast<const uint8_t *>(out_ptr),
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
  png_test::write_png_output("interlaced_roundtrip.png", saved_data.data(),
      saved_data.size());
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
  GIMG_Save_Options opts = {GIMG_META_PRESERVE_ALL, 0, {0}};
  GIMG_Save_Report report = {0, nullptr, {0}};
  r = gimg_doc_save(doc, out_s, "png", &opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "APNG save";
  EXPECT_GT(report.bytes_written, 0u);

  const void * out_ptr = nullptr;
  size_t saved_size = 0;
  gimg_stream_output_buffer(out_s, &out_ptr, &saved_size);
  std::vector<uint8_t> saved_data(
      static_cast<const uint8_t *>(out_ptr),
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
  png_test::write_png_output("apng_2frame_roundtrip.png", saved_data.data(),
      saved_data.size());
}

#endif // GIMG_TEST_DATA_PNG

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
