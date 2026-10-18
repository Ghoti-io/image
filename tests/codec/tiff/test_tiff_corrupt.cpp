/**
 * @file
 *
 * Malformed TIFFs built one tag at a time, each naming the rule it breaks.
 *
 * The same discipline tests/unit/test_corrupt.cpp applies to BMP, GIF and
 * PNG, and TIFF needs it more than any of them. Thirty-six sites in the
 * loader refuse a file by name, most of them returning one of two result
 * codes, so a test that asserts the code says nothing about whether the check
 * it was written for ran: it passes just as well when the file is refused one
 * tag earlier for an unrelated reason, and it keeps passing after the check
 * under test stops being reachable.
 *
 * So every case here names the diagnostic it expects, and the runner prints
 * the diagnostics it did get when that one is absent.
 *
 * The files are built from their fields rather than stored as blobs. A
 * malformed TIFF is a handful of numbers away from a valid one - a byte count
 * that does not match its offsets, a subsampling of zero, a directory that
 * points at itself - and saying which numbers in code is clearer than a blob
 * and a comment.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <algorithm>
#include <cstring>
#include <ghoti.io/color/color.h>
#include <ghoti.io/color/math.h>
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

// TIFF 6.0 field types, and the tags this codec reads.
enum : uint16_t { T_BYTE = 1, T_SHORT = 3, T_LONG = 4, T_RATIONAL = 5 };

struct Tag {
  uint16_t tag;
  uint16_t type;
  std::vector<uint32_t> values;
};

void put16(Bytes & b, uint32_t v, bool be) {
  if (be) {
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)v);
  }
  else {
    b.push_back((uint8_t)v);
    b.push_back((uint8_t)(v >> 8));
  }
}

void put32(Bytes & b, uint32_t v, bool be) {
  if (be) {
    b.push_back((uint8_t)(v >> 24));
    b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)v);
  }
  else {
    b.push_back((uint8_t)v);
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)(v >> 16));
    b.push_back((uint8_t)(v >> 24));
  }
}

size_t type_size(uint16_t type) {
  switch (type) {
  case T_BYTE: return 1u;
  case T_SHORT: return 2u;
  case T_LONG: return 4u;
  case T_RATIONAL: return 8u;
  default: return 1u;
  }
}

Bytes pack(uint16_t type, const std::vector<uint32_t> & values, bool be) {
  Bytes out;
  for (uint32_t v : values) {
    switch (type) {
    case T_BYTE: out.push_back((uint8_t)v); break;
    case T_SHORT: put16(out, v, be); break;
    case T_RATIONAL: put32(out, v, be); put32(out, 1u, be); break;
    default: put32(out, v, be); break;
    }
  }
  return out;
}

/**
 * Lay out a whole TIFF: header, pixels, directory, then the values too long
 * to sit in a directory entry.
 *
 * Two passes, because a directory's own offset depends on how much overflow
 * there is and the overflow is only known once every field is packed. The
 * generator under tests/data/tiff got this wrong first time and the mistake
 * was not loud: every offset moved by the length of one field and four
 * fixtures loaded as "the offsets and the byte counts do not describe the
 * same blocks".
 */
