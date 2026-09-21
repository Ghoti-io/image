/**
 * @file
 *
 * Unit tests for GIMG_Doc and GIMG_Item.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <cstring>
#include <gtest/gtest.h>

TEST(Doc, CreateHasOneItem) {
  GIMG_Doc * doc = nullptr;
  GIMG_Result res = gimg_doc_create(&doc);
  ASSERT_EQ(res, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  EXPECT_EQ(gimg_doc_item(doc, 1), nullptr);
  gimg_doc_destroy(doc);
}

TEST(Doc, DestroyNullNoOp) {
  gimg_doc_destroy(nullptr);
}

TEST(Doc, ItemAnimationDefaults) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  uint16_t num = 0xFFFF, den = 0xFFFF;
  gimg_item_frame_delay(item, &num, &den);
  EXPECT_EQ(num, 0u);
  EXPECT_EQ(den, 0u);
  EXPECT_EQ(gimg_item_dispose_op(item), GIMG_DISPOSE_NONE);
  EXPECT_EQ(gimg_item_blend_op(item), GIMG_BLEND_SOURCE);
  gimg_doc_destroy(doc);
}

TEST(Doc, ItemAnimationSetGet) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  gimg_item_set_frame_delay(item, 50, 100);
  gimg_item_set_dispose_op(item, GIMG_DISPOSE_BACKGROUND);
  gimg_item_set_blend_op(item, GIMG_BLEND_OVER);
  uint16_t num = 0, den = 0;
  gimg_item_frame_delay(item, &num, &den);
  EXPECT_EQ(num, 50u);
  EXPECT_EQ(den, 100u);
  EXPECT_EQ(gimg_item_dispose_op(item), GIMG_DISPOSE_BACKGROUND);
  EXPECT_EQ(gimg_item_blend_op(item), GIMG_BLEND_OVER);
  gimg_item_set_dispose_op(item, GIMG_DISPOSE_PREVIOUS);
  EXPECT_EQ(gimg_item_dispose_op(item), GIMG_DISPOSE_PREVIOUS);
  gimg_doc_destroy(doc);
}

TEST(Doc, SetItemCountMultiItemAnimation) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 3), GIMG_OK);
  EXPECT_EQ(gimg_doc_item_count(doc), 3u);
  for (size_t i = 0; i < 3; i++) {
    GIMG_Item * item = gimg_doc_item(doc, i);
    ASSERT_NE(item, nullptr);
    uint16_t num = 0xFFFF, den = 0xFFFF;
    gimg_item_frame_delay(item, &num, &den);
    EXPECT_EQ(num, 0u);
    EXPECT_EQ(den, 0u);
    EXPECT_EQ(gimg_item_dispose_op(item), GIMG_DISPOSE_NONE);
    EXPECT_EQ(gimg_item_blend_op(item), GIMG_BLEND_SOURCE);
  }
  gimg_item_set_frame_delay(gimg_doc_item(doc, 0), 1, 10);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 1), 2, 10);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 2), 3, 10);
  gimg_item_set_dispose_op(gimg_doc_item(doc, 1), GIMG_DISPOSE_BACKGROUND);
  gimg_item_set_blend_op(gimg_doc_item(doc, 2), GIMG_BLEND_OVER);
  uint16_t n0 = 0, d0 = 0, n1 = 0, d1 = 0, n2 = 0, d2 = 0;
  gimg_item_frame_delay(gimg_doc_item(doc, 0), &n0, &d0);
  gimg_item_frame_delay(gimg_doc_item(doc, 1), &n1, &d1);
  gimg_item_frame_delay(gimg_doc_item(doc, 2), &n2, &d2);
  EXPECT_EQ(n0, 1u);
  EXPECT_EQ(d0, 10u);
  EXPECT_EQ(n1, 2u);
  EXPECT_EQ(d1, 10u);
  EXPECT_EQ(n2, 3u);
  EXPECT_EQ(d2, 10u);
  EXPECT_EQ(gimg_item_dispose_op(gimg_doc_item(doc, 1)),
            GIMG_DISPOSE_BACKGROUND);
  EXPECT_EQ(gimg_item_blend_op(gimg_doc_item(doc, 2)), GIMG_BLEND_OVER);
  gimg_doc_destroy(doc);
}

TEST(Doc, DocCopyTwoItemsWithRasters) {
  GIMG_Doc * src = nullptr;
  ASSERT_EQ(gimg_doc_create(&src), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(src, 2), GIMG_OK);
  GIMG_Raster * r0 = nullptr;
  GIMG_Raster * r1 = nullptr;
  ASSERT_EQ(gimg_raster_create(4, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r0),
      GIMG_OK);
  ASSERT_EQ(gimg_raster_create(4, 2, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &r1),
      GIMG_OK);
  unsigned char * p0 = (unsigned char *)gimg_raster_pixels(r0);
  unsigned char * p1 = (unsigned char *)gimg_raster_pixels(r1);
  for (size_t i = 0; i < 4 * 2 * 4; i++) {
    p0[i] = (unsigned char)(i + 10);
  }
  for (size_t i = 0; i < 4 * 2; i++) {
    p1[i] = (unsigned char)(i + 20);
  }
  gimg_item_set_raster(gimg_doc_item(src, 0), r0);
  gimg_item_set_raster(gimg_doc_item(src, 1), r1);
  gimg_item_set_frame_delay(gimg_doc_item(src, 0), 1, 10);
  gimg_item_set_frame_delay(gimg_doc_item(src, 1), 2, 10);
  GIMG_Doc * copy = nullptr;
  ASSERT_EQ(gimg_doc_copy(src, &copy), GIMG_OK);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(gimg_doc_item_count(copy), 2u);
  uint16_t n0 = 0, d0 = 0, n1 = 0, d1 = 0;
  gimg_item_frame_delay(gimg_doc_item(copy, 0), &n0, &d0);
  gimg_item_frame_delay(gimg_doc_item(copy, 1), &n1, &d1);
  EXPECT_EQ(n0, 1u);
  EXPECT_EQ(d0, 10u);
  EXPECT_EQ(n1, 2u);
  EXPECT_EQ(d1, 10u);
  GIMG_Raster * c0 = gimg_item_raster(gimg_doc_item(copy, 0));
  GIMG_Raster * c1 = gimg_item_raster(gimg_doc_item(copy, 1));
  ASSERT_NE(c0, nullptr);
  ASSERT_NE(c1, nullptr);
  EXPECT_TRUE(gimg_ops_raster_equal(r0, c0));
  EXPECT_TRUE(gimg_ops_raster_equal(r1, c1));
  gimg_doc_destroy(copy);
  gimg_doc_destroy(src);
}

TEST(Doc, DocCopyOneItemNoRaster) {
  GIMG_Doc * src = nullptr;
  ASSERT_EQ(gimg_doc_create(&src), GIMG_OK);
  EXPECT_EQ(gimg_item_raster(gimg_doc_item(src, 0)), nullptr);
  GIMG_Doc * copy = nullptr;
  ASSERT_EQ(gimg_doc_copy(src, &copy), GIMG_OK);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(gimg_doc_item_count(copy), 1u);
  EXPECT_EQ(gimg_item_raster(gimg_doc_item(copy, 0)), nullptr);
  gimg_doc_destroy(copy);
  gimg_doc_destroy(src);
}

TEST(Doc, DocFromRaster) {
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(3, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  unsigned char * p = (unsigned char *)gimg_raster_pixels(r);
  for (size_t i = 0; i < 3 * 2 * 4; i++) {
    p[i] = (unsigned char)(i + 7);
  }
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_from_raster(r, &doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  GIMG_Raster * attached = gimg_item_raster(gimg_doc_item(doc, 0));
  ASSERT_NE(attached, nullptr);
  EXPECT_TRUE(gimg_ops_raster_equal(r, attached));
  gimg_doc_destroy(doc);
  gimg_raster_destroy(r);
}

TEST(Doc, ItemCopyWithRaster) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  ((unsigned char *)gimg_raster_pixels(r))[0] = 42;
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  gimg_item_set_frame_delay(gimg_doc_item(doc, 0), 5, 100);
  ASSERT_EQ(gimg_item_copy(gimg_doc_item(doc, 0), gimg_doc_item(doc, 1)),
      GIMG_OK);
  GIMG_Raster * r1 = gimg_item_raster(gimg_doc_item(doc, 1));
  ASSERT_NE(r1, nullptr);
  EXPECT_TRUE(gimg_ops_raster_equal(r, r1));
  uint16_t n = 0, d = 0;
  gimg_item_frame_delay(gimg_doc_item(doc, 1), &n, &d);
  EXPECT_EQ(n, 5u);
  EXPECT_EQ(d, 100u);
  gimg_doc_destroy(doc);
}

TEST(Doc, ItemCopyNoRaster) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_EQ(gimg_doc_set_item_count(doc, 2), GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(1, 1, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 1), r);
  ASSERT_EQ(gimg_item_copy(gimg_doc_item(doc, 0), gimg_doc_item(doc, 1)),
      GIMG_OK);
  EXPECT_EQ(gimg_item_raster(gimg_doc_item(doc, 1)), nullptr);
  gimg_doc_destroy(doc);
}

TEST(Doc, EnsureDecodedNoCodec) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Result r = gimg_item_ensure_decoded(item, nullptr);
  EXPECT_EQ(r, GIMG_ERR_UNSUPPORTED);
  EXPECT_EQ(gimg_item_raster(item), nullptr);
  gimg_doc_destroy(doc);
}

TEST(Doc, EnsureDecodedAlreadyHasRaster) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Raster * r = nullptr;
  ASSERT_EQ(gimg_raster_create(1, 1, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED,
                nullptr, 0, &r),
      GIMG_OK);
  gimg_item_set_raster(gimg_doc_item(doc, 0), r);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  GIMG_Result res = gimg_item_ensure_decoded(item, nullptr);
  EXPECT_EQ(res, GIMG_OK);
  EXPECT_EQ(gimg_item_raster(item), r);
  gimg_doc_destroy(doc);
}

// ---------------------------------------------------------------------------
// Loop count
// ---------------------------------------------------------------------------
//
// The count is document-level because that is where both animated formats put
// it, and it is a tri-state rather than a number: absent, forever (0), or a
// finite count.  Every test here is about keeping those three apart, because
// the bug this API exists to prevent is a player that cannot tell "repeat
// forever" from "the file did not say".

TEST(DocLoopCount, ANewDocumentDeclaresNone) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  uint32_t count = 0xABCDu;
  EXPECT_EQ(gimg_doc_loop_count(doc, &count), 0);
  // The out-param is left alone when there is nothing to report, so a caller
  // that ignores the return value does not read a count the document never
  // gave.
  EXPECT_EQ(count, 0xABCDu);
  gimg_doc_destroy(doc);
}

TEST(DocLoopCount, ZeroIsAnAnswerAndNotAnAbsence) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_doc_set_loop_count(doc, 0u);
  uint32_t count = 0xABCDu;
  EXPECT_EQ(gimg_doc_loop_count(doc, &count), 1) << "0 means forever, not unset";
  EXPECT_EQ(count, 0u);
  gimg_doc_destroy(doc);
}

TEST(DocLoopCount, ClearingReturnsToDeclaringNone) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_doc_set_loop_count(doc, 7u);
  uint32_t count = 0;
  ASSERT_EQ(gimg_doc_loop_count(doc, &count), 1);
  ASSERT_EQ(count, 7u);
  gimg_doc_clear_loop_count(doc);
  EXPECT_EQ(gimg_doc_loop_count(doc, &count), 0);
  gimg_doc_destroy(doc);
}

TEST(DocLoopCount, ACopyKeepsIt) {
  // codec_private does not survive a copy, so if the count lived only in the
  // codec's state this would lose it - which is the whole reason it does not.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_doc_set_loop_count(doc, 4u);
  GIMG_Doc * copy = nullptr;
  ASSERT_EQ(gimg_doc_copy(doc, &copy), GIMG_OK);
  uint32_t count = 0;
  EXPECT_EQ(gimg_doc_loop_count(copy, &count), 1);
  EXPECT_EQ(count, 4u);
  gimg_doc_destroy(copy);
  gimg_doc_destroy(doc);
}

TEST(DocLoopCount, ACopyOfADocumentWithNoneDeclaresNone) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  GIMG_Doc * copy = nullptr;
  ASSERT_EQ(gimg_doc_copy(doc, &copy), GIMG_OK);
  EXPECT_EQ(gimg_doc_loop_count(copy, nullptr), 0);
  gimg_doc_destroy(copy);
  gimg_doc_destroy(doc);
}

TEST(DocLoopCount, NullsAreTolerated) {
  EXPECT_EQ(gimg_doc_loop_count(nullptr, nullptr), 0);
  gimg_doc_set_loop_count(nullptr, 1u);
  gimg_doc_clear_loop_count(nullptr);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_doc_set_loop_count(doc, 2u);
  // A caller that only wants to know whether one was declared passes no
  // out-param at all.
  EXPECT_EQ(gimg_doc_loop_count(doc, nullptr), 1);
  gimg_doc_destroy(doc);
}

// ---------------------------------------------------------------------------
// Background colour and pixel aspect ratio
// ---------------------------------------------------------------------------
//
// Both are three-state for the same reason the loop count is: a file may state
// a value, or state nothing, and the two are different instructions.

TEST(DocScreen, ANewDocumentDeclaresNeither) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  uint8_t rgba[4] = {1, 2, 3, 4};
  uint32_t num = 5, den = 6;
  EXPECT_EQ(gimg_doc_background_color(doc, rgba), 0);
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(doc, &num, &den), 0);
  EXPECT_EQ(rgba[0], 1u);
  EXPECT_EQ(num, 5u);
  gimg_doc_destroy(doc);
}

TEST(DocScreen, ABackgroundColourRoundTripsAndClears) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  const uint8_t want[4] = {10, 20, 30, 40};
  gimg_doc_set_background_color(doc, want);
  uint8_t got[4] = {0, 0, 0, 0};
  ASSERT_EQ(gimg_doc_background_color(doc, got), 1);
  EXPECT_EQ(memcmp(got, want, 4), 0);
  gimg_doc_clear_background_color(doc);
  EXPECT_EQ(gimg_doc_background_color(doc, nullptr), 0);
  gimg_doc_destroy(doc);
}

TEST(DocScreen, ASquareRatioIsAnAnswerAndNotAnAbsence) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_doc_set_pixel_aspect_ratio(doc, 1u, 1u);
  uint32_t num = 0, den = 0;
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(doc, &num, &den), 1)
      << "square is something the file said, not the file saying nothing";
  EXPECT_EQ(num, 1u);
  EXPECT_EQ(den, 1u);
  gimg_doc_destroy(doc);
}

TEST(DocScreen, AZeroTermIsRefusedRatherThanStored) {
  // Storing one would hand the caller a division by zero later, dressed up as
  // an answer the file gave.
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_doc_set_pixel_aspect_ratio(doc, 4u, 0u);
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(doc, nullptr, nullptr), 0);
  gimg_doc_set_pixel_aspect_ratio(doc, 0u, 4u);
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(doc, nullptr, nullptr), 0);
  gimg_doc_destroy(doc);
}

TEST(DocScreen, ACopyKeepsBoth) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  const uint8_t want[4] = {7, 8, 9, 255};
  gimg_doc_set_background_color(doc, want);
  gimg_doc_set_pixel_aspect_ratio(doc, 79u, 64u);
  GIMG_Doc * copy = nullptr;
  ASSERT_EQ(gimg_doc_copy(doc, &copy), GIMG_OK);
  uint8_t got[4] = {0, 0, 0, 0};
  uint32_t num = 0, den = 0;
  EXPECT_EQ(gimg_doc_background_color(copy, got), 1);
  EXPECT_EQ(memcmp(got, want, 4), 0);
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(copy, &num, &den), 1);
  EXPECT_EQ(num, 79u);
  EXPECT_EQ(den, 64u);
  gimg_doc_destroy(copy);
  gimg_doc_destroy(doc);
}

TEST(DocScreen, NullsAreTolerated) {
  EXPECT_EQ(gimg_doc_background_color(nullptr, nullptr), 0);
  EXPECT_EQ(gimg_doc_pixel_aspect_ratio(nullptr, nullptr, nullptr), 0);
  gimg_doc_set_background_color(nullptr, nullptr);
  gimg_doc_set_pixel_aspect_ratio(nullptr, 1u, 1u);
  gimg_doc_clear_background_color(nullptr);
  gimg_doc_clear_pixel_aspect_ratio(nullptr);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  gimg_doc_set_background_color(doc, nullptr);
  EXPECT_EQ(gimg_doc_background_color(doc, nullptr), 0);
  gimg_doc_destroy(doc);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
