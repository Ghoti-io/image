/**
 * @file
 *
 * WebP Phase A–G: load, VP8L/VP8/ALPH decode, ANIM/ANMF composite, VP8L
 * lossless encode, VP8 lossy encode.
 *
 * Each lossless file written here is also left in GIMG_TEST_OUT_WEBP with
 * the source pixels and the encode time, for
 * tests/data/webp/verify_webp_output.py. That script asks dwebp whether
 * the pixels survived, and asks our decoder the same file. A round trip
 * through this decoder alone does not.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <sys/stat.h>

#include "../../../src/container/doc_internal.h"
#include "../../../src/core/alloc_internal.h"
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

bool extract_alph_payload(const char * webp_name, std::vector<uint8_t> & out,
    uint32_t * canvas_w, uint32_t * canvas_h) {
  GIMG_Doc * doc = nullptr;
  if (load_doc(webp_name, &doc) != GIMG_OK) {
    return false;
  }
  const gimg_webp_doc_state_t * st = state_of(doc);
  if (!st) {
    gimg_doc_destroy(doc);
    return false;
  }
  *canvas_w = st->canvas_width;
  *canvas_h = st->canvas_height;
  const gimg_webp_chunk_t * alph = nullptr;
  for (size_t i = 0; i < st->chunk_count; ++i) {
    if (st->chunks[i].fourcc == GIMG_WEBP_ALPH) {
      alph = &st->chunks[i];
      break;
    }
  }
  if (!alph) {
    gimg_doc_destroy(doc);
    return false;
  }
  const size_t off = alph->offset + 8u;
  out.assign(st->file_bytes + off, st->file_bytes + off + alph->payload_size);
  gimg_doc_destroy(doc);
  return true;
}

void expect_alph_matches_pam_alpha(const char * webp_name,
    const char * pam_name) {
  std::vector<uint8_t> payload;
  uint32_t w = 0, h = 0;
  ASSERT_TRUE(extract_alph_payload(webp_name, payload, &w, &h));
  uint8_t * alpha = nullptr;
  ASSERT_EQ(gimg_webp_alpha_decode(payload.data(), payload.size(), w, h,
               nullptr, &alpha),
      GIMG_OK);
  ASSERT_NE(alpha, nullptr);

  std::vector<uint8_t> rgba;
  uint32_t pw = 0, ph = 0;
  ASSERT_TRUE(load_pam_rgba(pam_name, rgba, &pw, &ph));
  ASSERT_EQ(w, pw);
  ASSERT_EQ(h, ph);
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      const size_t i = static_cast<size_t>(y) * w + x;
      EXPECT_EQ(alpha[i], rgba[i * 4u + 3u]) << "A y=" << y << " x=" << x;
    }
  }
  gimg_free(gimg_allocator_default(), alpha);
}

/** Forward spatial filters for crafting method-0 ALPH payloads in tests. */
int gradient_pred(uint8_t a, uint8_t b, uint8_t c) {
  const int g = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
  return ((g & ~0xff) == 0) ? g : (g < 0) ? 0 : 255;
}

void filter_alpha(const std::vector<uint8_t> & src, int w, int h, int filter,
    std::vector<uint8_t> & dst) {
  dst.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  if (filter == 0) {
    dst = src;
    return;
  }
  auto at = [&](int x, int y) -> uint8_t {
    return src[static_cast<size_t>(y) * static_cast<size_t>(w) +
        static_cast<size_t>(x)];
  };
  auto out = [&](int x, int y) -> uint8_t & {
    return dst[static_cast<size_t>(y) * static_cast<size_t>(w) +
        static_cast<size_t>(x)];
  };
  // Top-left always copied; top row left-predicted for all filters.
  out(0, 0) = at(0, 0);
  for (int x = 1; x < w; ++x) {
    out(x, 0) = static_cast<uint8_t>(at(x, 0) - at(x - 1, 0));
  }
  for (int y = 1; y < h; ++y) {
    if (filter == 1) { // horizontal: first from above, rest from left
      out(0, y) = static_cast<uint8_t>(at(0, y) - at(0, y - 1));
      for (int x = 1; x < w; ++x) {
        out(x, y) = static_cast<uint8_t>(at(x, y) - at(x - 1, y));
      }
    }
    else if (filter == 2) { // vertical
      for (int x = 0; x < w; ++x) {
        out(x, y) = static_cast<uint8_t>(at(x, y) - at(x, y - 1));
      }
    }
    else { // gradient
      out(0, y) = static_cast<uint8_t>(at(0, y) - at(0, y - 1));
      for (int x = 1; x < w; ++x) {
        const int pred =
            gradient_pred(at(x - 1, y), at(x, y - 1), at(x - 1, y - 1));
        out(x, y) = static_cast<uint8_t>(at(x, y) - pred);
      }
    }
  }
}

} // namespace

TEST(Webp, Capabilities) {
  const GIMG_Codec * codec = gimg_codec_by_name("webp");
  ASSERT_NE(codec, nullptr);
  const unsigned int caps = gimg_codec_capabilities(codec);
  EXPECT_TRUE(caps & GIMG_CAP_READ);
  EXPECT_TRUE(caps & GIMG_CAP_WRITE);
  EXPECT_TRUE(caps & GIMG_CAP_ANIMATION);
  EXPECT_TRUE(caps & GIMG_CAP_ICC);
}

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
  EXPECT_EQ(gimg_doc_item_count(doc), 2u);
  EXPECT_EQ(st->frame_count, 2u);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(doc, 0)), GIMG_ITEM_FRAME);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(doc, 1)), GIMG_ITEM_FRAME);
  uint16_t delay_num = 0, delay_den = 0;
  gimg_item_frame_delay(gimg_doc_item(doc, 0), &delay_num, &delay_den);
  EXPECT_EQ(delay_num, 100u);
  EXPECT_EQ(delay_den, 1000u);
  EXPECT_EQ(gimg_item_dispose_op(gimg_doc_item(doc, 0)), GIMG_DISPOSE_NONE);
  EXPECT_EQ(gimg_item_blend_op(gimg_doc_item(doc, 0)), GIMG_BLEND_SOURCE);
  EXPECT_EQ(gimg_item_blend_op(gimg_doc_item(doc, 1)), GIMG_BLEND_OVER);
  uint32_t loop = 0;
  EXPECT_TRUE(gimg_doc_loop_count(doc, &loop));
  EXPECT_EQ(loop, 0u);
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

