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

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
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

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