Bytes make_tiff(std::vector<Tag> tags, const Bytes & pixels, bool be = false,
    uint16_t version = 42u, uint32_t first_ifd_override = 0u) {
  std::sort(tags.begin(), tags.end(),
      [](const Tag & a, const Tag & b) { return a.tag < b.tag; });
  Bytes pool = pixels;
  if (pool.size() & 1u) { pool.push_back(0u); }

  size_t overflow_total = 0;
  for (const Tag & t : tags) {
    const size_t n = type_size(t.type) * t.values.size();
    if (n > 4u) { overflow_total += n + (n & 1u); }
  }
  const size_t ifd_at = 8u + pool.size() + overflow_total;

  Bytes overflow, directory;
  put16(directory, (uint32_t)tags.size(), be);
  for (const Tag & t : tags) {
    const Bytes raw = pack(t.type, t.values, be);
    put16(directory, t.tag, be);
    put16(directory, t.type, be);
    put32(directory, (uint32_t)t.values.size(), be);
    if (raw.size() <= 4u) {
      directory.insert(directory.end(), raw.begin(), raw.end());
      for (size_t i = raw.size(); i < 4u; i++) { directory.push_back(0u); }
    }
    else {
      put32(directory, (uint32_t)(8u + pool.size() + overflow.size()), be);
      overflow.insert(overflow.end(), raw.begin(), raw.end());
      if (overflow.size() & 1u) { overflow.push_back(0u); }
    }
  }
  put32(directory, 0u, be); // No next directory.

  Bytes out;
  out.push_back(be ? 'M' : 'I');
  out.push_back(be ? 'M' : 'I');
  put16(out, version, be);
  put32(out, first_ifd_override ? first_ifd_override : (uint32_t)ifd_at, be);
  out.insert(out.end(), pool.begin(), pool.end());
  out.insert(out.end(), overflow.begin(), overflow.end());
  out.insert(out.end(), directory.begin(), directory.end());
  return out;
}

/** A 2x2 eight-bit grayscale TIFF that loads, as the base for a mutation. */
std::vector<Tag> good_tags(size_t pixel_bytes = 4u) {
  return {
      {256u, T_LONG, {2u}},               // ImageWidth
      {257u, T_LONG, {2u}},               // ImageLength
      {258u, T_SHORT, {8u}},              // BitsPerSample
      {259u, T_SHORT, {1u}},              // Compression
      {262u, T_SHORT, {1u}},              // Photometric: BlackIsZero
      {273u, T_LONG, {8u}},               // StripOffsets
      {277u, T_SHORT, {1u}},              // SamplesPerPixel
      {278u, T_LONG, {2u}},               // RowsPerStrip
      {279u, T_LONG, {(uint32_t)pixel_bytes}}, // StripByteCounts
      {284u, T_SHORT, {1u}},              // PlanarConfiguration
  };
}

/** Replace or add one tag. */
std::vector<Tag> with(std::vector<Tag> tags, Tag t) {
  for (Tag & existing : tags) {
    if (existing.tag == t.tag) {
      existing = t;
      return tags;
    }
  }
  tags.push_back(t);
  return tags;
}

std::vector<Tag> without(std::vector<Tag> tags, uint16_t tag) {
  std::vector<Tag> out;
  for (const Tag & t : tags) {
    if (t.tag != tag) { out.push_back(t); }
  }
  return out;
}

struct Case {
  const char * name;
  const char * reason;
  GIMG_Result expect;
  Bytes bytes;
  bool no_seek = false;
  bool use_limits = false;
  GIMG_Limits limits{};
};

void run_case(const Case & c) {
  SCOPED_TRACE(c.name);
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(c.no_seek ? gimg_stream_create_memory_no_seek(
                            c.bytes.data(), c.bytes.size(), &s)
                      : gimg_stream_create_memory(
                            c.bytes.data(), c.bytes.size(), &s),
      GIMG_OK);
  GIMG_Load_Options opts = {};
  if (c.use_limits) { opts.limits = &c.limits; }
  GIMG_Diagnostics diag = {};
  GIMG_Doc * doc = nullptr;
  const GIMG_Result r =
      gimg_doc_load(s, c.use_limits ? &opts : nullptr, &diag, &doc);
  EXPECT_EQ(r, c.expect) << "wrong result code";
  if (c.reason) {
    bool found = false;
    std::string saw;
    for (size_t i = 0; i < diag.count; i++) {
      const char * a = diag.items[i].recommended_action;
      if (!a) { continue; }
      saw += std::string(saw.empty() ? "" : " | ") + a;
      if (std::string(a).find(c.reason) != std::string::npos) { found = true; }
    }
    EXPECT_TRUE(found) << "expected a diagnostic naming \"" << c.reason
                       << "\"; got: " << (saw.empty() ? "(nothing)" : saw);
  }
  gimg_diagnostics_destroy(&diag);
  if (doc) { gimg_doc_destroy(doc); }
  gimg_stream_destroy(s);
}

