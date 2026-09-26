/**
 * @file
 *
 * Malformed files built one field at a time, each naming the rule it breaks.
 *
 * The fuzz corpus is the population that walks error handling, and it is not
 * evenly spread: 5,992 of its files are JPEG against 146 BMP, 120 PNG and 15
 * GIF. So the three smaller codecs have refusal paths that nothing in the
 * suite reaches - not because they are hard to reach, but because no input in
 * the tree happens to break that particular rule. A mutation of a real file
 * is unlikely to land on "a bitmap array whose chain points backwards"; a
 * file written to be exactly that lands on it every time.
 *
 * Each case below is built from its fields rather than stored as a blob, so
 * the bytes say what they mean, and each asserts **which** rule fired rather
 * than only that something did. That second half is the point. Every one of
 * these refusals returns GIMG_ERR_CORRUPT or GIMG_ERR_UNSUPPORTED, and the
 * BMP loader alone has twenty-two sites returning the first of those: a test
 * asserting the code alone passes when the file is refused for an unrelated
 * reason one field earlier, which is exactly how a check stops being
 * reachable without anybody noticing.
 *
 * The diagnostic string is the discriminator, and it is checked as a
 * substring of `recommended_action` - the same text the library already
 * promises a caller in `codec.h`.
 *
 * Copyright 2026 by Corey Pennycuff
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
#include <string>
#include <utility>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

void u8(Bytes & b, uint32_t v) { b.push_back((uint8_t)(v & 0xFFu)); }

void u16le(Bytes & b, uint32_t v) {
  u8(b, v);
  u8(b, v >> 8);
}

void u32le(Bytes & b, uint32_t v) {
  u8(b, v);
  u8(b, v >> 8);
  u8(b, v >> 16);
  u8(b, v >> 24);
}

void u16be(Bytes & b, uint32_t v) {
  u8(b, v >> 8);
  u8(b, v);
}

void u32be(Bytes & b, uint32_t v) {
  u8(b, v >> 24);
  u8(b, v >> 16);
  u8(b, v >> 8);
  u8(b, v);
}

void append(Bytes & b, const Bytes & tail) {
  b.insert(b.end(), tail.begin(), tail.end());
}

// ---------------------------------------------------------------------------
// BMP
// ---------------------------------------------------------------------------

/** The fields of one BMP, as they sit in the file. */
struct BmpSpec {
  uint32_t header_size = 40u;   ///< BITMAPINFOHEADER unless stated.
  int32_t width = 2;
  int32_t height = 2;
  uint16_t bpp = 24u;
  uint32_t compression = 0u;    ///< biCompression, as written, not decoded.
  uint32_t size_image = 0u;
  uint32_t clr_used = 0u;
  Bytes after_header;           ///< Masks, palette and pixels, in that order.
  size_t pixels_at = 0u;        ///< Where the pixels start inside that.
  uint32_t off_bits = 0u;       ///< 0 = point it at pixels_at.
  /** Words to poke into the DIB header, by offset from its own start. Used
   * for the V4 and V5 fields past the BITMAPINFOHEADER the builder writes. */
  std::vector<std::pair<size_t, uint32_t>> dib_words;
};

Bytes make_bmp(const BmpSpec & s) {
  Bytes dib;
  u32le(dib, s.header_size);
  u32le(dib, (uint32_t)s.width);
  u32le(dib, (uint32_t)s.height);
  u16le(dib, 1u);
  u16le(dib, s.bpp);
  u32le(dib, s.compression);
  u32le(dib, s.size_image);
  u32le(dib, 0u);  // biXPelsPerMeter
  u32le(dib, 0u);  // biYPelsPerMeter
  u32le(dib, s.clr_used);
  u32le(dib, 0u);  // biClrImportant
  dib.resize(s.header_size, 0u);
  for (const auto & w : s.dib_words) {
    dib[w.first + 0u] = (uint8_t)(w.second);
    dib[w.first + 1u] = (uint8_t)(w.second >> 8);
    dib[w.first + 2u] = (uint8_t)(w.second >> 16);
    dib[w.first + 3u] = (uint8_t)(w.second >> 24);
  }

  Bytes out;
  out.push_back('B');
  out.push_back('M');
  const uint32_t total =
      14u + s.header_size + (uint32_t)s.after_header.size();
  u32le(out, total);
  u16le(out, 0u);
  u16le(out, 0u);
  u32le(out,
      s.off_bits ? s.off_bits
                 : 14u + s.header_size + (uint32_t)s.pixels_at);
  append(out, dib);
  append(out, s.after_header);
  return out;
}

/** One entry of an OS/2 bitmap array header: 'BA', size, offNext, screen. */
Bytes bmp_array_header(uint32_t size, uint32_t next) {
  Bytes b;
  b.push_back('B');
  b.push_back('A');
  u32le(b, size);
  u32le(b, next);
  u16le(b, 0u);  // cxDisplay
  u16le(b, 0u);  // cyDisplay
  return b;      // 14 bytes
}

// ---------------------------------------------------------------------------
// GIF
// ---------------------------------------------------------------------------

