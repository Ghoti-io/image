/**
 * @file
 *
 * Small hostile inputs that once reached memory they did not own.
 *
 * Each case here was a defect found by reading: a size multiplied without a
 * check, a count taken from the file and never compared with the bytes it
 * described, a nested load with nothing counting its depth.  None needed a
 * caller-set limit to be absent in any unusual way - the default is no limit
 * - and none had been seen by a fuzzer, because every harness sets a pixel
 * cap and these all sit behind it.
 *
 * Every case is built in memory and paired with a control: the same builder
 * with honest numbers must load, so a refusal of the hostile one is a refusal
 * of what is hostile about it and not of the builder.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#ifndef _WIN32
#include <pthread.h>
#endif
#include "../../src/codec/codec_internal.h"
#include "../../src/core/alloc_internal.h"
#include "../../src/meta/exif_internal.h"
#include "../../src/codec/bmp/bmp_internal.h"
#include "../../src/codec/jpeg/jpeg_internal.h"

namespace {

typedef std::vector<uint8_t> Bytes;

void le16(Bytes & b, uint16_t v) {
  b.push_back((uint8_t)(v & 0xFFu));
  b.push_back((uint8_t)(v >> 8));
}
void le32(Bytes & b, uint32_t v) {
  le16(b, (uint16_t)(v & 0xFFFFu));
  le16(b, (uint16_t)(v >> 16));
}
void be16(Bytes & b, uint16_t v) {
  b.push_back((uint8_t)(v >> 8));
  b.push_back((uint8_t)(v & 0xFFu));
}
void be32(Bytes & b, uint32_t v) {
  be16(b, (uint16_t)(v >> 16));
  be16(b, (uint16_t)(v & 0xFFFFu));
}

/** One little-endian TIFF directory entry. */
void entry(Bytes & b, uint16_t tag, uint16_t type, uint32_t count,
    uint32_t value) {
  le16(b, tag);
  le16(b, type);
  le32(b, count);
  if (type == 3 && count == 1) {
    le16(b, (uint16_t)value);
    le16(b, 0);
  }
  else {
    le32(b, value);
  }
}

enum { kShort = 3, kLong = 4 };

/** Load from memory and decode item @p index; the result of whichever failed. */
GIMG_Result load_and_decode(const Bytes & file, size_t index,
    const GIMG_Limits * decode_limits, GIMG_Raster ** out) {
  *out = nullptr;
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(file.data(), file.size(), &s) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r == GIMG_OK && doc) {
    if (index >= gimg_doc_item_count(doc)) {
      r = GIMG_ERR_UNSUPPORTED;
    }
    else {
      GIMG_Decode_Options o;
      memset(&o, 0, sizeof o);
      o.limits = decode_limits;
      r = gimg_item_decode(gimg_doc_item(doc, index),
          decode_limits ? &o : nullptr, out);
    }
  }
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return r;
}

// ---------------------------------------------------------------------------
// Raster creation
// ---------------------------------------------------------------------------

TEST(HostileInput, ARasterWhoseByteSizeOverflowsIsRefused) {
  // 2^31 x 2^31 RGBA8 is 2^64 bytes, which is zero in a size_t.  The
  // allocator hands back a minimal block for a zero-size request, so the
  // raster used to come back claiming 2^62 pixels over one byte, and every
  // decoder and operation ends in this constructor.
  struct Case {
    uint32_t w, h;
    const GIMG_Pixel_Format * f;
  };
  const Case cases[] = {
      {0x80000000u, 0x80000000u, &GIMG_PIXEL_RGBA8},
      {0x40000000u, 0x80000000u, &GIMG_PIXEL_RGBA16},
      {0xFFFFFFFFu, 0xFFFFFFFFu, &GIMG_PIXEL_RGBA16},
      {0x80000000u, 0x80000000u, &GIMG_PIXEL_RGBAF32},
  };
  for (const Case & c : cases) {
    GIMG_Raster * r = nullptr;
    GIMG_Result res =
        gimg_raster_create(c.w, c.h, c.f, GIMG_RASTER_OWNED, nullptr, 0, &r);
    EXPECT_NE(res, GIMG_OK) << c.w << " x " << c.h;
    EXPECT_EQ(r, nullptr);
    if (r) {
      gimg_raster_destroy(r);
    }
  }
  GIMG_Raster * ok = nullptr;
  ASSERT_EQ(gimg_raster_create(
                3, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &ok),
      GIMG_OK)
      << "control";
  gimg_raster_destroy(ok);
}

TEST(HostileInput, SettingARastersOwnColourInfoBackDoesNotReadFreedBytes) {
  // Copy the description out, change a field, set it back: the natural way
  // to edit one.  The copy's icc_bytes still points into the raster, and the
  // setter used to free that block before copying from it.
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(
                1, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &r),
      GIMG_OK);
  Bytes icc(4096);
  for (size_t i = 0; i < icc.size(); i++) {
    icc[i] = (uint8_t)(i * 7u + 3u);
  }
  GCOL_Color_Info ci;
  gcol_color_info_default(&ci);
  ci.icc_bytes = icc.data();
  ci.icc_size = icc.size();
  ASSERT_EQ(gimg_raster_set_color_info(r, &ci), GIMG_OK);

  GCOL_Color_Info edit = *gimg_raster_color_info_const(r);
  ASSERT_NE(edit.icc_bytes, (const void *)icc.data()) << "the raster copied";
  edit.gamma_value = 2.2;
  ASSERT_EQ(gimg_raster_set_color_info(r, &edit), GIMG_OK);
  const GCOL_Color_Info * after = gimg_raster_color_info_const(r);
  ASSERT_EQ(after->icc_size, icc.size());
  EXPECT_EQ(memcmp(after->icc_bytes, icc.data(), icc.size()), 0);
  EXPECT_EQ(after->gamma_value, 2.2);

  // And the literal same pointer, which must be a no-op for the profile.
  ASSERT_EQ(gimg_raster_set_color_info(r, gimg_raster_color_info_const(r)),
      GIMG_OK);
  after = gimg_raster_color_info_const(r);
  ASSERT_EQ(after->icc_size, icc.size());
  EXPECT_EQ(memcmp(after->icc_bytes, icc.data(), icc.size()), 0);
  gimg_raster_destroy(r);
}

// ---------------------------------------------------------------------------
// TIFF
// ---------------------------------------------------------------------------

/** An uncompressed RGB TIFF of one strip holding @p strip_bytes bytes. */
Bytes rgb_tiff(uint32_t w, uint32_t h, uint32_t strip_bytes) {
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  const uint32_t bps_at = 8u + 2u + 8u * 12u + 4u;
  const uint32_t strip_at = bps_at + 6u;
  le16(b, 8);
  entry(b, 256, kLong, 1, w);
  entry(b, 257, kLong, 1, h);
  entry(b, 258, kShort, 3, bps_at);
  entry(b, 259, kShort, 1, 1);
  entry(b, 262, kShort, 1, 2);
  entry(b, 273, kLong, 1, strip_at);
  entry(b, 277, kShort, 1, 3);
  entry(b, 279, kLong, 1, strip_bytes);
  le32(b, 0);
  le16(b, 8);
  le16(b, 8);
  le16(b, 8);
  for (uint32_t i = 0; i < strip_bytes; i++) {
    b.push_back((uint8_t)(i + 1u));
  }
  return b;
}

TEST(HostileInput, ATiffOfTwoToTheThirtyOneSquaredIsRefused) {
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(load_and_decode(rgb_tiff(2, 2, 12), 0, nullptr, &out), GIMG_OK)
      << "control: the builder writes a TIFF this library reads";
  ASSERT_NE(out, nullptr);
  gimg_raster_destroy(out);

  // The pixel count, 2^62, fits a size_t; the RGBA8 byte count does not.
  EXPECT_NE(
      load_and_decode(rgb_tiff(0x80000000u, 0x80000000u, 1), 0, nullptr, &out),
      GIMG_OK);
  EXPECT_EQ(out, nullptr);
  if (out) {
    gimg_raster_destroy(out);
  }
}
/** Load with limits, from memory; the loader's own verdict. */
GIMG_Result load_with(const Bytes & file, const GIMG_Limits * limits,
    GIMG_Doc ** out_doc) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(file.data(), file.size(), &s) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Load_Options o;
  memset(&o, 0, sizeof o);
  o.limits = limits;
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, &o, nullptr, &doc);
  gimg_stream_destroy(s);
  if (out_doc && r == GIMG_OK) {
    *out_doc = doc;
  }
  else if (doc) {
    gimg_doc_destroy(doc);
  }
  return r;
}

TEST(HostileInput, ATiffTileIsHeldToThePixelLimit) {
  // One pixel of picture in a tile 4096 x 1024 wide.  Every buffer decode
  // makes is sized from the tile, not the picture, so the picture's own
  // size said nothing about what the file could make the decoder ask for.
  Bytes c = {'I', 'I', 42, 0, 8, 0, 0, 0};
  const uint32_t tile_at2 = 8u + 2u + 10u * 12u + 4u;
  le16(c, 10);
  entry(c, 256, kLong, 1, 1);
  entry(c, 257, kLong, 1, 1);
  entry(c, 258, kShort, 1, 8);
  entry(c, 259, kShort, 1, 1);
  entry(c, 262, kShort, 1, 1);
  entry(c, 277, kShort, 1, 1);
  entry(c, 322, kLong, 1, 4096);
  entry(c, 323, kLong, 1, 1024);
  entry(c, 324, kLong, 1, tile_at2);
  entry(c, 325, kLong, 1, 1);
  le32(c, 0);
  c.push_back(0x80);

  GIMG_Limits roomy;
  memset(&roomy, 0, sizeof roomy);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(load_with(c, &roomy, &doc), GIMG_OK)
      << "control: the tile is within the default limit";
  gimg_doc_destroy(doc);

  GIMG_Limits tight;
  memset(&tight, 0, sizeof tight);
  tight.max_decoded_pixels = 4;
  EXPECT_EQ(load_with(c, &tight, nullptr), GIMG_ERR_LIMIT);
}