const Bytes kPixels = {0u, 64u, 128u, 255u};

} // namespace

TEST(TiffCorrupt, TheBaseFixtureThisFileMutatesActuallyLoads) {
  // Every case below is this file with one thing changed, so if this one did
  // not load the whole file would be asserting that a broken base is broken.
  GIMG_Stream * s = nullptr;
  const Bytes bytes = make_tiff(good_tags(), kPixels);
  ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &s),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Diagnostics diag = {};
  EXPECT_EQ(gimg_doc_load(s, nullptr, &diag, &doc), GIMG_OK);
  EXPECT_NE(doc, nullptr);
  gimg_diagnostics_destroy(&diag);
  if (doc) { gimg_doc_destroy(doc); }
  gimg_stream_destroy(s);
}

TEST(TiffCorrupt, TheHeaderAndTheChainNameWhatBrokeThem) {
  std::vector<Case> cases;
  cases.push_back({"a header cut off after four bytes", "truncated header",
      GIMG_ERR_CORRUPT, Bytes{'I', 'I', 42u, 0u}});
  {
    // Version 43 with a legal offset size, cut off before the IFD offset.
    Bytes b = {'I', 'I', 43u, 0u, 8u, 0u, 0u, 0u};
    cases.push_back({"little-endian BigTIFF cut off before its IFD offset",
        "truncated header", GIMG_ERR_CORRUPT, b});
  }
  {
    Bytes b = {'M', 'M', 0u, 43u, 0u, 0u, 0u, 8u};
    cases.push_back({"big-endian BigTIFF cut off before its IFD offset",
        "truncated header", GIMG_ERR_CORRUPT, b});
  }
  {
    Bytes b = {'I', 'I', 43u, 0u, 4u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    cases.push_back({"a BigTIFF whose offset size is not 8", "offset size",
        GIMG_ERR_UNSUPPORTED, b});
  }
  {
    Bytes b = {'I', 'I', 43u, 0u, 8u, 0u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    cases.push_back({"a BigTIFF whose reserved word is not zero", "reserved",
        GIMG_ERR_CORRUPT, b});
  }
  {
    Case c{"a file read from something that cannot be measured",
        "requires a sized stream", GIMG_ERR_UNSUPPORTED,
        make_tiff(good_tags(), kPixels)};
    c.no_seek = true;
    cases.push_back(c);
  }
  {
    // The header's own offset points past the end.
    Bytes b = make_tiff(good_tags(), kPixels, false, 42u, 0xFFFF0000u);
    cases.push_back({"a first directory past the end of the file",
        "directory begins past the end", GIMG_ERR_CORRUPT, b});
  }
  {
    // An offset of zero means "no directories at all".
    Bytes b = make_tiff(good_tags(), kPixels);
    b[4] = b[5] = b[6] = b[7] = 0u;
    cases.push_back({"a file naming no directories",
        "no image directories", GIMG_ERR_CORRUPT, b});
  }
  for (const Case & c : cases) { run_case(c); }
}

TEST(TiffCorrupt, TheGeometryTagsNameWhatDisagreesWithWhat) {
  std::vector<Case> cases;
  cases.push_back({"an image of no width", "zero width or height",
      GIMG_ERR_CORRUPT,
      make_tiff(with(good_tags(), {256u, T_LONG, {0u}}), kPixels)});
  cases.push_back({"a file with no PhotometricInterpretation",
      "no PhotometricInterpretation", GIMG_ERR_CORRUPT,
      make_tiff(without(good_tags(), 262u), kPixels)});
  cases.push_back({"as many byte counts as it has strips, and no more",
      "do not describe the same blocks", GIMG_ERR_CORRUPT,
      make_tiff(with(good_tags(), {279u, T_LONG, {4u, 4u}}), kPixels)});
  cases.push_back({"more strips than the geometry has",
      "different number of blocks",
      GIMG_ERR_CORRUPT,
      make_tiff(with(with(good_tags(), {273u, T_LONG, {8u, 10u}}),
                    {279u, T_LONG, {2u, 2u}}),
          kPixels)});
  cases.push_back({"a strip that runs off the end of the file",
      "lies outside the file", GIMG_ERR_CORRUPT,
      make_tiff(with(good_tags(), {279u, T_LONG, {4096u}}), kPixels)});
  cases.push_back({"a tile grid with no width", "geometry does not add up",
      GIMG_ERR_CORRUPT,
      make_tiff(with(with(good_tags(), {322u, T_LONG, {0u}}),
                    {323u, T_LONG, {2u}}),
          kPixels)});
  {
    Case c{"an image past max_decoded_pixels", "increase max_decoded_pixels",
        GIMG_ERR_LIMIT, make_tiff(good_tags(), kPixels)};
    c.use_limits = true;
    c.limits.max_decoded_pixels = 3u;
    cases.push_back(c);
  }
  for (const Case & c : cases) { run_case(c); }
}

TEST(TiffCorrupt, TheSampleTagsNameWhatTheyCannotDescribe) {
  std::vector<Case> cases;
  cases.push_back({"a compression this codec does not undo",
      "compression method this codec does not undo", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(good_tags(), {259u, T_LONG, {34712u}}), kPixels)});
  cases.push_back({"CCITT Group 3 on eight-bit samples",
      "one bit of one sample", GIMG_ERR_CORRUPT,
      make_tiff(with(good_tags(), {259u, T_SHORT, {3u}}), kPixels)});
  cases.push_back({"CCITT Group 4 asking for uncompressed mode",
      "uncompressed mode", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(with(with(good_tags(), {259u, T_SHORT, {4u}}),
                        {258u, T_SHORT, {1u}}),
                    {293u, T_LONG, {2u}}),
          kPixels)});
  cases.push_back({"a Predictor the format does not define",
      "Predictor other than 1 or 2", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(good_tags(), {317u, T_SHORT, {7u}}), kPixels)});
  cases.push_back({"horizontal differencing at four bits",
      "differencing at a depth it is not defined for", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(with(with(good_tags(), {258u, T_SHORT, {4u}}),
                        {317u, T_SHORT, {2u}}),
                    {259u, T_SHORT, {5u}}),
          kPixels)});
  cases.push_back({"a PlanarConfiguration of three",
      "PlanarConfiguration is neither 1 nor 2", GIMG_ERR_CORRUPT,
      make_tiff(with(good_tags(), {284u, T_SHORT, {3u}}), kPixels)});
  cases.push_back({"floating-point samples", "SampleFormat names something",
      GIMG_ERR_UNSUPPORTED,
      make_tiff(with(good_tags(), {339u, T_SHORT, {3u}}), kPixels)});
  cases.push_back({"samples of different widths",
      "BitsPerSample differs between samples", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(with(good_tags(), {277u, T_SHORT, {3u}}),
                    {258u, T_SHORT, {8u, 4u, 8u}}),
          kPixels)});
  cases.push_back({"forty bits per sample",
      "bit depth above thirty-two", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(good_tags(), {258u, T_SHORT, {40u}}), kPixels)});
  cases.push_back({"grayscale with three samples",
      "grayscale image with more than one sample", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(good_tags(), {277u, T_SHORT, {3u}}), kPixels)});
  cases.push_back({"RGB with two samples",
      "neither three nor four samples", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(with(good_tags(), {262u, T_SHORT, {2u}}),
                    {277u, T_SHORT, {2u}}),
          kPixels)});
  cases.push_back({"a palette image with three samples",
      "palette image with more than one sample", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(with(good_tags(), {262u, T_SHORT, {3u}}),
                    {277u, T_SHORT, {3u}}),
          kPixels)});
  cases.push_back({"a palette shorter than the depth needs",
      "ColorMap is shorter than the bit depth needs", GIMG_ERR_CORRUPT,
      make_tiff(with(with(good_tags(), {262u, T_SHORT, {3u}}),
                    {320u, T_SHORT, {0u, 0u, 0u}}),
          kPixels)});
  cases.push_back({"a photometric this codec does not read",
      "PhotometricInterpretation this codec does not read",
      GIMG_ERR_UNSUPPORTED,
      make_tiff(with(good_tags(), {262u, T_SHORT, {4u}}), kPixels)});
  for (const Case & c : cases) { run_case(c); }
}

