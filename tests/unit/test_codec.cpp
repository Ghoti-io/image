/**
 * @file
 *
 * Unit tests for codec registry and probing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <cstdio>
#include <string>
#include <vector>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

#include "../../src/codec/codec_internal.h"

TEST(Codec, RegisterAndList) {
  GIMG_Codec * stub = nullptr;
  GIMG_Result r = gimg_codec_create_stub("stub", nullptr, 0, &stub);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(stub, nullptr);
  r = gimg_codec_register(stub);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GE(gimg_codec_count(), 1u);
  GIMG_Codec * found = gimg_codec_by_name("stub");
  EXPECT_NE(found, nullptr);
  EXPECT_STREQ(gimg_codec_name(found), "stub");
}

TEST(Codec, ProbeEmptyStream) {
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(nullptr, 0, &s);
  GIMG_Probe_Result result = {};
  GIMG_Result r = gimg_probe(s, &result);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(result.confidence, 0u);
  gimg_stream_destroy(s);
}

namespace {

/** Declines every claim so a weak magic cannot steal a probe. */
int probe_always_decline(GIMG_Stream *, const unsigned char *, size_t) {
  return 0;
}

} // namespace

TEST(Codec, ProbeCallbackCanDeclineAMagicMatch) {
  // A magic that would otherwise claim every file starting with 0xFE, which
  // no real codec uses. With probe_cb declining, gimg_probe must keep looking
  // and leave confidence at zero.
  static const unsigned char weak[] = {0xFE};
  GIMG_Codec * stub = nullptr;
  ASSERT_EQ(gimg_codec_create_stub("weak-probe", weak, sizeof(weak), &stub),
      GIMG_OK);
  gimg_codec_set_probe_cb(stub, probe_always_decline);
  ASSERT_EQ(gimg_codec_register(stub), GIMG_OK);

  unsigned char buf[] = {0xFE, 0x00, 0x00, 0x00};
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(buf, sizeof(buf), &s), GIMG_OK);
  GIMG_Probe_Result result = {};
  ASSERT_EQ(gimg_probe(s, &result), GIMG_OK);
  EXPECT_EQ(result.confidence, 0u);
  EXPECT_EQ(result.format_name, nullptr);
  gimg_stream_destroy(s);
}

TEST(Codec, ProbeJpegReturnsJpeg) {
  unsigned char buf[] = {0xFF, 0xD8};
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(buf, sizeof(buf), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);
  GIMG_Probe_Result result = {};
  r = gimg_probe(s, &result);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_STREQ(result.format_name, "jpeg");
  EXPECT_EQ(result.confidence, 100u);
  gimg_stream_destroy(s);
}

TEST(Codec, DocLoadReturnsUnsupported) {
  unsigned char buf[1] = {0};
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf, 1, &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(Codec, ItemDecodeReturnsUnsupported) {
  GIMG_Doc * doc = nullptr;
  gimg_doc_create(&doc);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  GIMG_Raster * raster = nullptr;
  GIMG_Result r = gimg_item_decode(item, nullptr, &raster);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(raster, nullptr);
  gimg_doc_destroy(doc);
}


/**
 * A document carries codec_private state belonging to whichever codec loaded
 * it, and the type of that state is that codec's.  Saving through a different
 * codec must not reach into it.
 *
 * png_save cast doc->codec_private to its own state unconditionally, so a JPEG
 * re-saved as PNG walked a JPEG structure as a PNG one and read through a wild
 * pointer.  Every load/save pair is exercised here; the requirement is that
 * none of them crash or corrupt, whatever they return.
 */
TEST(Codec, SaveThroughADifferentCodecDoesNotTouchForeignState) {
  struct Sample {
    const char * path;
    const char * name;
  };
  const std::string jpeg_dir(GIMG_TEST_DATA_JPEG);
  const std::string png_dir(GIMG_TEST_DATA_PNG);
  const std::vector<std::string> inputs = {
      jpeg_dir + "/baseline_16x16_ycbcr.jpg",
      jpeg_dir + "/progressive_sample.jpg",
      png_dir + "/png_2x2_gray.png",
  };
  for (const std::string & path : inputs) {
    std::vector<uint8_t> bytes;
    {
      FILE * f = fopen(path.c_str(), "rb");
      if (!f) {
        continue;  // fixture not generated; other inputs still cover the case
      }
      fseek(f, 0, SEEK_END);
      long n = ftell(f);
      fseek(f, 0, SEEK_SET);
      bytes.resize((size_t)(n > 0 ? n : 0));
      size_t got = fread(bytes.data(), 1, bytes.size(), f);
      (void)got;
      fclose(f);
    }
    if (bytes.empty()) {
      continue;
    }
    for (const char * fmt : {"png", "jpeg", "bmp"}) {
      GIMG_Stream * in = nullptr;
      ASSERT_EQ(gimg_stream_create_memory(bytes.data(), bytes.size(), &in),
          GIMG_OK);
      GIMG_Doc * doc = nullptr;
      if (gimg_doc_load(in, nullptr, nullptr, &doc) != GIMG_OK) {
        gimg_stream_destroy(in);
        continue;
      }
      GIMG_Stream * out = nullptr;
      ASSERT_EQ(gimg_stream_create_memory_output(&out), GIMG_OK);
      GIMG_Save_Options opts = {};
      opts.metadata_policy = GIMG_META_PRESERVE_ALL;
      opts.jpeg_quality = 85;
      GIMG_Save_Report report = {};
      // The return value is not the point - some pairs are legitimately
      // unsupported.  Reaching this line without a crash is.
      (void)gimg_doc_save(doc, out, fmt, &opts, &report);
      gimg_stream_destroy(out);
      gimg_doc_destroy(doc);
      gimg_stream_destroy(in);
    }
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