TEST(Webp, DecodeLossyStill) {
  /* Colour is a VP8 keyframe with the loop filter on. Pixel identity
   * against dwebp is verify_webp_pixels.py. */
  static const char * kFiles[] = {
      "simple_lossy.webp", "lossy_grad.webp", "lossy_exif.webp",
      "lossy_alpha.webp", "lossy_grad_alpha.webp",
      "alph_m0_none.webp", "alph_m0_fast.webp", "alph_m0_best.webp",
      "alph_m1_none.webp", "alph_m1_fast.webp", "alph_m1_best.webp",
  };
  for (const char * name : kFiles) {
    SCOPED_TRACE(name);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(load_doc(name, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
        GIMG_OK);
    ASSERT_NE(raster, nullptr);
    EXPECT_GT(gimg_raster_width(raster), 0u);
    EXPECT_GT(gimg_raster_height(raster), 0u);
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
  }
}

TEST(Webp, DecodeAnimFramesMatchAnimDump) {
  static const struct {
    const char * webp;
    const char * pam0;
    const char * pam1;
  } kCases[] = {
      {"anim.webp", "anim_f_0000.pam", "anim_f_0001.pam"},
      {"anim_offset.webp", "anim_offset_f_0000.pam", "anim_offset_f_0001.pam"},
      {"anim_dispose.webp", "anim_dispose_f_0000.pam",
          "anim_dispose_f_0001.pam"},
  };
  for (const auto & c : kCases) {
    SCOPED_TRACE(c.webp);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(load_doc(c.webp, &doc), GIMG_OK);
    ASSERT_EQ(gimg_doc_item_count(doc), 2u);
    GIMG_Raster * r0 = nullptr;
    GIMG_Raster * r1 = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &r0), GIMG_OK);
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 1), nullptr, &r1), GIMG_OK);
    expect_raster_matches_pam(r0, c.pam0);
    expect_raster_matches_pam(r1, c.pam1);
    gimg_raster_destroy(r0);
    gimg_raster_destroy(r1);
    gimg_doc_destroy(doc);
  }
}

TEST(Webp, AnimOffsetGeometry) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("anim_offset.webp", &doc), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(doc);
  ASSERT_NE(st, nullptr);
  ASSERT_EQ(st->frame_count, 2u);
  EXPECT_EQ(st->canvas_width, 8u);
  EXPECT_EQ(st->canvas_height, 8u);
  EXPECT_EQ(st->frames[0].x, 0u);
  EXPECT_EQ(st->frames[0].y, 0u);
  EXPECT_EQ(st->frames[0].width, 8u);
  EXPECT_EQ(st->frames[0].height, 8u);
  EXPECT_EQ(st->frames[1].x, 2u);
  EXPECT_EQ(st->frames[1].y, 2u);
  EXPECT_EQ(st->frames[1].width, 4u);
  EXPECT_EQ(st->frames[1].height, 4u);
  uint32_t loop = 99;
  EXPECT_TRUE(gimg_doc_loop_count(doc, &loop));
  EXPECT_EQ(loop, 2u);
  gimg_doc_destroy(doc);
}

TEST(Webp, AnimDisposeBackground) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("anim_dispose.webp", &doc), GIMG_OK);
  EXPECT_EQ(gimg_item_dispose_op(gimg_doc_item(doc, 0)),
      GIMG_DISPOSE_BACKGROUND);
  EXPECT_EQ(gimg_item_dispose_op(gimg_doc_item(doc, 1)), GIMG_DISPOSE_NONE);
  EXPECT_EQ(gimg_item_blend_op(gimg_doc_item(doc, 1)), GIMG_BLEND_OVER);
  gimg_doc_destroy(doc);
}

GIMG_Raster * solid_rgba(uint32_t w, uint32_t h, uint8_t r, uint8_t g,
    uint8_t b, uint8_t a) {
  GIMG_Raster * raster = nullptr;
  if (gimg_raster_create(w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0,
          &raster) != GIMG_OK) {
    return nullptr;
  }
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  const size_t stride = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < h; ++y) {
    uint8_t * row = px + y * stride;
    for (uint32_t x = 0; x < w; ++x) {
      row[x * 4u + 0u] = r;
      row[x * 4u + 1u] = g;
      row[x * 4u + 2u] = b;
      row[x * 4u + 3u] = a;
    }
  }
  return raster;
}

/** Leave an animation where verify_webp_output.py can see it. A lossless
 *  file gets one sidecar per frame; a lossy file is marked so the script
 *  checks anim_dump parity and not fidelity to the source. */
void publish_animation(const char * name, const std::vector<uint8_t> & bytes,
    const std::vector<GIMG_Raster *> & frames, int64_t encode_us, bool lossy) {
  mkdir(GIMG_TEST_OUT_WEBP, 0755);
  const std::string path = std::string(GIMG_TEST_OUT_WEBP) + "/" + name;
  {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out)) << path;
    out.write(reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(static_cast<bool>(out)) << path;
  }
  {
    std::ofstream side(path + ".anim", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(side)) << path;
  }
  if (lossy) {
    std::ofstream side(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(side)) << path;
  }
  else {
    for (size_t i = 0; i < frames.size(); ++i) {
      const uint32_t w = gimg_raster_width(frames[i]);
      const uint32_t h = gimg_raster_height(frames[i]);
      const size_t stride = gimg_raster_stride_bytes(frames[i]);
      const auto * px =
          static_cast<const uint8_t *>(gimg_raster_pixels(frames[i]));
      std::ofstream side(path + ".f" + std::to_string(i) + ".rgba",
          std::ios::binary);
      ASSERT_TRUE(static_cast<bool>(side)) << path;
      for (uint32_t y = 0; y < h; ++y) {
        side.write(reinterpret_cast<const char *>(px + y * stride),
            static_cast<std::streamsize>(w) * 4);
      }
    }
  }
  {
    std::ofstream side(path + ".time", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(side)) << path;
    side << encode_us << "\n";
  }
}