TEST(TiffCorrupt, TheYCbCrAndSeparatedTagsNameTheirOwnRules) {
  std::vector<Case> cases;
  auto ycbcr = [](std::vector<Tag> extra) {
    std::vector<Tag> t = with(with(good_tags(12u), {262u, T_SHORT, {6u}}),
        {277u, T_SHORT, {3u}});
    for (const Tag & e : extra) { t = with(t, e); }
    return t;
  };
  cases.push_back({"YCbCr with one sample", "YCbCr image without three",
      GIMG_ERR_UNSUPPORTED,
      make_tiff(with(ycbcr({}), {277u, T_SHORT, {1u}}), Bytes(12u, 0u))});
  cases.push_back({"YCbCr at sixteen bits",
      "YCbCr at a depth other than eight", GIMG_ERR_UNSUPPORTED,
      make_tiff(ycbcr({{258u, T_SHORT, {16u, 16u, 16u}}}), Bytes(12u, 0u))});
  cases.push_back({"YCbCr with its channels apart",
      "YCbCr with its channels stored apart", GIMG_ERR_UNSUPPORTED,
      make_tiff(ycbcr({{284u, T_SHORT, {2u}}, {273u, T_LONG, {8u, 9u, 10u}},
                    {279u, T_LONG, {4u, 4u, 4u}}}),
          Bytes(12u, 0u))});
  cases.push_back({"a YCbCr subsampling of zero",
      "subsampling the format does not define", GIMG_ERR_CORRUPT,
      make_tiff(ycbcr({{530u, T_SHORT, {0u, 2u}}}), Bytes(12u, 0u))});
  {
    // Four rows, two rows per strip, and a vertical subsampling of four: a
    // strip would hold half a unit row, and nothing says which half.
    std::vector<Tag> t = ycbcr({{257u, T_LONG, {4u}}, {278u, T_LONG, {2u}},
        {530u, T_SHORT, {1u, 4u}}, {273u, T_LONG, {8u, 20u}},
        {279u, T_LONG, {12u, 12u}}});
    cases.push_back({"a strip holding half a subsampling unit",
        "does not hold whole YCbCr subsampling units", GIMG_ERR_CORRUPT,
        make_tiff(t, Bytes(24u, 0u))});
  }
  cases.push_back({"separated with five inks",
      "separated image with other than four inks", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(with(good_tags(20u), {262u, T_SHORT, {5u}}),
                    {277u, T_SHORT, {5u}}),
          Bytes(20u, 0u))});
  cases.push_back({"separated at four bits",
      "separated image at a depth with no CMYK raster", GIMG_ERR_UNSUPPORTED,
      make_tiff(with(with(with(good_tags(8u), {262u, T_SHORT, {5u}}),
                        {277u, T_SHORT, {4u}}),
                    {258u, T_SHORT, {4u, 4u, 4u, 4u}}),
          Bytes(8u, 0u))});
  for (const Case & c : cases) { run_case(c); }
}

