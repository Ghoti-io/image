/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The ICC profile of a decoded WebP.
 *
 * A WebP's ICCP chunk used to stay in the document's raw blocks, so a
 * WebP-loaded document saved as JPEG, PNG, TIFF or BMP lost it, and the save
 * report said so.  The decoder now puts it on the raster as the other decoders
 * do, and the raw block stays for a WebP save.  Lossy, lossless, lossy-with-
 * alpha and animated files are written with a profile (by the library's own
 * writer), decoded, and saved into the other formats, which must carry it and
 * must not say they dropped it; saved as WebP again they write the one chunk.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/color/color.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

// ---------------------------------------------------------------------------
// Pictures and files made with the library's own encoder
// ---------------------------------------------------------------------------

uint32_t next_rand(uint32_t & s) {
  s = s * 1664525u + 1013904223u;
  return s >> 8;
}

Bytes profile_bytes() {
  Bytes p(128, 0);
  p[3] = 128;
  std::memcpy(p.data() + 36, "acsp", 4);
  for (size_t i = 40; i < p.size(); i++) {
    p[i] = static_cast<uint8_t>((i * 7u) & 0xFFu);
  }
  return p;
}

/** Smooth gradients with a few hard edges, so macroblock borders have content. */
Bytes picture(uint32_t w, uint32_t h, uint32_t seed, bool alpha) {
  Bytes px(static_cast<size_t>(w) * h * 4u);
  uint32_t s = seed;
  const uint32_t cx = next_rand(s) % w;
  const uint32_t cy = next_rand(s) % h;
  for (uint32_t y = 0; y < h; ++y) {
    for (uint32_t x = 0; x < w; ++x) {
      uint8_t * p = &px[(static_cast<size_t>(y) * w + x) * 4u];
      const bool edge = (x > cx) != (y > cy);
      p[0] = static_cast<uint8_t>((x * 255u) / w + (edge ? 70u : 0u));
      p[1] = static_cast<uint8_t>((y * 255u) / h + seed * 3u);
      p[2] = static_cast<uint8_t>(((x + y) * 128u) / (w + h) + (edge ? 0u : 90u));
      p[3] = alpha ? static_cast<uint8_t>(60u + (x * 150u) / w) : 255u;
    }
  }
  return px;
}

GIMG_Raster * raster_of(
    uint32_t w, uint32_t h, const Bytes & px, const Bytes * icc) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                w, h, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  uint8_t * dst = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < h; ++y) {
    std::memcpy(dst + y * stride, &px[static_cast<size_t>(y) * w * 4u],
        static_cast<size_t>(w) * 4u);
  }
  if (icc) {
    GCOL_Color_Info ci;
    gcol_color_info_default(&ci);
    ci.icc_bytes = icc->data();
    ci.icc_size = icc->size();
    EXPECT_EQ(gimg_raster_set_color_info(r, &ci), GIMG_OK);
  }
  return r;
}

Bytes save_doc(const GIMG_Doc * doc, const char * format,
    const GIMG_Save_Options * opts, GIMG_Save_Report * report = nullptr) {
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  EXPECT_EQ(gimg_doc_save(doc, out, format, opts, report), GIMG_OK) << format;
  const void * b = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &b, &n);
  Bytes file(
      static_cast<const uint8_t *>(b), static_cast<const uint8_t *>(b) + n);
  gimg_stream_destroy(out);
  return file;
}

/** A still WebP of @p px, lossy or lossless, carrying @p icc when given. */
Bytes encode_still(uint32_t w, uint32_t h, const Bytes & px, bool lossy,
    const Bytes * icc = nullptr) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster_of(w, h, px, icc));
  GIMG_Save_Options opts = {};
  opts.webp_lossless =
      lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
  opts.webp_effort = 1;
  Bytes file = save_doc(doc, "webp", &opts);
  gimg_doc_destroy(doc);
  return file;
}

/** The payload of the first chunk named @p fourcc in a WebP file. */
Bytes chunk_payload(const Bytes & file, const char * fourcc) {
  size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const uint32_t size = file[pos + 4] | (file[pos + 5] << 8) |
        (file[pos + 6] << 16) | (static_cast<uint32_t>(file[pos + 7]) << 24);
    if (std::memcmp(&file[pos], fourcc, 4) == 0) {
      return Bytes(file.begin() + pos + 8, file.begin() + pos + 8 + size);
    }
    pos += 8 + size + (size & 1u);
  }
  return Bytes();
}