TEST(Webp, SaveAnimationRoundTrip) {
  /* Two full-canvas frames. The raster is the canvas at that moment, so
   * the file blends nothing and a later load shows those pixels. The
   * delay comes back in milliseconds over 1000, which is how ANMF stores
   * it. Dispose to background is a flag on the first frame. */
  GIMG_Raster * red = solid_rgba(8, 8, 200, 10, 10, 255);
  GIMG_Raster * blue = solid_rgba(8, 8, 10, 10, 200, 255);
  ASSERT_NE(red, nullptr);
  ASSERT_NE(blue, nullptr);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), red);
  gimg_item_set_raster(gimg_doc_item(doc, 1), blue);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 0), 100, 1000);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 1), 50, 100);
  gimg_item_set_dispose_op(gimg_doc_item(doc, 0), GIMG_DISPOSE_BACKGROUND);
  gimg_doc_set_loop_count(doc, 3);
  const uint8_t bg[4] = { 1, 2, 3, 255 };
  gimg_doc_set_background_color(doc, bg);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_effort = 4;
  opts.webp_exact = 1;
  GIMG_Save_Report report = {};
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, &report), GIMG_OK);
  const int64_t encode_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + nbytes);
  publish_animation("stub_anim.webp", saved, {red, blue}, encode_us, false);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(round), 2u);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(round, 0)), GIMG_ITEM_FRAME);
  GIMG_Raster * back0 = nullptr;
  GIMG_Raster * back1 = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back0),
      GIMG_OK);
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 1), nullptr, &back1),
      GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(red, back0));
  EXPECT_TRUE(gimg_ops_raster_equal(blue, back1));
  uint16_t num = 0;
  uint16_t den = 0;
  gimg_item_frame_delay(gimg_doc_item(round, 0), &num, &den);
  EXPECT_EQ(num, 100);
  EXPECT_EQ(den, 1000);
  gimg_item_frame_delay(gimg_doc_item(round, 1), &num, &den);
  EXPECT_EQ(num, 500);
  EXPECT_EQ(den, 1000);
  EXPECT_EQ(gimg_item_dispose_op(gimg_doc_item(round, 0)),
      GIMG_DISPOSE_BACKGROUND);
  EXPECT_EQ(gimg_item_dispose_op(gimg_doc_item(round, 1)), GIMG_DISPOSE_NONE);
  EXPECT_EQ(gimg_item_blend_op(gimg_doc_item(round, 0)), GIMG_BLEND_SOURCE);
  EXPECT_EQ(gimg_item_blend_op(gimg_doc_item(round, 1)), GIMG_BLEND_SOURCE);
  uint32_t loop = 99;
  EXPECT_TRUE(gimg_doc_loop_count(round, &loop));
  EXPECT_EQ(loop, 3u);
  uint8_t got_bg[4] = { 0, 0, 0, 0 };
  EXPECT_TRUE(gimg_doc_background_color(round, got_bg));
  EXPECT_EQ(got_bg[0], 1);
  EXPECT_EQ(got_bg[1], 2);
  EXPECT_EQ(got_bg[2], 3);
  EXPECT_EQ(got_bg[3], 255);

  gimg_raster_destroy(back0);
  gimg_raster_destroy(back1);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveAnimationWritesTheChangedRectangle) {
  /* Frame 1 changes one pixel at (4,4), which is already an even origin,
   * so the frame is 1×1 there. Frame 2 changes (1,2). The file stores
   * the origin in half-pixels, so that box grows left to x = 0 and is
   * 2×1. Both decoded frames are the canvases that were handed in. */
  GIMG_Raster * base = solid_rgba(8, 8, 200, 10, 10, 255);
  GIMG_Raster * dot = solid_rgba(8, 8, 200, 10, 10, 255);
  GIMG_Raster * nudge = solid_rgba(8, 8, 200, 10, 10, 255);
  ASSERT_NE(base, nullptr);
  ASSERT_NE(dot, nullptr);
  ASSERT_NE(nudge, nullptr);
  uint8_t * dot_px = static_cast<uint8_t *>(gimg_raster_pixels(dot));
  uint8_t * nudge_px = static_cast<uint8_t *>(gimg_raster_pixels(nudge));
  const size_t dot_stride = gimg_raster_stride_bytes(dot);
  memcpy(nudge_px, dot_px, dot_stride * 8u);
  uint8_t * changed = dot_px + 4u * dot_stride + 4u * 4u;
  changed[0] = 10;
  changed[1] = 10;
  changed[2] = 220;
  changed[3] = 255;
  memcpy(nudge_px, dot_px, dot_stride * 8u);
  uint8_t * odd = nudge_px + 2u * dot_stride + 1u * 4u;
  odd[0] = 10;
  odd[1] = 220;
  odd[2] = 10;
  odd[3] = 255;

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 3), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), base);
  gimg_item_set_raster(gimg_doc_item(doc, 1), dot);
  gimg_item_set_raster(gimg_doc_item(doc, 2), nudge);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 0), 20, 1000);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 1), 20, 1000);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 2), 20, 1000);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_effort = 4;
  opts.webp_exact = 1;
  GIMG_Save_Report report = {};
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, &report), GIMG_OK);
  const int64_t encode_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + nbytes);
  publish_animation("stub_anim_rect.webp", saved, {base, dot, nudge},
      encode_us, false);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(round);
  ASSERT_NE(st, nullptr);
  ASSERT_EQ(st->frame_count, 3u);
  EXPECT_EQ(st->frames[1].x, 4u);
  EXPECT_EQ(st->frames[1].y, 4u);
  EXPECT_EQ(st->frames[1].width, 1u);
  EXPECT_EQ(st->frames[1].height, 1u);
  EXPECT_EQ(st->frames[2].x, 0u);
  EXPECT_EQ(st->frames[2].y, 2u);
  EXPECT_EQ(st->frames[2].width, 2u);
  EXPECT_EQ(st->frames[2].height, 1u);
  GIMG_Raster * back0 = nullptr;
  GIMG_Raster * back1 = nullptr;
  GIMG_Raster * back2 = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back0),
      GIMG_OK);
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 1), nullptr, &back1),
      GIMG_OK);
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 2), nullptr, &back2),
      GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(base, back0));
  EXPECT_TRUE(gimg_ops_raster_equal(dot, back1));
  EXPECT_TRUE(gimg_ops_raster_equal(nudge, back2));

  gimg_raster_destroy(back0);
  gimg_raster_destroy(back1);
  gimg_raster_destroy(back2);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveAnimationUnchangedFrameKeepsItsDuration) {
  /* The second canvas matches the first, so nothing needs drawing. The
   * frame is still there: one pixel at the origin, and its delay. */
  GIMG_Raster * a = solid_rgba(8, 8, 40, 80, 120, 255);
  GIMG_Raster * b = solid_rgba(8, 8, 40, 80, 120, 255);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), a);
  gimg_item_set_raster(gimg_doc_item(doc, 1), b);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 0), 10, 1000);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 1), 30, 1000);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_effort = 4;
  opts.webp_exact = 1;
  GIMG_Save_Report report = {};
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, &report), GIMG_OK);
  const int64_t encode_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + nbytes);
  publish_animation("stub_anim_hold.webp", saved, {a, b}, encode_us, false);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(round);
  ASSERT_NE(st, nullptr);
  ASSERT_EQ(st->frame_count, 2u);
  EXPECT_EQ(st->frames[1].x, 0u);
  EXPECT_EQ(st->frames[1].y, 0u);
  EXPECT_EQ(st->frames[1].width, 1u);
  EXPECT_EQ(st->frames[1].height, 1u);
  uint16_t num = 0;
  uint16_t den = 0;
  gimg_item_frame_delay(gimg_doc_item(round, 1), &num, &den);
  EXPECT_EQ(num, 30);
  EXPECT_EQ(den, 1000);
  GIMG_Raster * back0 = nullptr;
  GIMG_Raster * back1 = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back0),
      GIMG_OK);
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 1), nullptr, &back1),
      GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(a, back0));
  EXPECT_TRUE(gimg_ops_raster_equal(b, back1));

  gimg_raster_destroy(back0);
  gimg_raster_destroy(back1);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveAnimationRefusesWhatTheFormatCannotHold) {
  GIMG_Raster * a = solid_rgba(8, 8, 1, 2, 3, 255);
  GIMG_Raster * b = solid_rgba(8, 8, 4, 5, 6, 255);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), a);
  gimg_item_set_raster(gimg_doc_item(doc, 1), b);
  gimg_item_set_dispose_op(gimg_doc_item(doc, 0), GIMG_DISPOSE_PREVIOUS);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_exact = 1;
  EXPECT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr),
      GIMG_ERR_UNSUPPORTED);
  gimg_stream_destroy(out);

  gimg_item_set_dispose_op(gimg_doc_item(doc, 0), GIMG_DISPOSE_NONE);
  b = solid_rgba(4, 4, 4, 5, 6, 255);
  ASSERT_NE(b, nullptr);
  gimg_item_set_raster(gimg_doc_item(doc, 1), b);
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  EXPECT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr),
      GIMG_ERR_UNSUPPORTED);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveAnimationLossyDecodable) {
  GIMG_Raster * red = solid_rgba(16, 16, 220, 20, 20, 255);
  GIMG_Raster * green = solid_rgba(16, 16, 20, 200, 20, 255);
  ASSERT_NE(red, nullptr);
  ASSERT_NE(green, nullptr);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), red);
  gimg_item_set_raster(gimg_doc_item(doc, 1), green);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 0), 40, 1000);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 1), 40, 1000);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 4;
  GIMG_Save_Report report = {};
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, &report), GIMG_OK);
  const int64_t encode_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + nbytes);
  publish_animation("stub_anim_lossy.webp", saved, {red, green}, encode_us,
      true);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  ASSERT_EQ(gimg_doc_item_count(round), 2u);
  GIMG_Raster * back0 = nullptr;
  GIMG_Raster * back1 = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back0),
      GIMG_OK);
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 1), nullptr, &back1),
      GIMG_OK);
  const uint8_t * p0 = static_cast<const uint8_t *>(gimg_raster_pixels(back0));
  const uint8_t * p1 = static_cast<const uint8_t *>(gimg_raster_pixels(back1));
  ASSERT_NE(p0, nullptr);
  ASSERT_NE(p1, nullptr);
  EXPECT_GT(p0[0], 150);
  EXPECT_GT(p1[1], 150);

  gimg_raster_destroy(back0);
  gimg_raster_destroy(back1);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

