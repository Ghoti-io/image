/**
 * @file
 *
 * One allocator, three libraries.
 *
 * `GIMG_Allocator`, compress's `gcomp_allocator_t` and cutil's
 * `GCU_Allocator` used to be three separate declarations of the same struct.
 * They are one type now, which is only worth doing if a single allocator
 * instance really can be handed to all three — so that is what this checks,
 * rather than checking that the typedefs exist.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdlib>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/array.h>
#include <ghoti.io/compress/allocator.h>
#include <ghoti.io/compress/options.h>
#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/raster.h>
#include <gtest/gtest.h>

namespace {

/** Counts what each library takes and gives back. */
struct Tally {
  size_t live = 0;
  size_t requests = 0;
};

void * tally_malloc(void * ctx, size_t size) {
  Tally * t = static_cast<Tally *>(ctx);
  void * p = malloc(size ? size : 1);
  if (p) {
    t->live++;
    t->requests++;
  }
  return p;
}

void * tally_calloc(void * ctx, size_t nitems, size_t size) {
  Tally * t = static_cast<Tally *>(ctx);
  if (nitems && size > SIZE_MAX / nitems) {
    return nullptr;
  }
  size_t total = nitems * size;
  void * p = calloc(1, total ? total : 1);
  if (p) {
    t->live++;
    t->requests++;
  }
  return p;
}

void * tally_realloc(void * ctx, void * ptr, size_t size) {
  Tally * t = static_cast<Tally *>(ctx);
  void * p = realloc(ptr, size ? size : 1);
  if (p) {
    t->requests++;
    if (!ptr) {
      t->live++;
    }
  }
  return p;
}

void tally_free(void * ctx, void * ptr) {
  Tally * t = static_cast<Tally *>(ctx);
  if (ptr) {
    t->live--;
  }
  free(ptr);
}

GCU_Allocator make_tally(Tally * t) {
  GCU_Allocator a;
  a.ctx = t;
  a.malloc_fn = tally_malloc;
  a.calloc_fn = tally_calloc;
  a.realloc_fn = tally_realloc;
  a.free_fn = tally_free;
  return a;
}

} // namespace

TEST(SharedAllocator, OneInstanceServesAllThreeLibraries) {
  Tally tally;
  GCU_Allocator allocator = make_tally(&tally);

  // cutil: a container.
  GCU_Array array;
  ASSERT_TRUE(gcu_array_create_in_place(&array, sizeof(int), 0, &allocator));
  for (int i = 0; i < 64; i++) {
    ASSERT_TRUE(gcu_array_append(&array, &i));
  }
  size_t after_cutil = tally.requests;
  EXPECT_GT(after_cutil, 0u) << "cutil should have allocated";

  // compress: the same instance, passed where a gcomp_allocator_t is wanted.
  gcomp_options_t * options = nullptr;
  ASSERT_EQ(gcomp_options_create_with_allocator(&allocator, &options),
      GCOMP_OK);
  ASSERT_NE(options, nullptr);
  size_t after_compress = tally.requests;
  EXPECT_GT(after_compress, after_cutil) << "compress should have allocated";

  // image: the same instance again, where a GIMG_Allocator is wanted.
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create_with_allocator(&allocator, 16, 16,
                &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &raster),
      GIMG_OK);
  ASSERT_NE(raster, nullptr);
  EXPECT_GT(tally.requests, after_compress) << "image should have allocated";

  // Everything each library took, it gives back to the same allocator.
  gimg_raster_destroy(raster);
  gcomp_options_destroy(options);
  gcu_array_destroy_in_place(&array);
  EXPECT_EQ(tally.live, 0u) << "all three libraries freed through this allocator";
}

TEST(SharedAllocator, DefaultsAreTheSameInstance) {
  // Not merely compatible: the three default allocators are one object, so
  // there is a single stdlib-backed implementation rather than three.
  EXPECT_EQ(gimg_allocator_default(), gcu_allocator_default());
  EXPECT_EQ(gcomp_allocator_default(), gcu_allocator_default());
}

TEST(SharedAllocator, DefaultNeverReturnsNullForZeroSize) {
  // image's allocator guaranteed this before the migration; the guarantee had
  // to survive it, because callers read NULL as failure.
  const GIMG_Allocator * a = gimg_allocator_default();
  void * p = a->malloc_fn(a->ctx, 0);
  EXPECT_NE(p, nullptr);
  a->free_fn(a->ctx, p);

  unsigned char * z = (unsigned char *)a->calloc_fn(a->ctx, 0, 0);
  ASSERT_NE(z, nullptr);
  EXPECT_EQ(z[0], 0u) << "a zero-item calloc must still be zeroed";
  a->free_fn(a->ctx, z);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
