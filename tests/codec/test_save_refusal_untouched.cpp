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
 * A GIF or PNG save that is refused leaves the caller's stream untouched.
 *
 * Each case is a document the writer cannot express, found at a different
 * depth: before any frame is looked at, while frame 0 is planned, only when a
 * later frame is reached, or only when metadata is rewritten.  The sink here
 * records every call to write, so "untouched" means no call at all, not merely
 * an empty file.  Each case first confirms that the same document is accepted
 * when the defect is removed, so a pass is not the writer refusing everything.
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

/** A sink that remembers whether it was ever written to. */
struct Recorder {
  size_t calls = 0;
  Bytes bytes;
};

GIMG_Result record_write(
    void * ctx, const void * buf, size_t size, size_t * out_written) {
  auto * rec = static_cast<Recorder *>(ctx);
  rec->calls++;
  const auto * p = static_cast<const uint8_t *>(buf);
  rec->bytes.insert(rec->bytes.end(), p, p + size);
  *out_written = size;
  return GIMG_OK;
}

/** A sink whose write always fails, to check an error is never read as OK. */
GIMG_Result failing_write(
    void * ctx, const void *, size_t, size_t * out_written) {
  static_cast<Recorder *>(ctx)->calls++;
  *out_written = 0;
  return GIMG_ERR_IO;
}

GIMG_Result save_to(const GIMG_Doc * doc, const char * format,
    const GIMG_Save_Options * opts, Recorder & rec,
    GIMG_Result (*write)(void *, const void *, size_t, size_t *) =
        record_write) {
  GIMG_Stream_Callbacks cb = {};
  cb.write = write;
  cb.ctx = &rec;
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_callbacks(&cb, &s), GIMG_OK);
  if (!s) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  const GIMG_Result r = gimg_doc_save(doc, s, format, opts, &report);
  gimg_stream_destroy(s);
  return r;
}

GIMG_Raster * make(uint32_t w, uint32_t h, const GIMG_Pixel_Format * fmt,
    uint32_t seed, bool distinct, bool translucent = false) {
  GIMG_Raster * r = nullptr;
  if (gimg_raster_create(w, h, fmt, GIMG_RASTER_OWNED, nullptr, 0, &r) !=
      GIMG_OK) {
    return nullptr;
  }
  auto * base = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  const size_t bpp = fmt->channel_count * fmt->bits_per_channel[0] / 8u;
  for (uint32_t y = 0; y < h; y++) {
    for (uint32_t x = 0; x < w; x++) {
      uint8_t * p = base + y * stride + x * bpp;
      const uint32_t n = distinct ? y * w + x : 0u;
      for (size_t c = 0; c < bpp; c++) {
        p[c] = static_cast<uint8_t>(seed * 40u + (c == 0 ? n & 255u
                                                  : c == 1 ? n >> 8
                                                           : 7u * c));
      }
      if (bpp == 4) {
        p[3] = translucent ? 128u : 255u;
      }
    }
  }
  return r;
}

/** A document of the given frames; it takes ownership of each raster. */
GIMG_Doc * doc_of(std::vector<GIMG_Raster *> frames) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, frames.size()), GIMG_OK);
  for (size_t i = 0; i < frames.size(); i++) {
    gimg_item_set_raster(gimg_doc_item(doc, i), frames[i]);
    gimg_item_set_frame_delay(gimg_doc_item(doc, i), 5, 100);
  }
  return doc;
}

void expect_refused_untouched(const GIMG_Doc * doc, const char * format,
    const GIMG_Save_Options * opts, GIMG_Result expect) {
  Recorder rec;
  EXPECT_EQ(save_to(doc, format, opts, rec), expect);
  EXPECT_EQ(rec.calls, 0u) << "the stream was written to before the refusal";
  EXPECT_TRUE(rec.bytes.empty());
}

void expect_accepted(const GIMG_Doc * doc, const char * format,
    const GIMG_Save_Options * opts = nullptr) {
  Recorder rec;
  EXPECT_EQ(save_to(doc, format, opts, rec), GIMG_OK);
  EXPECT_FALSE(rec.bytes.empty());
}

uint32_t crc32(const uint8_t * p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) {
      c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
    }
  }
  return ~c;
}

