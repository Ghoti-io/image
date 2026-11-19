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
 * What a WebP animation's ANIM and ANMF fields come out as on the document,
 * for every shape of file: one frame or several, any loop count, any
 * background colour (stored blue, green, red, alpha; reported red, green, blue,
 * alpha), and how the frame flags and durations map to dispose, blend and
 * delay.  The files are assembled here byte by byte, so that what the file says
 * is known and not an echo of the library's writer.  A load, decode and save
 * writes the same ANIM chunk back.
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kW = 8;
constexpr uint32_t kH = 6;

void le(Bytes & b, uint32_t v, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    b.push_back(static_cast<uint8_t>(v >> (8 * i)));
  }
}

void put_chunk(Bytes & out, const char * fourcc, const Bytes & payload) {
  out.insert(out.end(), fourcc, fourcc + 4);
  le(out, static_cast<uint32_t>(payload.size()), 4);
  out.insert(out.end(), payload.begin(), payload.end());
  if (payload.size() & 1u) {
    out.push_back(0);
  }
}

Bytes save_doc(const GIMG_Doc * doc, const GIMG_Save_Options * opts,
    GIMG_Save_Report * report = nullptr) {
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  EXPECT_EQ(gimg_doc_save(doc, out, "webp", opts, report), GIMG_OK);
  const void * b = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &b, &n);
  Bytes file(
      static_cast<const uint8_t *>(b), static_cast<const uint8_t *>(b) + n);
  gimg_stream_destroy(out);
  return file;
}

/** The VP8L payload of a flat picture whose colour is chosen by @p seed. */
Bytes lossless_payload(uint32_t seed) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(
                kW, kH, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  uint8_t * px = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < kH; ++y) {
    for (uint32_t x = 0; x < kW; ++x) {
      uint8_t * p = px + y * stride + x * 4u;
      p[0] = static_cast<uint8_t>(seed * 40u + x * 9u);
      p[1] = static_cast<uint8_t>(y * 30u + seed);
      p[2] = static_cast<uint8_t>(200u - x * 5u);
      p[3] = 255u;
    }
  }
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Save_Options opts = {};
  opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSLESS;
  const Bytes file = save_doc(doc, &opts);
  gimg_doc_destroy(doc);
  size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const uint32_t size = file[pos + 4] | (file[pos + 5] << 8) |
        (file[pos + 6] << 16) | (static_cast<uint32_t>(file[pos + 7]) << 24);
    if (std::memcmp(&file[pos], "VP8L", 4) == 0) {
      return Bytes(file.begin() + pos + 8, file.begin() + pos + 8 + size);
    }
    pos += 8 + size + (size & 1u);
  }
  return Bytes();
}

struct Frame {
  uint32_t duration_ms;
  uint8_t flags; ///< Bit 0 dispose to background, bit 1 do not blend.
};

/** An animated file whose ANIM chunk is exactly @p anim (six bytes). */
Bytes build(const Bytes & anim, const std::vector<Frame> & frames) {
  Bytes body = {'W', 'E', 'B', 'P'};
  Bytes vp8x;
  vp8x.push_back(0x02);
  le(vp8x, 0, 3);
  le(vp8x, kW - 1, 3);
  le(vp8x, kH - 1, 3);
  put_chunk(body, "VP8X", vp8x);
  put_chunk(body, "ANIM", anim);
  uint32_t seed = 1;
  for (const Frame & f : frames) {
    Bytes anmf;
    le(anmf, 0, 3);
    le(anmf, 0, 3);
    le(anmf, kW - 1, 3);
    le(anmf, kH - 1, 3);
    le(anmf, f.duration_ms, 3);
    anmf.push_back(f.flags);
    put_chunk(anmf, "VP8L", lossless_payload(seed++));
    put_chunk(body, "ANMF", anmf);
  }
  Bytes file = {'R', 'I', 'F', 'F'};
  le(file, static_cast<uint32_t>(body.size()), 4);
  file.insert(file.end(), body.begin(), body.end());
  return file;
}

Bytes anim_chunk(uint8_t b, uint8_t g, uint8_t r, uint8_t a, uint16_t loop) {
  Bytes v = {b, g, r, a};
  le(v, loop, 2);
  return v;
}

Bytes payload_of(const Bytes & file, const char * fourcc) {
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
};

struct Shape {
  size_t frames;
  uint8_t b, g, r, a;
  uint16_t loop;
};

const Shape kShapes[] = {
    {1, 10, 20, 30, 40, 7},
    {3, 10, 20, 30, 40, 7},
    {1, 0, 0, 0, 0, 0},
    {3, 255, 255, 255, 255, 0},
    {2, 1, 2, 3, 0, 65535},
};

}  // namespace

