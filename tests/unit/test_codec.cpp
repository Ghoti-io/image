/**
 * @file
 *
 * Unit tests for codec registry and probing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>

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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
