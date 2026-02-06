/**
 * @file
 *
 * Document and item types for multi-image containers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_DOC_H
#define GHOTI_IO_IMAGE_DOC_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque document (container with one or more items). */
typedef struct GIMG_Doc GIMG_Doc;
/** @brief Opaque image item (page/frame/level/thumbnail). */
typedef struct GIMG_Item GIMG_Item;

/**
 * @brief Frame dispose operation (APNG fcTL).
 * Values match PNG APNG fcTL dispose_op byte.
 */
typedef enum {
  GIMG_DISPOSE_NONE = 0,   ///< Do not dispose; leave frame as-is.
  GIMG_DISPOSE_BACKGROUND, ///< Clear frame area to background.
  GIMG_DISPOSE_PREVIOUS,   ///< Restore to previous frame content.
  GIMG_DISPOSE_OP_COUNT
} GIMG_Dispose_Op;

/**
 * @brief Frame blend operation (APNG fcTL).
 * Values match PNG APNG fcTL blend_op byte.
 */
typedef enum {
  GIMG_BLEND_SOURCE = 0, ///< Replace (no blend).
  GIMG_BLEND_OVER,       ///< Alpha-blend over previous frame.
  GIMG_BLEND_OP_COUNT
} GIMG_Blend_Op;

/**
 * @brief Number of items in the document (always >= 1).
 */
GIMG_API size_t gimg_doc_item_count(const GIMG_Doc * doc);

/**
 * @brief Get item by index (0 = primary).
 * @param doc Document.
 * @param index Zero-based index.
 * @return Item pointer or NULL if index out of range.
 */
GIMG_API GIMG_Item * gimg_doc_item(const GIMG_Doc * doc, size_t index);

/**
 * @brief Set number of items (reallocates item array; new items get default
 * animation fields). Existing items keep their data up to the new count.
 * @param doc Document.
 * @param count New item count (must be >= 1).
 * @return GIMG_OK or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_doc_set_item_count(GIMG_Doc * doc, size_t count);

/**
 * @brief Get frame delay numerator and denominator (e.g. fcTL delay_num/den).
 * @param item Item.
 * @param num On output, delay numerator (0 if item is NULL).
 * @param den On output, delay denominator (0 if item is NULL).
 */
GIMG_API void gimg_item_frame_delay(
    const GIMG_Item * item, uint16_t * num, uint16_t * den);

/**
 * @brief Set frame delay (e.g. for APNG fcTL).
 * @param item Item.
 * @param num Delay numerator.
 * @param den Delay denominator (0 interpreted as 100 per APNG spec when used).
 */
GIMG_API void gimg_item_set_frame_delay(
    GIMG_Item * item, uint16_t num, uint16_t den);

/**
 * @brief Get frame dispose operation (APNG fcTL).
 */
GIMG_API GIMG_Dispose_Op gimg_item_dispose_op(const GIMG_Item * item);

/**
 * @brief Set frame dispose operation.
 * @param item Item.
 * @param op Dispose op (GIMG_DISPOSE_NONE, GIMG_DISPOSE_BACKGROUND,
 * GIMG_DISPOSE_PREVIOUS).
 */
GIMG_API void gimg_item_set_dispose_op(GIMG_Item * item, GIMG_Dispose_Op op);

/**
 * @brief Get frame blend operation (APNG fcTL).
 */
GIMG_API GIMG_Blend_Op gimg_item_blend_op(const GIMG_Item * item);

/**
 * @brief Set frame blend operation.
 * @param item Item.
 * @param op Blend op (GIMG_BLEND_SOURCE or GIMG_BLEND_OVER).
 */
GIMG_API void gimg_item_set_blend_op(GIMG_Item * item, GIMG_Blend_Op op);

/**
 * @brief Get attached raster (for programmatically created documents).
 * @param item Item.
 * @return Attached raster or NULL. Caller does not take ownership.
 */
GIMG_API GIMG_Raster * gimg_item_raster(const GIMG_Item * item);

/**
 * @brief Attach a raster to an item (e.g. for saving a synthetic document).
 * The item takes ownership of the raster; any previously attached raster is
 * destroyed.
 * @param item Item.
 * @param raster Raster to attach (may be NULL to clear).
 */
GIMG_API void gimg_item_set_raster(GIMG_Item * item, GIMG_Raster * raster);

/**
 * @brief Create a minimal document with one item (uses default allocator).
 */
GIMG_API GIMG_Result gimg_doc_create(GIMG_Doc ** out_doc);

/**
 * @brief Create a minimal document with one item using a specific allocator.
 * @param allocator Allocator for doc and items (NULL = default).
 */
GIMG_API GIMG_Result gimg_doc_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Doc ** out_doc);

/**
 * @brief Destroy document and its items.
 * @param doc Document to destroy (no-op if NULL).
 */
GIMG_API void gimg_doc_destroy(GIMG_Doc * doc);

/**
 * @brief Get raw metadata (eXIf, etc.). Returns NULL if not set.
 */
GIMG_API GIMG_Meta_Raw * gimg_doc_meta_raw(const GIMG_Doc * doc);

/**
 * @brief Ensure document has a raw metadata container; create if missing.
 * @param doc Document.
 * @param out_raw On success, set to the document's meta_raw (caller may attach
 * blocks).
 * @return GIMG_OK or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_doc_ensure_meta_raw(
    GIMG_Doc * doc, GIMG_Meta_Raw ** out_raw);

/**
 * @brief Get normalized common metadata (orientation, DPI). Returns NULL if not
 * set.
 */
GIMG_API GIMG_Meta_Common * gimg_doc_meta_common(const GIMG_Doc * doc);

/**
 * @brief Ensure document has common metadata; create if missing.
 * @param doc Document.
 * @param out_meta On success, set to the document's meta_common.
 * @return GIMG_OK or GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_doc_ensure_meta_common(
    GIMG_Doc * doc, GIMG_Meta_Common ** out_meta);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_DOC_H