TEST(WebpAnimMetadata, BackgroundAndLoopCountAreReportedForEveryShape) {
  for (const Shape & s : kShapes) {
    std::vector<Frame> frames(s.frames, Frame{40, 0});
    const Bytes file = build(anim_chunk(s.b, s.g, s.r, s.a, s.loop), frames);
    Loaded l(file);
    ASSERT_EQ(gimg_doc_item_count(l.doc), s.frames);
    uint8_t bg[4] = {9, 9, 9, 9};
    ASSERT_EQ(gimg_doc_background_color(l.doc, bg), 1) << s.frames << " frames";
    // Stored blue, green, red, alpha; reported red, green, blue, alpha.
    EXPECT_EQ(bg[0], s.r);
    EXPECT_EQ(bg[1], s.g);
    EXPECT_EQ(bg[2], s.b);
    EXPECT_EQ(bg[3], s.a);
    uint32_t loop = 99;
    // A loop count of zero (forever) is declared, not absent.
    ASSERT_EQ(gimg_doc_loop_count(l.doc, &loop), 1) << s.frames << " frames";
    EXPECT_EQ(loop, s.loop);
  }
}

TEST(WebpAnimMetadata, FrameFlagsAndDurationsBecomeDisposeBlendAndDelay) {
  const std::vector<Frame> frames = {
      {40, 0}, {100, 1}, {250, 2}, {1, 3}, {65535, 0}, {70000, 0}};
  const Bytes file = build(anim_chunk(0, 0, 0, 0, 0), frames);
  Loaded l(file);
  ASSERT_EQ(gimg_doc_item_count(l.doc), frames.size());
  for (size_t i = 0; i < frames.size(); ++i) {
    const GIMG_Item * item = gimg_doc_item(l.doc, i);
    EXPECT_EQ(gimg_item_role(item), GIMG_ITEM_FRAME) << i;
    EXPECT_EQ(gimg_item_dispose_op(item),
        (frames[i].flags & 1u) ? GIMG_DISPOSE_BACKGROUND : GIMG_DISPOSE_NONE)
        << i;
    EXPECT_EQ(gimg_item_blend_op(item),
        (frames[i].flags & 2u) ? GIMG_BLEND_SOURCE : GIMG_BLEND_OVER)
        << i;
    uint16_t num = 0, den = 0;
    gimg_item_frame_delay(item, &num, &den);
    EXPECT_EQ(den, 1000u) << i;
    // The delay is sixteen bits; a longer frame is reported as the longest.
    EXPECT_EQ(num, frames[i].duration_ms > 65535u ? 65535u : frames[i].duration_ms)
        << i;
  }
}

TEST(WebpAnimMetadata, ASingleFrameAnimationIsOneImageThatStillCarriesIt) {
  const Bytes file =
      build(anim_chunk(10, 20, 30, 40, 5), std::vector<Frame>{{80, 1}});
  Loaded l(file);
  ASSERT_EQ(gimg_doc_item_count(l.doc), 1u);
  EXPECT_EQ(gimg_item_role(gimg_doc_item(l.doc, 0)), GIMG_ITEM_IMAGE);
  uint16_t num = 0, den = 0;
  gimg_item_frame_delay(gimg_doc_item(l.doc, 0), &num, &den);
  EXPECT_EQ(num, 80u);
  uint32_t loop = 0;
  EXPECT_EQ(gimg_doc_loop_count(l.doc, &loop), 1);
  EXPECT_EQ(loop, 5u);
  // Saved again it is a still image, which has nowhere to put the loop count,
  // and the save report says so.
  ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(l.doc, 0), nullptr), GIMG_OK);
  GIMG_Diagnostics diag;
  std::memset(&diag, 0, sizeof(diag));
  gimg_diagnostics_init(&diag, nullptr);
  GIMG_Save_Report report = {};
  report.diagnostics = &diag;
  GIMG_Save_Options opts = {};
  const Bytes out = save_doc(l.doc, &opts, &report);
  bool loop_warned = false;
  for (size_t i = 0; i < diag.count; ++i) {
    if (std::string(diag.items[i].recommended_action).rfind("Loop count dropped", 0) == 0) {
      loop_warned = true;
    }
  }
  gimg_diagnostics_destroy(&diag);
  EXPECT_TRUE(loop_warned);
  EXPECT_TRUE(payload_of(out, "ANIM").empty());
}

TEST(WebpAnimMetadata, LoadDecodeSaveWritesTheSameAnimChunk) {
  for (const Shape & s : kShapes) {
    if (s.frames < 2) {
      continue; // a one-frame document is saved as a still image
    }
    std::vector<Frame> frames(s.frames, Frame{40, 0});
    const Bytes anim = anim_chunk(s.b, s.g, s.r, s.a, s.loop);
    const Bytes file = build(anim, frames);
    Loaded l(file);
    for (size_t i = 0; i < gimg_doc_item_count(l.doc); ++i) {
      ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(l.doc, i), nullptr), GIMG_OK);
    }
    GIMG_Save_Options opts = {};
    opts.webp_lossless = GIMG_WEBP_COMPRESS_LOSSLESS;
    const Bytes out = save_doc(l.doc, &opts);
    EXPECT_EQ(payload_of(out, "ANIM"), anim) << s.frames << " frames, loop " << s.loop;
  }
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
