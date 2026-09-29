/**
 * @file
 *
 * WebP Phase A+B: load, VP8L decode, refuse lossy/anim/save.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "../../../src/container/doc_internal.h"
#include "webp_internal.h"

namespace {

std::string data_path(const char * name) {
  return std::string(GIMG_TEST_DATA_WEBP) + "/" + name;
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

const gimg_webp_doc_state_t * state_of(GIMG_Doc * doc) {
  return static_cast<const gimg_webp_doc_state_t *>(doc->codec_private);
}

/** PAM RGB_ALPHA pixels after ENDHDR\\n (fixtures from dwebp -pam). */
bool load_pam_rgba(const char * name, std::vector<uint8_t> & rgba,
    uint32_t * out_w, uint32_t * out_h) {
  std::vector<uint8_t> file;
  if (!load_file(name, file)) {
    return false;
  }
  static const char kEnd[] = "ENDHDR\n";
  auto it = std::search(file.begin(), file.end(), kEnd, kEnd + 7);
  if (it == file.end()) {
    return false;
  }
  size_t off = static_cast<size_t>(it - file.begin()) + 7u;
  std::string hdr(reinterpret_cast<const char *>(file.data()), off);
  auto width_pos = hdr.find("WIDTH ");
  auto height_pos = hdr.find("HEIGHT ");
  if (width_pos == std::string::npos || height_pos == std::string::npos) {
    return false;
  }
  *out_w = static_cast<uint32_t>(std::stoul(hdr.substr(width_pos + 6)));
  *out_h = static_cast<uint32_t>(std::stoul(hdr.substr(height_pos + 7)));
  size_t expect = static_cast<size_t>(*out_w) * *out_h * 4u;
  if (file.size() < off + expect) {
    return false;
  }
  rgba.assign(file.begin() + static_cast<std::ptrdiff_t>(off),
      file.begin() + static_cast<std::ptrdiff_t>(off + expect));
  return true;
}

void expect_raster_matches_pam(GIMG_Raster * raster, const char * pam_name) {
  std::vector<uint8_t> expect;
  uint32_t ew = 0, eh = 0;
  ASSERT_TRUE(load_pam_rgba(pam_name, expect, &ew, &eh));
  ASSERT_EQ(gimg_raster_width(raster), ew);
  ASSERT_EQ(gimg_raster_height(raster), eh);
  const uint8_t * px =
      static_cast<const uint8_t *>(gimg_raster_pixels(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < eh; ++y) {
    for (uint32_t x = 0; x < ew; ++x) {
      const size_t ei = (static_cast<size_t>(y) * ew + x) * 4u;
      const size_t oi = static_cast<size_t>(y) * stride + x * 4u;
      EXPECT_EQ(px[oi + 0], expect[ei + 0]) << "R y=" << y << " x=" << x;
      EXPECT_EQ(px[oi + 1], expect[ei + 1]) << "G y=" << y << " x=" << x;
      EXPECT_EQ(px[oi + 2], expect[ei + 2]) << "B y=" << y << " x=" << x;
      EXPECT_EQ(px[oi + 3], expect[ei + 3]) << "A y=" << y << " x=" << x;
    }
  }
}

} // namespace

TEST(Webp, ProbeAndLoadSimpleLossy) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("simple_lossy.webp", &doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  const gimg_webp_doc_state_t * st = state_of(doc);
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->canvas_width, 4u);
  EXPECT_EQ(st->canvas_height, 4u);
  EXPECT_TRUE(st->is_lossy);
  EXPECT_FALSE(st->has_vp8x);
  EXPECT_GE(st->chunk_count, 1u);
  EXPECT_EQ(st->chunks[0].fourcc, GIMG_WEBP_VP8);
  gimg_doc_destroy(doc);
}

TEST(Webp, LoadSimpleLossless) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("simple_lossless.webp", &doc), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(doc);
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->canvas_width, 4u);
  EXPECT_EQ(st->canvas_height, 4u);
  EXPECT_TRUE(st->is_lossless);
  EXPECT_EQ(st->chunks[0].fourcc, GIMG_WEBP_VP8L);
  gimg_doc_destroy(doc);
}

TEST(Webp, DecodeSimpleLosslessMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("simple_lossless.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "simple_lossless.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

TEST(Webp, DecodeLosslessAlphaMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossless_alpha.webp", &doc), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(doc);
  ASSERT_NE(st, nullptr);
  EXPECT_TRUE(st->has_alpha);
  EXPECT_TRUE(st->is_lossless);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossless_alpha.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** Predictor + cross-colour (cwebp default lossless on a gradient). */
TEST(Webp, DecodeLosslessGradientMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossless_gradient.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossless_gradient.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** Predictor + cross-colour + subtract-green (-m 6). */
TEST(Webp, DecodeLosslessGradientM6MatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossless_gradient_m6.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossless_gradient_m6.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** Colour-indexing / palette. */
TEST(Webp, DecodeLosslessCheckerMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossless_checker.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossless_checker.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

TEST(Webp, LoadExtendedAlphaAndExif) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossy_alpha.webp", &doc), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(doc);
  ASSERT_NE(st, nullptr);
  EXPECT_TRUE(st->has_vp8x);
  EXPECT_TRUE(st->has_alpha);
  EXPECT_EQ(st->canvas_width, 4u);
  gimg_doc_destroy(doc);

  ASSERT_EQ(load_doc("lossy_exif.webp", &doc), GIMG_OK);
  st = state_of(doc);
  ASSERT_NE(st, nullptr);
  EXPECT_GT(st->exif_size, 0u);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t exif_size = 0;
  ASSERT_EQ(gimg_meta_raw_get(raw, "webp", GIMG_WEBP_EXIF, nullptr, &exif_size),
      GIMG_OK);
  EXPECT_EQ(exif_size, st->exif_size);
  gimg_doc_destroy(doc);
}

TEST(Webp, LoadAnimationWalksNestedChunks) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("anim.webp", &doc), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(doc);
  ASSERT_NE(st, nullptr);
  EXPECT_TRUE(st->is_animation);
  EXPECT_TRUE(st->has_vp8x);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  int anmf = 0;
  int nested = 0;
  for (size_t i = 0; i < st->chunk_count; i++) {
    if (st->chunks[i].fourcc == GIMG_WEBP_ANMF) {
      anmf++;
    }
    if (st->chunks[i].fourcc == GIMG_WEBP_VP8L ||
        st->chunks[i].fourcc == GIMG_WEBP_VP8) {
      nested++;
    }
  }
  EXPECT_GE(anmf, 1);
  EXPECT_GE(nested, 1);
  gimg_doc_destroy(doc);
}

TEST(Webp, CorruptRefused) {
  GIMG_Doc * doc = nullptr;
  EXPECT_NE(load_doc("corrupt_trunc.webp", &doc), GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  EXPECT_NE(load_doc("corrupt_riff_size.webp", &doc), GIMG_OK);
  EXPECT_EQ(doc, nullptr);
}

TEST(Webp, LossyAndAnimDecodeStillUnsupported) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("simple_lossy.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(raster, nullptr);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  EXPECT_EQ(gimg_doc_save(doc, out, "webp", nullptr, nullptr),
      GIMG_ERR_UNSUPPORTED);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  ASSERT_EQ(load_doc("anim.webp", &doc), GIMG_OK);
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);

  ASSERT_EQ(load_doc("lossy_alpha.webp", &doc), GIMG_OK);
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
