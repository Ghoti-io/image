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
 * Internal document and item structures.
 */

#ifndef GHOTI_IO_GIMG_SRC_CONTAINER_DOC_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CONTAINER_DOC_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>

struct GIMG_Codec;

/**
 * @brief Single item (page/frame/level/thumbnail).
 */
struct GIMG_Item {
  size_t index;               ///< Index in parent doc.
  GIMG_Doc * doc;             ///< Parent document (for decode dispatch).
  uint16_t frame_delay_num;   ///< Frame delay numerator (fcTL delay_num).
  uint16_t frame_delay_den;   ///< Frame delay denominator (fcTL delay_den).
  GIMG_Dispose_Op dispose_op; ///< Dispose op (fcTL dispose_op).
  GIMG_Blend_Op blend_op;     ///< Blend op (fcTL blend_op).
  GIMG_Raster * raster;       ///< Attached raster for synthetic docs (owned).
};

/**
 * @brief Document container.
 */
struct GIMG_Doc {
  const GIMG_Allocator * allocator;
  GIMG_Item * items;
  size_t item_count;
  struct GIMG_Codec * loaded_by_codec; ///< Codec that loaded this doc (NULL if
                                       ///< created, not loaded).
  void * codec_private; ///< Format-specific state; owned and freed by codec.
  GIMG_Meta_Raw *
      meta_raw; ///< Optional raw metadata (eXIf, etc.); owned by doc.
  GIMG_Meta_Common * meta_common; ///< Optional normalized metadata
                                  ///< (orientation, DPI); owned by doc.
  /** Copy of the GIMG_Limits supplied to gimg_doc_load, and a flag saying
   * whether one was.  Held by value because the caller's GIMG_Limits need not
   * outlive the load call.  Decoding is deferred - loading a document only
   * reads headers - so the limits an application sets have to survive the load
   * in order to bound the decode that happens later, including one the library
   * performs on its own behalf (a save re-decoding its source). */
  GIMG_Limits load_limits;
  int has_load_limits;

  /** How many times the animation asks to be played, and whether it asked at
   * all.  Document-level rather than per-item because that is where both
   * formats that carry one put it: GIF in a NETSCAPE2.0 Application Extension
   * (89a 26) and APNG in acTL's num_plays.  Zero means forever in both, which
   * is why the flag is needed - "play forever" and "said nothing" are
   * different instructions and a caller has to be able to tell them apart. */
  uint32_t loop_count;
  int has_loop_count;

  /** The colour a viewer is told to put behind the image, and whether the file
   * named one.  GIF names it as an index into the Global Color Table (89a 18)
   * and PNG as bKGD; both are resolved to RGBA here, because an index is
   * meaningless once the palette has been resolved away. */
  uint8_t background[4];
  int has_background;

  /** The shape of a pixel, as a ratio of width to height, and whether the file
   * named one.  GIF's Pixel Aspect Ratio byte (89a 18) and PNG's pHYs with the
   * unit specifier set to "aspect ratio only" say the same thing; neither is a
   * physical size, which is why this is not the DPI in GIMG_Meta_Common. */
  uint32_t aspect_num;
  uint32_t aspect_den;
  int has_aspect;
};

#endif // GHOTI_IO_GIMG_SRC_CONTAINER_DOC_INTERNAL_H