/**
 * Header plus logical screen descriptor, with a two-entry global table when
 * `with_table` - which is what makes a frame decodable without one of its
 * own.
 */
Bytes gif_head(uint16_t w, uint16_t h, bool with_table = true) {
  Bytes b;
  const char * sig = "GIF89a";
  b.insert(b.end(), sig, sig + 6);
  u16le(b, w);
  u16le(b, h);
  // Packed: global table present, colour resolution 1, size 0 = two entries.
  u8(b, with_table ? 0x80u : 0x00u);
  u8(b, 0u);  // Background colour index.
  u8(b, 0u);  // Pixel aspect ratio: none stated.
  if (with_table) {
    for (int i = 0; i < 2; i++) {
      u8(b, i ? 0xFFu : 0x00u);
      u8(b, i ? 0xFFu : 0x00u);
      u8(b, i ? 0xFFu : 0x00u);
    }
  }
  return b;
}

/** An image descriptor with no local colour table. */
Bytes gif_image_descriptor(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  Bytes b;
  u8(b, 0x2Cu);
  u16le(b, x);
  u16le(b, y);
  u16le(b, w);
  u16le(b, h);
  u8(b, 0u);
  return b;
}

/** A sub-block chain carrying `payload`, split at the 255-byte limit. */
Bytes gif_sub_blocks(const Bytes & payload) {
  Bytes b;
  size_t at = 0;
  while (at < payload.size()) {
    const size_t n = payload.size() - at < 255u ? payload.size() - at : 255u;
    u8(b, (uint32_t)n);
    b.insert(b.end(), payload.begin() + (long)at, payload.begin() + (long)(at + n));
    at += n;
  }
  u8(b, 0u);
  return b;
}

// ---------------------------------------------------------------------------
// PNG
// ---------------------------------------------------------------------------

uint32_t crc32_of(const uint8_t * data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= data[i];
    for (int k = 0; k < 8; k++) {
      c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1u)));
    }
  }
  return c ^ 0xFFFFFFFFu;
}

/** One PNG chunk: length, type, payload, CRC over type and payload. */
Bytes png_chunk(const char type[5], const Bytes & payload) {
  Bytes b;
  u32be(b, (uint32_t)payload.size());
  Bytes crcd;
  crcd.insert(crcd.end(), type, type + 4);
  append(crcd, payload);
  append(b, crcd);
  u32be(b, crc32_of(crcd.data(), crcd.size()));
  return b;
}

Bytes png_signature() {
  const uint8_t sig[8] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n'};
  return Bytes(sig, sig + 8);
}

/** An fcTL for a 1x1 frame at the origin, numbered `sequence`. */
Bytes png_fctl(uint32_t sequence) {
  Bytes p;
  u32be(p, sequence);
  u32be(p, 1u);   // width
  u32be(p, 1u);   // height
  u32be(p, 0u);   // x_offset
  u32be(p, 0u);   // y_offset
  u16be(p, 1u);   // delay_num
  u16be(p, 10u);  // delay_den
  u8(p, 0u);      // dispose_op
  u8(p, 0u);      // blend_op
  return p;
}

Bytes png_ihdr(uint32_t w, uint32_t h, uint8_t depth, uint8_t colour,
    uint8_t interlace = 0u) {
  Bytes p;
  u32be(p, w);
  u32be(p, h);
  u8(p, depth);
  u8(p, colour);
  u8(p, 0u);  // Compression method.
  u8(p, 0u);  // Filter method.
  u8(p, interlace);
  return png_chunk("IHDR", p);
}

/**
 * A zlib stream holding `raw` as one stored deflate block. Enough to make a
 * decodable 1x1 or 2x2 image without pulling a compressor into a test.
 */
Bytes zlib_stored(const Bytes & raw) {
  Bytes b;
  u8(b, 0x78u);
  u8(b, 0x01u);
  u8(b, 0x01u);  // Final stored block.
  u16le(b, (uint32_t)raw.size());
  u16le(b, (uint32_t)(~raw.size() & 0xFFFFu));
  append(b, raw);
  uint32_t a = 1u, s = 0u;
  for (uint8_t v : raw) {
    a = (a + v) % 65521u;
    s = (s + a) % 65521u;
  }
  u32be(b, (s << 16) | a);
  return b;
}

// ---------------------------------------------------------------------------
// The runner
// ---------------------------------------------------------------------------

/** When the refusal is expected: at load, or at decode of the first item. */
enum Stage { AT_LOAD, AT_DECODE };

struct Case {
  const char * name;     ///< What is wrong with the file.
  const char * reason;   ///< Substring of the diagnostic that must appear.
  GIMG_Result expect;
  Bytes bytes;
  Stage stage = AT_LOAD;
  bool no_seek = false;  ///< Read it the way a pipe would be read.
  bool use_limits = false;
  GIMG_Limits limits{};
};