TEST(HostileInput, ASubDirectoryThatIsItsOwnPageIsNotReadTwice) {
  Bytes b = rgb_tiff(2, 2, 12);
  // Add a SubIFDs entry (tag 330) naming the directory itself, at offset 8.
  Bytes d = {'I', 'I', 42, 0, 8, 0, 0, 0};
  const uint32_t bps_at = 8u + 2u + 9u * 12u + 4u;
  const uint32_t strip_at = bps_at + 6u;
  le16(d, 9);
  entry(d, 256, kLong, 1, 2);
  entry(d, 257, kLong, 1, 2);
  entry(d, 258, kShort, 3, bps_at);
  entry(d, 259, kShort, 1, 1);
  entry(d, 262, kShort, 1, 2);
  entry(d, 273, kLong, 1, strip_at);
  entry(d, 277, kShort, 1, 3);
  entry(d, 279, kLong, 1, 12);
  entry(d, 330, kLong, 1, 8);
  le32(d, 0);
  le16(d, 8);
  le16(d, 8);
  le16(d, 8);
  for (uint32_t i = 0; i < 12; i++) {
    d.push_back((uint8_t)(i + 1u));
  }
  GIMG_Doc * doc = nullptr;
  EXPECT_NE(load_with(d, nullptr, &doc), GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  EXPECT_EQ(load_with(b, nullptr, &doc), GIMG_OK) << "control";
  gimg_doc_destroy(doc);
}

TEST(HostileInput, ARepeatedTiffTagKeepsItsFirstValue) {
  Bytes d = {'I', 'I', 42, 0, 8, 0, 0, 0};
  const uint32_t bps_at = 8u + 2u + 9u * 12u + 4u;
  const uint32_t strip_at = bps_at + 6u;
  le16(d, 9);
  entry(d, 256, kLong, 1, 2);
  entry(d, 257, kLong, 1, 2);
  entry(d, 258, kShort, 3, bps_at);
  entry(d, 259, kShort, 1, 1);
  entry(d, 262, kShort, 1, 2);
  entry(d, 273, kLong, 1, strip_at);
  entry(d, 277, kShort, 1, 3);
  entry(d, 279, kLong, 1, 12);
  entry(d, 256, kLong, 1, 64); // a second ImageWidth
  le32(d, 0);
  le16(d, 8);
  le16(d, 8);
  le16(d, 8);
  for (uint32_t i = 0; i < 12; i++) {
    d.push_back((uint8_t)(i + 1u));
  }
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(load_and_decode(d, 0, nullptr, &out), GIMG_OK);
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(gimg_raster_width(out), 2u);
  gimg_raster_destroy(out);
}

TEST(HostileInput, ManyTiffDirectoriesCannotEachKeepTheSameProfile) {
  // 200 directories, each naming the same 4 MB of the file as its ICC
  // profile.  Each is under the per-object limit; kept 200 times they are
  // 800 MB from a file of about 4 MB.
  const uint32_t blob = 4000000u;
  const uint32_t n = 200u;
  const uint32_t ifd_bytes = 2u + 9u * 12u + 4u;
  const uint32_t bps_at = 8u;
  const uint32_t strip_at = bps_at + 6u;
  const uint32_t first = strip_at + 12u;
  const uint32_t blob_at = first + n * ifd_bytes;
  Bytes b = {'I', 'I', 42, 0};
  le32(b, first);
  le16(b, 8);
  le16(b, 8);
  le16(b, 8);
  for (uint32_t i = 0; i < 12; i++) {
    b.push_back((uint8_t)(i + 1u));
  }
  for (uint32_t k = 0; k < n; k++) {
    le16(b, 9);
    entry(b, 256, kLong, 1, 2);
    entry(b, 257, kLong, 1, 2);
    entry(b, 258, kShort, 3, bps_at);
    entry(b, 259, kShort, 1, 1);
    entry(b, 262, kShort, 1, 2);
    entry(b, 273, kLong, 1, strip_at);
    entry(b, 277, kShort, 1, 3);
    entry(b, 279, kLong, 1, 12);
    entry(b, 34675, 7, blob, blob_at);
    le32(b, k + 1u < n ? first + (k + 1u) * ifd_bytes : 0u);
  }
  b.resize(b.size() + blob, 0x55);

  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(load_with(b, nullptr, &doc), GIMG_ERR_LIMIT);
  EXPECT_EQ(doc, nullptr);
}


TEST(HostileInput, ATiffWhoseJpegStripIsTheFileItselfDoesNotRecurse) {
  // Compression 7 with a strip that starts at byte 0 and runs the length of
  // the file.  The strip went to the format-sniffing loader, which saw a
  // TIFF, decoded it, and so asked for the strip again.
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  le16(b, 9);
  entry(b, 256, kLong, 1, 1);
  entry(b, 257, kLong, 1, 1);
  entry(b, 258, kShort, 1, 8);
  entry(b, 259, kShort, 1, 7);
  entry(b, 262, kShort, 1, 1);
  entry(b, 273, kLong, 1, 0);
  entry(b, 277, kShort, 1, 1);
  entry(b, 278, kLong, 1, 1);
  entry(b, 279, kLong, 1, 122);
  le32(b, 0);
  ASSERT_EQ(b.size(), 122u);
  GIMG_Raster * out = nullptr;
  EXPECT_NE(load_and_decode(b, 0, nullptr, &out), GIMG_OK);
  EXPECT_EQ(out, nullptr);
  if (out) {
    gimg_raster_destroy(out);
  }
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

uint32_t crc32_of(const uint8_t * p, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= p[i];
    for (int k = 0; k < 8; k++) {
      c = (c & 1u) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
  }
  return ~c;
}

void chunk(Bytes & out, const char * type, const Bytes & payload) {
  be32(out, (uint32_t)payload.size());
  const size_t at = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), payload.begin(), payload.end());
  be32(out, crc32_of(out.data() + at, out.size() - at));
}

/** @p raw as a zlib stream of one stored block. */
Bytes zlib_stored(const Bytes & raw) {
  Bytes z = {0x78, 0x01, 0x01};
  le16(z, (uint16_t)raw.size());
  le16(z, (uint16_t)~raw.size());
  z.insert(z.end(), raw.begin(), raw.end());
  uint32_t a = 1, b = 0;
  for (uint8_t v : raw) {
    a = (a + v) % 65521u;
    b = (b + a) % 65521u;
  }
  be32(z, (b << 16) | a);
  return z;
}

/**
 * An RGBA16 APNG of canvas @p w x @p h whose one frame is a single pixel at
 * (0, @p fy), carried in fdAT after an empty IDAT.  @p iccp, when not empty,
 * goes in as the compressed part of an iCCP chunk.
 */
Bytes apng(uint32_t w, uint32_t h, uint32_t fy, const Bytes & iccp,
    uint32_t declared_frames = 1) {
  Bytes f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes p;
  be32(p, w);
  be32(p, h);
  p.insert(p.end(), {16, 6, 0, 0, 0});
  chunk(f, "IHDR", p);
  if (!iccp.empty()) {
    p = {'x', 0, 0};
    p.insert(p.end(), iccp.begin(), iccp.end());
    chunk(f, "iCCP", p);
  }
  p.clear();
  be32(p, declared_frames);
  be32(p, 0);
  chunk(f, "acTL", p);
  chunk(f, "IDAT", Bytes());
  p.clear();
  be32(p, 0); // sequence
  be32(p, 1); // width
  be32(p, 1); // height
  be32(p, 0); // x
  be32(p, fy);
  be16(p, 1);
  be16(p, 10);
  p.push_back(0); // dispose none
  p.push_back(0); // blend source
  chunk(f, "fcTL", p);
  p.clear();
  be32(p, 1);
  const Bytes z = zlib_stored(Bytes{0, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
      0xFF, 0xFF});
  p.insert(p.end(), z.begin(), z.end());
  chunk(f, "fdAT", p);
  chunk(f, "IEND", Bytes());
  return f;
}

TEST(HostileInput, AnApngCanvasWhoseByteSizeOverflowsIsRefused) {
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(load_and_decode(apng(4, 4, 2, Bytes()), 0, nullptr, &out), GIMG_OK)
      << "control: the builder writes an APNG this library decodes";
  ASSERT_NE(out, nullptr);
  const uint16_t * px = (const uint16_t *)((const uint8_t *)
          gimg_raster_pixels_const(out) + 2u * gimg_raster_stride_bytes(out));
  EXPECT_NE(px[0], 0u) << "control: the frame's pixel was composited";
  gimg_raster_destroy(out);

  // Both dimensions are inside PNG's 2^31-1.  The RGBA16 canvas is
  // 17179344912 bytes a row, and that times the height is 2^64 + 524288.
  EXPECT_NE(load_and_decode(apng(2147418114u, 1073774592u, 2, Bytes()), 0,
                nullptr, &out),
      GIMG_OK);
  EXPECT_EQ(out, nullptr);
  if (out) {
    gimg_raster_destroy(out);
  }
}