size_t count_chunks(const Bytes & file, const char * fourcc) {
  size_t n = 0;
  size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const uint32_t size = file[pos + 4] | (file[pos + 5] << 8) |
        (file[pos + 6] << 16) | (static_cast<uint32_t>(file[pos + 7]) << 24);
    if (std::memcmp(&file[pos], fourcc, 4) == 0) {
      n++;
    }
    pos += 8 + size + (size & 1u);
  }
  return n;
}

// ---------------------------------------------------------------------------
// The profile
// ---------------------------------------------------------------------------

constexpr uint32_t kIccp = 'I' | ('C' << 8) | ('C' << 16) | (static_cast<uint32_t>('P') << 24);

struct Loaded {
  GIMG_Stream * in = nullptr;
  GIMG_Doc * doc = nullptr;
  explicit Loaded(const Bytes & file) {
    EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &in), GIMG_OK);
    EXPECT_EQ(gimg_doc_load(in, nullptr, nullptr, &doc), GIMG_OK);
  }
  ~Loaded() {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(in);
  }
  void decode_all() {
    for (size_t i = 0; i < gimg_doc_item_count(doc); ++i) {
      EXPECT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, i), nullptr), GIMG_OK);
    }
  }
};

/** The profile on a raster, or empty. */
Bytes profile_of(const GIMG_Raster * r) {
  const GCOL_Color_Info * ci = r ? gimg_raster_color_info_const(r) : nullptr;
  if (!ci || !ci->icc_bytes || ci->icc_size == 0) {
    return Bytes();
  }
  const uint8_t * p = static_cast<const uint8_t *>(ci->icc_bytes);
  return Bytes(p, p + ci->icc_size);
}

Bytes raw_iccp(const GIMG_Doc * doc) {
  const GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  size_t n = 0;
  if (!raw || gimg_meta_raw_get(raw, "webp", kIccp, nullptr, &n) != GIMG_OK || n == 0) {
    return Bytes();
  }
  Bytes b(n);
  EXPECT_EQ(gimg_meta_raw_get(raw, "webp", kIccp, b.data(), &n), GIMG_OK);
  return b;
}

struct Case {
  const char * label;
  bool lossy;
  bool alpha;
  bool animated;
};

const Case kCases[] = {
    {"lossless", false, false, false},
    {"lossless_alpha", false, true, false},
    {"lossy", true, false, false},
    {"lossy_alpha", true, true, false},
    {"animated_lossless", false, false, true},
    {"animated_lossy", true, false, true},
};

Bytes file_for(const Case & c, const Bytes & icc) {
  if (!c.animated) {
    return encode_still(24, 16, picture(24, 16, 5, c.alpha), c.lossy, &icc);
  }
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 3), GIMG_OK);
  for (size_t i = 0; i < 3; ++i) {
    gimg_item_set_raster(gimg_doc_item(doc, i),
        raster_of(24, 16, picture(24, 16, 11 + static_cast<uint32_t>(i), false), &icc));
    gimg_item_set_frame_delay(gimg_doc_item(doc, i), 40, 1000);
  }
  GIMG_Save_Options opts = {};
  opts.webp_lossless =
      c.lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
  opts.webp_effort = 1;
  Bytes file = save_doc(doc, "webp", &opts);
  gimg_doc_destroy(doc);
  return file;
}

class WebpProfile : public ::testing::TestWithParam<Case> {};

}  // namespace

TEST_P(WebpProfile, DecoderPutsTheChunkOnTheRasterAndKeepsTheRawBlock) {
  const Case c = GetParam();
  const Bytes icc = profile_bytes();
  const Bytes file = file_for(c, icc);
  ASSERT_EQ(count_chunks(file, "ICCP"), 1u) << "the writer should carry it";
  Loaded l(file);
  EXPECT_EQ(raw_iccp(l.doc), icc);
  for (size_t i = 0; i < gimg_doc_item_count(l.doc); ++i) {
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(l.doc, i), nullptr, &r), GIMG_OK);
    EXPECT_EQ(profile_of(r), icc) << c.label << " item " << i;
    gimg_raster_destroy(r);
  }
}

