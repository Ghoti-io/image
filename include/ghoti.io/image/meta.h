/**
 * @file
 *
 * Metadata model: common (normalized), raw (round-trip), save policies.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_META_H
#define GHOTI_IO_IMAGE_META_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Document or item may carry metadata; types are opaque. */
typedef struct GIMG_META_COMMON GIMG_META_COMMON;
typedef struct GIMG_META_RAW GIMG_META_RAW;

//
// Normalized common fields (spec §5.1). Storage is internal; accessors here.
//

/**
 * @brief Orientation (EXIF-style 1–8).
 */
typedef enum {
  GIMG_ORIENTATION_UNKNOWN = 0,
  GIMG_ORIENTATION_NORMAL = 1,
  GIMG_ORIENTATION_FLIP_H = 2,
  GIMG_ORIENTATION_ROTATE_180 = 3,
  GIMG_ORIENTATION_FLIP_V = 4,
  GIMG_ORIENTATION_TRANSPOSE = 5,
  GIMG_ORIENTATION_ROTATE_90_CW = 6,
  GIMG_ORIENTATION_TRANSVERSE = 7,
  GIMG_ORIENTATION_ROTATE_90_CCW = 8
} GIMG_ORIENTATION;

/**
 * @brief Set/get orientation on common metadata.
 */
GIMG_API void gimg_meta_common_set_orientation(
    GIMG_META_COMMON * meta, GIMG_ORIENTATION value);
GIMG_API GIMG_ORIENTATION gimg_meta_common_orientation(
    const GIMG_META_COMMON * meta);

/**
 * @brief DPI (0 = unknown).
 */
GIMG_API void gimg_meta_common_set_dpi(
    GIMG_META_COMMON * meta, uint32_t x_dpi, uint32_t y_dpi);
GIMG_API void gimg_meta_common_dpi(
    const GIMG_META_COMMON * meta, uint32_t * out_x, uint32_t * out_y);

/**
 * @brief Create/destroy common metadata block (create uses default allocator).
 */
GIMG_API GIMG_RESULT gimg_meta_common_create(GIMG_META_COMMON ** out_meta);
GIMG_API GIMG_RESULT gimg_meta_common_create_with_allocator(
    const GIMG_ALLOCATOR * allocator, GIMG_META_COMMON ** out_meta);
GIMG_API void gimg_meta_common_destroy(GIMG_META_COMMON * meta);

//
// Raw metadata (spec §5.2): format-native chunks/tags preserved by identity.
//

/**
 * @brief Attach raw block (format_id + tag/chunk id, bytes). Library copies.
 */
GIMG_API GIMG_RESULT gimg_meta_raw_attach(GIMG_META_RAW * raw,
    const char * format_id, uint32_t tag_or_chunk_id, const void * data,
    size_t size);

/**
 * @brief Retrieve raw block; returns size. NULL data = query size only.
 */
GIMG_API GIMG_RESULT gimg_meta_raw_get(const GIMG_META_RAW * raw,
    const char * format_id, uint32_t tag_or_chunk_id, void * data,
    size_t * size);

/**
 * @brief Create/destroy raw metadata container (create uses default
 * allocator).
 */
GIMG_API GIMG_RESULT gimg_meta_raw_create(GIMG_META_RAW ** out_raw);
GIMG_API GIMG_RESULT gimg_meta_raw_create_with_allocator(
    const GIMG_ALLOCATOR * allocator, GIMG_META_RAW ** out_raw);
GIMG_API void gimg_meta_raw_destroy(GIMG_META_RAW * raw);

//
// Save policies (spec §5.3). No silent stripping.
//

typedef enum {
  GIMG_META_PRESERVE_ALL = 0,
  GIMG_META_DROP_ALL,
  GIMG_META_STRIP_GPS,
  GIMG_META_NORMALIZE_EXIF,
  GIMG_META_KEEP_RAW_ONLY,
  GIMG_META_KEEP_COMMON_ONLY,
  GIMG_META_POLICY_COUNT
} GIMG_META_POLICY;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_META_H
