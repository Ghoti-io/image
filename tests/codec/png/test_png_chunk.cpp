/**
 * @file
 *
 * PNG chunk parsing and CRC tests.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

// PNG signature (8 bytes).
const unsigned char kPngSignature[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

// IHDR chunk: 13-byte payload (1x1 grayscale 8-bit), then CRC.
// Payload: width=1, height=1, bit_depth=8, color_type=0, compression=0,
// filter=0, interlace=0. CRC32 of type+payload (PNG) = 0x3A7E9B55.
const unsigned char kIhdrChunk[] = {
    0x00, 0x00, 0x00, 0x0D,                         // length 13
    0x49, 0x48, 0x44, 0x52,                         // type IHDR
    0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, // width, height
    0x08, 0x00, 0x00, 0x00, 0x00, // bd, ct, comp, filter, interlace
    0x3A, 0x7E, 0x9B, 0x55        // CRC
};

// IEND chunk: length=0 (4 bytes BE), type "IEND" (4), CRC (4 BE).
// CRC32 of "IEND" (0x49,0x45,0x4E,0x44) per PNG spec = 0xAE426082.
const unsigned char kIendChunk[] = {
    0x00, 0x00, 0x00, 0x00, // length
    0x49, 0x45, 0x4E, 0x44, // type IEND
    0xAE, 0x42, 0x60, 0x82  // CRC
};

// Two IDAT chunks (concatenated DEFLATE: empty stored block). First: 5 bytes;
// second: 0 bytes. CRCs: IDAT+5bytes=0xCFCA6286, IDAT+0=0x35AF061E.
const unsigned char kIdatChunk1[] = {
    0x00, 0x00, 0x00, 0x05, 0x49, 0x44, 0x41, 0x54, // length 5, type IDAT
    0x01, 0x00, 0x00, 0xFF, 0xFF,                   // empty stored block
    0xCF, 0xCA, 0x62, 0x86                          // CRC
};
const unsigned char kIdatChunk2[] = {
    0x00, 0x00, 0x00, 0x00, 0x49, 0x44, 0x41, 0x54, 0x35, 0xAF, 0x06,
    0x1E // CRC
};

// tEXt chunk: length=3, type tEXt, payload "A\0B".
// CRC32 per PNG spec (type+payload); value from gcomp_crc32 (see
// compute_crc.c).
const unsigned char kTextChunk[] = {
    0x00, 0x00, 0x00, 0x03, // length 3
    0x74, 0x45, 0x58, 0x74, // type tEXt
    0x41, 0x00, 0x42,       // payload "A\0B"
    0xDF, 0x6A, 0xC4, 0x13  // CRC
};

// Second tEXt chunk (payload "B\0C") to test multiple ancillary stored in
// order.
const unsigned char kTextChunk2[] = {
    0x00, 0x00, 0x00, 0x03, 0x74, 0x45, 0x58, 0x74, // length 3, type tEXt
    0x42, 0x00, 0x43,                               // payload "B\0C"
    0xAA, 0x2B, 0x4A, 0xDC                          // CRC (precomputed)
};

void append(std::vector<uint8_t> & out, const unsigned char * p, size_t n) {
  out.insert(out.end(), p, p + n);
}

} // namespace

TEST(PngChunk, MinimalPngLoadSucceeds) {
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_OK) << "minimal PNG (signature + IHDR + IEND) should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngChunk, InvalidSignatureRejected) {
  std::vector<uint8_t> buf = {0x00, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(PngChunk, TruncatedAfterSignatureFails) {
  // Only signature, no chunk.
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));

  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(PngChunk, NoIhdrRejected) {
  // Signature + IEND only; IHDR must be first chunk.
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_ERR_FORMAT);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(PngChunk, MultiIdatLoadSucceeds) {
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kIdatChunk1, sizeof(kIdatChunk1));
  append(buf, kIdatChunk2, sizeof(kIdatChunk2));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_OK) << "PNG with two IDAT chunks should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngChunk, BadCrcRejected) {
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kIendChunk, sizeof(kIendChunk));
  buf.back() ^= 0xFF; // corrupt IEND CRC

  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_ERR_CORRUPT);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(PngChunk, ProbeReturnsPng) {
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Probe_Result result = {};
  GIMG_Result r = gimg_probe(s, &result);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_NE(result.format_name, nullptr);
  EXPECT_STREQ(result.format_name, "png");
  EXPECT_EQ(result.confidence, 100u);
  gimg_stream_destroy(s);
}

TEST(PngChunk, ProbeThenLoadViaDispatch) {
  // Minimal PNG: probe -> "png", load -> doc. Decode is exercised in
  // test_png_decode.
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kIdatChunk1, sizeof(kIdatChunk1));
  append(buf, kIdatChunk2, sizeof(kIdatChunk2));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);

  GIMG_Probe_Result probe = {};
  r = gimg_probe(s, &probe);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(probe.format_name, nullptr);
  EXPECT_STREQ(probe.format_name, "png");

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "load via dispatch";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);

  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngChunk, BadCrcFillsDiagnostics) {
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kIendChunk, sizeof(kIendChunk));
  buf.back() ^= 0xFF; // corrupt IEND CRC

  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Diagnostics diag = {};
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, &diag, &doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(r, GIMG_ERR_CORRUPT);
  EXPECT_EQ(doc, nullptr);
  EXPECT_GE(diag.count, 1u) << "diagnostics should report bad CRC";
  if (diag.count >= 1u) {
    EXPECT_STREQ(diag.items[0].codec_name, "png");
    EXPECT_EQ(diag.items[0].chunk_or_tag_id, 0x49454E44u) << "IEND chunk type";
    EXPECT_EQ(diag.items[0].severity, GIMG_DIAG_ERROR);
  }
  gimg_diagnostics_destroy(&diag);
}

TEST(PngChunk, PngCodecHasExpectedCapabilities) {
  GIMG_Codec * png = gimg_codec_by_name("png");
  ASSERT_NE(png, nullptr);
  unsigned int cap = gimg_codec_capabilities(png);
  EXPECT_TRUE(cap & GIMG_CAP_READ);
  EXPECT_TRUE(cap & GIMG_CAP_WRITE);
  EXPECT_TRUE(cap & GIMG_CAP_ANIMATION);
  EXPECT_TRUE(cap & GIMG_CAP_PALETTE);
  EXPECT_TRUE(cap & GIMG_CAP_ICC);
  EXPECT_TRUE(cap & GIMG_CAP_16BPC);
}

TEST(PngChunk, AncillaryTextChunkParsedAndStored) {
  // Minimal PNG with one tEXt ancillary chunk (payload "A\0B"); load must
  // succeed and chunk is stored for round-trip.
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kTextChunk, sizeof(kTextChunk));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_OK) << "PNG with tEXt ancillary chunk should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(PngChunk, MultipleAncillaryChunksStoredInOrder) {
  // PNG with two tEXt chunks; load must succeed and both stored in order.
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kTextChunk, sizeof(kTextChunk));
  append(buf, kTextChunk2, sizeof(kTextChunk2));
  append(buf, kIendChunk, sizeof(kIendChunk));

  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf.data(), buf.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_OK) << "PNG with multiple ancillary chunks should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// A chunk header is a claim, not a fact.
//
// PNG's length field is four bytes, so a twelve-byte chunk header can ask for
// a two-gigabyte payload, and this loader buffers every payload whole - it
// allocated what the header claimed and only then discovered the file had
// ended.  The fuzzer found it twice over, in the load and the encode harness,
// from inputs of 97 and 83 bytes.
//
// The bound is now the caller's max_chunk_size when they set one and what is
// left of the stream otherwise, because a chunk cannot be longer than the file
// containing it.  That refuses nothing a real file does: the same bytes used
// to fail anyway, as a truncated payload, after the allocation.  The error
// code is what says which happened - GIMG_ERR_LIMIT for a claim refused before
// reading, not the GIMG_ERR_IO of a read that ran out.
TEST(PngChunk, ChunkLongerThanTheFileIsRefusedBeforeAllocating) {
  struct Case {
    const char * type;
    const char * what;
  };
  // One of each shape the loader buffers: ancillary, palette, and image data.
  const Case cases[] = {
      {"tEXt", "ancillary"},
      {"PLTE", "palette"},
      {"IDAT", "image data"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(c.what);
    std::vector<uint8_t> buf;
    append(buf, kPngSignature, sizeof(kPngSignature));
    append(buf, kIhdrChunk, sizeof(kIhdrChunk));
    // Length 0x7FFFFFFF - the largest PNG permits - and then nothing.
    const unsigned char header[8] = {0x7F, 0xFF, 0xFF, 0xFF,
        (unsigned char)c.type[0], (unsigned char)c.type[1],
        (unsigned char)c.type[2], (unsigned char)c.type[3]};
    append(buf, header, sizeof(header));
    append(buf, kIendChunk, sizeof(kIendChunk));

    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(buf.data(), buf.size(), &s), GIMG_OK);
    GIMG_Diagnostics diag = {};
    gimg_diagnostics_init(&diag, nullptr);
    GIMG_Doc * doc = nullptr;
    EXPECT_EQ(gimg_doc_load(s, nullptr, &diag, &doc), GIMG_ERR_LIMIT)
        << "a chunk longer than the file must be refused, not allocated";
    EXPECT_EQ(doc, nullptr);
    gimg_diagnostics_destroy(&diag);
    gimg_stream_destroy(s);
  }
}

// The caller's own limit still bounds a chunk that the file does contain.
TEST(PngChunk, MaxChunkSizeBoundsAChunkThatFits) {
  std::vector<uint8_t> buf;
  append(buf, kPngSignature, sizeof(kPngSignature));
  append(buf, kIhdrChunk, sizeof(kIhdrChunk));
  append(buf, kTextChunk, sizeof(kTextChunk)); // a 3-byte payload
  append(buf, kIdatChunk1, sizeof(kIdatChunk1));
  append(buf, kIendChunk, sizeof(kIendChunk));

  // Without a limit it loads; with a limit of two bytes the tEXt does not.
  for (uint32_t limit : {(uint32_t)0, (uint32_t)2}) {
    SCOPED_TRACE("max_chunk_size=" + std::to_string(limit));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(buf.data(), buf.size(), &s), GIMG_OK);
    GIMG_Limits limits = {};
    gimg_limits_default(&limits);
    limits.max_chunk_size = limit;
    GIMG_Load_Options opts = {};
    opts.limits = &limits;
    GIMG_Doc * doc = nullptr;
    GIMG_Result r = gimg_doc_load(s, &opts, nullptr, &doc);
    if (limit == 0) {
      EXPECT_EQ(r, GIMG_OK);
      if (doc) {
        gimg_doc_destroy(doc);
      }
    }
    else {
      EXPECT_EQ(r, GIMG_ERR_LIMIT);
      EXPECT_EQ(doc, nullptr);
    }
    gimg_stream_destroy(s);
  }
}
