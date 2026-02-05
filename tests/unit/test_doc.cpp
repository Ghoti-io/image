/**
 * @file
 *
 * Unit tests for GIMG_Doc and GIMG_Item.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/doc.h>
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