namespace {

// A 2x2 LogL image in SGILog: each row is two run-length planes, the high
// bytes then the low ones.  Row 0 is literals, row 1 a run of each byte.
const uint16_t kLogLCodes[4] = {0x0000u, 0x4000u, 0xc123u, 0xc123u};
const Bytes kLogLRows = {
    0x02u, 0x00u, 0x40u, 0x02u, 0x00u, 0x00u, // row 0: literal, literal
    0x80u, 0xc1u, 0x80u, 0x23u,               // row 1: run of 2, run of 2
};

std::vector<Tag> logl_tags(size_t bytes = 10u) {
  return {
      {256u, T_LONG, {2u}},
      {257u, T_LONG, {2u}},
      // What libtiff stamps for a 16-bit signed caller buffer.  Not read.
      {258u, T_SHORT, {16u}},
      {259u, T_SHORT, {34676u}},
      {262u, T_SHORT, {32844u}},
      {273u, T_LONG, {8u}},
      {277u, T_SHORT, {1u}},
      {278u, T_LONG, {2u}},
      {279u, T_LONG, {(uint32_t)bytes}},
      {284u, T_SHORT, {1u}},
      {339u, T_SHORT, {2u}},
  };
}

GIMG_Raster * decode_bytes(const Bytes & b, GIMG_Result * out) {
  GIMG_Stream * s = nullptr;
  GIMG_Doc * doc = nullptr;
  GIMG_Raster * raster = nullptr;
  *out = gimg_stream_create_memory(b.data(), b.size(), &s);
  if (*out == GIMG_OK) {
    *out = gimg_doc_load(s, nullptr, nullptr, &doc);
  }
  if (*out == GIMG_OK) {
    *out = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
  }
  if (doc) { gimg_doc_destroy(doc); }
  if (s) { gimg_stream_destroy(s); }
  return raster;
}

float float_at(const GIMG_Raster * r, uint32_t x, uint32_t y, unsigned c) {
  const uint8_t * p = (const uint8_t *)gimg_raster_pixels_const(r) +
      (size_t)y * gimg_raster_stride_bytes(r);
  const size_t ch = gimg_raster_format(r)->channel_count;
  float v;
  std::memcpy(&v, p + ((size_t)x * ch + c) * sizeof(float), sizeof v);
  return v;
}

} // namespace

