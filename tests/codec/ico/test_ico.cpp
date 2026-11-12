/**
 * @file
 *
 * ICO/CUR load, decode, and structure tests.
 *
 * Encode cases that leave files in GIMG_TEST_OUT_ICO are the input to
 * tests/data/ico/verify_ico_output.py: outside readers must open them and
 * match the sidecars. A self round trip alone is not that evidence.
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
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "ico_internal.h"

namespace {

std::string data_path(const char * name) {
  return std::string(GIMG_TEST_DATA_ICO) + "/" + name;
}

/**
 * Pack a raster's visible pixels tightly (no row padding) for a sidecar.
 */
std::vector<uint8_t> packed_rgba(const GIMG_Raster * raster) {
  const uint32_t w = gimg_raster_width(raster);
  const uint32_t h = gimg_raster_height(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);
  const uint8_t * pixels =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(raster));
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4u);
  for (uint32_t y = 0; y < h; y++) {
    std::memcpy(out.data() + static_cast<size_t>(y) * w * 4u,
        pixels + static_cast<size_t>(y) * stride, static_cast<size_t>(w) * 4u);
  }
  return out;
}

/**
 * Leave a saved icon in GIMG_TEST_OUT_ICO with per-entry RGBA sidecars for
 * verify_ico_output.py.
 */
void publish_for_verification(const char * name,
    const std::vector<uint8_t> & bytes, GIMG_Doc * source) {
  const std::string dir = GIMG_TEST_OUT_ICO;
  const std::string path = dir + "/" + name;
  std::ofstream out(path, std::ios::binary);
  ASSERT_TRUE(out) << "cannot write " << path;
  out.write(reinterpret_cast<const char *>(bytes.data()),
      static_cast<std::streamsize>(bytes.size()));
  out.close();

  const size_t count = gimg_doc_item_count(source);
  for (size_t i = 0; i < count; i++) {
    GIMG_Raster * raster = gimg_item_raster(gimg_doc_item(source, i));
    ASSERT_NE(raster, nullptr) << "entry " << i << " has no attached raster";
    const uint32_t w = gimg_raster_width(raster);
    const uint32_t h = gimg_raster_height(raster);
    const std::vector<uint8_t> rgba = packed_rgba(raster);
    const std::string side =
        path + ".expected." + std::to_string(i) + ".rgba";
    std::ofstream expected(side, std::ios::binary);
    ASSERT_TRUE(expected) << "cannot write " << side;
    expected.write(reinterpret_cast<const char *>(rgba.data()),
        static_cast<std::streamsize>(rgba.size()));
    expected.close();
    const std::string size_path =
        path + ".expected." + std::to_string(i) + ".size";
    std::ofstream size_out(size_path);
    ASSERT_TRUE(size_out) << "cannot write " << size_path;
    size_out << w << " " << h << "\n";
  }
}

GIMG_Doc * doc_from_solid(uint32_t w, uint32_t h, uint8_t r, uint8_t g,
    uint8_t b, uint8_t a) {
  GIMG_Doc * doc = nullptr;
  if (gimg_doc_create(&doc) != GIMG_OK ||
      gimg_doc_set_item_count(doc, 1) != GIMG_OK) {
    if (doc) {
      gimg_doc_destroy(doc);
    }
    return nullptr;
  }
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &raster) != GIMG_OK ||
      !raster) {
    gimg_doc_destroy(doc);
    return nullptr;
  }
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      uint8_t * p = px + static_cast<size_t>(y) * stride + x * 4u;
      p[0] = r;
      p[1] = g;
      p[2] = b;
      p[3] = a;
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  return doc;
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

/**
 * Publish an opaque DIB icon for outside readers. GdkPixbuf refuses PNG
 * payloads; this file is the one every reader must open.
 */
TEST(Ico, SavePublishOpaqueDib) {
  GIMG_Doc * doc = doc_from_solid(16, 16, 200, 40, 40, 255);
  ASSERT_NE(doc, nullptr);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.ico_payload = GIMG_ICO_PAYLOAD_DIB;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "ico", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),
      static_cast<const uint8_t *>(data) + size);
  publish_for_verification("save_opaque_dib.ico", bytes, doc);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

/** Publish a PNG-payload icon; Pillow and ImageMagick must accept it. */
TEST(Ico, SavePublishPng) {
  GIMG_Doc * doc = doc_from_solid(32, 32, 40, 120, 200, 255);
  ASSERT_NE(doc, nullptr);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.ico_payload = GIMG_ICO_PAYLOAD_PNG;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "ico", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),
      static_cast<const uint8_t *>(data) + size);
  publish_for_verification("save_png.ico", bytes, doc);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

/** Two opaque DIB alternates — ImageMagick must see both; Gdk picks one. */
TEST(Ico, SavePublishMultiDib) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  gimg_item_set_role(gimg_doc_item(doc, 0), GIMG_ITEM_IMAGE, 0);
  gimg_item_set_role(gimg_doc_item(doc, 1), GIMG_ITEM_ALTERNATE, 0);

  GIMG_Raster * r16 = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                   nullptr, 0, &r16),
      GIMG_OK);
  {
    uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(r16));
    size_t stride = gimg_raster_stride_bytes(r16);
    for (uint32_t y = 0; y < 16; y++) {
      for (uint32_t x = 0; x < 16; x++) {
        uint8_t * p = px + y * stride + x * 4u;
        p[0] = 255;
        p[1] = 0;
        p[2] = 0;
        p[3] = 255;
      }
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 0), r16);

  GIMG_Raster * r32 = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 32, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                   nullptr, 0, &r32),
      GIMG_OK);
  {
    uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(r32));
    size_t stride = gimg_raster_stride_bytes(r32);
    for (uint32_t y = 0; y < 32; y++) {
      for (uint32_t x = 0; x < 32; x++) {
        uint8_t * p = px + y * stride + x * 4u;
        p[0] = 0;
        p[1] = 255;
        p[2] = 0;
        p[3] = 255;
      }
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 1), r32);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.ico_payload = GIMG_ICO_PAYLOAD_DIB;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "ico", &opts, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  std::vector<uint8_t> bytes(static_cast<const uint8_t *>(data),
      static_cast<const uint8_t *>(data) + size);
  publish_for_verification("save_multi_dib.ico", bytes, doc);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}