size_t save_webp(GIMG_Doc * doc, int effort, std::vector<uint8_t> * out,
    int64_t * encode_us = nullptr) {
  GIMG_Stream * stream = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&stream), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_effort = static_cast<uint8_t>(effort);
  opts.webp_exact = 1;
  GIMG_Save_Report report = {};
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_EQ(gimg_doc_save(doc, stream, "webp", &opts, &report), GIMG_OK);
  const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
      std::chrono::steady_clock::now() - t0).count();
  if (encode_us) {
    *encode_us = us;
  }
  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(stream, &bytes, &nbytes);
  out->assign(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + nbytes);
  gimg_stream_destroy(stream);
  return nbytes;
}

/** Leave a lossless file, its source pixels, and its encode time. */
void publish_encoded(const char * name, const std::vector<uint8_t> & bytes,
    GIMG_Raster * raster, int64_t encode_us) {
  mkdir(GIMG_TEST_OUT_WEBP, 0755);
  const std::string path = std::string(GIMG_TEST_OUT_WEBP) + "/" + name;
  {
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out)) << path;
    out.write(reinterpret_cast<const char *>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(static_cast<bool>(out)) << path;
  }
  const uint32_t w = gimg_raster_width(raster);
  const uint32_t h = gimg_raster_height(raster);
  const size_t stride = gimg_raster_stride_bytes(raster);
  const auto * px = static_cast<const uint8_t *>(gimg_raster_pixels(raster));
  {
    std::ofstream side(path + ".expected.rgba", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(side)) << path;
    for (uint32_t y = 0; y < h; ++y) {
      side.write(reinterpret_cast<const char *>(px + y * stride),
          static_cast<std::streamsize>(w) * 4);
    }
    ASSERT_TRUE(static_cast<bool>(side)) << path;
  }
  {
    std::ofstream side(path + ".time", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(side)) << path;
    side << encode_us << "\n";
  }
}

bool round_trip_matches(const std::vector<uint8_t> & bytes, GIMG_Raster * expect) {
  GIMG_Stream * in = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &in) != GIMG_OK) {
    return false;
  }
  GIMG_Doc * round = nullptr;
  const GIMG_Result loaded = gimg_doc_load(in, nullptr, nullptr, &round);
  gimg_stream_destroy(in);
  if (loaded != GIMG_OK) {
    return false;
  }
  GIMG_Raster * back = nullptr;
  const GIMG_Result decoded =
      gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back);
  const bool same =
      decoded == GIMG_OK && back && gimg_ops_raster_equal(expect, back);
  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  return same;
}