// The bytes this decoder must undo, built by hand rather than by libtiff, so
// the storage is checked here without an oracle: literals and runs, and a
// block that ends inside its second row.  test_tiff_logluv.cpp is where the
// same decoder is held to libtiff over every form and layout.
TEST(TiffLogLuvBytes, AHandBuiltLogLDecodesToItsCodes) {
  GIMG_Result r;
  GIMG_Raster * raster = decode_bytes(make_tiff(logl_tags(), kLogLRows), &r);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_EQ(gimg_raster_format(raster)->channel_type, GIMG_CHANNEL_FLOAT);
  ASSERT_EQ(gimg_raster_format(raster)->channel_count, 1u);
  for (uint32_t i = 0; i < 4u; i++) {
    EXPECT_EQ(float_at(raster, i % 2u, i / 2u, 0),
        (float)gcol_logl16_to_y(kLogLCodes[i]))
        << i;
  }
  EXPECT_LT(float_at(raster, 0, 1, 0), 0.0f); // The sign bit is kept.
  gimg_raster_destroy(raster);

  // Cut inside row 1: row 0 survives and row 1 stays as created, zero.
  Bytes cut(kLogLRows.begin(), kLogLRows.begin() + 8);
  raster = decode_bytes(make_tiff(logl_tags(8u), cut), &r);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(float_at(raster, 1, 0, 0), (float)gcol_logl16_to_y(0x4000u));
  EXPECT_EQ(float_at(raster, 0, 1, 0), 0.0f);
  EXPECT_EQ(float_at(raster, 1, 1, 0), 0.0f);
  gimg_raster_destroy(raster);
}

