/**
 * @file
 *
 * ICO/CUR load, decode, and structure tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "ico_internal.h"

namespace {

std::string data_path(const char * name) {
  return std::string(GIMG_TEST_DATA_ICO) + "/" + name;
}

bool load_file(const char * name, std::vector<uint8_t> & out) {
  std::ifstream in(data_path(name), std::ios::binary);
  if (!in) {
    return false;
  }
  out.assign(std::istreambuf_iterator<char>(in),
      std::istreambuf_iterator<char>());
  return !out.empty();
}

GIMG_Result load_doc(const char * name, GIMG_Doc ** out,
    GIMG_Diagnostics * diag = nullptr) {
  std::vector<uint8_t> bytes;
  if (!load_file(name, bytes)) {
    return GIMG_ERR_IO;
  }
  GIMG_Stream * s = nullptr;
  GIMG_Result r =
      gimg_stream_create_memory(bytes.data(), bytes.size(), &s);
  if (r != GIMG_OK) {
    return r;
  }
  r = gimg_doc_load(s, nullptr, diag, out);
  gimg_stream_destroy(s);
  return r;
}

} // namespace

TEST(Ico, ProbeIcoAndCur) {
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(load_file("ico_16_dib_32.ico", bytes));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &s), GIMG_OK);
  GIMG_Probe_Result pr = {};
  ASSERT_EQ(gimg_probe(s, &pr), GIMG_OK);
  EXPECT_STREQ(pr.format_name, "ico");
  gimg_stream_destroy(s);

  ASSERT_TRUE(load_file("cur_hotspot.cur", bytes));
  ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &s), GIMG_OK);
  pr = {};
  ASSERT_EQ(gimg_probe(s, &pr), GIMG_OK);
  EXPECT_STREQ(pr.format_name, "ico");
  gimg_stream_destroy(s);
}

TEST(Ico, ProbeDeclinesCountZero) {
  std::vector<uint8_t> bytes;
  ASSERT_TRUE(load_file("ico_corrupt_count0.ico", bytes));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &s), GIMG_OK);
  GIMG_Probe_Result pr = {};
  ASSERT_EQ(gimg_probe(s, &pr), GIMG_OK);
  EXPECT_EQ(pr.confidence, 0u);
  gimg_stream_destroy(s);
}

TEST(Ico, LoadAlternateRoles) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("ico_multi_dib.ico", &doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(doc), 2u);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(doc, 0)), GIMG_ITEM_IMAGE);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(doc, 1)), GIMG_ITEM_ALTERNATE);
  EXPECT_EQ(gimg_item_role_subject(gimg_doc_item(doc, 1)), 0u);
  gimg_doc_destroy(doc);
}

TEST(Ico, DecodeDibAndPng) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("ico_16_dib_32.ico", &doc), GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r), 16u);
  EXPECT_EQ(gimg_raster_height(r), 16u);
  const uint8_t * px =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(r));
  EXPECT_EQ(px[0], 255);
  EXPECT_EQ(px[1], 0);
  EXPECT_EQ(px[2], 0);
  gimg_raster_destroy(r);
  gimg_doc_destroy(doc);

  ASSERT_EQ(load_doc("ico_png_32.ico", &doc), GIMG_OK);
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r), 32u);
  gimg_raster_destroy(r);
  gimg_doc_destroy(doc);
}

TEST(Ico, ZeroAlphaFallsBackToAndMask) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("ico_zero_alpha_and.ico", &doc), GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r), GIMG_OK);
  // Checkerboard AND: (0,0) opaque (mask bit clear), (1,0) transparent.
  const uint8_t * px =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(r));
  EXPECT_EQ(px[3], 255);
  EXPECT_EQ(px[4 + 3], 0);
  gimg_raster_destroy(r);
  gimg_doc_destroy(doc);
}

TEST(Ico, CurHotspot) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("cur_hotspot.cur", &doc), GIMG_OK);
  uint16_t x = 0, y = 0;
  gimg_item_hotspot(gimg_doc_item(doc, 0), &x, &y);
  EXPECT_EQ(x, 3u);
  EXPECT_EQ(y, 5u);
  gimg_doc_destroy(doc);
}

TEST(Ico, DirMismatchStillLoads) {
  GIMG_Diagnostics diag = {};
  gimg_diagnostics_init(&diag, nullptr);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("ico_dir_mismatch.ico", &doc, &diag), GIMG_OK);
  EXPECT_GE(diag.count, 1u);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r), 8u);
  EXPECT_EQ(gimg_raster_height(r), 8u);
  gimg_raster_destroy(r);
  gimg_doc_destroy(doc);
  gimg_diagnostics_destroy(&diag);
}

TEST(Ico, CorruptOverflowRefused) {
  GIMG_Doc * doc = nullptr;
  EXPECT_NE(load_doc("ico_corrupt_overflow.ico", &doc), GIMG_OK);
  EXPECT_EQ(doc, nullptr);
}

TEST(Ico, SaveRoundTrip) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("ico_16_dib_32.ico", &doc), GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  // raster ownership transferred? set_raster takes ownership — decode gave us
  // ownership, so this is correct.
  r = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.ico_payload = GIMG_ICO_PAYLOAD_DIB;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "ico", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  GIMG_Stream * back = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, size, &back), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(back, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * r2 = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &r2), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r2), 16u);
  gimg_raster_destroy(r2);
  gimg_doc_destroy(round);
  gimg_stream_destroy(back);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Ico, SaveRefusesFrames) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  gimg_item_set_role(gimg_doc_item(doc, 0), GIMG_ITEM_FRAME, 0);
  gimg_item_set_role(gimg_doc_item(doc, 1), GIMG_ITEM_FRAME, 0);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  EXPECT_EQ(gimg_doc_save(doc, out, "ico", nullptr, &report),
      GIMG_ERR_UNSUPPORTED);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