TEST(Webp, SaveLosslessRoundTrip) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("simple_lossless.webp", &doc), GIMG_OK);
  GIMG_Raster * orig = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &orig), GIMG_OK);
  ASSERT_NE(orig, nullptr);
  const uint32_t w = gimg_raster_width(orig);
  const uint32_t h = gimg_raster_height(orig);
  gimg_item_set_raster(gimg_doc_item(doc, 0), orig);
  orig = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_effort = 4;
  opts.webp_exact = 1;
  GIMG_Save_Report report = {};
  const auto t0 = std::chrono::steady_clock::now();
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, &report), GIMG_OK);
  const int64_t encode_us =
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - t0).count();
  EXPECT_GT(report.bytes_written, 0u);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  ASSERT_GT(nbytes, 0u);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(bytes),
      static_cast<const uint8_t *>(bytes) + nbytes);
  publish_encoded("simple_lossless_e4.webp", saved,
      gimg_item_raster(gimg_doc_item(doc, 0)), encode_us);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back),
      GIMG_OK);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(gimg_raster_width(back), w);
  EXPECT_EQ(gimg_raster_height(back), h);

  /* Compare against a fresh decode of the source fixture. */
  GIMG_Doc * again = nullptr;
  ASSERT_EQ(load_doc("simple_lossless.webp", &again), GIMG_OK);
  GIMG_Raster * expect = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(again, 0), nullptr, &expect),
      GIMG_OK);
  EXPECT_TRUE(gimg_ops_raster_equal(expect, back));

  gimg_raster_destroy(expect);
  gimg_doc_destroy(again);
  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLosslessEffortShrinksGradient) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossless_gradient.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  std::vector<uint8_t> literals;
  std::vector<uint8_t> predicted;
  std::vector<uint8_t> crossed;
  int64_t us_lit = 0;
  int64_t us_pred = 0;
  int64_t us_cross = 0;
  const size_t n_lit = save_webp(doc, 1, &literals, &us_lit);
  const size_t n_pred = save_webp(doc, 2, &predicted, &us_pred);
  const size_t n_cross = save_webp(doc, 4, &crossed, &us_cross);
  publish_encoded("gradient_e1.webp", literals, raster, us_lit);
  publish_encoded("gradient_e2.webp", predicted, raster, us_pred);
  publish_encoded("gradient_e4.webp", crossed, raster, us_cross);
  /* One Huffman group on the literals was 2160 bytes. Predictor selection
   * by residual histogram cost lands effort 2 under cwebp's 60-byte file.
   * No cross-colour grid shortens that file, so effort 4 stays with it. */
  EXPECT_LT(n_lit, 2160u);
  EXPECT_LT(n_pred, n_lit);
  EXPECT_LT(n_pred, 60u);
  EXPECT_LE(n_cross, n_pred);
  EXPECT_TRUE(round_trip_matches(crossed, raster));
  EXPECT_TRUE(round_trip_matches(predicted, raster));
  EXPECT_TRUE(round_trip_matches(literals, raster));
  gimg_doc_destroy(doc);
}

GIMG_Doc * doc_with_colors(int width, int height, int ncolors) {
  GIMG_Raster * raster = nullptr;
  EXPECT_EQ(gimg_raster_create(static_cast<uint32_t>(width),
                static_cast<uint32_t>(height), &GIMG_PIXEL_RGBA8,
                GIMG_RASTER_OWNED, nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  EXPECT_NE(px, nullptr);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const int c = (ncolors == 2) ? ((x + y) & 1) : ((x * 3 + y) % ncolors);
      uint8_t * p = px + (y * width + x) * 4;
      p[0] = static_cast<uint8_t>(20 + c * 30);
      p[1] = static_cast<uint8_t>(200 - c * 15);
      p[2] = static_cast<uint8_t>(c * 40);
      p[3] = 255;
    }
  }
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  return doc;
}

TEST(Webp, SaveLosslessPaletteShrinksChecker) {
  GIMG_Doc * doc = doc_with_colors(32, 8, 2);
  ASSERT_NE(doc, nullptr);
  GIMG_Raster * raster = gimg_item_raster(gimg_doc_item(doc, 0));
  std::vector<uint8_t> plain;
  std::vector<uint8_t> pal;
  int64_t us_plain = 0;
  int64_t us_pal = 0;
  const size_t n_plain = save_webp(doc, 3, &plain, &us_plain);
  const size_t n_pal = save_webp(doc, 4, &pal, &us_pal);
  publish_encoded("palette_plain_e3.webp", plain, raster, us_plain);
  publish_encoded("palette_checker_e4.webp", pal, raster, us_pal);
  EXPECT_LT(n_pal, n_plain);
  EXPECT_TRUE(round_trip_matches(pal, raster));
  gimg_doc_destroy(doc);

  GIMG_Doc * wide = doc_with_colors(15, 7, 6);
  ASSERT_NE(wide, nullptr);
  std::vector<uint8_t> six;
  int64_t us_six = 0;
  ASSERT_GT(save_webp(wide, 4, &six, &us_six), 0u);
  publish_encoded("palette_six_e4.webp", six,
      gimg_item_raster(gimg_doc_item(wide, 0)), us_six);
  EXPECT_TRUE(round_trip_matches(six, gimg_item_raster(gimg_doc_item(wide, 0))));
  gimg_doc_destroy(wide);
}

