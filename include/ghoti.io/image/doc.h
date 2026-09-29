/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Document and item types for multi-image containers.
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
/** @brief Opaque image item; gimg_item_role() says which kind. */
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
 * @brief What an item is, relative to the document that holds it.
 *
 * Without this, `gimg_doc_item(doc, 1)` means three different things and
 * nothing says which: the second frame of a GIF or an APNG, the Exif IFD1
 * thumbnail of a JPEG, or another rendering of the same picture from a BMP
 * `BA` array. A caller telling them apart had to know which format it
 * loaded, which is the thing a format-independent document model exists to
 * avoid.
 *
 * The default is ::GIMG_ITEM_IMAGE, so a document built from a raster, or an
 * item a caller zero-initialized, already says the true thing.
 */
typedef enum {
  /** A picture in its own right: a lone image, or one page of several. */
  GIMG_ITEM_IMAGE = 0,
  /** A moment in an animation. gimg_item_frame_delay(), the dispose op and
   * the blend op describe how it is played. */
  GIMG_ITEM_FRAME,
  /** A small preview of another item, which gimg_item_role_subject() names. */
  GIMG_ITEM_THUMBNAIL,
  /** A reduced-resolution version of another item, which
   * gimg_item_role_subject() names. A pyramid level. */
  GIMG_ITEM_LEVEL,
  /** Another rendering of the same picture, for the caller to choose between
   * - an OS/2 `BA` array's entries, one per display device. The picture it is
   * a rendering of is gimg_item_role_subject(). */
  GIMG_ITEM_ALTERNATE,
  GIMG_ITEM_ROLE_COUNT
} GIMG_Item_Role;

/**
 * @brief What this item is. ::GIMG_ITEM_IMAGE when @p item is NULL.
 */
GIMG_API GIMG_Item_Role gimg_item_role(const GIMG_Item * item);

/**
 * @brief The index of the item this one is a thumbnail, level or alternate
 * rendering of.
 *
 * Its own index for ::GIMG_ITEM_IMAGE and ::GIMG_ITEM_FRAME, which are not
 * *of* anything, so a caller may read it without first testing the role.
 * Zero when @p item is NULL.
 */
GIMG_API size_t gimg_item_role_subject(const GIMG_Item * item);

/**
 * @brief Say what this item is.
 * @param item Item to label; NULL is a no-op.
 * @param role The role. A value outside the enum is ignored.
 * @param subject The index of the item it is a thumbnail, level or alternate
 *   rendering of. Ignored - and set to the item's own index - for
 *   ::GIMG_ITEM_IMAGE and ::GIMG_ITEM_FRAME. Not bounds-checked against the
 *   document's item count, because a codec sets this while it is still
 *   building the item list.
 */
GIMG_API void gimg_item_set_role(
    GIMG_Item * item, GIMG_Item_Role role, size_t subject);

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
 * A GIF written for this document carries the count in its NETSCAPE2.0 block
 * and an APNG in acTL's `num_plays`, whatever format it arrived as. GIF's
 * field is two bytes wide where APNG's is four, so a count above 65535 is
 * written as 65535 rather than truncated - truncation would land on 0, the one
 * value that means something else entirely.
 *
 * ::GIMG_Save_Options::gif_loop_count overrides this when it is non-zero. Its
 * zero already means "repeat forever" and so cannot also mean "not set", which
 * is why the option is an override rather than the source: a caller who wants
 * "for ever" on a document that says otherwise says so here, with a count of 0.
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
 * NETSCAPE2.0 block at all, and browsers play such a file once.
 *
 * **APNG cannot say it.** acTL is what makes a PNG an APNG and it always
 * carries a `num_plays`, so a document declaring no count is written there as
 * 0 - the format's own word for "repeat forever" and what every encoder writes
 * with nothing to say. A GIF with no NETSCAPE2.0 block converted to APNG
 * therefore gains an instruction it did not have; that is a limit of the
 * destination, not a choice made here.
 *
 * @param doc Document.
 */
GIMG_API void gimg_doc_clear_loop_count(GIMG_Doc * doc);

/**
 * @brief Get the colour the file says to put behind the image.
 *
 * GIF names it as an index into the Global Color Table (89a 18) and PNG as
 * bKGD (11.3.4.1); both are reported here as RGBA, because by the time a
 * caller has a decoded raster the palette an index referred to is gone. JPEG
 * and BMP have no such field, so a document from one of those never declares
 * a background.
 *
 * **The alpha is part of the answer.** A GIF has no way to leave the field
 * out - a file with a Global Color Table always names one of its entries - so
 * an encoder says "nothing is behind this" by naming an entry its first frame
 * marks transparent. That arrives here as the entry's colour at alpha 0,
 * which composites to the no-op the file asked for while still saying which
 * entry was named. PNG has no such convention and always reports alpha 255.
 *
 * **This library does not paint it.** A GIF decodes onto a transparent canvas
 * unless ::GIMG_GIF_BACKGROUND_PAINT is asked for, because that is what every
 * viewer real files were authored against does - see the GIF page's
 * deviations. A PNG's bKGD is never painted by anyone but ImageMagick, and the
 * spec itself says viewers need not use it. The colour is reported so that a
 * caller who wants to honour it can, not because anything here has.
 *
 * @param doc Document.
 * @param out_rgba Receives four bytes, red first.  Untouched when the document
 *        declares no background colour.
 * @return 1 when the document declares one, 0 when it does not or when @p doc
 *         is NULL.
 */
GIMG_API int gimg_doc_background_color(
    const GIMG_Doc * doc, uint8_t * out_rgba);

/**
 * @brief Say what colour belongs behind the image (four bytes, red first).
 *
 * What a save can do with it depends on what the format can hold. GIF puts it
 * in the Global Color Table and points the Background Color Index at it,
 * adding an entry when the table does not already have that colour. PNG writes
 * a bKGD, which for a palette image is an index and so can only state a colour
 * the palette holds, and for a grayscale image is one gray level and so can
 * only state a gray. Where a format cannot state the colour asked for, nothing
 * is written rather than the nearest thing it could say. JPEG and BMP have
 * nowhere to put one at all.
 *
 * An alpha of 0 is the same statement as declaring none, and GIF writes it as
 * such.
 */
GIMG_API void gimg_doc_set_background_color(
    GIMG_Doc * doc, const uint8_t * rgba);

/**
 * @brief Remove a background colour, so the document declares none.
 *
 * This is the only way to express "say nothing", which is a different output
 * from any colour: a PNG written for such a document carries no bKGD at all,
 * and a GIF names the entry its first frame marks transparent, which is the
 * nearest thing 89a 18 has to leaving the field out.
 */
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
 * @brief Cursor hotspot for a CUR entry (pixels from the top-left of the image).
 *
 * ICO entries leave both coordinates at zero. A non-zero hotspot on save makes
 * the written file a CUR (`type = 2`) rather than an ICO.
 */
GIMG_API void gimg_item_hotspot(
    const GIMG_Item * item, uint16_t * x, uint16_t * y);

/** @brief Set the cursor hotspot. NULL @p item is a no-op. */
GIMG_API void gimg_item_set_hotspot(GIMG_Item * item, uint16_t x, uint16_t y);

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
