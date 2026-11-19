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
 * The byte order of 16-bit samples in the PNG codec, and the length limit of a
 * chunk.
 *
 * A 16-bit raster holds `uint16_t` samples in the host's order; a PNG file
 * stores them big-endian.  Every file here is written out by hand, byte for
 * byte, with samples whose two bytes differ so that a swap is visible, and
 * every raster is read back through a `uint16_t`.  The same expectations hold
 * on a big-endian host, where the codec used to hand back byte-swapped samples.
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/compress/compress.h>
#include <ghoti.io/compress/registry.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>

#include "../../src/codec/png/png_internal.h"

namespace {

using Bytes = std::vector<uint8_t>;
using Samples = std::vector<uint16_t>;

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

uint32_t adler32(const Bytes & b) {
  uint32_t a = 1, s = 0;
  for (uint8_t v : b) {
    a = (a + v) % 65521u;
    s = (s + a) % 65521u;
  }
  return (s << 16) | a;
}

void be32(Bytes & b, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    b.push_back(static_cast<uint8_t>(v >> s));
  }
}

void chunk(Bytes & out, const char * type, const Bytes & payload) {
  be32(out, static_cast<uint32_t>(payload.size()));
  Bytes body(type, type + 4);
  body.insert(body.end(), payload.begin(), payload.end());
  out.insert(out.end(), body.begin(), body.end());
  be32(out, crc32(body.data(), body.size()));
}

Bytes ihdr(uint32_t w, uint32_t h, uint8_t color_type) {
  Bytes p;
  be32(p, w);
  be32(p, h);
  p.push_back(16);
  p.push_back(color_type);
  p.push_back(0);
  p.push_back(0);
  p.push_back(0);
  return p;
}

/** A zlib stream of one stored block: nothing here depends on a compressor. */
Bytes zlib_stored(const Bytes & raw) {
  Bytes z = {0x78, 0x01, 0x01};
  const uint16_t n = static_cast<uint16_t>(raw.size());
  z.push_back(n & 255u);
  z.push_back(n >> 8);
  z.push_back(static_cast<uint8_t>(~n & 255u));
  z.push_back(static_cast<uint8_t>((~n >> 8) & 255u));
  z.insert(z.end(), raw.begin(), raw.end());
  be32(z, adler32(raw));
  return z;
}

/** The raw image data of a file: each row is filter type 0 and the samples
 *  big-endian, written here and not by the codec. */
Bytes raw_rows(uint32_t h, const Samples & samples) {
  const size_t per_row = samples.size() / h;
  Bytes raw;
  for (uint32_t y = 0; y < h; y++) {
    raw.push_back(0);
    for (size_t i = 0; i < per_row; i++) {
      const uint16_t v = samples[y * per_row + i];
      raw.push_back(static_cast<uint8_t>(v >> 8));
      raw.push_back(static_cast<uint8_t>(v & 255u));
    }
  }
  return raw;
}

const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};

Bytes png_file(uint32_t w, uint32_t h, uint8_t color_type,
    const Samples & samples, const Bytes * trns = nullptr) {
  Bytes f(kSignature, kSignature + 8);
  chunk(f, "IHDR", ihdr(w, h, color_type));
  if (trns) {
    chunk(f, "tRNS", *trns);
  }
  chunk(f, "IDAT", zlib_stored(raw_rows(h, samples)));
  chunk(f, "IEND", {});
  return f;
}