TEST(Webp, SaveLosslessRepeatedRowRoundTrip) {
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(24, 12, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int y = 0; y < 12; ++y) {
    for (int x = 0; x < 24; ++x) {
      uint8_t * p = px + (y * 24 + x) * 4;
      p[0] = static_cast<uint8_t>(x * 9);
      p[1] = static_cast<uint8_t>(40 + (x % 5) * 20);
      p[2] = static_cast<uint8_t>(255 - x * 4);
      p[3] = 255;
    }
  }
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  std::vector<uint8_t> bytes;
  int64_t encode_us = 0;
  ASSERT_GT(save_webp(doc, 4, &bytes, &encode_us), 0u);
  publish_encoded("repeated_row_e4.webp", bytes,
      gimg_item_raster(gimg_doc_item(doc, 0)), encode_us);
  EXPECT_TRUE(round_trip_matches(bytes, gimg_item_raster(gimg_doc_item(doc, 0))));
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLosslessSkewedHistogramRoundTrip) {
  /* One dominant green and 200 singletons. The Huffman tree for that
   * histogram is deeper than 15. A flat code over fewer than 256 symbols
   * does not fill every branch, and the decoder rejects it. */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 32, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int i = 0; i < 32 * 32; ++i) {
    uint8_t * p = px + i * 4;
    p[0] = 10;
    p[1] = (i < 200) ? static_cast<uint8_t>(i) : 0;
    p[2] = 20;
    p[3] = 255;
  }
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  std::vector<uint8_t> bytes;
  int64_t encode_us = 0;
  ASSERT_GT(save_webp(doc, 0, &bytes, &encode_us), 0u);
  publish_encoded("skewed_e0.webp", bytes,
      gimg_item_raster(gimg_doc_item(doc, 0)), encode_us);
  EXPECT_TRUE(round_trip_matches(bytes, gimg_item_raster(gimg_doc_item(doc, 0))));
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossyDecodable) {
  /* 32x16 opaque solid: two macroblocks, so the second is predicted from
   * the first. Chroma is uniform, so replicating each sample onto its 2x2
   * matches dwebp. The sidecar tells the oracle to check that parity and
   * not lossless fidelity to the source. */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int i = 0; i < 32 * 16; ++i) {
    px[i * 4 + 0] = 200;
    px[i * 4 + 1] = 100;
    px[i * 4 + 2] = 50;
    px[i * 4 + 3] = 255;
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 4;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, &report), GIMG_OK);
  EXPECT_GT(report.bytes_written, 0u);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  const gimg_webp_doc_state_t * st = state_of(round);
  ASSERT_NE(st, nullptr);
  EXPECT_TRUE(st->is_lossy);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  ASSERT_NE(back, nullptr);
  EXPECT_EQ(gimg_raster_width(back), 32u);
  EXPECT_EQ(gimg_raster_height(back), 16u);
  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_32x16.webp";
    std::ofstream out(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(out));
    out.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossyGradientKeepsDetail) {
  /* A horizontal ramp. Intra16 DC alone paints each macroblock one
   * colour, so the two ends of a block match. The 4x4 residual must
   * separate them. The sidecar checks dwebp parity, not fidelity. */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 32; ++x) {
      uint8_t * p = px + (y * 32 + x) * 4;
      p[0] = static_cast<uint8_t>(x * 255 / 31);
      p[1] = 40;
      p[2] = static_cast<uint8_t>(255 - x * 255 / 31);
      p[3] = 255;
    }
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 4;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got = static_cast<const uint8_t *>(gimg_raster_pixels(back));
  ASSERT_NE(got, nullptr);
  const int left = got[0];
  const int right = got[15 * 4];
  const int span = left > right ? left - right : right - left;
  EXPECT_GT(span, 20);

  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_gradient.webp";
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossyRampPricesBits) {
  /* A 32×32 ramp at effort 0. The frame quantizer is index 68, and a
   * macroblock the 16×16 predictors cannot explain takes the finer
   * index. The corners come back as 19 and 78, and the file is 60 bytes.
   */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 32, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 32; ++x) {
      uint8_t * p = px + (y * 32 + x) * 4;
      const uint8_t v = (uint8_t)(16 + x + y);
      p[0] = v;
      p[1] = v;
      p[2] = v;
      p[3] = 255;
    }
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 0;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got = static_cast<const uint8_t *>(gimg_raster_pixels(back));
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got[0], 19);
  EXPECT_EQ(got[(31 * 32 + 31) * 4], 78);
  EXPECT_EQ(nbytes, 60u);

  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_ramp.webp";
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossyStepPricesVertical) {
  /* Two macroblocks, both a left-black/right-white step. Error alone
   * ties, so both stay DC. The lower block's
   * vertical predictor repeats the reconstructed row and its residual
   * is shorter; pricing the bits selects it. The file is 52 bytes.
   */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 32, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint8_t * p = px + (y * 16 + x) * 4;
      const uint8_t v = x < 8 ? 0 : 255;
      p[0] = v;
      p[1] = v;
      p[2] = v;
      p[3] = 255;
    }
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 0;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got = static_cast<const uint8_t *>(gimg_raster_pixels(back));
  ASSERT_NE(got, nullptr);
  EXPECT_LT(got[20 * 16 * 4], 40);
  EXPECT_GT(got[(20 * 16 + 12) * 4], 200);
  EXPECT_EQ(nbytes, 52u);

  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_step.webp";
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossyCheckerUsesSubblocks) {
  /* A one-pixel checkerboard, two macroblocks tall. At effort 4
   * both macroblocks take the finer index and the sixteen 4×4
   * predictors, and the file is 186 bytes. The lower block's modes are
   * coded against the upper block's modes. The origin stays dark and
   * its right neighbour stays bright, and the same holds on the next
   * row of macroblocks.
   */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 32, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int y = 0; y < 32; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint8_t * p = px + (y * 16 + x) * 4;
      const uint8_t v = ((x ^ y) & 1) ? 255 : 0;
      p[0] = v;
      p[1] = v;
      p[2] = v;
      p[3] = 255;
    }
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 4;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got = static_cast<const uint8_t *>(gimg_raster_pixels(back));
  ASSERT_NE(got, nullptr);
  EXPECT_LT(got[0], 40);
  EXPECT_GT(got[4], 200);
  EXPECT_LT(got[16 * 16 * 4], 40);
  EXPECT_GT(got[(16 * 16 + 1) * 4], 200);
  EXPECT_EQ(nbytes, 186u);

  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_checker.webp";
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossySpikeDropsWeakCoeff) {
  /* One bright pixel at effort 2. The block is not explained by a
   * 16×16 predictor, so the frame takes the finer index, and the file
   * is 60 bytes.
   */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  memset(px, 0, 16u * 16u * 4u);
  for (int i = 0; i < 16 * 16; ++i) {
    px[i * 4 + 3] = 255;
  }
  px[(8 * 16 + 8) * 4 + 0] = 255;
  px[(8 * 16 + 8) * 4 + 1] = 255;
  px[(8 * 16 + 8) * 4 + 2] = 255;

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 2;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);
  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(nbytes, 60u);
  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_spike.webp";
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossyAlphaKeepsPlane) {
  /* Opaque red on the left, fully transparent green on the right.
   * Alpha is stored exactly. The colour is premultiplied, so the
   * transparent side comes back black and the red side stays red.
   */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(16, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      uint8_t * p = px + (y * 16 + x) * 4;
      if (x < 8) {
        p[0] = 220;
        p[1] = 10;
        p[2] = 10;
        p[3] = 255;
      }
      else {
        p[0] = 10;
        p[1] = 220;
        p[2] = 10;
        p[3] = 0;
      }
    }
  }
  px[(4 * 16 + 4) * 4 + 3] = 128;

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 4;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got = static_cast<const uint8_t *>(gimg_raster_pixels(back));
  ASSERT_NE(got, nullptr);
  EXPECT_EQ(got[3], 255);
  EXPECT_GT(got[0], 150);
  EXPECT_EQ(got[(8) * 4 + 3], 0);
  EXPECT_LT(got[8 * 4], 40);
  EXPECT_EQ(got[(4 * 16 + 4) * 4 + 3], 128);

  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_alpha.webp";
    std::ofstream file(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(file));
    file.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