TEST(TiffLogLuvBytes, AHandBuiltLogLuv24DecodesToItsCodes) {
  // Two pixels, three bytes each, most significant first in a little-endian
  // file: the packing does not follow the byte order.
  const uint32_t codes[2] = {0xc03ffbu, 0x5f1f40u};
  const Bytes px = {0xc0u, 0x3fu, 0xfbu, 0x5fu, 0x1fu, 0x40u};
  std::vector<Tag> tags = logl_tags(6u);
  tags = with(tags, {257u, T_LONG, {1u}});
  tags = with(tags, {258u, T_SHORT, {32u, 32u, 32u}});
  tags = with(tags, {259u, T_SHORT, {34677u}});
  tags = with(tags, {262u, T_SHORT, {32845u}});
  tags = with(tags, {277u, T_SHORT, {3u}});
  tags = with(tags, {278u, T_LONG, {1u}});
  tags = with(tags, {339u, T_SHORT, {3u, 3u, 3u}});
  GIMG_Result r;
  GIMG_Raster * raster = decode_bytes(make_tiff(tags, px), &r);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_EQ(gimg_raster_format(raster)->channel_count, 4u);
  GCOL_Gamut srgb;
  double m[9];
  ASSERT_TRUE(gcol_gamut_named(GCOL_PRIMARIES_SRGB, &srgb));
  ASSERT_EQ(gcol_xyz_to_gamut(&srgb, m), GCOL_OK);
  for (uint32_t x = 0; x < 2u; x++) {
    double xyz[3];
    ASSERT_EQ(gcol_logluv24_to_xyz(codes[x], xyz), GCOL_OK);
    for (unsigned c = 0; c < 3u; c++) {
      const double want =
          m[c * 3] * xyz[0] + m[c * 3 + 1] * xyz[1] + m[c * 3 + 2] * xyz[2];
      EXPECT_EQ(float_at(raster, x, 0, c), (float)want) << x << " " << c;
    }
    EXPECT_EQ(float_at(raster, x, 0, 3), 1.0f);
  }
  gimg_raster_destroy(raster);
}

TEST(TiffCorrupt, TheLogLuvTagsNameTheirOwnRules) {
  run_case({"the LogL base", nullptr, GIMG_OK,
      make_tiff(logl_tags(), kLogLRows)});
  std::vector<Case> cases;
  cases.push_back({"SGILog on BlackIsZero samples",
      "SGILog compression on samples that are not LogL", GIMG_ERR_CORRUPT,
      make_tiff(with(logl_tags(), {262u, T_SHORT, {1u}}), kLogLRows)});
  cases.push_back({"LogL stored uncompressed", "outside SGILog compression",
      GIMG_ERR_CORRUPT,
      make_tiff(with(logl_tags(), {259u, T_SHORT, {1u}}), kLogLRows)});
  cases.push_back({"LogL in the 24-bit coding", "LogL in 24-bit SGILog",
      GIMG_ERR_CORRUPT,
      make_tiff(with(logl_tags(), {259u, T_SHORT, {34677u}}), kLogLRows)});
  cases.push_back({"LogL with three samples", "LogL with other than one",
      GIMG_ERR_CORRUPT,
      make_tiff(with(logl_tags(), {277u, T_SHORT, {3u}}), kLogLRows)});
  cases.push_back({"LogLuv with one sample", "LogLuv with other than three",
      GIMG_ERR_CORRUPT,
      make_tiff(with(logl_tags(), {262u, T_SHORT, {32845u}}), kLogLRows)});
  cases.push_back({"LogL in separate planes", "PlanarConfiguration 2",
      GIMG_ERR_UNSUPPORTED,
      make_tiff(with(logl_tags(), {284u, T_SHORT, {2u}}), kLogLRows)});
  cases.push_back({"LogL with a Predictor", "Predictor on LogLuv codes",
      GIMG_ERR_UNSUPPORTED,
      make_tiff(with(logl_tags(), {317u, T_SHORT, {2u}}), kLogLRows)});
  // BitsPerSample and SampleFormat describe the writer's buffer, so values
  // the integer checks would refuse load here.
  cases.push_back({"LogL stamped 32-bit float", nullptr, GIMG_OK,
      make_tiff(with(with(logl_tags(), {258u, T_SHORT, {32u}}),
                    {339u, T_SHORT, {3u}}),
          kLogLRows)});
  for (const Case & c : cases) {
    run_case(c);
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