GIMG_Doc * load(Bytes & file) {
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Load_Options lo = {};
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_load(s, &lo, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  return doc;
}

/** Samples of an item's raster, read through a uint16_t as an operation does. */
Samples read_samples(const GIMG_Raster * r, size_t channels) {
  Samples out;
  const uint8_t * base =
      static_cast<const uint8_t *>(gimg_raster_pixels_const(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  for (uint32_t y = 0; y < gimg_raster_height(r); y++) {
    const uint16_t * row = reinterpret_cast<const uint16_t *>(base + y * stride);
    for (size_t i = 0; i < gimg_raster_width(r) * channels; i++) {
      out.push_back(row[i]);
    }
  }
  return out;
}

Samples decode_doc_samples(GIMG_Doc * doc, size_t channels, size_t item) {
  EXPECT_NE(doc, nullptr);
  if (!doc) {
    return {};
  }
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, item), nullptr, &r), GIMG_OK);
  Samples s;
  if (r) {
    EXPECT_EQ(gimg_raster_format(r)->bits_per_channel[0], 16);
    EXPECT_EQ(gimg_raster_format(r)->channel_count, channels);
    s = read_samples(r, channels);
    gimg_raster_destroy(r);
  }
  return s;
}

Samples decode_samples(Bytes & file, size_t channels, size_t item = 0) {
  GIMG_Doc * doc = load(file);
  Samples s = decode_doc_samples(doc, channels, item);
  gimg_doc_destroy(doc);
  return s;
}