/** Load (and optionally decode) one case and report what it answered. */
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
  GIMG_Result r =
      gimg_doc_load(s, c.use_limits ? &opts : nullptr, &diag, &doc);

  if (c.stage == AT_DECODE) {
    ASSERT_EQ(r, GIMG_OK) << "the case is about decode, but the load refused";
    ASSERT_NE(doc, nullptr);
    ASSERT_GT(gimg_doc_item_count(doc), 0u);
    GIMG_Raster * raster = nullptr;
    r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
    if (raster) { gimg_raster_destroy(raster); }
  }

  EXPECT_EQ(r, c.expect) << "wrong result code";

  // At decode there is no diagnostics parameter, by design, so the reason is
  // only checkable for a refusal at load.
  if (c.stage == AT_LOAD && c.reason) {
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

void run_all(const std::vector<Case> & cases) {
  for (const Case & c : cases) { run_case(c); }
}

} // namespace

TEST(Corrupt, TheBmpLoaderNamesTheHeaderFieldItRefused) {
  std::vector<Case> cases;

  {
    // BI_BITFIELDS with a red mask of zero. The masks live in the three words
    // after a plain BITMAPINFOHEADER, where the palette would otherwise be.
    BmpSpec s;
    s.bpp = 32u;
    s.compression = 3u;
    Bytes masks;
    u32le(masks, 0x00000000u);  // Red: empty, which is what is refused.
    u32le(masks, 0x0000FF00u);
    u32le(masks, 0x000000FFu);
    s.after_header = masks;
    s.pixels_at = masks.size();
    cases.push_back({"a bitfields red mask of zero", "empty color channel mask",
        GIMG_ERR_CORRUPT, make_bmp(s)});
  }
  {
    // A row stride times a height that does not fit a size_t. Sixty-four bits
    // per pixel is what gets there: the largest biWidth is INT32_MAX and the
    // largest biHeight is 2^31, and at 32bpp their product lands just under
    // SIZE_MAX rather than over it.
    //
    // The neighbouring check on width times *height* - "pixel count
    // overflows", the one before any allocation - cannot be reached at all on
    // a 64-bit size_t for the same arithmetic reason, and has no case here.
    BmpSpec s;
    s.width = 0x7FFFFFFF;
    s.height = -0x7FFFFFFF - 1;  // Read back as a top-down height of 2^31.
    s.bpp = 64u;
    cases.push_back({"a stride times a height that overflows",
        "pixel data size overflows", GIMG_ERR_LIMIT, make_bmp(s)});
  }
  {
    // A negative biWidth reads back above INT32_MAX.
    BmpSpec s;
    s.width = -4;
    cases.push_back({"a negative width", "negative width", GIMG_ERR_CORRUPT,
        make_bmp(s)});
  }
  {
    BmpSpec s;
    s.height = 0;
    cases.push_back({"a height of zero", "zero width or height",
        GIMG_ERR_CORRUPT, make_bmp(s)});
  }
  {
    // A cap one pixel below what the header declares.
    BmpSpec s;
    s.width = 4;
    s.height = 4;
    s.after_header = Bytes(4u * 4u * 3u, 0u);
    Case c{"more pixels than max_decoded_pixels allows",
        "increase max_decoded_pixels", GIMG_ERR_LIMIT, make_bmp(s)};
    c.use_limits = true;
    c.limits.max_decoded_pixels = 15u;
    cases.push_back(c);
  }
  {
    // biSizeImage is believed only as far as the file goes.
    BmpSpec s;
    s.width = 64;
    s.height = 64;
    s.after_header = Bytes(16u, 0u);
    cases.push_back({"a header naming more pixels than the file holds",
        "the header names more pixel data than the file holds",
        GIMG_ERR_CORRUPT, make_bmp(s)});
  }

  {
    // Run-length data has no length the header can state: it runs from
    // bfOffBits to the end of the file, so a stream that cannot say where its
    // end is cannot supply it.
    BmpSpec s;
    s.width = 2;
    s.height = 1;
    s.bpp = 8u;
    s.compression = 1u;  // BI_RLE8.
    s.clr_used = 2u;
    Bytes tail(8u, 0u);  // Two palette entries.
    s.pixels_at = tail.size();
    u8(tail, 2u);
    u8(tail, 1u);
    u8(tail, 0u);
    u8(tail, 1u);
    s.after_header = tail;
    Case c{"run-length data read from something that cannot be measured",
        "this compression requires a sized stream", GIMG_ERR_UNSUPPORTED,
        make_bmp(s)};
    c.no_seek = true;
    cases.push_back(c);
  }

  run_all(cases);
}

TEST(Corrupt, TheBmpRleDecoderRefusesAnIndexThePaletteLacks) {
  // A run-length run names a palette entry by number, and the number is in
  // the compressed stream rather than the header, so it is only checkable
  // while decoding. This file's palette has two entries and its one run asks
  // for the sixth.
  BmpSpec s;
  s.width = 2;
  s.height = 1;
  s.bpp = 8u;
  s.compression = 1u;  // BI_RLE8.
  s.clr_used = 2u;
  Bytes tail;
  for (int i = 0; i < 2; i++) {
    u8(tail, 0u); u8(tail, 0u); u8(tail, (uint32_t)(i * 255)); u8(tail, 0u);
  }
  s.pixels_at = tail.size();
  u8(tail, 2u);  // A run of two...
  u8(tail, 5u);  // ...of an index the two-entry palette has no room for.
  u8(tail, 0u);
  u8(tail, 1u);  // End of bitmap.
  s.after_header = tail;

  Case c{"an RLE run naming a palette entry that is not there", nullptr,
      GIMG_ERR_CORRUPT, make_bmp(s), AT_DECODE};
  run_case(c);
}