/** One prob-128 bool, matching the decoder's bit-at-a-time renorm. */
static int vp8_uniform_bit(const uint8_t ** p, const uint8_t * end,
    uint32_t * value, uint32_t * range, int * bit_count) {
  const uint32_t split = 1u + (((*range - 1u) * 128u) >> 8);
  int bit;
  if (*value >= (split << 8)) {
    bit = 1;
    *range -= split;
    *value -= split << 8;
  }
  else {
    bit = 0;
    *range = split;
  }
  while (*range < 128u) {
    *value <<= 1;
    *range <<= 1;
    ++(*bit_count);
    if (*bit_count == 8) {
      *bit_count = 0;
      if (*p < end) {
        *value |= *(*p)++;
      }
    }
  }
  return bit;
}

/** L(n), most significant bit first, the same order the encoder writes. */
static int vp8_literal(const uint8_t ** p, const uint8_t * end, uint32_t * value,
    uint32_t * range, int * bit_count, int nbits) {
  int v = 0;
  for (int i = 0; i < nbits; ++i) {
    v = (v << 1) | vp8_uniform_bit(p, end, value, range, bit_count);
  }
  return v;
}

TEST(Webp, SaveLossySegmentsFlatBesideDetail) {
  /* Left macroblock is flat, right one is a checkerboard. The checker
   * is not explained by a 16×16 predictor, so it takes the finer
   * segment. The segmentation flag after the two keyframe header bools
   * is on. The flat side stays near mid-gray and the checker keeps its
   * contrast.
   */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(32, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 32; ++x) {
      uint8_t * p = px + (y * 32 + x) * 4;
      const uint8_t v = x < 16 ? 128 : (((x ^ y) & 1) ? 255 : 0);
      p[0] = v;
      p[1] = v;
      p[2] = v;
      p[3] = 255;
    }
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 4;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  const auto * file = static_cast<const uint8_t *>(bytes);
  const uint8_t * vp8 = nullptr;
  for (size_t i = 12; i + 8 < nbytes; ) {
    const uint32_t sz = (uint32_t)file[i + 4] | ((uint32_t)file[i + 5] << 8) |
        ((uint32_t)file[i + 6] << 16) | ((uint32_t)file[i + 7] << 24);
    if (file[i] == 'V' && file[i + 1] == 'P' && file[i + 2] == '8' &&
        file[i + 3] == ' ') {
      vp8 = file + i + 8;
      break;
    }
    i += 8u + sz + (sz & 1u);
  }
  ASSERT_NE(vp8, nullptr);
  const uint8_t * p = vp8 + 12;
  const uint8_t * end = file + nbytes;
  uint32_t value = ((uint32_t)vp8[10] << 8) | vp8[11];
  uint32_t range = 255;
  int bit_count = 0;
  EXPECT_EQ(vp8_uniform_bit(&p, end, &value, &range, &bit_count), 0);
  EXPECT_EQ(vp8_uniform_bit(&p, end, &value, &range, &bit_count), 0);
  EXPECT_EQ(vp8_uniform_bit(&p, end, &value, &range, &bit_count), 1);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got = static_cast<const uint8_t *>(gimg_raster_pixels(back));
  ASSERT_NE(got, nullptr);
  EXPECT_GT(got[0], 100);
  EXPECT_LT(got[0], 160);
  EXPECT_LT(got[(20) * 4], 40);
  EXPECT_GT(got[(21) * 4], 200);

  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_segments.webp";
    std::ofstream outfile(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(outfile));
    outfile.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossySkipDropsEmptyTokens) {
  /* Sixteen flat macroblocks. RGB (130,130,129) lands on Y=U=V=128, so
   * every residual quantizes to zero. Writing the skip flag is 48
   * bytes. Leaving it off and emitting the zero tokens is 50.
   */
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(256, 16, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &raster),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
  ASSERT_NE(px, nullptr);
  for (int i = 0; i < 256 * 16; ++i) {
    px[i * 4 + 0] = 130;
    px[i * 4 + 1] = 130;
    px[i * 4 + 2] = 129;
    px[i * 4 + 3] = 255;
  }

  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  raster = nullptr;

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  opts.webp_effort = 4;
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);

  const void * bytes = nullptr;
  size_t nbytes = 0;
  gimg_stream_output_buffer(out, &bytes, &nbytes);
  ASSERT_NE(bytes, nullptr);
  EXPECT_EQ(nbytes, 48u);
  const auto * file = static_cast<const uint8_t *>(bytes);
  const uint8_t * vp8 = nullptr;
  for (size_t i = 12; i + 8 < nbytes;) {
    const uint32_t sz = (uint32_t)file[i + 4] | ((uint32_t)file[i + 5] << 8) |
        ((uint32_t)file[i + 6] << 16) | ((uint32_t)file[i + 7] << 24);
    if (file[i] == 'V' && file[i + 1] == 'P' && file[i + 2] == '8' &&
        file[i + 3] == ' ') {
      vp8 = file + i + 8;
      break;
    }
    i += 8u + sz + (sz & 1u);
  }
  ASSERT_NE(vp8, nullptr);
  const uint32_t tag = (uint32_t)vp8[0] | ((uint32_t)vp8[1] << 8) |
      ((uint32_t)vp8[2] << 16);
  EXPECT_EQ(tag >> 5, 16u);

  GIMG_Stream * in = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bytes, nbytes, &in), GIMG_OK);
  GIMG_Doc * round = nullptr;
  ASSERT_EQ(gimg_doc_load(in, nullptr, nullptr, &round), GIMG_OK);
  GIMG_Raster * back = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(round, 0), nullptr, &back), GIMG_OK);
  const uint8_t * got = static_cast<const uint8_t *>(gimg_raster_pixels(back));
  ASSERT_NE(got, nullptr);
  EXPECT_NEAR(got[0], 130, 8);
  EXPECT_NEAR(got[1], 130, 8);
  EXPECT_NEAR(got[2], 130, 8);
  EXPECT_EQ(got[3], 255);
  EXPECT_NEAR(got[16 * 4], 130, 8);

  {
    mkdir(GIMG_TEST_OUT_WEBP, 0755);
    const std::string path =
        std::string(GIMG_TEST_OUT_WEBP) + "/stub_lossy_skip.webp";
    std::ofstream outfile(path, std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(outfile));
    outfile.write(static_cast<const char *>(bytes),
        static_cast<std::streamsize>(nbytes));
    std::ofstream mark(path + ".lossy", std::ios::binary);
    ASSERT_TRUE(static_cast<bool>(mark));
  }

  gimg_raster_destroy(back);
  gimg_doc_destroy(round);
  gimg_stream_destroy(in);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