TEST(HostileInput, AnApngWhoseProfileIsRefusedIsFreedOnce) {
  // A profile that does not inflate, under a caller's metadata cap, is a
  // refusal.  The APNG arm freed the canvas the raster already owned, and
  // the caller of the decoder then destroyed the raster.
  const Bytes bad_zlib(16, 0xFF);
  GIMG_Limits lim;
  gimg_limits_default(&lim);
  lim.max_metadata_size = 4096;
  GIMG_Raster * out = nullptr;
  EXPECT_EQ(load_and_decode(apng(4, 4, 2, bad_zlib), 0, &lim, &out),
      GIMG_ERR_LIMIT);
  EXPECT_EQ(out, nullptr);
  if (out) {
    gimg_raster_destroy(out);
  }
}

/** An allocator that records the largest request and refuses large ones. */
struct Watching {
  GIMG_Allocator a{};
  size_t largest = 0;
};
const size_t kRefuseAbove = (size_t)16 << 20;
void * w_malloc(void * ctx, size_t n) {
  Watching * w = (Watching *)ctx;
  w->largest = n > w->largest ? n : w->largest;
  return n > kRefuseAbove ? nullptr : malloc(n ? n : 1u);
}
void * w_calloc(void * ctx, size_t n, size_t size) {
  Watching * w = (Watching *)ctx;
  const size_t total = (size != 0 && n > (size_t)-1 / size) ? (size_t)-1
                                                           : n * size;
  w->largest = total > w->largest ? total : w->largest;
  return total > kRefuseAbove ? nullptr : calloc(n ? n : 1u, size ? size : 1u);
}
void * w_realloc(void * ctx, void * p, size_t n) {
  Watching * w = (Watching *)ctx;
  w->largest = n > w->largest ? n : w->largest;
  return n > kRefuseAbove ? nullptr : realloc(p, n ? n : 1u);
}
void w_free(void *, void * p) {
  free(p);
}

TEST(HostileInput, AnApngFrameCountIsHeldToWhatTheFileCouldHold) {
  // acTL is eight bytes and its frame count sized a table of 48-byte
  // entries: a 200-byte file asked for about a hundred gigabytes.
  GIMG_Codec * codec = gimg_codec_by_name("png");
  ASSERT_NE(codec, nullptr);
  const GIMG_Allocator * saved = codec->allocator;
  for (uint32_t frames : {1u, 0x7FFFFFFFu, 100000u}) {
    Watching w;
    w.a.ctx = &w;
    w.a.malloc_fn = w_malloc;
    w.a.calloc_fn = w_calloc;
    w.a.realloc_fn = w_realloc;
    w.a.free_fn = w_free;
    const Bytes file = apng(4, 4, 2, Bytes(), frames);
    codec->allocator = &w.a;
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s),
        GIMG_OK);
    GIMG_Doc * doc = nullptr;
    GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
    if (doc) {
      gimg_doc_destroy(doc);
    }
    gimg_stream_destroy(s);
    codec->allocator = saved;
    if (frames == 1u) {
      EXPECT_EQ(r, GIMG_OK) << "control";
    }
    else {
      EXPECT_NE(r, GIMG_OK) << frames;
    }
    EXPECT_LT(w.largest, (size_t)1 << 20)
        << frames << " declared frames in a " << file.size() << " byte file";
  }
}

/** A 1 x 1 gray 8-bit PNG whose IDAT is @p raw as a stored zlib stream. */
Bytes gray_png(const Bytes & raw) {
  Bytes f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes p;
  be32(p, 1);
  be32(p, 1);
  p.insert(p.end(), {8, 0, 0, 0, 0});
  chunk(f, "IHDR", p);
  chunk(f, "IDAT", zlib_stored(raw));
  chunk(f, "IEND", Bytes());
  return f;
}

TEST(HostileInput, MoreImageDataThanTheHeaderCallsForIsCorruptNotOverALimit) {
  GIMG_Raster * out = nullptr;
  ASSERT_EQ(load_and_decode(gray_png(Bytes{0, 0x80}), 0, nullptr, &out),
      GIMG_OK)
      << "control";
  gimg_raster_destroy(out);
  // The same picture with three more bytes than its one row and filter byte.
  // There is no limit to raise: the file is damaged.
  EXPECT_EQ(load_and_decode(
                gray_png(Bytes{0, 0x80, 1, 2, 3}), 0, nullptr, &out),
      GIMG_ERR_CORRUPT);
  EXPECT_EQ(out, nullptr);
}

TEST(HostileInput, AChunkCapDoesNotLoosenWhatTheStreamCouldHold) {
  // An IDAT that claims 200 MB at the end of a file of a few dozen bytes.  A
  // caller who sets max_chunk_size well above that must not be the one who
  // gets a 200 MB allocation for it.
  // signature + IHDR + a bare IDAT length and name.
  Bytes g = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes ihdr;
  be32(ihdr, 1);
  be32(ihdr, 1);
  ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
  chunk(g, "IHDR", ihdr);
  be32(g, 200000000u);
  g.insert(g.end(), {'I', 'D', 'A', 'T'});

  GIMG_Codec * codec = gimg_codec_by_name("png");
  ASSERT_NE(codec, nullptr);
  const GIMG_Allocator * saved = codec->allocator;
  Watching w;
  w.a.ctx = &w;
  w.a.malloc_fn = w_malloc;
  w.a.calloc_fn = w_calloc;
  w.a.realloc_fn = w_realloc;
  w.a.free_fn = w_free;
  codec->allocator = &w.a;
  GIMG_Limits lim;
  gimg_limits_default(&lim);
  lim.max_chunk_size = (size_t)1 << 30;
  GIMG_Raster * out = nullptr;
  const GIMG_Result r = load_and_decode(g, 0, &lim, &out);
  codec->allocator = saved;
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(out, nullptr);
  EXPECT_LT(w.largest, (size_t)1 << 20);
}

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

/** A 1 x 1 GIF; @p trailer says whether the 0x3B is there. */
Bytes tiny_gif(bool trailer) {
  Bytes g = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0};
  g.insert(g.end(), {0, 0, 0, 255, 255, 255}); // two-entry colour table
  g.insert(g.end(), {0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0});
  g.insert(g.end(), {2, 2, 0x44, 0x01, 0});
  if (trailer) {
    g.push_back(0x3B);
  }
  return g;
}

size_t warnings_loading(const Bytes & file, GIMG_Result * r) {
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Diagnostics d = {};
  gimg_diagnostics_init(&d, nullptr);
  GIMG_Doc * doc = nullptr;
  *r = gimg_doc_load(s, nullptr, &d, &doc);
  size_t n = 0;
  for (size_t i = 0; i < d.count; i++) {
    if (d.items[i].severity == GIMG_DIAG_WARNING) {
      n++;
    }
  }
  gimg_diagnostics_destroy(&d);
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return n;
}

TEST(HostileInput, AGifThatEndsWithoutATrailerLoadsAndSaysSo) {
  GIMG_Result r = GIMG_ERR_INTERNAL;
  EXPECT_EQ(warnings_loading(tiny_gif(true), &r), 0u);
  EXPECT_EQ(r, GIMG_OK) << "control";
  EXPECT_EQ(warnings_loading(tiny_gif(false), &r), 1u);
  EXPECT_EQ(r, GIMG_OK);
}

TEST(HostileInput, AGifDelayLongerThanTheFieldHoldsIsClampedNotWrapped) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  for (size_t k = 0; k < 2; k++) {
    GIMG_Raster * ras = nullptr;
    ASSERT_EQ(gimg_raster_create(
                  2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &ras),
        GIMG_OK);
    uint8_t * px = (uint8_t *)gimg_raster_pixels(ras);
    for (size_t y = 0; y < 2; y++) {
      for (size_t x = 0; x < 2; x++) {
        uint8_t * q = px + y * gimg_raster_stride_bytes(ras) + x * 4u;
        q[0] = (uint8_t)(60u * (k + 1u)); // opaque: a GIF has no partial alpha
        q[1] = 0;
        q[2] = 0;
        q[3] = 255;
      }
    }
    gimg_item_set_raster(gimg_doc_item(doc, k), ras);
    gimg_item_set_frame_delay(gimg_doc_item(doc, k), 700, 1); // 70000 cs
  }
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "gif", nullptr, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  const uint8_t * b = (const uint8_t *)data;
  size_t seen = 0;
  for (size_t at = 0; at + 8 <= size; at++) {
    if (b[at] == 0x21 && b[at + 1] == 0xF9 && b[at + 2] == 4) {
      EXPECT_EQ((uint16_t)(b[at + 4] | (b[at + 5] << 8)), 65535u);
      seen++;
    }
  }
  EXPECT_EQ(seen, 2u);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
}

// ---------------------------------------------------------------------------
// Pixel formats
// ---------------------------------------------------------------------------

GIMG_Result create_with(const GIMG_Pixel_Format & f, GIMG_Raster ** out) {
  *out = nullptr;
  return gimg_raster_create(8, 2, &f, GIMG_RASTER_OWNED, nullptr, 0, out);
}