TEST(Corrupt, TheBmpHuffmanDecoderSurvivesAStreamThatIsNotOne) {
  // Huffman 1D is an OS/2 2.x compression: a 64-byte header, one bit per
  // pixel, biCompression 3 in that header's vocabulary. Its bit stream has no
  // length field and no structure a header can check, so everything about it
  // is decided while decoding.
  struct Huff {
    const char * name;
    Bytes stream;
    GIMG_Result expect;
  };
  const Huff streams[] = {
      // A white terminating code for a run of 63 (0x34, eight bits) on a line
      // eight pixels wide. T.4 allows the run; the line has nowhere to put
      // fifty-five of it, so it is clipped rather than refused - the same
      // judgment the run-length decoder makes.
      {"a run longer than the line it is on", Bytes{0x34u}, GIMG_OK},
      // Sixty-five zero bits and then nothing. The end-of-line scanner gives
      // up after sixty-four rather than reading to the end of the file, and
      // what is left decodes as no code at all.
      {"a stream of nothing but zero bits", Bytes(9u, 0x00u), GIMG_ERR_CORRUPT},
  };

  for (const Huff & h : streams) {
    SCOPED_TRACE(h.name);
    BmpSpec s;
    s.header_size = 64u;  // BITMAPINFOHEADER2.
    s.width = 8;
    s.height = 1;
    s.bpp = 1u;
    s.compression = 3u;  // Huffman 1D, in the OS/2 2.x vocabulary.
    s.clr_used = 2u;
    Bytes tail;
    for (int i = 0; i < 2; i++) {
      u8(tail, (uint32_t)(i * 255)); u8(tail, (uint32_t)(i * 255));
      u8(tail, (uint32_t)(i * 255)); u8(tail, 0u);
    }
    s.pixels_at = tail.size();
    append(tail, h.stream);
    s.after_header = tail;

    // A Huffman stream is expanded during the load rather than on the far
    // side of one, so a refusal arrives with a diagnostic naming it.
    Case c{h.name, "the Huffman 1D stream could not be decoded", h.expect,
        make_bmp(s)};
    if (h.expect != GIMG_OK) { run_case(c); continue; }
    // A case that decodes has to be asked for its pixels rather than only for
    // its result code, or nothing distinguishes it from one that refused.
    GIMG_Stream * st = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(c.bytes.data(), c.bytes.size(), &st),
        GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(st, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    EXPECT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
        GIMG_OK);
    EXPECT_NE(raster, nullptr);
    if (raster) {
      EXPECT_EQ(gimg_raster_width(raster), 8u);
      gimg_raster_destroy(raster);
    }
    gimg_doc_destroy(doc);
    gimg_stream_destroy(st);
  }
}

TEST(Corrupt, TheBmpArrayWalkerNamesWhatBrokeTheChain) {
  std::vector<Case> cases;

  // A well-formed entry to point the chain at, so that only the thing under
  // test is wrong.
  BmpSpec good;
  good.width = 1;
  good.height = 1;
  good.after_header = Bytes(4u, 0u);
  const Bytes entry = make_bmp(good);

  {
    // offNext points at bytes that are not another array header.
    Bytes b = bmp_array_header((uint32_t)entry.size(), 14u);
    append(b, entry);
    cases.push_back({"a chain pointing at something that is not 'BA'",
        "bitmap array entry lacks 'BA'", GIMG_ERR_CORRUPT, b});
  }
  {
    // The second header's offNext points at itself, so the walk cannot move.
    Bytes b = bmp_array_header(0u, 14u);
    append(b, bmp_array_header(0u, 14u));
    append(b, entry);
    cases.push_back({"a chain that points at itself",
        "bitmap array chain does not advance", GIMG_ERR_CORRUPT, b});
  }
  {
    // Two headers back to back: the first entry has no bytes between them.
    Bytes b = bmp_array_header(0u, 14u);
    append(b, bmp_array_header(0u, 0u));
    append(b, entry);
    cases.push_back({"an entry with no bytes in it",
        "bitmap array entry is empty", GIMG_ERR_CORRUPT, b});
  }
  {
    // An entry whose own first bytes are 'BA'. Nesting is refused rather than
    // recursed into: the GIF fuzz harness reached this through the shared
    // magic probe as a stack overflow.
    Bytes inner = bmp_array_header(0u, 0u);
    append(inner, entry);
    Bytes b = bmp_array_header(0u, 0u);
    append(b, inner);
    cases.push_back({"an array holding another array",
        "bitmap array entry is another array", GIMG_ERR_CORRUPT, b});
  }
  {
    // A header whose offNext lands past the end of the file.
    Bytes b = bmp_array_header(0u, 4096u);
    append(b, entry);
    cases.push_back({"a chain pointing past the end of the file",
        "bitmap array header past the end", GIMG_ERR_CORRUPT, b});
  }
  {
    // An entry that is a bitmap the loader refuses. The wrapper says an entry
    // failed; the entry's own diagnostic says why, and both are present.
    BmpSpec bad;
    bad.height = 0;
    Bytes b = bmp_array_header(0u, 0u);
    append(b, make_bmp(bad));
    cases.push_back({"an entry the loader refuses",
        "a bitmap array entry did not load", GIMG_ERR_CORRUPT, b});
  }

  {
    // The array chain is walked by absolute offset, which is the same
    // requirement, for the same reason.
    Bytes b = bmp_array_header(0u, 0u);
    append(b, entry);
    Case c{"an array read from something that cannot be measured",
        "a bitmap array requires a sized stream", GIMG_ERR_UNSUPPORTED, b};
    c.no_seek = true;
    cases.push_back(c);
  }

  run_all(cases);
}

