/**
 * @file
 *
 * WebP Phase A–E: load, VP8L/VP8/ALPH decode, ANIM/ANMF composite; refuse save.
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

TEST(Webp, DecodeSimpleLossyMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("simple_lossy.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "simple_lossy.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

TEST(Webp, DecodeLossyGradMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossy_grad.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossy_grad.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

TEST(Webp, DecodeLossyExifMatchesDwebpPixels) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossy_exif.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossy_exif.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

TEST(Webp, DecodeLossyAlphaMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossy_alpha.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossy_alpha.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

TEST(Webp, DecodeLossyGradAlphaMatchesDwebp) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("lossy_grad_alpha.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  expect_raster_matches_pam(raster, "lossy_grad_alpha.pam");
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

TEST(Webp, DecodeAlphMethodFixturesMatchDwebp) {
  static const char * kFiles[] = {
      "alph_m0_none", "alph_m0_fast", "alph_m0_best",
      "alph_m1_none", "alph_m1_fast", "alph_m1_best",
  };
  for (const char * base : kFiles) {
    const std::string webp = std::string(base) + ".webp";
    const std::string pam = std::string(base) + ".pam";
    SCOPED_TRACE(webp);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(load_doc(webp.c_str(), &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
        GIMG_OK);
    ASSERT_NE(raster, nullptr);
    expect_raster_matches_pam(raster, pam.c_str());
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

TEST(Webp, SaveStillUnsupported) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_doc("simple_lossy.webp", &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
      GIMG_OK);
  gimg_raster_destroy(raster);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  EXPECT_EQ(gimg_doc_save(doc, out, "webp", nullptr, nullptr),
      GIMG_ERR_UNSUPPORTED);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
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