TEST(HostileInput, AFormatThatDisagreesWithItselfIsRefusedByTheConstructor) {
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(create_with(GIMG_PIXEL_RGBA8, &r), GIMG_OK) << "control";
  gimg_raster_destroy(r);
  GIMG_Pixel_Format five;
  ASSERT_EQ(gimg_pixel_format_multichannel(5, 16, &five), GIMG_OK);
  ASSERT_EQ(create_with(five, &r), GIMG_OK) << "control: five 16-bit channels";
  gimg_raster_destroy(r);

  GIMG_Pixel_Format f = GIMG_PIXEL_RGBA16;
  f.bits_per_channel[1] = f.bits_per_channel[2] = f.bits_per_channel[3] = 8;
  EXPECT_NE(create_with(f, &r), GIMG_OK) << "RGBA of 16, 8, 8, 8 bits";
  EXPECT_EQ(r, nullptr);

  f = GIMG_PIXEL_GRAY8;
  f.channel_count = 2;
  f.bits_per_channel[1] = 8;
  EXPECT_NE(create_with(f, &r), GIMG_OK) << "two gray channels";

  f = GIMG_PIXEL_CMYK8;
  f.channel_count = 5;
  f.bits_per_channel[4] = 8;
  EXPECT_NE(create_with(f, &r), GIMG_OK) << "five CMYK channels";

  f = GIMG_PIXEL_GRAY8;
  f.bits_per_channel[0] = 4;
  EXPECT_NE(create_with(f, &r), GIMG_OK) << "four bits a sample";

  f = GIMG_PIXEL_RGBA8;
  f.layout = GIMG_LAYOUT_PLANAR;
  EXPECT_NE(create_with(f, &r), GIMG_OK) << "planar";

  f = GIMG_PIXEL_RGBA8;
  f.channel_model = (GIMG_Channel_Model)200;
  EXPECT_NE(create_with(f, &r), GIMG_OK) << "a model outside the enum";
}

TEST(HostileInput, ABorrowedBufferTheOperationsCouldNotReadSafelyIsRefused) {
  alignas(8) unsigned char buf[64] = {0};
  GIMG_Raster * r = nullptr;
  EXPECT_EQ(gimg_raster_create(2, 2, &GIMG_PIXEL_GRAY16, GIMG_RASTER_BORROWED,
                buf, 4, &r),
      GIMG_OK)
      << "control: aligned";
  gimg_raster_destroy(r);
  r = nullptr;
  EXPECT_NE(gimg_raster_create(2, 2, &GIMG_PIXEL_GRAY16, GIMG_RASTER_BORROWED,
                buf + 1, 4, &r),
      GIMG_OK)
      << "an odd address for 16-bit samples";
  EXPECT_NE(gimg_raster_create(2, 2, &GIMG_PIXEL_GRAY16, GIMG_RASTER_BORROWED,
                buf, 5, &r),
      GIMG_OK)
      << "an odd stride";
  // Bytes are read as bytes wherever they sit.
  EXPECT_EQ(gimg_raster_create(2, 2, &GIMG_PIXEL_GRAY8, GIMG_RASTER_BORROWED,
                buf + 1, 3, &r),
      GIMG_OK);
  gimg_raster_destroy(r);
}

TEST(HostileInput, AnItemCountThatOverflowsItsArrayIsRefused) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  EXPECT_NE(gimg_doc_set_item_count(doc, SIZE_MAX / 8u), GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u) << "the document is as it was";
  gimg_doc_destroy(doc);
}

TEST(HostileInput, ConvertingBetweenAlphaFormsIsNotARelabel) {
  GIMG_Raster * src = nullptr;
  ASSERT_EQ(create_with(GIMG_PIXEL_RGBA8, &src), GIMG_OK);
  GIMG_Pixel_Format premultiplied = GIMG_PIXEL_RGBA8;
  premultiplied.alpha_form = GIMG_ALPHA_PREMULTIPLIED;
  GIMG_Raster * out = nullptr;
  EXPECT_NE(gimg_ops_convert_pixel_format(src, &premultiplied, &out), GIMG_OK);
  EXPECT_EQ(out, nullptr);
  gimg_raster_destroy(out);
  gimg_raster_destroy(src);
}

// ---------------------------------------------------------------------------
// Orientation
// ---------------------------------------------------------------------------

/** A little-endian Exif blob whose IFD0 holds only Orientation = @p value. */
Bytes exif_with_orientation(uint16_t value) {
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  le16(b, 1);
  entry(b, 0x0112, kShort, 1, value);
  le32(b, 0);
  return b;
}

GIMG_Orientation orientation_of(const Bytes & exif) {
  GIMG_Orientation o = GIMG_ORIENTATION_UNKNOWN;
  EXPECT_EQ(gimg_exif_parse_orientation(exif.data(), exif.size(), &o), GIMG_OK);
  return o;
}

TEST(HostileInput, AWriterPutsOutExifOrientationOneWheneverItKeepsExif) {
  // Decoding has already turned the pixels upright, so a tag that still says
  // 6 turns them a second time in every viewer.
  const Bytes six = exif_with_orientation(6);
  ASSERT_EQ(orientation_of(six), 6) << "control";
  for (GIMG_Meta_Policy p : {GIMG_META_PRESERVE_ALL, GIMG_META_STRIP_GPS,
           GIMG_META_NORMALIZE_EXIF}) {
    SCOPED_TRACE((int)p);
    void * out = nullptr;
    size_t n = 0;
    ASSERT_EQ(gimg_exif_for_save(nullptr, p, six.data(), six.size(), &out, &n),
        GIMG_OK);
    ASSERT_NE(out, nullptr);
    const Bytes got((uint8_t *)out, (uint8_t *)out + n);
    EXPECT_EQ(orientation_of(got), 1);
    free(out);
  }
  // A policy that says to leave Exif alone leaves it alone.
  void * out = nullptr;
  size_t n = 0;
  ASSERT_EQ(gimg_exif_for_save(nullptr, GIMG_META_KEEP_RAW_ONLY, six.data(),
                six.size(), &out, &n),
      GIMG_OK);
  EXPECT_EQ(out, nullptr);
  // PRESERVE_ALL does not lose what it cannot read.
  const Bytes junk(40, 0x7F);
  ASSERT_EQ(gimg_exif_for_save(nullptr, GIMG_META_PRESERVE_ALL, junk.data(),
                junk.size(), &out, &n),
      GIMG_OK);
  EXPECT_EQ(out, nullptr);
}

TEST(HostileInput, APngRoundTripDoesNotCarryARotationItAlreadyApplied) {
  Bytes f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes p;
  be32(p, 2);
  be32(p, 1);
  p.insert(p.end(), {8, 0, 0, 0, 0});
  chunk(f, "IHDR", p);
  chunk(f, "eXIf", exif_with_orientation(6));
  chunk(f, "IDAT", zlib_stored(Bytes{0, 0x10, 0x20}));
  chunk(f, "IEND", Bytes());

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(f.data(), f.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  ASSERT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr), GIMG_OK);
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "png", nullptr, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  GIMG_Stream * back = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(data, size, &back), GIMG_OK);
  GIMG_Doc * again = nullptr;
  ASSERT_EQ(gimg_doc_load(back, nullptr, nullptr, &again), GIMG_OK);
  EXPECT_EQ(gimg_meta_common_orientation(gimg_doc_meta_common(again)),
      GIMG_ORIENTATION_NORMAL);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(again, 0), nullptr, &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r), 1u) << "rotated once: 2 x 1 became 1 x 2";
  EXPECT_EQ(gimg_raster_height(r), 2u);
  gimg_raster_destroy(r);
  gimg_doc_destroy(again);
  gimg_stream_destroy(back);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

// ---------------------------------------------------------------------------
// Exif thumbnails
// ---------------------------------------------------------------------------

/**
 * An Exif TIFF blob whose IFD1 is an uncompressed gray thumbnail of
 * @p w x @p h with one strip of @p strip_bytes bytes.
 */
Bytes exif_gray_thumbnail(uint32_t w, uint32_t h, uint32_t strip_bytes) {
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  le16(b, 0);
  le32(b, 14);
  const uint32_t strip_at = 14u + 2u + 7u * 12u + 4u;
  le16(b, 7);
  entry(b, 256, kLong, 1, w);
  entry(b, 257, kLong, 1, h);
  entry(b, 258, kShort, 1, 8);
  entry(b, 259, kShort, 1, 1);
  entry(b, 262, kShort, 1, 1);
  entry(b, 273, kLong, 1, strip_at);
  entry(b, 279, kLong, 1, strip_bytes);
  le32(b, 0);
  for (uint32_t i = 0; i < strip_bytes; i++) {
    b.push_back((uint8_t)(0xA0u + i));
  }
  return b;
}

/** Overwrite the 12-byte entry for @p tag in IFD1 of exif_gray_thumbnail(). */
void set_entry(Bytes & b, uint16_t tag, uint16_t type, uint32_t count,
    uint32_t value) {
  for (size_t at = 16; at + 12 <= 16 + 7 * 12; at += 12) {
    if ((uint16_t)(b[at] | (b[at + 1] << 8)) == tag) {
      Bytes e;
      entry(e, tag, type, count, value);
      memcpy(&b[at], e.data(), 12);
      return;
    }
  }
  FAIL() << "no entry for tag " << tag;
}

