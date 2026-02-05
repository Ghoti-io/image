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
  GIMG_CODEC * stub = nullptr;
  GIMG_RESULT r = gimg_codec_create_stub("stub", nullptr, 0, &stub);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(stub, nullptr);
  r = gimg_codec_register(stub);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GE(gimg_codec_count(), 1u);
  GIMG_CODEC * found = gimg_codec_by_name("stub");
  EXPECT_NE(found, nullptr);
  EXPECT_STREQ(gimg_codec_name(found), "stub");
}

TEST(Codec, ProbeEmptyStream) {
  GIMG_STREAM * s = nullptr;
  gimg_stream_create_memory(nullptr, 0, &s);
  GIMG_PROBE_RESULT result = {};
  GIMG_RESULT r = gimg_probe(s, &result);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(result.confidence, 0u);
  gimg_stream_destroy(s);
}

TEST(Codec, DocLoadReturnsUnsupported) {
  unsigned char buf[1] = {0};
  GIMG_STREAM * s = nullptr;
  gimg_stream_create_memory(buf, 1, &s);
  GIMG_DOC * doc = nullptr;
  GIMG_RESULT r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(Codec, ItemDecodeReturnsUnsupported) {
  GIMG_DOC * doc = nullptr;
  gimg_doc_create(&doc);
  GIMG_ITEM * item = gimg_doc_item(doc, 0);
  GIMG_RASTER * raster = nullptr;
  GIMG_RESULT r = gimg_item_decode(item, nullptr, &raster);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(raster, nullptr);
  gimg_doc_destroy(doc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