TEST(Corrupt, TheBmpV5ProfileFieldsAreCheckedBeforeTheyAreBelieved) {
  // bV5ProfileData and bV5ProfileSize are two attacker-controlled words that
  // name a range inside the file. A profile that does not fit is dropped and
  // the image still decodes - the picture is not wrong just because the tag
  // is - but a profile past a cap the caller set is reported, because the
  // caller asked to be told.
  //
  // Offsets are from the start of the DIB header: 56 is bV5CSType, 112
  // bV5ProfileData and 116 bV5ProfileSize.
  const uint32_t kCsType = 56u, kProfileData = 112u, kProfileSize = 116u;
  const uint32_t kMbed = 0x4D424544u;  // 'MBED'
  const uint32_t kLink = 0x4C494E4Bu;  // 'LINK'

  struct Profile {
    const char * name;
    uint32_t cs_type;
    uint32_t offset;
    uint32_t size;
    bool cap_memory;
    GIMG_Result expect;
  };
  const Profile profiles[] = {
      {"an embedded profile running past the end of the file", kMbed, 200u,
          4096u, false, GIMG_OK},
      {"an embedded profile bigger than the codec accepts", kMbed, 0u,
          8u * 1024u * 1024u, false, GIMG_OK},
      {"an embedded profile bigger than max_memory", kMbed, 0u, 64u, true,
          GIMG_ERR_LIMIT},
      {"a linked path longer than a path can be", kLink, 0u, 8192u, false,
          GIMG_OK},
      {"a linked path running past the end of the file", kLink, 200u, 64u,
          false, GIMG_OK},
  };

  for (const Profile & pr : profiles) {
    SCOPED_TRACE(pr.name);
    BmpSpec s;
    s.header_size = 124u;
    s.width = 2;
    s.height = 2;
    s.after_header = Bytes(2u * 3u * 2u + 128u, 0x40u);
    s.dib_words = {{kCsType, pr.cs_type}, {kProfileData, pr.offset},
        {kProfileSize, pr.size}};
    const Bytes bytes = make_bmp(s);

    GIMG_Stream * st = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &st),
        GIMG_OK);
    GIMG_Limits limits = {};
    limits.max_memory = 32u;
    GIMG_Load_Options opts = {};
    if (pr.cap_memory) { opts.limits = &limits; }
    GIMG_Doc * doc = nullptr;
    const GIMG_Result r =
        gimg_doc_load(st, pr.cap_memory ? &opts : nullptr, nullptr, &doc);
    EXPECT_EQ(r, pr.expect);
    if (doc) { gimg_doc_destroy(doc); }
    gimg_stream_destroy(st);
  }
}

