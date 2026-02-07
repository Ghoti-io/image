/**
 * @file
 *
 * Basic test to ensure the image library can be linked.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/image.h>
#include <gtest/gtest.h>

// Basic test to ensure the library can be linked.
TEST(ImageLibrary, BasicTest) {
  EXPECT_TRUE(true);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
