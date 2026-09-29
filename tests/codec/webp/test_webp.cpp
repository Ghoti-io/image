/**
 * @file
 *
 * WebP Phase A: load, refuse decode/save, canvas and metadata.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

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
  // Phase A: still one IMAGE item; frames arrive in phase E.
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

TEST(Webp, DecodeAndSaveUnsupportedInPhaseA) {
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
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
