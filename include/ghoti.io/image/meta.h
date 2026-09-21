/**
 * @file
 *
 * Metadata model: common (normalized), raw (round-trip), save policies.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_META_H
#define GHOTI_IO_GIMG_META_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Document or item may carry metadata; types are opaque. */
typedef struct GIMG_Meta_Common GIMG_Meta_Common;
typedef struct GIMG_Meta_Raw GIMG_Meta_Raw;

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
} GIMG_Orientation;

/**
 * @brief Set/get orientation on common metadata.
 */
GIMG_API void gimg_meta_common_set_orientation(
    GIMG_Meta_Common * meta, GIMG_Orientation value);
GIMG_API GIMG_Orientation gimg_meta_common_orientation(
    const GIMG_Meta_Common * meta);

/**
 * @brief DPI (0 = unknown).
 */
GIMG_API void gimg_meta_common_set_dpi(
    GIMG_Meta_Common * meta, uint32_t x_dpi, uint32_t y_dpi);
GIMG_API void gimg_meta_common_dpi(
    const GIMG_Meta_Common * meta, uint32_t * out_x, uint32_t * out_y);

/**
 * @brief Normalized description/comment (populated from format-native storage
 * on load, e.g. JPEG COM, GIF's Comment Extension, or PNG tEXt
 * "Description"/"Comment"; written on save when set and policy allows). UTF-8,
 * null-terminated; storage is internal.
 *
 * @warning **This is at most one comment, and a file may hold several.** JPEG
 * permits any number of COM segments and GIF any number of Comment Extensions;
 * what appears here is the **first** one that is readable as text. The rest are
 * not lost - every one of them is kept verbatim in the document's raw
 * metadata, under that format's id - but they are not here, and a caller that
 * reads only this field will silently see one of them and not know there were
 * others. Read `gimg_doc_meta_raw()` as well when it matters which, or how
 * many, a file carried. Each format's page documents the id and the framing
 * its raw block uses.
 *
 * Reference decoders do not agree on what to do with several, which is part of
 * why this field cannot: reading one GIF holding two comments, ImageMagick
 * reports the last, Pillow reports both joined by a newline, and this library
 * reports the first here and all of them in the raw block. None is wrong;
 * there is no convention to be right about.
 *
 * @note On save, what a format does with text that is **not 7-bit ASCII** is
 * the format's own decision and is stated on its page, because some of them
 * specify ASCII and no more. GIF is the case worth knowing: 89a 24 calls a
 * comment 7-bit ASCII, and this library writes the UTF-8 bytes as given
 * anyway, matching every GIF writer in use rather than the text of the
 * specification. A reader that assumes ASCII will see the encoded bytes.
 */
GIMG_API GIMG_Result gimg_meta_common_set_description(
    GIMG_Meta_Common * meta, const char * description);
GIMG_API const char * gimg_meta_common_description(
    const GIMG_Meta_Common * meta);

/**
 * @brief Create/destroy common metadata block (create uses default allocator).
 */
GIMG_API GIMG_Result gimg_meta_common_create(GIMG_Meta_Common ** out_meta);
GIMG_API GIMG_Result gimg_meta_common_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Meta_Common ** out_meta);
GIMG_API void gimg_meta_common_destroy(GIMG_Meta_Common * meta);

//
// Raw metadata (spec §5.2): format-native chunks/tags preserved by identity.
//

/**
 * @brief Attach raw block (format_id + tag/chunk id, bytes). Library copies.
 */
GIMG_API GIMG_Result gimg_meta_raw_attach(GIMG_Meta_Raw * raw,
    const char * format_id, uint32_t tag_or_chunk_id, const void * data,
    size_t size);

/**
 * @brief Retrieve raw block; returns size. NULL data = query size only.
 */
GIMG_API GIMG_Result gimg_meta_raw_get(const GIMG_Meta_Raw * raw,
    const char * format_id, uint32_t tag_or_chunk_id, void * data,
    size_t * size);

/**
 * @brief Create/destroy raw metadata container (create uses default
 * allocator).
 */
GIMG_API GIMG_Result gimg_meta_raw_create(GIMG_Meta_Raw ** out_raw);
GIMG_API GIMG_Result gimg_meta_raw_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Meta_Raw ** out_raw);
GIMG_API void gimg_meta_raw_destroy(GIMG_Meta_Raw * raw);

/**
 * @brief Deep-copy raw metadata (all blocks). Uses default allocator.
 */
GIMG_API GIMG_Result gimg_meta_raw_copy(
    const GIMG_Meta_Raw * src, GIMG_Meta_Raw ** out_raw);

/**
 * @brief Deep-copy raw metadata with a specific allocator (NULL = default).
 */
GIMG_API GIMG_Result gimg_meta_raw_copy_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Meta_Raw * src,
    GIMG_Meta_Raw ** out_raw);

//
// Save policies (spec §5.3). No silent stripping.
//

/** @see api_options for behavior per policy. */
typedef enum {
  GIMG_META_PRESERVE_ALL = 0,
  GIMG_META_DROP_ALL,
  GIMG_META_STRIP_GPS,
  GIMG_META_NORMALIZE_EXIF,
  GIMG_META_KEEP_RAW_ONLY,
  GIMG_META_KEEP_COMMON_ONLY,
  GIMG_META_POLICY_COUNT
} GIMG_Meta_Policy;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_META_H
