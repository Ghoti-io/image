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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