/** The least JPEG the loader accepts, carrying @p exif in APP1. */
Bytes jpeg_with_exif(const Bytes & exif) {
  Bytes j = {0xFF, 0xD8, 0xFF, 0xE1};
  be16(j, (uint16_t)(2u + 6u + exif.size()));
  j.insert(j.end(), {'E', 'x', 'i', 'f', 0, 0});
  j.insert(j.end(), exif.begin(), exif.end());
  j.insert(j.end(), {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08,
      0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9});
  return j;
}

size_t jpeg_item_count(const Bytes & file, GIMG_Result * load_result) {
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  *load_result = gimg_doc_load(s, nullptr, nullptr, &doc);
  size_t n = doc ? gimg_doc_item_count(doc) : 0;
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return n;
}

TEST(HostileInput, AnExifThumbnailLargerThanItsStripIsNotRead) {
  GIMG_Result r = GIMG_ERR_INTERNAL;
  ASSERT_EQ(jpeg_item_count(jpeg_with_exif(exif_gray_thumbnail(2, 2, 4)), &r),
      2u)
      << "control: an honest 2x2 thumbnail becomes item 1 (load gave " << r
      << ")";

  // 2000 x 2000 pixels claimed over a one-byte strip: four megabytes used to
  // be copied from a one-byte block into a raster handed to the caller.
  const uint32_t sizes[][3] = {{2000, 2000, 1}, {2, 2, 3}, {0x10000, 0x10000, 4},
      {0xFFFFFFFFu, 2, 4}};
  for (const uint32_t * c : sizes) {
    size_t n = jpeg_item_count(
        jpeg_with_exif(exif_gray_thumbnail(c[0], c[1], c[2])), &r);
    EXPECT_LE(n, 1u) << c[0] << " x " << c[1] << " over " << c[2] << " bytes";
  }
}

GIMG_Result uncompressed_thumbnail(const Bytes & exif) {
  uint32_t w = 0, h = 0;
  uint8_t bits = 0;
  uint16_t photo = 0;
  void * data = nullptr;
  size_t size = 0;
  GIMG_Result r = gimg_exif_embedded_thumbnail_uncompressed(nullptr,
      exif.data(), exif.size(), &w, &h, &bits, &photo, &data, &size);
  if (data) {
    gimg_free(gimg_allocator_default(), data);
  }
  return r;
}

TEST(HostileInput, ExifThumbnailTagOffsetsAreBoundedBeforeTheyAreFollowed) {
  ASSERT_EQ(uncompressed_thumbnail(exif_gray_thumbnail(2, 2, 4)), GIMG_OK)
      << "control";

  // BitsPerSample with three values lives at an offset; every RGB thumbnail
  // has one.  The offset was added to the buffer and read with no check.
  for (uint32_t off : {0xFFFFFFF0u, 0x7FFFFFFFu, 0x10000u, 120u}) {
    Bytes e = exif_gray_thumbnail(2, 2, 4);
    set_entry(e, 258, kShort, 3, off);
    EXPECT_EQ(uncompressed_thumbnail(e), GIMG_ERR_CORRUPT)
        << "BitsPerSample at 0x" << std::hex << off;
  }

  // Two strips whose offset array sits at 0xFFFFFFFC: the bound was computed
  // in 32 bits, wrapped to zero, and passed.
  for (uint32_t off : {0xFFFFFFFCu, 0xFFFFFFF8u, 0x80000000u}) {
    Bytes e = exif_gray_thumbnail(2, 2, 4);
    set_entry(e, 273, kLong, 2, off);
    set_entry(e, 279, kLong, 2, 8);
    EXPECT_EQ(uncompressed_thumbnail(e), GIMG_ERR_CORRUPT)
        << "StripOffsets at 0x" << std::hex << off;
    e = exif_gray_thumbnail(2, 2, 4);
    set_entry(e, 273, kLong, 2, 8);
    set_entry(e, 279, kLong, 2, off);
    EXPECT_EQ(uncompressed_thumbnail(e), GIMG_ERR_CORRUPT)
        << "StripByteCounts at 0x" << std::hex << off;
  }

  // One strip whose offset plus length wraps.
  Bytes e = exif_gray_thumbnail(2, 2, 4);
  set_entry(e, 273, kLong, 1, 0xFFFFFFFEu);
  set_entry(e, 279, kLong, 1, 4);
  EXPECT_EQ(uncompressed_thumbnail(e), GIMG_ERR_CORRUPT);
}

// ---------------------------------------------------------------------------
// Thumbnails inside thumbnails
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// JPEG segments
// ---------------------------------------------------------------------------

Bytes jpeg_segment(uint8_t marker, const Bytes & payload) {
  Bytes b = {0xFF, marker};
  be16(b, (uint16_t)(payload.size() + 2u));
  b.insert(b.end(), payload.begin(), payload.end());
  return b;
}

/** SOI, @p segments, then a two-component 8 x 8 baseline frame, @p scans, EOI. */
Bytes jpeg_of(const std::vector<Bytes> & segments,
    const std::vector<Bytes> & scans) {
  Bytes j = {0xFF, 0xD8};
  for (const Bytes & s : segments) {
    j.insert(j.end(), s.begin(), s.end());
  }
  const Bytes sof = jpeg_segment(0xC0,
      Bytes{8, 0, 8, 0, 8, 2, 1, 0x11, 0, 2, 0x11, 0});
  j.insert(j.end(), sof.begin(), sof.end());
  for (const Bytes & s : scans) {
    j.insert(j.end(), s.begin(), s.end());
  }
  j.insert(j.end(), {0xFF, 0xD9});
  return j;
}

Bytes sos_for(uint8_t component) {
  return jpeg_segment(0xDA, Bytes{1, component, 0x00, 0, 63, 0});
}

GIMG_Result load_jpeg(const Bytes & file, const GIMG_Limits * limits,
    std::string * diagnostics_text) {
  GIMG_Stream * s = nullptr;
  EXPECT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Load_Options o;
  memset(&o, 0, sizeof o);
  o.limits = limits;
  GIMG_Diagnostics d = {};
  gimg_diagnostics_init(&d, nullptr);
  GIMG_Doc * doc = nullptr;
  const GIMG_Result r = gimg_doc_load(s, &o, &d, &doc);
  if (diagnostics_text) {
    for (size_t i = 0; i < d.count; i++) {
      *diagnostics_text += d.items[i].recommended_action;
      *diagnostics_text += ";";
    }
  }
  gimg_diagnostics_destroy(&d);
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return r;
}

TEST(HostileInput, CommentsOverTheMetadataCapFreeWhatTheLoadHadRead) {
  // The refusal returned without releasing the document state.  LeakSanitizer
  // is the instrument; the result is only the precondition.
  Bytes com(100, 'c');
  GIMG_Limits lim;
  gimg_limits_default(&lim);
  lim.max_metadata_size = 8;
  EXPECT_EQ(load_jpeg(jpeg_of({jpeg_segment(0xFE, com)}, {}), &lim, nullptr),
      GIMG_ERR_LIMIT);
}

TEST(HostileInput, AnIccProfileSentTwoWaysIsFreedOnce) {
  // APP2 ICC_PROFILE 1 of 1, then 1 of 2 and 2 of 2: the second form
  // assembled a profile over the first without freeing it.
  auto icc = [](uint8_t seq, uint8_t total, size_t n) {
    Bytes p = {'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0, seq,
        total};
    p.insert(p.end(), n, 0x5A);
    return jpeg_segment(0xE2, p);
  };
  EXPECT_EQ(load_jpeg(jpeg_of({icc(1, 1, 40), icc(1, 2, 20), icc(2, 2, 20)}, {}),
                nullptr, nullptr),
      GIMG_OK);
}

TEST(HostileInput, ASequentialScanCannotCodeAComponentTwice) {
  // Each repeat is a full pass over the samples, and a frame may hold 1024.
  std::string why;
  load_jpeg(jpeg_of({}, {sos_for(1), sos_for(2)}), nullptr, &why);
  EXPECT_EQ(why.find("already coded"), std::string::npos)
      << "control: one scan per component is what T.81 A.2.3 allows";
  why.clear();
  EXPECT_EQ(load_jpeg(jpeg_of({}, {sos_for(1), sos_for(1)}), nullptr, &why),
      GIMG_ERR_FORMAT);
  EXPECT_NE(why.find("already coded"), std::string::npos) << why;
}