TEST_P(WebpProfile, SavingAsAnotherFormatCarriesItWithoutAWarning) {
  const Case c = GetParam();
  const Bytes icc = profile_bytes();
  const Bytes file = file_for(c, icc);
  const char * targets_still[] = {"jpeg", "png", "tiff", "bmp"};
  const char * targets_anim[] = {"png", "tiff"};
  const char * const * targets = c.animated ? targets_anim : targets_still;
  const size_t count = c.animated ? 2 : 4;
  // Once with the document's items decoded first, and once straight after the
  // load, when the writer decodes each item for itself.
  for (size_t n = 0; n < count * 2; ++n) {
    const bool decode_first = n >= count;
    const size_t t = n % count;
    Loaded l(file);
    if (decode_first) {
      l.decode_all();
    }
    GIMG_Save_Options opts = {};
    opts.metadata_policy = GIMG_META_PRESERVE_ALL;
    GIMG_Diagnostics diag;
    std::memset(&diag, 0, sizeof(diag));
    gimg_diagnostics_init(&diag, nullptr);
    GIMG_Save_Report report = {};
    report.diagnostics = &diag;
    const Bytes out = save_doc(l.doc, targets[t], &opts, &report);
    for (size_t i = 0; i < diag.count; ++i) {
      const std::string msg = diag.items[i].recommended_action;
      EXPECT_NE(msg.rfind("ICC profile dropped", 0), 0u)
          << c.label << " -> " << targets[t] << ": " << msg
          << (decode_first ? " (decoded first)" : " (undecoded)");
    }
    gimg_diagnostics_destroy(&diag);
    Loaded back(out);
    ASSERT_GT(gimg_doc_item_count(back.doc), 0u) << targets[t];
    GIMG_Raster * r = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(back.doc, 0), nullptr, &r), GIMG_OK)
        << c.label << " -> " << targets[t];
    EXPECT_EQ(profile_of(r), icc) << c.label << " -> " << targets[t];
    gimg_raster_destroy(r);
  }
}

TEST_P(WebpProfile, SavingAsWebPAgainWritesTheOneChunk) {
  const Case c = GetParam();
  const Bytes icc = profile_bytes();
  const Bytes file = file_for(c, icc);
  Loaded l(file);
  l.decode_all();
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  opts.webp_lossless =
      c.lossy ? GIMG_WEBP_COMPRESS_LOSSY : GIMG_WEBP_COMPRESS_LOSSLESS;
  opts.webp_effort = 1;
  GIMG_Diagnostics diag;
  std::memset(&diag, 0, sizeof(diag));
  gimg_diagnostics_init(&diag, nullptr);
  GIMG_Save_Report report = {};
  report.diagnostics = &diag;
  const Bytes out = save_doc(l.doc, "webp", &opts, &report);
  for (size_t i = 0; i < diag.count; ++i) {
    EXPECT_NE(std::string(diag.items[i].recommended_action).rfind("ICC profile dropped", 0), 0u);
  }
  gimg_diagnostics_destroy(&diag);
  EXPECT_EQ(count_chunks(out, "ICCP"), 1u) << c.label;
  EXPECT_EQ(chunk_payload(out, "ICCP"), icc) << c.label;
  // Same file, decoded or not first: the raw block is what the writer reads,
  // and it holds what it held.
  Loaded undecoded(file);
  EXPECT_EQ(raw_iccp(undecoded.doc), icc);
}

INSTANTIATE_TEST_SUITE_P(Files, WebpProfile, ::testing::ValuesIn(kCases),
    [](const ::testing::TestParamInfo<Case> & i) {
      return std::string(i.param.label);
    });

TEST(WebpProfile, AFileWithoutAProfileStillDecodesWithout) {
  const Bytes file = encode_still(24, 16, picture(24, 16, 5, false), true);
  Loaded l(file);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(l.doc, 0), nullptr, &r), GIMG_OK);
  EXPECT_TRUE(profile_of(r).empty());
  gimg_raster_destroy(r);
  EXPECT_TRUE(raw_iccp(l.doc).empty());
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