Bytes encode(GIMG_Raster * raster) {
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);
  GIMG_Stream * out = nullptr;
  EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options so = {};
  so.png_filter = GIMG_PNG_FILTER_NONE;
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  EXPECT_EQ(gimg_doc_save(doc, out, "png", &so, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  Bytes b(static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  return b;
}

GIMG_Raster * raster_of(
    const GIMG_Pixel_Format * fmt, uint32_t w, uint32_t h, const Samples & s) {
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(
      gimg_raster_create(w, h, fmt, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  uint8_t * base = static_cast<uint8_t *>(gimg_raster_pixels(r));
  const size_t stride = gimg_raster_stride_bytes(r);
  const size_t per_row = s.size() / h;
  for (uint32_t y = 0; y < h; y++) {
    for (size_t i = 0; i < per_row; i++) {
      std::memcpy(base + y * stride + i * 2, &s[y * per_row + i], 2);
    }
  }
  return r;
}

struct Chunk {
  std::string type;
  Bytes payload;
};

std::vector<Chunk> chunks_of(const Bytes & f) {
  std::vector<Chunk> out;
  size_t at = 8;
  while (at + 12 <= f.size()) {
    const uint32_t len = (uint32_t(f[at]) << 24) | (uint32_t(f[at + 1]) << 16) |
        (uint32_t(f[at + 2]) << 8) | f[at + 3];
    Chunk c;
    c.type.assign(reinterpret_cast<const char *>(&f[at + 4]), 4);
    c.payload.assign(f.begin() + at + 8, f.begin() + at + 8 + len);
    out.push_back(c);
    at += 12 + len;
  }
  return out;
}

const Chunk * find(const std::vector<Chunk> & cs, const char * type) {
  for (const Chunk & c : cs) {
    if (c.type == type) {
      return &c;
    }
  }
  return nullptr;
}

/** The inflated image data of an encoded file. */
Bytes inflated_idat(const Bytes & f) {
  Bytes z;
  for (const Chunk & c : chunks_of(f)) {
    if (c.type == "IDAT") {
      z.insert(z.end(), c.payload.begin(), c.payload.end());
    }
  }
  Bytes out(1 << 16);
  size_t n = 0;
  EXPECT_EQ(gcomp_decode_buffer(gcomp_registry_default(), "zlib", nullptr,
                z.data(), z.size(), out.data(), out.size(), &n),
      GCOMP_OK);
  out.resize(n);
  return out;
}

// Two bytes of every sample differ, and no sample equals its own byte swap.
const Samples kGray = {0x0102, 0xFE01, 0x8070, 0x00FF};
const Samples kGrayAlpha = {0x0102, 0x9A0B, 0xFE01, 0x0C0D, 0x8070, 0x1234,
    0x00FF, 0xF0E1};
const Samples kRgb = {0x0102, 0x0304, 0x0506, 0xFE01, 0xDC02, 0xBA03, 0x8070,
    0x6050, 0x4030, 0x00FF, 0xFF00, 0x0F0E};
const Samples kRgba = {0x0102, 0x0304, 0x0506, 0x7788, 0xFE01, 0xDC02, 0xBA03,
    0x99AA, 0x8070, 0x6050, 0x4030, 0x1122, 0x00FF, 0xFF00, 0x0F0E, 0xF1F2};

Samples gray_to_rgba(const Samples & g, const Samples * alpha) {
  Samples out;
  for (size_t i = 0; i < g.size(); i++) {
    out.insert(out.end(), {g[i], g[i], g[i], alpha ? (*alpha)[i] : static_cast<uint16_t>(65535)});
  }
  return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Decoding: the file's bytes are big-endian, the raster's samples are values.
// ---------------------------------------------------------------------------

TEST(Png16BitContract, DecodeGray) {
  Bytes f = png_file(2, 2, 0, kGray);
  EXPECT_EQ(decode_samples(f, 1), kGray);
}

TEST(Png16BitContract, DecodeGrayAlpha) {
  Samples interleaved = kGrayAlpha;
  Samples gray, alpha;
  for (size_t i = 0; i < interleaved.size(); i += 2) {
    gray.push_back(interleaved[i]);
    alpha.push_back(interleaved[i + 1]);
  }
  Bytes f = png_file(2, 2, 4, kGrayAlpha);
  EXPECT_EQ(decode_samples(f, 4), gray_to_rgba(gray, &alpha));
}

TEST(Png16BitContract, DecodeRgb) {
  Samples expect;
  for (size_t i = 0; i < kRgb.size(); i += 3) {
    expect.insert(expect.end(), {kRgb[i], kRgb[i + 1], kRgb[i + 2], 65535});
  }
  Bytes f = png_file(2, 2, 2, kRgb);
  EXPECT_EQ(decode_samples(f, 4), expect);
}

TEST(Png16BitContract, DecodeRgba) {
  Bytes f = png_file(2, 2, 6, kRgba);
  EXPECT_EQ(decode_samples(f, 4), kRgba);
}

TEST(Png16BitContract, DecodeGrayWithTransparentKey) {
  // tRNS holds one 16-bit sample, big-endian; the pixel equal to it is clear.
  const Bytes key = {0x80, 0x70};
  Bytes f = png_file(2, 2, 0, kGray, &key);
  Samples alpha = {65535, 65535, 0, 65535};
  EXPECT_EQ(decode_samples(f, 4), gray_to_rgba(kGray, &alpha));
}

TEST(Png16BitContract, DecodeRgbWithTransparentKey) {
  const Bytes key = {0xFE, 0x01, 0xDC, 0x02, 0xBA, 0x03};
  Bytes f = png_file(2, 2, 2, kRgb, &key);
  Samples expect;
  for (size_t i = 0; i < kRgb.size(); i += 3) {
    expect.insert(expect.end(),
        {kRgb[i], kRgb[i + 1], kRgb[i + 2], static_cast<uint16_t>(i == 3 ? 0 : 65535)});
  }
  EXPECT_EQ(decode_samples(f, 4), expect);
}

// ---------------------------------------------------------------------------
// Encoding: the file's bytes are a function of the sample values alone.
// ---------------------------------------------------------------------------

TEST(Png16BitContract, EncodeGrayIsBigEndian) {
  Bytes f = encode(raster_of(&GIMG_PIXEL_GRAY16, 2, 2, kGray));
  const std::vector<Chunk> h_all = chunks_of(f);
  const Chunk * h = find(h_all, "IHDR");
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->payload[8], 16);
  EXPECT_EQ(h->payload[9], 0) << "grayscale";
  EXPECT_EQ(inflated_idat(f), raw_rows(2, kGray));
}

TEST(Png16BitContract, EncodeRgbaIsBigEndian) {
  Bytes f = encode(raster_of(&GIMG_PIXEL_RGBA16, 2, 2, kRgba));
  const std::vector<Chunk> h_all = chunks_of(f);
  const Chunk * h = find(h_all, "IHDR");
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->payload[8], 16);
  EXPECT_EQ(h->payload[9], 6) << "truecolor with alpha";
  EXPECT_EQ(inflated_idat(f), raw_rows(2, kRgba));
}

// ---------------------------------------------------------------------------
// Round trips: file -> raster -> file -> raster, for every 16-bit color type.
// ---------------------------------------------------------------------------

namespace {

void round_trip(uint8_t color_type, const Samples & file_samples,
    const Bytes * trns, size_t channels) {
  Bytes f = png_file(2, 2, color_type, file_samples, trns);
  GIMG_Doc * doc = load(f);
  ASSERT_NE(doc, nullptr);
  const Samples first = decode_doc_samples(doc, channels, 0);

  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Options so = {};
  so.png_filter = GIMG_PNG_FILTER_NONE;
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  ASSERT_EQ(gimg_doc_save(doc, out, "png", &so, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  Bytes again(
      static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  const std::vector<Chunk> h_all = chunks_of(again);
  const Chunk * h = find(h_all, "IHDR");
  ASSERT_NE(h, nullptr);
  EXPECT_EQ(h->payload[8], 16) << "depth survives";
  EXPECT_EQ(decode_samples(again, channels), first);
}

} // namespace

TEST(Png16BitContract, RoundTripGray) {
  round_trip(0, kGray, nullptr, 1);
}

TEST(Png16BitContract, RoundTripGrayAlpha) {
  round_trip(4, kGrayAlpha, nullptr, 4);
}

TEST(Png16BitContract, RoundTripRgb) {
  round_trip(2, kRgb, nullptr, 4);
}

TEST(Png16BitContract, RoundTripRgba) {
  round_trip(6, kRgba, nullptr, 4);
}

TEST(Png16BitContract, RoundTripGrayKeepsItsKeyInBigEndian) {
  const Bytes key = {0x80, 0x70};
  Bytes f = png_file(2, 2, 0, kGray, &key);
  GIMG_Doc * doc = load(f);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  ASSERT_EQ(gimg_doc_save(doc, out, "png", nullptr, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  Bytes again(
      static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  const std::vector<Chunk> t_all = chunks_of(again);
  const Chunk * t = find(t_all, "tRNS");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->payload, key);
}

TEST(Png16BitContract, RoundTripRgbKeepsItsKeyInBigEndian) {
  const Bytes key = {0xFE, 0x01, 0xDC, 0x02, 0xBA, 0x03};
  Bytes f = png_file(2, 2, 2, kRgb, &key);
  GIMG_Doc * doc = load(f);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report;
  std::memset(&report, 0, sizeof(report));
  ASSERT_EQ(gimg_doc_save(doc, out, "png", nullptr, &report), GIMG_OK);
  const void * p = nullptr;
  size_t n = 0;
  gimg_stream_output_buffer(out, &p, &n);
  Bytes again(
      static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  const std::vector<Chunk> t_all = chunks_of(again);
  const Chunk * t = find(t_all, "tRNS");
  ASSERT_NE(t, nullptr);
  EXPECT_EQ(t->payload, key);
}

// ---------------------------------------------------------------------------
// APNG: a 16-bit frame composited over the canvas compares sample values.
// ---------------------------------------------------------------------------

TEST(Png16BitContract, ApngBlendOverUsesSampleValues) {
  // Frame 0 (the default image) is the 1x1 opaque pixel D.  Frame 1 is one
  // pixel S at half alpha, blended over it.
  const Samples D = {0x0102, 0x0304, 0x0506, 0xFFFF};
  const Samples S = {0xFE01, 0xDC02, 0xBA03, 0x8001};

  Bytes f(kSignature, kSignature + 8);
  chunk(f, "IHDR", ihdr(1, 1, 6));
  Bytes actl;
  be32(actl, 2);
  be32(actl, 0);
  chunk(f, "acTL", actl);
  auto fctl = [](uint32_t seq, uint8_t blend) {
    Bytes p;
    be32(p, seq);
    be32(p, 1);
    be32(p, 1);
    be32(p, 0);
    be32(p, 0);
    p.insert(p.end(), {0, 1, 0, 10, 0, blend}); // delay 1/10, dispose none, blend
    return p;
  };
  chunk(f, "fcTL", fctl(0, 0));
  chunk(f, "IDAT", zlib_stored(raw_rows(1, D)));
  chunk(f, "fcTL", fctl(1, 1));
  Bytes fdat;
  be32(fdat, 2);
  const Bytes z = zlib_stored(raw_rows(1, S));
  fdat.insert(fdat.end(), z.begin(), z.end());
  chunk(f, "fdAT", fdat);
  chunk(f, "IEND", {});

  Samples expect;
  const uint32_t sa = S[3];
  for (int c = 0; c < 3; c++) {
    // Destination alpha is 65535, so this is (Cs*As + Cd*(65535-As)) / 65535.
    expect.push_back(static_cast<uint16_t>(
        (uint64_t(S[c]) * sa + uint64_t(D[c]) * (65535u - sa)) / 65535u));
  }
  expect.push_back(65535);

  EXPECT_EQ(decode_samples(f, 4, 0), D);
  EXPECT_EQ(decode_samples(f, 4, 1), expect);
}

// ---------------------------------------------------------------------------
// gimg_png_write_chunk: the length field is 31 bits.
// ---------------------------------------------------------------------------

namespace {

struct Sink {
  size_t calls = 0;
  Bytes bytes;
};

GIMG_Result sink_write(
    void * ctx, const void * buf, size_t size, size_t * out_written) {
  auto * s = static_cast<Sink *>(ctx);
  s->calls++;
  const auto * p = static_cast<const uint8_t *>(buf);
  s->bytes.insert(s->bytes.end(), p, p + size);
  *out_written = size;
  return GIMG_OK;
}

GIMG_Result write_chunk_to(Sink & sink, gimg_png_chunk_type_t type,
    const unsigned char * payload, size_t size) {
  GIMG_Stream_Callbacks cb = {};
  cb.write = sink_write;
  cb.ctx = &sink;
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_callbacks(&cb, &s), GIMG_OK);
  const GIMG_Result r = gimg_png_write_chunk(s, type, payload, size);
  gimg_stream_destroy(s);
  return r;
}

} // namespace

TEST(PngChunkLength, OrdinaryChunkIsWrittenWithItsCrc) {
  // IEND is the one chunk whose bytes every PNG reader knows by heart.
  Sink sink;
  ASSERT_EQ(write_chunk_to(sink, GIMG_PNG_IEND, nullptr, 0), GIMG_OK);
  const Bytes iend = {0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
  EXPECT_EQ(sink.bytes, iend);
}

TEST(PngChunkLength, LargestLengthFieldValue) {
  EXPECT_EQ(GIMG_PNG_CHUNK_LENGTH_MAX, size_t{0x7FFFFFFF});
}

// A payload this size is never read: the refusal is decided from the length
// alone, so a pointer that could not be dereferenced stands in for 2 GiB of
// data and no such allocation is needed.
TEST(PngChunkLength, OneOverTheLimitIsRefusedBeforeAnythingIsWritten) {
  Sink sink;
  const auto * never_read = reinterpret_cast<const unsigned char *>(1);
  EXPECT_EQ(write_chunk_to(sink, GIMG_PNG_tEXt, never_read,
                GIMG_PNG_CHUNK_LENGTH_MAX + 1u),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(sink.calls, 0u);
  EXPECT_TRUE(sink.bytes.empty());
}

TEST(PngChunkLength, TheLargestSizeValueIsRefusedToo) {
  Sink sink;
  const auto * never_read = reinterpret_cast<const unsigned char *>(1);
  EXPECT_EQ(write_chunk_to(sink, GIMG_PNG_eXIf, never_read, SIZE_MAX),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(sink.calls, 0u);
}

TEST(PngChunkLength, ASizeWithNoPayloadIsRefused) {
  Sink sink;
  EXPECT_EQ(write_chunk_to(sink, GIMG_PNG_tEXt, nullptr, 4),
      GIMG_ERR_INVALID);
  EXPECT_EQ(sink.calls, 0u);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
