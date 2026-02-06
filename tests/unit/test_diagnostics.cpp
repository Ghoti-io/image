/**
 * @file
 *
 * Unit tests for GIMG_Diagnostics: init, append, clear, destroy, allocator.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <gtest/gtest.h>
#include <cstddef>
#include <cstdint>

TEST(Diagnostics, ZeroInitAppendUsesDefaultAllocator) {
  GIMG_Diagnostics d = {};
  GIMG_Result r = gimg_diagnostics_append(&d, "png", 0u, 0x49454E44u,
      GIMG_DIAG_ERROR, "fix crc");
  EXPECT_EQ(r, GIMG_OK);
  EXPECT_EQ(d.count, 1u);
  EXPECT_NE(d.items, nullptr);
  EXPECT_STREQ(d.items[0].codec_name, "png");
  gimg_diagnostics_destroy(&d);
}

TEST(Diagnostics, InitNullUsesDefaultAllocator) {
  GIMG_Diagnostics d = {};
  gimg_diagnostics_init(&d, nullptr);
  GIMG_Result r = gimg_diagnostics_append(&d, "png", 8u, 0x49484452u,
      GIMG_DIAG_WARNING, nullptr);
  EXPECT_EQ(r, GIMG_OK);
  EXPECT_EQ(d.count, 1u);
  gimg_diagnostics_destroy(&d);
}

TEST(Diagnostics, ClearFreesListAndResetsCount) {
  GIMG_Diagnostics d = {};
  (void)gimg_diagnostics_append(&d, "png", 0u, 0u, GIMG_DIAG_ERROR, nullptr);
  EXPECT_EQ(d.count, 1u);
  EXPECT_NE(d.items, nullptr);
  gimg_diagnostics_clear(&d);
  EXPECT_EQ(d.count, 0u);
  EXPECT_EQ(d.capacity, 0u);
  EXPECT_EQ(d.items, nullptr);
  gimg_diagnostics_destroy(&d);
}

TEST(Diagnostics, ClearIdempotentWhenEmpty) {
  GIMG_Diagnostics d = {};
  gimg_diagnostics_clear(&d);
  gimg_diagnostics_clear(&d);
  EXPECT_EQ(d.count, 0u);
  EXPECT_EQ(d.items, nullptr);
}

TEST(Diagnostics, DestroyZerosStruct) {
  GIMG_Diagnostics d = {};
  (void)gimg_diagnostics_append(&d, "png", 0u, 0u, GIMG_DIAG_ERROR, nullptr);
  gimg_diagnostics_destroy(&d);
  EXPECT_EQ(d.items, nullptr);
  EXPECT_EQ(d.count, 0u);
  EXPECT_EQ(d.capacity, 0u);
  EXPECT_EQ(d.allocator, nullptr);
}

TEST(Diagnostics, ReuseAfterDestroy) {
  GIMG_Diagnostics d = {};
  (void)gimg_diagnostics_append(&d, "a", 0u, 0u, GIMG_DIAG_WARNING, nullptr);
  gimg_diagnostics_destroy(&d);
  gimg_diagnostics_init(&d, nullptr);
  GIMG_Result r = gimg_diagnostics_append(&d, "b", 1u, 1u, GIMG_DIAG_ERROR, nullptr);
  EXPECT_EQ(r, GIMG_OK);
  EXPECT_EQ(d.count, 1u);
  EXPECT_STREQ(d.items[0].codec_name, "b");
  gimg_diagnostics_destroy(&d);
}

static unsigned g_diag_realloc_count = 0;
static void * g_diag_realloc_ctx = nullptr;

static void * track_realloc(void * ctx, void * ptr, size_t size) {
  g_diag_realloc_ctx = ctx;
  if (ptr != nullptr) {
    g_diag_realloc_count++;
  }
  return gimg_allocator_default()->realloc_fn(
      gimg_allocator_default()->ctx, ptr, size);
}

TEST(Diagnostics, AppendGrowsAndUsesStoredAllocator) {
  GIMG_Allocator custom = {
      .ctx = &custom,
      .malloc_fn = [](void *, size_t size) {
        return gimg_allocator_default()->malloc_fn(nullptr, size);
      },
      .calloc_fn = [](void *, size_t n, size_t size) {
        return gimg_allocator_default()->calloc_fn(nullptr, n, size);
      },
      .realloc_fn = track_realloc,
      .free_fn = [](void *, void * ptr) {
        gimg_allocator_default()->free_fn(nullptr, ptr);
      },
  };

  g_diag_realloc_count = 0;
  g_diag_realloc_ctx = nullptr;
  GIMG_Diagnostics d = {};
  gimg_diagnostics_init(&d, &custom);
  for (int i = 0; i < 8; i++) {
    GIMG_Result r = gimg_diagnostics_append(&d, "png", (size_t)i, (uint32_t)i,
        GIMG_DIAG_WARNING, nullptr);
    ASSERT_EQ(r, GIMG_OK);
  }
  EXPECT_GE(g_diag_realloc_count, 1u)
      << "custom allocator realloc should have been used";
  EXPECT_EQ(g_diag_realloc_ctx, &custom);
  gimg_diagnostics_clear(&d);
  EXPECT_EQ(d.count, 0u);
  EXPECT_EQ(d.items, nullptr);
  gimg_diagnostics_destroy(&d);
}

TEST(Diagnostics, DestroyNullSafe) {
  gimg_diagnostics_init(nullptr, nullptr);
  gimg_diagnostics_clear(nullptr);
  gimg_diagnostics_destroy(nullptr);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