/** Insert a chunk of @p type with @p payload right after IHDR. */
Bytes with_chunk_after_ihdr(
    const Bytes & png, const char * type, const Bytes & payload) {
  Bytes chunk;
  const uint32_t len = static_cast<uint32_t>(payload.size());
  for (int s = 24; s >= 0; s -= 8) {
    chunk.push_back(static_cast<uint8_t>(len >> s));
  }
  Bytes body(type, type + 4);
  body.insert(body.end(), payload.begin(), payload.end());
  chunk.insert(chunk.end(), body.begin(), body.end());
  const uint32_t crc = crc32(body.data(), body.size());
  for (int s = 24; s >= 0; s -= 8) {
    chunk.push_back(static_cast<uint8_t>(crc >> s));
  }
  const size_t at = 8 + 8 + 13 + 4; // signature, IHDR header, data, CRC
  Bytes out(png.begin(), png.begin() + at);
  out.insert(out.end(), chunk.begin(), chunk.end());
  out.insert(out.end(), png.begin() + at, png.end());
  return out;
}

Bytes encode(const GIMG_Doc * doc, const char * format,
    const GIMG_Save_Options * opts = nullptr) {
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  EXPECT_EQ(gimg_doc_save(doc, out, format, opts, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  Bytes b(static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out);
  return b;
}

GIMG_Doc * load_bytes(Bytes & keep) {
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(keep.data(), keep.size(), &s), GIMG_OK);
  GIMG_Load_Options lo = {};
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_load(s, &lo, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  return doc;
}

} // namespace

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

TEST(GifRefusalUntouched, CanvasWiderThanTheFormatCanHold) {
  GIMG_Doc * ok = doc_of({make(8, 1, &GIMG_PIXEL_RGBA8, 1, false)});
  expect_accepted(ok, "gif");
  gimg_doc_destroy(ok);
  GIMG_Doc * doc = doc_of({make(65536, 1, &GIMG_PIXEL_RGBA8, 1, false)});
  expect_refused_untouched(doc, "gif", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(GifRefusalUntouched, CanvasTallerThanTheFormatCanHold) {
  GIMG_Doc * doc = doc_of({make(1, 65536, &GIMG_PIXEL_RGBA8, 1, false)});
  expect_refused_untouched(doc, "gif", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(GifRefusalUntouched, FirstFrameNotRgba8) {
  GIMG_Doc * doc = doc_of({make(4, 4, &GIMG_PIXEL_GRAY8, 1, false)});
  expect_refused_untouched(doc, "gif", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(GifRefusalUntouched, FirstFrameHasMoreColorsThanOnePalette) {
  GIMG_Doc * ok = doc_of({make(16, 16, &GIMG_PIXEL_RGBA8, 1, true)});
  // 256 distinct colors fit; one more does not.
  expect_accepted(ok, "gif");
  gimg_doc_destroy(ok);
  GIMG_Doc * doc = doc_of({make(17, 17, &GIMG_PIXEL_RGBA8, 1, true)});
  expect_refused_untouched(doc, "gif", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(GifRefusalUntouched, SecondFrameOfAnotherSize) {
  GIMG_Doc * ok = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, false),
      make(4, 4, &GIMG_PIXEL_RGBA8, 2, false)});
  expect_accepted(ok, "gif");
  gimg_doc_destroy(ok);
  GIMG_Doc * doc = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, false),
      make(5, 4, &GIMG_PIXEL_RGBA8, 2, false)});
  expect_refused_untouched(doc, "gif", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(GifRefusalUntouched, LaterFrameNotRgba8) {
  GIMG_Doc * doc = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, false),
      make(4, 4, &GIMG_PIXEL_RGBA8, 2, false),
      make(4, 4, &GIMG_PIXEL_GRAY8, 3, false)});
  expect_refused_untouched(doc, "gif", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(GifRefusalUntouched, LaterFrameHasMoreColorsThanOnePalette) {
  GIMG_Doc * doc = doc_of({make(17, 17, &GIMG_PIXEL_RGBA8, 1, false),
      make(17, 17, &GIMG_PIXEL_RGBA8, 2, false),
      make(17, 17, &GIMG_PIXEL_RGBA8, 3, true)});
  expect_refused_untouched(doc, "gif", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(GifRefusalUntouched, ASinkThatFailsIsAnErrorNotSuccess) {
  GIMG_Doc * doc = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, false)});
  Recorder rec;
  EXPECT_EQ(save_to(doc, "gif", nullptr, rec, failing_write), GIMG_ERR_IO);
  gimg_doc_destroy(doc);
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

TEST(PngRefusalUntouched, UnknownFilterChoice) {
  GIMG_Doc * doc = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, false)});
  GIMG_Save_Options opts = {};
  opts.png_filter = 6;
  expect_refused_untouched(doc, "png", &opts, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(PngRefusalUntouched, FramesOfDifferentSizes) {
  GIMG_Doc * doc = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, false),
      make(4, 4, &GIMG_PIXEL_RGBA8, 2, false),
      make(4, 5, &GIMG_PIXEL_RGBA8, 3, false)});
  expect_refused_untouched(doc, "png", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(PngRefusalUntouched, LaterFrameNeedsAnotherColorTypeOrDepth) {
  // One IHDR governs an APNG.  A later frame that needs another color type or
  // bit depth used to be found after frame 0 and its fcTL were already on the
  // stream.
  GIMG_Doc * ok = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, true),
      make(4, 4, &GIMG_PIXEL_RGBA8, 2, true, true)});
  expect_accepted(ok, "png");
  gimg_doc_destroy(ok);
  GIMG_Doc * gray = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, true),
      make(4, 4, &GIMG_PIXEL_GRAY8, 2, true)});
  expect_refused_untouched(gray, "png", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(gray);
  GIMG_Doc * deep = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, true),
      make(4, 4, &GIMG_PIXEL_RGBA8, 2, true),
      make(4, 4, &GIMG_PIXEL_RGBA16, 3, true)});
  expect_refused_untouched(deep, "png", nullptr, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(deep);
}