void put16(std::vector<uint8_t> & b, uint16_t v) {
  b.push_back((uint8_t)(v & 0xFFu));
  b.push_back((uint8_t)(v >> 8));
}
void put32(std::vector<uint8_t> & b, uint32_t v) {
  put16(b, (uint16_t)(v & 0xFFFFu));
  put16(b, (uint16_t)(v >> 16));
}

/** One 1 x 1 icon whose DIB has a header of @p header_size bytes, the
 * compression @p compression, and @p extra bytes after the pixels. */
std::vector<uint8_t> one_pixel_icon(uint32_t header_size,
    uint32_t compression, const std::vector<uint8_t> & extra,
    uint32_t cs_type, uint32_t profile_offset, uint32_t profile_size) {
  std::vector<uint8_t> dib;
  put32(dib, header_size);
  put32(dib, 1);
  put32(dib, 2); // XOR rows plus AND rows
  put16(dib, 1);
  put16(dib, 32);
  put32(dib, compression);
  put32(dib, 8);
  put32(dib, 0);
  put32(dib, 0);
  put32(dib, 0);
  put32(dib, 0);
  dib.resize(header_size, 0);
  if (header_size >= 124) {
    for (int k = 0; k < 4; k++) {
      dib[56 + k] = (uint8_t)(cs_type >> (8 * k));
      dib[112 + k] = (uint8_t)(profile_offset >> (8 * k));
      dib[116 + k] = (uint8_t)(profile_size >> (8 * k));
    }
  }
  const uint8_t pixel[4] = {10, 20, 30, 255};
  dib.insert(dib.end(), pixel, pixel + 4);
  dib.insert(dib.end(), 4, 0); // AND mask
  dib.insert(dib.end(), extra.begin(), extra.end());

  std::vector<uint8_t> f;
  put16(f, 0);
  put16(f, 1);
  put16(f, 1);
  f.push_back(1);
  f.push_back(1);
  f.push_back(0);
  f.push_back(0);
  put16(f, 1);
  put16(f, 32);
  put32(f, (uint32_t)dib.size());
  put32(f, 22);
  f.insert(f.end(), dib.begin(), dib.end());
  return f;
}

GIMG_Result decode_icon(const std::vector<uint8_t> & f, GIMG_Raster ** out) {
  *out = nullptr;
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(f.data(), f.size(), &s) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r == GIMG_OK) {
    r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, out);
  }
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return r;
}

TEST(Ico, AnEntryThatIsAJpegOrPngWrapperIsUnsupported) {
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode_icon(one_pixel_icon(40, 0, {}, 0, 0, 0), &r), GIMG_OK)
      << "control";
  gimg_raster_destroy(r);
  for (uint32_t c : {4u, 5u}) {
    SCOPED_TRACE(c);
    EXPECT_EQ(decode_icon(one_pixel_icon(40, c, {'x', 'y'}, 0, 0, 0), &r),
        GIMG_ERR_UNSUPPORTED);
  }
}

TEST(Ico, AV5IconsProfileIsReadFromItsOwnHeaderNotFromByteFourteen) {
  // bV5ProfileData counts from the start of the DIB header, which is byte 0
  // of an icon entry and not byte 14 as it is in a BM file.
  const std::vector<uint8_t> profile = {'A', 'B', 'C', 'D'};
  // The pixel (4) and mask (4) follow the 124-byte header; the profile is next.
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(decode_icon(one_pixel_icon(124, 0, profile, 0x4D424544u, 132, 4), &r),
      GIMG_OK);
  const GCOL_Color_Info * ci = gimg_raster_color_info_const(r);
  ASSERT_NE(ci, nullptr);
  ASSERT_EQ(ci->icc_size, 4u);
  EXPECT_EQ(memcmp(ci->icc_bytes, "ABCD", 4), 0);
  gimg_raster_destroy(r);
}

TEST(Ico, TheWriterRefusesASizeADirectoryEntryCannotState) {
  for (int payload : {GIMG_ICO_PAYLOAD_DIB, GIMG_ICO_PAYLOAD_PNG}) {
    GIMG_Doc * ok = doc_from_solid(256, 1, 1, 2, 3, 255);
    GIMG_Doc * big = doc_from_solid(257, 1, 1, 2, 3, 255);
    ASSERT_NE(ok, nullptr);
    ASSERT_NE(big, nullptr);
    GIMG_Save_Options opts = {};
    GIMG_Save_Report rep = {};
    opts.ico_payload = payload;
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    EXPECT_EQ(gimg_doc_save(ok, out, "ico", &opts, &rep), GIMG_OK)
        << "control: 256 is the largest an entry states";
    gimg_stream_destroy(out);
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    EXPECT_EQ(gimg_doc_save(big, out, "ico", &opts, &rep),
        GIMG_ERR_UNSUPPORTED);
    gimg_stream_destroy(out);
    gimg_doc_destroy(ok);
    gimg_doc_destroy(big);
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