TEST(Webp, SaveLossyFilterLevelFollowsError) {
  /* The level field is six bits whatever the value, so the choice is
   * the filtered reconstruction closest to the source, and a tie stays
   * at the lower level. RGB (130,130,129) is Y=U=V=128, every level
   * reconstructs it the same, and the level stays 0. A horizontal ramp
   * is closer after the filter. When the segmentation flag is on, the
   * quantizer and loop-filter deltas and the segment probabilities
   * sit between that flag and the filter type.
   */
  auto level_of = [](int w, int h, bool ramp) {
    GIMG_Raster * raster = nullptr;
    EXPECT_EQ(gimg_raster_create((uint32_t)w, (uint32_t)h, &GIMG_PIXEL_RGBA8,
                  GIMG_RASTER_OWNED, nullptr, 0, &raster),
        GIMG_OK);
    uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(raster));
    EXPECT_NE(px, nullptr);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        uint8_t * p = px + (y * w + x) * 4;
        if (ramp) {
          p[0] = static_cast<uint8_t>(x * 255 / (w - 1));
          p[1] = 40;
          p[2] = static_cast<uint8_t>(255 - x * 255 / (w - 1));
        }
        else {
          p[0] = 130;
          p[1] = 130;
          p[2] = 129;
        }
        p[3] = 255;
      }
    }
    GIMG_Doc * doc = nullptr;
    EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
    EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
    gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
    GIMG_Stream * out = nullptr;
    EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Options opts = {};
    opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
    opts.webp_effort = 4;
    EXPECT_EQ(gimg_doc_save(doc, out, "webp", &opts, nullptr), GIMG_OK);
    const void * bytes = nullptr;
    size_t nbytes = 0;
    gimg_stream_output_buffer(out, &bytes, &nbytes);
    EXPECT_NE(bytes, nullptr);
    const auto * file = static_cast<const uint8_t *>(bytes);
    const uint8_t * vp8 = nullptr;
    for (size_t i = 12; i + 8 < nbytes;) {
      const uint32_t sz = (uint32_t)file[i + 4] | ((uint32_t)file[i + 5] << 8) |
          ((uint32_t)file[i + 6] << 16) | ((uint32_t)file[i + 7] << 24);
      if (file[i] == 'V' && file[i + 1] == 'P' && file[i + 2] == '8' &&
          file[i + 3] == ' ') {
        vp8 = file + i + 8;
        break;
      }
      i += 8u + sz + (sz & 1u);
    }
    EXPECT_NE(vp8, nullptr);
    const uint8_t * p = vp8 + 12;
    const uint8_t * end = file + nbytes;
    uint32_t value = ((uint32_t)vp8[10] << 8) | vp8[11];
    uint32_t range = 255;
    int bit_count = 0;
    EXPECT_EQ(vp8_uniform_bit(&p, end, &value, &range, &bit_count), 0);
    EXPECT_EQ(vp8_uniform_bit(&p, end, &value, &range, &bit_count), 0);
    if (vp8_uniform_bit(&p, end, &value, &range, &bit_count)) {
      const int update_map = vp8_uniform_bit(&p, end, &value, &range, &bit_count);
      const int update_data =
          vp8_uniform_bit(&p, end, &value, &range, &bit_count);
      if (update_data) {
        auto skip_signed = [&](int nbits) {
          if (vp8_uniform_bit(&p, end, &value, &range, &bit_count)) {
            vp8_literal(&p, end, &value, &range, &bit_count, nbits);
            vp8_uniform_bit(&p, end, &value, &range, &bit_count);
          }
        };
        vp8_uniform_bit(&p, end, &value, &range, &bit_count);
        for (int s = 0; s < 4; ++s) {
          skip_signed(7);
        }
        for (int s = 0; s < 4; ++s) {
          skip_signed(6);
        }
      }
      if (update_map) {
        for (int s = 0; s < 3; ++s) {
          if (vp8_uniform_bit(&p, end, &value, &range, &bit_count)) {
            vp8_literal(&p, end, &value, &range, &bit_count, 8);
          }
        }
      }
    }
    const int simple = vp8_literal(&p, end, &value, &range, &bit_count, 1);
    const int level = vp8_literal(&p, end, &value, &range, &bit_count, 6);
    const int sharp = vp8_literal(&p, end, &value, &range, &bit_count, 3);
    EXPECT_EQ(simple, 0);
    EXPECT_EQ(sharp, 0);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    return level;
  };
  EXPECT_EQ(level_of(256, 16, false), 0);
  EXPECT_EQ(level_of(32, 16, true), 15);
}

TEST(Webp, AlphPlaneMatchesDwebpLossyAlpha) {
  expect_alph_matches_pam_alpha("lossy_alpha.webp", "lossy_alpha.pam");
}

TEST(Webp, AlphPlaneMatchesDwebpMethod0And1) {
  static const char * kFiles[] = {
      "alph_m0_none", "alph_m0_fast", "alph_m0_best",
      "alph_m1_none", "alph_m1_fast", "alph_m1_best",
  };
  for (const char * base : kFiles) {
    const std::string webp = std::string(base) + ".webp";
    const std::string pam = std::string(base) + ".pam";
    SCOPED_TRACE(webp);
    expect_alph_matches_pam_alpha(webp.c_str(), pam.c_str());
  }
}

/** Craft method-0 ALPH for each spatial filter and round-trip the plane. */
TEST(Webp, AlphUncompressedAllFiltersRoundTrip) {
  const int w = 5;
  const int h = 4;
  std::vector<uint8_t> src(static_cast<size_t>(w * h));
  for (int i = 0; i < w * h; ++i) {
    src[static_cast<size_t>(i)] =
        static_cast<uint8_t>((i * 37 + 11) & 0xff);
  }
  for (int filter = 0; filter < 4; ++filter) {
    SCOPED_TRACE(filter);
    std::vector<uint8_t> filtered;
    filter_alpha(src, w, h, filter, filtered);
    std::vector<uint8_t> payload(1u + filtered.size());
    payload[0] = static_cast<uint8_t>(
        (0u /*method*/) | (static_cast<unsigned>(filter) << 2));
    std::copy(filtered.begin(), filtered.end(), payload.begin() + 1);
    uint8_t * alpha = nullptr;
    ASSERT_EQ(gimg_webp_alpha_decode(payload.data(), payload.size(),
                   static_cast<uint32_t>(w), static_cast<uint32_t>(h), nullptr,
                   &alpha),
        GIMG_OK);
    ASSERT_NE(alpha, nullptr);
    for (int i = 0; i < w * h; ++i) {
      EXPECT_EQ(alpha[i], src[static_cast<size_t>(i)]) << "i=" << i;
    }
    gimg_free(gimg_allocator_default(), alpha);
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