TEST(HostileInput, ACommentRightAfterAScansDataIsKept) {
  // The segment loop that reads a scan's entropy data used to read the first
  // DQT, APPn or COM after it and throw it away.
  const Bytes com = jpeg_segment(0xFE, Bytes{'h', 'i'});
  const Bytes file = jpeg_of({}, {sos_for(1), com, sos_for(2)});
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(file.data(), file.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t n = 0;
  EXPECT_EQ(gimg_meta_raw_get(raw, "jpeg", GIMG_JPEG_RAW_COM, nullptr, &n),
      GIMG_OK);
  EXPECT_GT(n, 0u) << "the comment was dropped";
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(HostileInput, EveryJpegWriterRefusesASizeItsOwnReaderRefuses) {
  // 32769 is past what the reader accepts and, for the others, the frame
  // header would have held it modulo 65536 only from 65536 up; either way the
  // file written was one this library cannot read.
  GIMG_Raster * ras = nullptr;
  ASSERT_EQ(gimg_raster_create(32769, 1, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &ras),
      GIMG_OK);
  memset(gimg_raster_pixels(ras), 0x40, 32769);
  struct Mode {
    const char * name;
    uint8_t precision, predictor, levels;
  } modes[] = {{"baseline", 0, 0, 0}, {"12-bit", 12, 0, 0},
      {"lossless", 0, 1, 0}, {"hierarchical", 0, 0, 2}};
  for (const Mode & m : modes) {
    SCOPED_TRACE(m.name);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
    ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
    GIMG_Raster * copy = nullptr;
    ASSERT_EQ(gimg_raster_create(32769, 1, &GIMG_PIXEL_GRAY8,
                  GIMG_RASTER_OWNED, nullptr, 0, &copy),
        GIMG_OK);
    memcpy(gimg_raster_pixels(copy), gimg_raster_pixels(ras), 32769);
    gimg_item_set_raster(gimg_doc_item(doc, 0), copy);
    GIMG_Save_Options o;
    memset(&o, 0, sizeof o);
    o.jpeg_precision = m.precision;
    o.jpeg_lossless_predictor = m.predictor;
    o.jpeg_hierarchical_levels = m.levels;
    GIMG_Stream * out = nullptr;
    ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Report report = {};
    EXPECT_NE(gimg_doc_save(doc, out, "jpeg", &o, &report), GIMG_OK);
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
  }
  gimg_raster_destroy(ras);
}

TEST(HostileInput, YCbCrAtSixteenBitsDoesNotOverflowItsProducts) {
  int r = 0, g = 0, b = 0;
  jpeg_ycbcr_to_rgb(30000, 0, 0, 32768, 65535, &r, &g, &b);
  // Red and blue fall below zero and clamp.
  EXPECT_EQ(r, 0);
  EXPECT_EQ(b, 0);
  // Green is y + 0.34414 * 32768 + 0.71414 * 32768 = 64678.
  EXPECT_NEAR(g, 64678, 3);
}

#ifndef _WIN32
/** A JPEG whose JFXX thumbnail is @p inner. */
Bytes jfxx_wrap(const Bytes & inner) {
  Bytes j = {0xFF, 0xD8, 0xFF, 0xE0};
  be16(j, (uint16_t)(2u + 6u + inner.size()));
  j.insert(j.end(), {'J', 'F', 'X', 'X', 0, 0x10});
  j.insert(j.end(), inner.begin(), inner.end());
  j.insert(j.end(), {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00, 0x08,
      0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9});
  return j;
}

struct NestedLoad {
  const Bytes * file;
  GIMG_Result result;
};

void * nested_load_thread(void * arg) {
  NestedLoad * n = (NestedLoad *)arg;
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(n->file->data(), n->file->size(), &s);
  GIMG_Doc * doc = nullptr;
  n->result = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return nullptr;
}

TEST(HostileInput, ThumbnailsInsideThumbnailsAreNotFollowed) {
  // Each level costs 27 bytes of file, so one 64 KiB segment nests about
  // 2400 deep, and each level used to be a full load inside the one above.
  // Run on a thread with a small stack, which is what an application's
  // worker has: the recursion overran it, and a load that stops at the first
  // thumbnail does not.
  Bytes file = {0xFF, 0xD8, 0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08, 0x00,
      0x08, 0x01, 0x01, 0x11, 0x00, 0xFF, 0xD9};
  size_t depth = 0;
  while (file.size() + 27u < 65000u) {
    file = jfxx_wrap(file);
    depth++;
  }
  ASSERT_GT(depth, 2000u);

  NestedLoad n = {&file, GIMG_ERR_INTERNAL};
  pthread_attr_t attr;
  ASSERT_EQ(pthread_attr_init(&attr), 0);
  ASSERT_EQ(pthread_attr_setstacksize(&attr, 512u * 1024u), 0);
  pthread_t t;
  ASSERT_EQ(pthread_create(&t, &attr, nested_load_thread, &n), 0);
  ASSERT_EQ(pthread_join(t, nullptr), 0);
  pthread_attr_destroy(&attr);
  EXPECT_EQ(n.result, GIMG_OK) << "the outer picture is still a JPEG";
}
#endif

TEST(HostileInput, ExifStripsThatOverlapCannotAddUpToMoreThanTheBlob) {
  // Three strips, each of 60 bytes, all at offset 40 of a 160-byte blob: 180
  // bytes of thumbnail from a blob that cannot hold them.
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  le16(b, 0);
  le32(b, 14);
  const uint32_t arrays = 14u + 2u + 6u * 12u + 4u;
  le16(b, 6);
  entry(b, 256, kLong, 1, 2);
  entry(b, 257, kLong, 1, 2);
  entry(b, 258, kShort, 1, 8);
  entry(b, 259, kShort, 1, 1);
  entry(b, 273, kLong, 3, arrays);
  entry(b, 279, kLong, 3, arrays + 12u);
  le32(b, 0);
  for (int k = 0; k < 3; k++) {
    le32(b, 40);
  }
  for (int k = 0; k < 3; k++) {
    le32(b, 60);
  }
  b.resize(160, 0x33);
  ASSERT_EQ(b.size(), 160u);
  uint32_t w = 0, h = 0;
  uint8_t bits = 0;
  uint16_t photo = 0;
  void * data = nullptr;
  size_t n = 0;
  EXPECT_EQ(gimg_exif_embedded_thumbnail_uncompressed(nullptr, b.data(),
                b.size(), &w, &h, &bits, &photo, &data, &n),
      GIMG_OK);
  EXPECT_EQ(n, 0u) << "no thumbnail, not 180 bytes of aliased ones";
  free(data);
}

TEST(HostileInput, ALoadReadsASeekableStreamFromItsStart) {
  const Bytes png = gray_png(Bytes{0, 0x80});
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(png.data(), png.size(), &s), GIMG_OK);
  ASSERT_EQ(gimg_stream_seek(s, 5), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK)
      << "identified and decoded from the same place";
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
}

/** A RIFF chunk: fourcc, little-endian size, payload, pad. */
void riff_chunk(Bytes & out, const char * cc, const Bytes & payload) {
  out.insert(out.end(), cc, cc + 4);
  le32(out, (uint32_t)payload.size());
  out.insert(out.end(), payload.begin(), payload.end());
  if (payload.size() & 1u) {
    out.push_back(0);
  }
}

TEST(HostileInput, AWebpBitstreamLargerThanItsCanvasIsRefusedBeforeItIsDecoded) {
  // VP8X says one pixel; the VP8L header says 16384 x 16384.  Each pixel of
  // such a stream costs no bits, so the decoder would allocate gigabytes to
  // produce a picture of a size no limit had been asked about.
  Bytes body = {'W', 'E', 'B', 'P'};
  riff_chunk(body, "VP8X", Bytes{0, 0, 0, 0, 0, 0, 0, 0, 0, 0});
  riff_chunk(body, "VP8L", Bytes{0x2F, 0xFF, 0xFF, 0xFF, 0x0F, 0, 0, 0});
  Bytes f = {'R', 'I', 'F', 'F'};
  le32(f, (uint32_t)body.size());
  f.insert(f.end(), body.begin(), body.end());

  GIMG_Codec * codec = gimg_codec_by_name("webp");
  ASSERT_NE(codec, nullptr);
  const GIMG_Allocator * saved = codec->allocator;
  Watching w;
  w.a.ctx = &w;
  w.a.malloc_fn = w_malloc;
  w.a.calloc_fn = w_calloc;
  w.a.realloc_fn = w_realloc;
  w.a.free_fn = w_free;
  codec->allocator = &w.a;
  GIMG_Raster * out = nullptr;
  const GIMG_Result r = load_and_decode(f, 0, nullptr, &out);
  codec->allocator = saved;
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(out, nullptr);
  EXPECT_LT(w.largest, (size_t)1 << 20);
}

TEST(HostileInput, ALossyWebpWhoseTokenPartitionIsCutShortIsRefused) {
  GIMG_Raster * ras = nullptr;
  ASSERT_EQ(gimg_raster_create(64, 64, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &ras),
      GIMG_OK);
  uint8_t * px = (uint8_t *)gimg_raster_pixels(ras);
  uint32_t seed = 12345;
  for (size_t i = 0; i < 64u * 64u * 4u; i++) {
    seed = seed * 1664525u + 1013904223u;
    px[i] = (uint8_t)(seed >> 24);
    if ((i & 3u) == 3u) {
      px[i] = 255;
    }
  }
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 1), GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), ras);
  GIMG_Save_Options o;
  memset(&o, 0, sizeof o);
  o.webp_lossless = GIMG_WEBP_COMPRESS_LOSSY;
  GIMG_Stream * out = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out, "webp", &o, &report), GIMG_OK);
  const void * data = nullptr;
  size_t size = 0;
  gimg_stream_output_buffer(out, &data, &size);
  Bytes f((const uint8_t *)data, (const uint8_t *)data + size);
  gimg_stream_destroy(out);
  gimg_doc_destroy(doc);

  GIMG_Raster * got = nullptr;
  ASSERT_EQ(load_and_decode(f, 0, nullptr, &got), GIMG_OK) << "control";
  gimg_raster_destroy(got);

  // Find the VP8 chunk, drop the second half of its payload and keep every
  // size in the file consistent with what is left.
  size_t at = 0;
  for (size_t i = 12; i + 8 < f.size(); i++) {
    if (memcmp(&f[i], "VP8 ", 4) == 0) {
      at = i;
      break;
    }
  }
  ASSERT_NE(at, 0u);
  const uint32_t payload = (uint32_t)(f[at + 4] | (f[at + 5] << 8) |
      (f[at + 6] << 16) | ((uint32_t)f[at + 7] << 24));
  ASSERT_GT(payload, 400u) << "noise should not compress to nothing";
  const uint32_t keep = (payload / 2u) & ~1u;
  Bytes cut(f.begin(), f.begin() + at + 8 + keep);
  for (int k = 0; k < 4; k++) {
    cut[at + 4 + k] = (uint8_t)(keep >> (8 * k));
  }
  const uint32_t riff = (uint32_t)cut.size() - 8u;
  for (int k = 0; k < 4; k++) {
    cut[4 + k] = (uint8_t)(riff >> (8 * k));
  }
  EXPECT_NE(load_and_decode(cut, 0, nullptr, &got), GIMG_OK)
      << "half of the picture's tokens are missing";
  EXPECT_EQ(got, nullptr);
}