TEST(Corrupt, TheGifLoaderNamesTheBlockItRefused) {
  std::vector<Case> cases;

  {
    // A graphic control extension declaring a length other than four.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0xF9u);
    u8(b, 5u);  // Every writer puts 4 here.
    for (int i = 0; i < 5; i++) { u8(b, 0u); }
    u8(b, 0u);
    u8(b, 0x3Bu);
    cases.push_back({"a graphic control extension of the wrong length",
        "graphic control extension is not four bytes", GIMG_ERR_CORRUPT, b});
  }
  {
    // The same block, the right length, without its terminator.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0xF9u);
    u8(b, 4u);
    for (int i = 0; i < 4; i++) { u8(b, 0u); }
    u8(b, 0x07u);  // Not the zero that ends the block.
    u8(b, 0x3Bu);
    cases.push_back({"a graphic control extension left unterminated",
        "graphic control extension is not terminated", GIMG_ERR_CORRUPT, b});
  }
  {
    // An application extension whose identifier block is cut short.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0xFFu);
    u8(b, 7u);  // Not the eleven a NETSCAPE block has, so it is skipped...
    u8(b, 'X');  // ...but only three of the seven bytes are there.
    u8(b, 'Y');
    u8(b, 'Z');
    cases.push_back({"an application extension cut off in its identifier",
        "truncated application identifier", GIMG_ERR_CORRUPT, b});
  }
  {
    // An LZW minimum code size the format does not define.
    Bytes b = gif_head(2u, 2u);
    append(b, gif_image_descriptor(0u, 0u, 2u, 2u));
    u8(b, 1u);  // Two is the smallest 89a 22 allows.
    u8(b, 0u);
    u8(b, 0x3Bu);
    cases.push_back({"an LZW code size below the minimum",
        "LZW minimum code size out of range", GIMG_ERR_CORRUPT, b});
  }
  {
    // A frame of no area.
    Bytes b = gif_head(2u, 2u);
    append(b, gif_image_descriptor(0u, 0u, 0u, 2u));
    u8(b, 2u);
    u8(b, 0u);
    u8(b, 0x3Bu);
    cases.push_back({"a frame of zero width", "image has zero width or height",
        GIMG_ERR_CORRUPT, b});
  }
  {
    // A file with nothing in it but a screen descriptor.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x3Bu);
    cases.push_back({"a file with no image blocks",
        "no image blocks in the file", GIMG_ERR_CORRUPT, b});
  }
  {
    // A block introducer that is neither an image, an extension nor the
    // trailer.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x7Fu);
    cases.push_back({"an unknown block introducer",
        "unrecognized block introducer", GIMG_ERR_CORRUPT, b});
  }
  {
    // A sub-block chain longer than the caller's chunk cap.
    Bytes b = gif_head(2u, 2u);
    append(b, gif_image_descriptor(0u, 0u, 2u, 2u));
    u8(b, 2u);
    append(b, gif_sub_blocks(Bytes(600u, 0u)));
    u8(b, 0x3Bu);
    Case c{"an LZW chain past max_chunk_size",
        "sub-block chain exceeds max_chunk_size", GIMG_ERR_LIMIT, b};
    c.use_limits = true;
    c.limits.max_chunk_size = 256u;
    cases.push_back(c);
  }
  {
    // More frames than the caller allowed.
    Bytes b = gif_head(2u, 2u);
    for (int i = 0; i < 3; i++) {
      append(b, gif_image_descriptor(0u, 0u, 2u, 2u));
      u8(b, 2u);
      append(b, gif_sub_blocks(Bytes(4u, 0x44u)));
    }
    u8(b, 0x3Bu);
    Case c{"more frames than max_frame_count", "increase max_frame_count",
        GIMG_ERR_LIMIT, b};
    c.use_limits = true;
    c.limits.max_frame_count = 2u;
    cases.push_back(c);
  }
  {
    // A Plain Text extension whose sub-block chain is cut off. Nothing
    // renders Plain Text, so its blocks are walked past rather than kept -
    // and walking past still has to notice that the file ended.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0x01u);
    u8(b, 200u);  // The chain says two hundred bytes follow.
    for (int i = 0; i < 5; i++) { u8(b, 0u); }
    cases.push_back({"an extension walked past that ends early",
        "truncated inside a skipped sub-block", GIMG_ERR_CORRUPT, b});
  }
  {
    // A frame larger than the caller's pixel cap.
    Bytes b = gif_head(64u, 64u);
    append(b, gif_image_descriptor(0u, 0u, 64u, 64u));
    u8(b, 2u);
    append(b, gif_sub_blocks(Bytes(8u, 0x44u)));
    u8(b, 0x3Bu);
    Case c{"a frame past max_decoded_pixels", "increase max_decoded_pixels",
        GIMG_ERR_LIMIT, b};
    c.use_limits = true;
    c.limits.max_decoded_pixels = 64u * 64u - 1u;
    cases.push_back(c);
  }

  run_all(cases);
}

TEST(Corrupt, TheGifDecoderRefusesAnLzwStreamThatRunsOut) {
  std::vector<Case> cases;

  {
    // An image block whose sub-block chain is empty: the frame loads, and
    // there is nothing to expand.
    Bytes b = gif_head(2u, 2u);
    append(b, gif_image_descriptor(0u, 0u, 2u, 2u));
    u8(b, 2u);
    u8(b, 0u);  // The chain terminator, with no data before it.
    u8(b, 0x3Bu);
    cases.push_back({"a frame carrying no LZW data at all", nullptr,
        GIMG_ERR_CORRUPT, b, AT_DECODE});
  }
  {
    // A chain that carries a clear code and then stops, which is fewer codes
    // than the frame has pixels.
    Bytes b = gif_head(2u, 2u);
    append(b, gif_image_descriptor(0u, 0u, 2u, 2u));
    u8(b, 2u);
    append(b, gif_sub_blocks(Bytes{0x04u}));
    u8(b, 0x3Bu);
    cases.push_back({"an LZW stream that ends before the pixels do", nullptr,
        GIMG_ERR_CORRUPT, b, AT_DECODE});
  }

  run_all(cases);
}

