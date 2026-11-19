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
 * What a save could not carry from the document into the file.
 *
 * Each writer has its own idea of what it can store, and a document that was
 * loaded from one format and saved as another meets the difference as a
 * silent loss: the file is written, the result is GIMG_OK, and the Exif, the
 * profile or the loop count is simply not in it.  This module is the one place
 * that knows, for every writer and every metadata policy, which kinds of
 * metadata reach the file, and it turns the difference between what the
 * document holds and what the writer stored into one warning per kind,
 * appended to GIMG_Save_Report.diagnostics.
 *
 * Nothing here changes what is written.  What a policy asked to have removed
 * is not a loss and is not reported.
 */

#ifndef GHOTI_IO_GIMG_SRC_META_META_LOSS_H
#define GHOTI_IO_GIMG_SRC_META_META_LOSS_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A kind of metadata (or document-level fact) a save can fail to carry. */
typedef enum {
  GIMG_LOSS_EXIF = 0, ///< An Exif block.
  GIMG_LOSS_XMP,      ///< An XMP packet.
  GIMG_LOSS_ICC,      ///< An ICC profile (known to the writer, not the document).
  GIMG_LOSS_RESOLUTION,  ///< The document's dots per inch.
  GIMG_LOSS_DESCRIPTION, ///< The document's common description.
  GIMG_LOSS_COMMENTS,    ///< Comments beyond the one the description holds.
  GIMG_LOSS_TEXT,        ///< PNG text chunks, TIFF text tags.
  GIMG_LOSS_NATIVE,      ///< Any other format-specific block (IPTC, tIME, ...).
  GIMG_LOSS_LOOP_COUNT,  ///< An animation's loop count.
  GIMG_LOSS_FRAME_DELAY, ///< Per-frame delays.
  GIMG_LOSS_HOTSPOT,     ///< A cursor hotspot.
  GIMG_LOSS_ITEM_ROLES,  ///< Thumbnail, level and alternate roles.
  GIMG_LOSS_ITEMS,       ///< Items the writer has no place for.
  GIMG_LOSS_COUNT
} gimg_loss_kind_t;

/**
 * One save's collection of what its writer reported.  Lives on the stack of
 * gimg_doc_save(); a writer reaches it through gimg_save_loss_note().
 */
typedef struct gimg_save_loss {
  uint32_t noted;                ///< Bit per gimg_loss_kind_t.
  struct gimg_save_loss * outer; ///< The save this one is nested inside.
} gimg_save_loss_t;

/** Open a scope for the current thread's save. */
void gimg_save_loss_begin(gimg_save_loss_t * scope);

/** Close it, restoring the enclosing scope (a BMP wrapper saves a PNG). */
void gimg_save_loss_end(gimg_save_loss_t * scope);

/**
 * A writer's report that the document holds @p kind and it was not written.
 * Only for what the writer alone can see (the profile on a decoded raster);
 * what the document itself shows is worked out in gimg_save_loss_report().  A
 * no-op outside a scope.
 */
void gimg_save_loss_note(gimg_loss_kind_t kind);

/**
 * Append one warning to @p report->diagnostics (when the caller supplied
 * one) for each kind the document holds that the named writer did not
 * write and the policy did not ask to remove.  Allocation failure while
 * appending drops the remaining warnings; the save itself has succeeded.
 *
 * @param codec_name The codec's own name ("jpeg", "png", ...).
 */
void gimg_save_loss_report(const gimg_save_loss_t * scope, const GIMG_Doc * doc,
    const char * codec_name, const GIMG_Save_Options * options,
    GIMG_Save_Report * report);

/** The kinds a PNG-loaded document's chunk list holds (bit per kind). */
uint32_t gimg_png_doc_loss_kinds(const GIMG_Doc * doc);

/** Whether an ICO-loaded document is a cursor file. */
int gimg_ico_doc_is_cursor(const GIMG_Doc * doc);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_SRC_META_META_LOSS_H