TEST(PngRefusalUntouched, ExifThatCannotBeRewrittenUnderAStrippingPolicy) {
  // The eXIf chunk is rewritten after the signature, IHDR and other chunks
  // have gone out, which is where this refusal used to be found.
  GIMG_Doc * src = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, true)});
  Bytes plain = encode(src, "png");
  gimg_doc_destroy(src);
  Bytes damaged = with_chunk_after_ihdr(
      plain, "eXIf", Bytes{'n', 'o', 't', ' ', 'e', 'x', 'i', 'f', '!', '!'});
  GIMG_Doc * doc = load_bytes(damaged);
  ASSERT_NE(doc, nullptr);
  GIMG_Save_Options opts = {};
  opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  expect_accepted(doc, "png", &opts);
  opts.metadata_policy = GIMG_META_STRIP_GPS;
  Recorder rec;
  const GIMG_Result r = save_to(doc, "png", &opts, rec);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(rec.calls, 0u);
  gimg_doc_destroy(doc);
}

TEST(PngRefusalUntouched, KeptPaletteThatTheImageHasLeft) {
  // Five opaque colors in 4x4 blocks: lossless as a palette, and smaller than
  // truecolor, so the writer picks color type 3 and the loaded document keeps
  // that palette.
  GIMG_Raster * two = make(64, 64, &GIMG_PIXEL_RGBA8, 1, false);
  auto * px = static_cast<uint8_t *>(gimg_raster_pixels(two));
  const size_t stride = gimg_raster_stride_bytes(two);
  for (uint32_t y = 0; y < 64; y++) {
    for (uint32_t x = 0; x < 64; x++) {
      px[y * stride + x * 4u] = static_cast<uint8_t>(((x / 4u + y / 4u) % 5u) * 50u);
    }
  }
  GIMG_Doc * src = doc_of({two});
  Bytes png = encode(src, "png");
  gimg_doc_destroy(src);
  ASSERT_GT(png.size(), 26u);
  ASSERT_EQ(png[8 + 8 + 9], 3) << "the fixture should have been written paletted";
  GIMG_Doc * doc = load_bytes(png);
  ASSERT_NE(doc, nullptr);
  GIMG_Raster * edited = make(16, 16, &GIMG_PIXEL_RGBA8, 5, true);
  gimg_item_set_raster(gimg_doc_item(doc, 0), edited);
  GIMG_Save_Options opts = {};
  opts.png_palette = GIMG_PNG_PALETTE_KEEP;
  expect_refused_untouched(doc, "png", &opts, GIMG_ERR_UNSUPPORTED);
  gimg_doc_destroy(doc);
}

TEST(PngRefusalUntouched, ASinkThatFailsIsAnErrorNotSuccess) {
  GIMG_Doc * doc = doc_of({make(4, 4, &GIMG_PIXEL_RGBA8, 1, false)});
  Recorder rec;
  EXPECT_EQ(save_to(doc, "png", nullptr, rec, failing_write), GIMG_ERR_IO);
  gimg_doc_destroy(doc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