TEST(Corrupt, TheGifLoaderKeepsWhatItCanOfAnOddBlock) {
  // These files are not corrupt - they are legal and unusual, which is the
  // other half of the same gap. Nothing in the corpus carries ANIMEXTS1.0,
  // an application block that is not eleven bytes of identifier, or a comment
  // with a byte in it that is not text, so the arms that handle them are
  // reached by nothing.
  struct Odd {
    const char * name;
    Bytes bytes;
    bool expect_loop;
    uint32_t loop_count;
    const char * expect_description;  ///< NULL = none should be set.
  };

  // A minimal frame that decodes, so the loader has an image to attach
  // whatever the extension said to.
  Bytes frame = gif_image_descriptor(0u, 0u, 2u, 2u);
  u8(frame, 2u);
  append(frame, gif_sub_blocks(Bytes{0x44u, 0x06u, 0x05u}));

  std::vector<Odd> odds;
  {
    // ANIMEXTS1.0 is the other spelling of the NETSCAPE loop block, and it is
    // read the same way.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0xFFu);
    u8(b, 11u);
    const char * ident = "ANIMEXTS1.0";
    b.insert(b.end(), ident, ident + 11);
    u8(b, 3u);
    u8(b, 1u);
    u16le(b, 7u);
    u8(b, 0u);
    append(b, frame);
    u8(b, 0x3Bu);
    odds.push_back({"an ANIMEXTS1.0 loop block", b, true, 7u, nullptr});
  }
  {
    // An application block whose identifier is not eleven bytes is not one
    // this codec knows; its body is a sub-block chain like any other, so it
    // is walked past.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0xFFu);
    u8(b, 4u);
    const char * ident = "WHAT";
    b.insert(b.end(), ident, ident + 4);
    append(b, gif_sub_blocks(Bytes{1u, 2u, 3u}));
    append(b, frame);
    u8(b, 0x3Bu);
    odds.push_back({"an application block of an odd length", b, false, 0u,
        nullptr});
  }
  {
    // A comment stops at its first NUL: the bytes past it are a writer's
    // padding, not part of the text.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0xFEu);
    append(b, gif_sub_blocks(Bytes{'h', 'i', 0u, 'x', 'x'}));
    append(b, frame);
    u8(b, 0x3Bu);
    odds.push_back({"a comment with a NUL in it", b, false, 0u, "hi"});
  }
  {
    // A comment carrying a byte 89a 24 does not allow is kept as a raw block
    // but is not presented as a description.
    Bytes b = gif_head(2u, 2u);
    u8(b, 0x21u);
    u8(b, 0xFEu);
    append(b, gif_sub_blocks(Bytes{'a', 0x01u, 'b'}));
    append(b, frame);
    u8(b, 0x3Bu);
    odds.push_back({"a comment that is not text", b, false, 0u, nullptr});
  }

  for (const Odd & o : odds) {
    SCOPED_TRACE(o.name);
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(o.bytes.data(), o.bytes.size(), &s),
        GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    ASSERT_NE(doc, nullptr);

    uint32_t loops = 0;
    const bool stated = gimg_doc_loop_count(doc, &loops) != 0;
    EXPECT_EQ(stated, o.expect_loop);
    if (o.expect_loop) { EXPECT_EQ(loops, o.loop_count); }

    GIMG_Meta_Common * common = gimg_doc_meta_common(doc);
    const char * description =
        common ? gimg_meta_common_description(common) : nullptr;
    if (o.expect_description) {
      ASSERT_NE(description, nullptr) << "the comment should be the description";
      EXPECT_STREQ(description, o.expect_description);
    }
    else {
      EXPECT_EQ(description, nullptr);
    }

    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

TEST(Corrupt, ThePngLoaderNamesTheChunkRuleItRefused) {
  std::vector<Case> cases;

  const Bytes idat = png_chunk("IDAT", zlib_stored(Bytes{0u, 0u, 0u, 0u}));
  const Bytes iend = png_chunk("IEND", Bytes());

  {
    // Colour type 4 carries alpha, so PNG Table 11.1 allows it only at 8 or
    // 16 bits.
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 1u, 4u));
    append(b, idat);
    append(b, iend);
    cases.push_back({"an alpha colour type at one bit deep",
        "a colour type carrying alpha is 8 or 16 bits deep", GIMG_ERR_FORMAT,
        b});
  }
  {
    // A tRNS after the image data it is supposed to precede.
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, idat);
    Bytes trns;
    u16be(trns, 0u);
    append(b, png_chunk("tRNS", trns));
    append(b, iend);
    cases.push_back({"a tRNS after the first IDAT",
        "tRNS must appear once, before the first IDAT", GIMG_ERR_FORMAT, b});
  }
  {
    // A PLTE on a grayscale image.
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("PLTE", Bytes(3u, 0u)));
    append(b, idat);
    append(b, iend);
    cases.push_back({"a PLTE on a grayscale image",
        "PLTE shall not appear for a grayscale image", GIMG_ERR_FORMAT, b});
  }
  {
    // A critical chunk this decoder does not know. PNG 5.4: a decoder may not
    // skip one.
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("CrIt", Bytes(2u, 0u)));
    append(b, idat);
    append(b, iend);
    cases.push_back({"an unknown critical chunk",
        "unknown critical chunk", GIMG_ERR_FORMAT, b});
  }
  {
    // An acTL declaring more frames than the file carries fcTL chunks for.
    Bytes actl;
    u32be(actl, 4u);  // num_frames
    u32be(actl, 0u);  // num_plays
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("acTL", actl));
    append(b, png_chunk("fcTL", png_fctl(0u)));
    append(b, idat);
    append(b, iend);
    cases.push_back({"an acTL promising frames the file does not have",
        "fewer fcTL chunks than the acTL declared frames", GIMG_ERR_FORMAT,
        b});
  }
  {
    // An acTL declaring no frames at all.
    Bytes actl;
    u32be(actl, 0u);
    u32be(actl, 0u);
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("acTL", actl));
    append(b, idat);
    append(b, iend);
    cases.push_back({"an acTL declaring no frames",
        "acTL declares no frames", GIMG_ERR_FORMAT, b});
  }
  {
    // An fdAT with no fcTL before it.
    Bytes fdat;
    u32be(fdat, 1u);
    Bytes actl;
    u32be(actl, 1u);
    u32be(actl, 0u);
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("acTL", actl));
    append(b, idat);
    append(b, png_chunk("fdAT", fdat));
    append(b, iend);
    cases.push_back({"an fdAT before any fcTL", "fdAT before any fcTL",
        GIMG_ERR_FORMAT, b});
  }
  {
    // An APNG whose fcTL count matches its acTL, but whose second frame has
    // no fdAT after it. The first frame is the default image, which IDAT
    // supplies; the second has nothing.
    Bytes actl;
    u32be(actl, 2u);
    u32be(actl, 0u);
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("acTL", actl));
    append(b, png_chunk("fcTL", png_fctl(0u)));
    append(b, idat);
    append(b, png_chunk("fcTL", png_fctl(1u)));
    append(b, iend);
    cases.push_back({"an APNG frame with no data behind it",
        "an APNG frame carries no data", GIMG_ERR_FORMAT, b});
  }
  {
    // An fcTL numbered out of the sequence APNG 4.2 requires.
    Bytes actl;
    u32be(actl, 1u);
    u32be(actl, 0u);
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("acTL", actl));
    append(b, png_chunk("fcTL", png_fctl(7u)));
    append(b, idat);
    append(b, iend);
    cases.push_back({"an fcTL numbered out of order",
        "fcTL sequence number out of order", GIMG_ERR_FORMAT, b});
  }
  {
    // An unknown ancillary chunk too large for the stack buffer the chunk
    // reader uses. Its payload is not wanted, but its CRC still has to be
    // taken over every byte, so the reader takes a buffer from the heap.
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("unKn", Bytes(8192u, 0x5Au)));
    append(b, idat);
    append(b, iend);
    cases.push_back({"an ancillary chunk past the stack buffer", nullptr,
        GIMG_OK, b});
  }
  {
    // An acTL declaring one frame with two fcTL chunks behind it.
    Bytes actl;
    u32be(actl, 1u);
    u32be(actl, 0u);
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("acTL", actl));
    append(b, png_chunk("fcTL", png_fctl(0u)));
    append(b, idat);
    append(b, png_chunk("fcTL", png_fctl(1u)));
    append(b, iend);
    cases.push_back({"more fcTL chunks than the acTL declared",
        "more fcTL chunks than the acTL declared frames", GIMG_ERR_FORMAT, b});
  }
  {
    // An IEND with a payload. Nothing is kept of it, but its CRC is still
    // taken over every byte, and at this size that needs a buffer from the
    // heap rather than the reader's own stack.
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, idat);
    append(b, png_chunk("IEND", Bytes(8192u, 0x11u)));
    cases.push_back({"an IEND carrying eight kilobytes of payload", nullptr,
        GIMG_OK, b});
  }
  {
    // A chunk declaring more payload than the caller allows.
    Bytes b = png_signature();
    append(b, png_ihdr(1u, 1u, 8u, 0u));
    append(b, png_chunk("IDAT", zlib_stored(Bytes(600u, 0u))));
    append(b, iend);
    Case c{"a chunk past max_chunk_size",
        "chunk declares more data than there is", GIMG_ERR_LIMIT, b};
    c.use_limits = true;
    c.limits.max_chunk_size = 64u;
    cases.push_back(c);
  }

  run_all(cases);
}

TEST(Corrupt, ThePngDecoderHonoursAPixelCapSetAtLoadTime) {
  // PNG reads max_decoded_pixels while decoding, not while loading, so the
  // cap is only visible on the far side of a load that succeeded.
  Bytes b = png_signature();
  append(b, png_ihdr(64u, 64u, 8u, 0u));
  append(b, png_chunk("IDAT", zlib_stored(Bytes(8u, 0u))));
  append(b, png_chunk("IEND", Bytes()));

  Case c{"a canvas one pixel over the cap", nullptr, GIMG_ERR_LIMIT, b,
      AT_DECODE};
  c.use_limits = true;
  c.limits.max_decoded_pixels = 64u * 64u - 1u;
  run_case(c);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