TEST(HostileInput, ABmpEndpointOutsideTheFixedPointRangeIsClampedNotUndefined) {
  // A PNG cHRM can carry 0xFFFFFFFF, which loads as a coordinate of 42949.67;
  // converted to FXPT2DOT30 as it stood, that is undefined and came out as
  // INT32_MIN.
  GCOL_Color_Info info;
  gcol_color_info_default(&info);
  info.primaries_stated = true;
  info.gamut.red.x = 42949.67;
  info.gamut.red.y = -42949.67;
  unsigned char tail[GIMG_BMP_V5HEADER_SIZE - GIMG_BMP_V4_TAIL_AT];
  ASSERT_NE(gimg_bmp_color_to_header(&info, tail), 0u);
  auto field = [&](size_t i) {
    const unsigned char * q = tail + GIMG_BMP_V4_ENDPOINTS_AT + i * 4u;
    return (int32_t)((uint32_t)q[0] | ((uint32_t)q[1] << 8) |
        ((uint32_t)q[2] << 16) | ((uint32_t)q[3] << 24));
  };
  EXPECT_EQ(field(0), INT32_MAX) << "red x, far above 2";
  EXPECT_EQ(field(1), INT32_MIN) << "red y, far below -2";
}

TEST(HostileInput, ABmpOnAStreamThatCannotSeekIsRefusedAndSaysWhy) {
  Bytes f = {'B', 'M'};
  le32(f, 14 + 40 + 16);
  le32(f, 0);
  le32(f, 14 + 40);
  le32(f, 40);
  le32(f, 2);
  le32(f, 2);
  le16(f, 1);
  le16(f, 24);
  le32(f, 0);
  le32(f, 16);
  le32(f, 0);
  le32(f, 0);
  le32(f, 0);
  le32(f, 0);
  f.insert(f.end(), 16, 0x55);

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_no_seek(f.data(), f.size(), &s), GIMG_OK);
  GIMG_Diagnostics d = {};
  gimg_diagnostics_init(&d, nullptr);
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(gimg_doc_load(s, nullptr, &d, &doc), GIMG_ERR_UNSUPPORTED);
  bool said = false;
  for (size_t i = 0; i < d.count; i++) {
    said = said || strstr(d.items[i].recommended_action, "seek") != nullptr;
  }
  EXPECT_TRUE(said) << "the refusal names its reason";
  gimg_diagnostics_destroy(&d);
  gimg_stream_destroy(s);
}

TEST(HostileInput, PngImageDataMustBeConsecutiveAndPresent) {
  auto png = [](const std::vector<std::pair<const char *, Bytes>> & chunks) {
    Bytes f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ihdr;
    be32(ihdr, 1);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    for (const auto & c : chunks) {
      chunk(f, c.first, c.second);
    }
    chunk(f, "IEND", Bytes());
    return f;
  };
  const Bytes z = zlib_stored(Bytes{0, 0x80});
  const Bytes a(z.begin(), z.begin() + 5), b(z.begin() + 5, z.end());
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(load_with(png({{"IDAT", a}, {"IDAT", b}}), nullptr, &doc), GIMG_OK)
      << "control: split across two consecutive chunks";
  gimg_doc_destroy(doc);
  doc = nullptr;
  EXPECT_EQ(load_with(png({{"IDAT", a}, {"tEXt", Bytes{'k', 0, 'v'}},
                          {"IDAT", b}}),
                nullptr, &doc),
      GIMG_ERR_FORMAT);
  // A file with no image data loads - its chunks are readable - and fails when
  // a picture is asked of it.
  GIMG_Raster * r = nullptr;
  EXPECT_NE(load_and_decode(png({}), 0, nullptr, &r), GIMG_OK)
      << "no image data at all";
  EXPECT_EQ(r, nullptr);
}

TEST(HostileInput, EachPageOfATiffIsTurnedByItsOwnOrientation) {
  // Two 2 x 1 RGB pages; page 1 upright, page 2 stored rotated (6).  The
  // document's orientation is the first page's, and every page used to be
  // turned by it.
  const uint32_t entries = 10;
  const uint32_t ifd_bytes = 2u + entries * 12u + 4u;
  const uint32_t first = 8u;
  const uint32_t second = first + ifd_bytes;
  const uint32_t bps_at = second + ifd_bytes;
  const uint32_t strip_at = bps_at + 6u;
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  for (int page = 0; page < 2; page++) {
    le16(b, (uint16_t)entries);
    entry(b, 256, kLong, 1, 2);
    entry(b, 257, kLong, 1, 1);
    entry(b, 258, kShort, 3, bps_at);
    entry(b, 259, kShort, 1, 1);
    entry(b, 262, kShort, 1, 2);
    entry(b, 273, kLong, 1, strip_at);
    entry(b, 274, kShort, 1, page == 0 ? 1 : 6);
    entry(b, 277, kShort, 1, 3);
    entry(b, 278, kLong, 1, 1);
    entry(b, 279, kLong, 1, 6);
    le32(b, page == 0 ? second : 0u);
  }
  le16(b, 8);
  le16(b, 8);
  le16(b, 8);
  for (uint32_t i = 0; i < 6; i++) {
    b.push_back((uint8_t)(i + 1u));
  }
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(load_and_decode(b, 0, nullptr, &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r), 2u);
  EXPECT_EQ(gimg_raster_height(r), 1u);
  gimg_raster_destroy(r);
  ASSERT_EQ(load_and_decode(b, 1, nullptr, &r), GIMG_OK);
  EXPECT_EQ(gimg_raster_width(r), 1u) << "page 2 turned a quarter by its own tag";
  EXPECT_EQ(gimg_raster_height(r), 2u);
  gimg_raster_destroy(r);
}

Bytes ycbcr_tiff(bool bad_coefficients) {
  const uint32_t entries = bad_coefficients ? 11u : 10u;
  const uint32_t ifd_bytes = 2u + entries * 12u + 4u;
  const uint32_t bps_at = 8u + ifd_bytes;
  const uint32_t coeff_at = bps_at + 6u;
  const uint32_t strip_at = coeff_at + (bad_coefficients ? 24u : 0u);
  Bytes b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  le16(b, (uint16_t)entries);
  entry(b, 256, kLong, 1, 1);
  entry(b, 257, kLong, 1, 1);
  entry(b, 258, kShort, 3, bps_at);
  entry(b, 259, kShort, 1, 1);
  entry(b, 262, kShort, 1, 6); // YCbCr
  entry(b, 273, kLong, 1, strip_at);
  entry(b, 277, kShort, 1, 3);
  entry(b, 278, kLong, 1, 1);
  entry(b, 279, kLong, 1, 3);
  entry(b, 530, kShort, 2, 1u | (1u << 16)); // 1 x 1 subsampling
  if (bad_coefficients) {
    entry(b, 529, 5, 3, coeff_at); // RATIONAL x 3
  }
  le32(b, 0);
  le16(b, 8);
  le16(b, 8);
  le16(b, 8);
  if (bad_coefficients) {
    for (int c = 0; c < 3; c++) {
      le32(b, 0xFFFFFFFFu); // 4294967295 / 1
      le32(b, 1);
    }
  }
  b.insert(b.end(), {100, 90, 200});
  return b;
}

TEST(HostileInput, TiffYcbcrCoefficientsThatAreNotSharesOfLumaAreIgnored) {
  GIMG_Raster * plain = nullptr;
  GIMG_Raster * bad = nullptr;
  ASSERT_EQ(load_and_decode(ycbcr_tiff(false), 0, nullptr, &plain), GIMG_OK);
  ASSERT_EQ(load_and_decode(ycbcr_tiff(true), 0, nullptr, &bad), GIMG_OK);
  const uint8_t * a = (const uint8_t *)gimg_raster_pixels_const(plain);
  const uint8_t * c = (const uint8_t *)gimg_raster_pixels_const(bad);
  EXPECT_EQ(memcmp(a, c, 3), 0) << "the 601 defaults stand";
  gimg_raster_destroy(plain);
  gimg_raster_destroy(bad);
}

