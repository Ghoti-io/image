/**
 * @file
 *
 * Document and item types for multi-image containers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_DOC_H
#define GHOTI_IO_GIMG_DOC_H

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
 * @brief Get how many times the animation asks to be played.
 *
 * Both animated formats this library reads carry such a count - GIF in a
 * NETSCAPE2.0 Application Extension, APNG in acTL's `num_plays` - and in both
 * a count of zero means "repeat forever".  A file may also carry no count at
 * all, which is not the same instruction: it is the absence of one, and what
 * to do about it is the player's policy rather than the file's.  A GIF with
 * no NETSCAPE2.0 block is shown once by every browser; that convention is not
 * applied here, because a library that guesses leaves the caller unable to
 * tell a guess from a reading.
 *
 * @param doc Document.
 * @param out_count On output, the count; 0 means forever.  Untouched when the
 *        document declares no count, so initialize it if the return value is
 *        not checked.
 * @return 1 when the document declares a loop count, 0 when it does not or
 *         when @p doc is NULL.
 */
GIMG_API int gimg_doc_loop_count(const GIMG_Doc * doc, uint32_t * out_count);

/**
 * @brief Say how many times the animation should be played.
 *
 * @param doc Document.
 * @param count Times to play; 0 means forever.
 */
GIMG_API void gimg_doc_set_loop_count(GIMG_Doc * doc, uint32_t count);

/**
 * @brief Remove a loop count, so the document declares none.
 *
 * This is the only way to express "say nothing", which is a different output
 * from any count: a GIF written for a document with no loop count carries no
 * NETSCAPE2.0 block at all.
 *
 * @param doc Document.
 */
GIMG_API void gimg_doc_clear_loop_count(GIMG_Doc * doc);

/**
 * @brief Get the colour the file says to put behind the image.
 *
 * GIF names it as an index into the Global Color Table (89a 18) and PNG as
 * bKGD; both are reported here as RGBA, because by the time a caller has a
 * decoded raster the palette an index referred to is gone.
 *
 * **This library does not paint it.** A GIF decodes onto a transparent canvas,
 * because that is what every viewer real files were authored against does -
 * see the GIF page's deviations. The colour is reported so that a caller who
 * wants to honour it can, not because anything here has.
 *
 * @param doc Document.
 * @param out_rgba Receives four bytes, red first.  Untouched when the document
 *        declares no background colour.
 * @return 1 when the document declares one, 0 when it does not or when @p doc
 *         is NULL.
 */
GIMG_API int gimg_doc_background_color(
    const GIMG_Doc * doc, uint8_t * out_rgba);

/** @brief Say what colour belongs behind the image (four bytes, red first). */
GIMG_API void gimg_doc_set_background_color(
    GIMG_Doc * doc, const uint8_t * rgba);

/** @brief Remove a background colour, so the document declares none. */
GIMG_API void gimg_doc_clear_background_color(GIMG_Doc * doc);

/**
 * @brief Get the shape of a pixel, as a ratio of width to height.
 *
 * Not a physical size: this is the answer to "is a pixel square", which GIF
 * states in the Pixel Aspect Ratio byte (89a 18) and PNG in pHYs when its unit
 * specifier says "aspect ratio only". A file that states a physical density
 * instead reports that through `gimg_meta_common_dpi()`, and a file may state
 * either, both or neither.
 *
 * A ratio of 1/1 is a square pixel and is a real answer, distinct from the
 * file having said nothing - which is why this reports whether it was stated
 * rather than defaulting to 1.
 *
 * @param doc Document.
 * @param out_num Receives the width term; untouched when none is declared.
 * @param out_den Receives the height term; untouched when none is declared.
 * @return 1 when the document declares a ratio, 0 when it does not or when
 *         @p doc is NULL.
 */
GIMG_API int gimg_doc_pixel_aspect_ratio(
    const GIMG_Doc * doc, uint32_t * out_num, uint32_t * out_den);

/** @brief Say what shape a pixel is.  A zero in either term is ignored. */
GIMG_API void gimg_doc_set_pixel_aspect_ratio(
    GIMG_Doc * doc, uint32_t num, uint32_t den);

/** @brief Remove a pixel aspect ratio, so the document declares none. */
GIMG_API void gimg_doc_clear_pixel_aspect_ratio(GIMG_Doc * doc);

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

/**
 * @brief Copy document: same item count, per-item frame delay/dispose/blend and
 * attached rasters (each raster is copied via gimg_raster_copy). Doc-level
 * meta_common and meta_raw are deep-copied if present. loaded_by_codec and
 * codec_private are not copied (the copy is a synthetic document). Caller owns
 * the returned document.
 */
GIMG_API GIMG_Result gimg_doc_copy(const GIMG_Doc * src, GIMG_Doc ** out_doc);
/**
 * @brief Copy document with a specific allocator (NULL = default).
 */
GIMG_API GIMG_Result gimg_doc_copy_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Doc * src, GIMG_Doc ** out_doc);

/**
 * @brief Create a one-item document with a copy of the given raster attached to
 * item 0. Caller keeps ownership of the original raster. Use with
 * gimg_doc_save to "save this one raster".
 */
GIMG_API GIMG_Result gimg_doc_from_raster(const GIMG_Raster * raster,
    GIMG_Doc ** out_doc);
/**
 * @brief Same as gimg_doc_from_raster with a specific allocator (NULL = default).
 */
GIMG_API GIMG_Result gimg_doc_from_raster_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Raster * raster,
    GIMG_Doc ** out_doc);

/**
 * @brief Copy one item into another: frame delay, dispose_op, blend_op, and
 * attached raster (via gimg_raster_copy). Source and destination may be in
 * the same or different documents. Any existing raster on @a dst_item is
 * destroyed and replaced.
 */
GIMG_API GIMG_Result gimg_item_copy(const GIMG_Item * src_item,
    GIMG_Item * dst_item);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_DOC_H