TEST(HostileInput, AOneComponentJpegWithSamplingFactorsIsReadInRasterOrder) {
  // A single-component scan is non-interleaved (T.81 A.2.2): its blocks run
  // left to right, top to bottom, whatever H and V say.  Eight blocks, each a
  // DC step of +32 (so each is 4 levels brighter than the one before it in
  // file order), for a 32 x 16 picture.
  Bytes j = {0xFF, 0xD8};
  const Bytes dqt = jpeg_segment(0xDB, [] {
    Bytes q = {0};
    q.insert(q.end(), 64, 1);
    return q;
  }());
  j.insert(j.end(), dqt.begin(), dqt.end());
  const Bytes sof = jpeg_segment(0xC0, Bytes{8, 0, 16, 0, 32, 1, 1, 0x22, 0});
  j.insert(j.end(), sof.begin(), sof.end());
  Bytes dc = {0x00, 1};
  dc.insert(dc.end(), 15, 0);
  dc.push_back(6);
  Bytes ac = {0x10, 1};
  ac.insert(ac.end(), 15, 0);
  ac.push_back(0);
  const Bytes dht = jpeg_segment(0xC4, [&] {
    Bytes t = dc;
    t.insert(t.end(), ac.begin(), ac.end());
    return t;
  }());
  j.insert(j.end(), dht.begin(), dht.end());
  const Bytes sos = jpeg_segment(0xDA, Bytes{1, 1, 0x00, 0, 63, 0});
  j.insert(j.end(), sos.begin(), sos.end());
  j.insert(j.end(), 8, 0x40);
  j.insert(j.end(), {0xFF, 0xD9});

  GIMG_Raster * r = nullptr;
  ASSERT_EQ(load_and_decode(j, 0, nullptr, &r), GIMG_OK);
  ASSERT_EQ(gimg_raster_width(r), 32u);
  const uint8_t * px = (const uint8_t *)gimg_raster_pixels_const(r);
  const size_t stride = gimg_raster_stride_bytes(r);
  const size_t bpp = gimg_raster_bytes_per_pixel(gimg_raster_format(r));
  // Block (0, 1) is the fifth stored: DC 5 * 32 / 8 above 128.
  EXPECT_NEAR(px[8 * stride], 148, 2) << "bpp " << bpp;
  EXPECT_NEAR(px[0], 132, 2);
  EXPECT_NEAR(px[8 * bpp * 3], 144, 2) << "block (3,0) is the fourth";
  gimg_raster_destroy(r);
}

TEST(HostileInput, AJpegFrameOf255ComponentsIsHeldToTheMemoryCapAsAWhole) {
  // 16 million pixels of 255 components is four gigabytes of planes, asked for
  // by a few hundred bytes of header; each plane alone is under the cap.
  Bytes sof = {8, 0x10, 0x00, 0x10, 0x00, 255};
  for (int c = 1; c <= 255; c++) {
    sof.insert(sof.end(), {(uint8_t)c, 0x11, 0});
  }
  Bytes j = {0xFF, 0xD8};
  const Bytes f = jpeg_segment(0xC0, sof);
  j.insert(j.end(), f.begin(), f.end());
  const Bytes s1 = sos_for(1);
  j.insert(j.end(), s1.begin(), s1.end());
  j.insert(j.end(), {0xFF, 0xD9});
  std::string why;
  EXPECT_EQ(load_jpeg(j, nullptr, &why), GIMG_ERR_LIMIT) << why;
  EXPECT_NE(why.find("max_memory"), std::string::npos) << why;
}

TEST(HostileInput, APngWriterDoesNotCopyAChunkThatIsUnsafeToCopy) {
  auto resaved_has = [](const char * type) {
    Bytes f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ihdr;
    be32(ihdr, 1);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    chunk(f, type, Bytes{1, 2, 3});
    chunk(f, "IDAT", zlib_stored(Bytes{0, 0x80}));
    chunk(f, "IEND", Bytes());
    GIMG_Stream * s = nullptr;
    EXPECT_EQ(gimg_stream_create_memory(f.data(), f.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    EXPECT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    EXPECT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr), GIMG_OK);
    GIMG_Stream * out = nullptr;
    EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Report report = {};
    EXPECT_EQ(gimg_doc_save(doc, out, "png", nullptr, &report), GIMG_OK);
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    const std::string bytes((const char *)data, size);
    const bool found = bytes.find(type) != std::string::npos;
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
    return found;
  };
  EXPECT_TRUE(resaved_has("prVt")) << "control: safe to copy";
  EXPECT_FALSE(resaved_has("prVT")) << "the fourth letter says it depends on the data";
}

TEST(HostileInput, APngChunkTypeThatIsNotFourLettersIsRefused) {
  Bytes f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  Bytes ihdr;
  be32(ihdr, 1);
  be32(ihdr, 1);
  ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
  chunk(f, "IHDR", ihdr);
  chunk(f, "pr1t", Bytes{1});
  chunk(f, "IDAT", zlib_stored(Bytes{0, 0x80}));
  chunk(f, "IEND", Bytes());
  GIMG_Doc * doc = nullptr;
  EXPECT_NE(load_with(f, nullptr, &doc), GIMG_OK);
  EXPECT_EQ(doc, nullptr);
}

TEST(HostileInput, StripGpsAlsoDropsTheXmpPacketAndRawProfilesOfAPng) {
  auto kept = [](const char * keyword, GIMG_Meta_Policy policy) {
    Bytes f = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ihdr;
    be32(ihdr, 1);
    be32(ihdr, 1);
    ihdr.insert(ihdr.end(), {8, 0, 0, 0, 0});
    chunk(f, "IHDR", ihdr);
    Bytes text(keyword, keyword + strlen(keyword));
    text.insert(text.end(), {0, 0, 0, 0, 0, 'x'}); // iTXt: flags, language, translated
    chunk(f, "iTXt", text);
    chunk(f, "IDAT", zlib_stored(Bytes{0, 0x80}));
    chunk(f, "IEND", Bytes());
    GIMG_Stream * s = nullptr;
    EXPECT_EQ(gimg_stream_create_memory(f.data(), f.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    EXPECT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    EXPECT_EQ(gimg_item_ensure_decoded(gimg_doc_item(doc, 0), nullptr), GIMG_OK);
    GIMG_Save_Options o;
    memset(&o, 0, sizeof o);
    o.metadata_policy = policy;
    GIMG_Stream * out = nullptr;
    EXPECT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
    GIMG_Save_Report report = {};
    EXPECT_EQ(gimg_doc_save(doc, out, "png", &o, &report), GIMG_OK);
    const void * data = nullptr;
    size_t size = 0;
    gimg_stream_output_buffer(out, &data, &size);
    const std::string bytes((const char *)data, size);
    const bool found = bytes.find("iTXt") != std::string::npos;
    gimg_stream_destroy(out);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
    return found;
  };
  EXPECT_TRUE(kept("XML:com.adobe.xmp", GIMG_META_PRESERVE_ALL)) << "control";
  EXPECT_FALSE(kept("XML:com.adobe.xmp", GIMG_META_STRIP_GPS));
  EXPECT_FALSE(kept("Raw profile type exif", GIMG_META_STRIP_GPS));
  EXPECT_TRUE(kept("Raw profile type icc", GIMG_META_STRIP_GPS));
  EXPECT_TRUE(kept("Author", GIMG_META_STRIP_GPS)) << "ordinary text stays";
}

TEST(HostileInput, CountingColoursOfARedGreenGradientIsNotQuadratic) {
  // Every colour differs in red and green only, so alpha and blue are
  // constant: the low bits of the old hash were the same for all of them and
  // the whole histogram was one probe chain.
  GIMG_Raster * ras = nullptr;
  ASSERT_EQ(gimg_raster_create(512, 512, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &ras),
      GIMG_OK);
  uint8_t * px = (uint8_t *)gimg_raster_pixels(ras);
  const size_t stride = gimg_raster_stride_bytes(ras);
  for (size_t y = 0; y < 512; y++) {
    for (size_t x = 0; x < 512; x++) {
      uint8_t * q = px + y * stride + x * 4u;
      q[0] = (uint8_t)(x & 255u);
      q[1] = (uint8_t)(y & 255u);
      q[2] = 7;
      q[3] = 255;
    }
  }
  const auto begin = std::chrono::steady_clock::now();
  size_t n = 0;
  bool exact = false;
  ASSERT_EQ(gimg_ops_count_colors(ras, 1u << 20, &n, &exact), GIMG_OK);
  const double seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - begin).count();
  EXPECT_EQ(n, 65536u);
  EXPECT_TRUE(exact);
  EXPECT_LT(seconds, 2.0) << "the old hash took many times this";
  gimg_raster_destroy(ras);
}

TEST(HostileInput, AGifOfMoreFramesThanTheDefaultHoldsIsRefused) {
  auto gif_of = [](size_t frames) {
    Bytes g = {'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0};
    g.insert(g.end(), {0, 0, 0, 255, 255, 255});
    for (size_t i = 0; i < frames; i++) {
      g.insert(g.end(), {0x2C, 0, 0, 0, 0, 1, 0, 1, 0, 0});
      g.insert(g.end(), {2, 2, 0x44, 0x01, 0});
    }
    g.push_back(0x3B);
    return g;
  };
  GIMG_Doc * doc = nullptr;
  EXPECT_EQ(load_with(gif_of(2), nullptr, &doc), GIMG_OK) << "control";
  gimg_doc_destroy(doc);
  doc = nullptr;
  EXPECT_EQ(load_with(gif_of(65537), nullptr, &doc), GIMG_ERR_LIMIT);
  EXPECT_EQ(doc, nullptr);
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
