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
 * Internal ICO/CUR codec structures. One codec covers both type=1 (ICO) and
 * type=2 (CUR): the on-disk layout is identical aside from the hotspot fields.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_ICO_ICO_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_ICO_ICO_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ICONDIR is six bytes; both ICO and CUR share the same first four. */
#define GIMG_ICO_SIGNATURE_LEN 4

/** Hard cap on directory entries. Alternates, not frames — not max_frame_count. */
#define GIMG_ICO_MAX_ENTRIES 64u

/** ICONDIR type values. */
#define GIMG_ICO_TYPE_ICON 1u
#define GIMG_ICO_TYPE_CURSOR 2u

/** Directory entry bytes on disk. */
#define GIMG_ICO_DIRENTRY_SIZE 16u

/** ICONDIR size on disk. */
#define GIMG_ICO_DIR_SIZE 6u

extern const unsigned char gimg_ico_signature[GIMG_ICO_SIGNATURE_LEN];
extern const unsigned char gimg_cur_signature[GIMG_ICO_SIGNATURE_LEN];

/** Payload kind for one directory entry. */
typedef enum {
  GIMG_ICO_PAYLOAD_KIND_DIB = 0,
  GIMG_ICO_PAYLOAD_KIND_PNG
} gimg_ico_payload_kind_t;

/**
 * @brief One directory entry, as stored for decode.
 *
 * Directory width/height/bpp are advisory; the payload is authoritative.
 * When they disagree, load still succeeds and a diagnostic is recorded.
 */
typedef struct {
  uint32_t offset; ///< Absolute file offset of the payload.
  uint32_t size;   ///< Payload byte length from the directory.
  uint8_t dir_width;  ///< Directory byte (0 means 256).
  uint8_t dir_height; ///< Directory byte (0 means 256).
  uint16_t dir_bpp;   ///< Directory planes/bitcount field (ICO) or unused.
  uint16_t hotspot_x;
  uint16_t hotspot_y;
  gimg_ico_payload_kind_t kind;
} gimg_ico_entry_t;

/** Document state owned by the codec from load until free_doc_private. */
typedef struct {
  const GIMG_Allocator * allocator;
  uint16_t type; ///< GIMG_ICO_TYPE_ICON or GIMG_ICO_TYPE_CURSOR.
  size_t entry_count;
  gimg_ico_entry_t * entries;
  /** Contiguous copy of every payload, indexed by entry offset/size into the
   * original file — held so decode does not need the load stream. */
  unsigned char * file_bytes;
  size_t file_size;
} gimg_ico_doc_state_t;

/** Post-magic probe: count in range and first entry inside the file. */
int gimg_ico_probe(
    GIMG_Stream * stream, const unsigned char * peek, size_t peeked);

GIMG_Result gimg_ico_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

GIMG_Result gimg_ico_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

GIMG_Result gimg_ico_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report);

void gimg_ico_free_doc_state(GIMG_Codec * codec, void * codec_private);

/**
 * @brief Apply a 1-bit AND mask as alpha onto an RGBA8 raster.
 *
 * Rows are bottom-up, padded to 32-bit boundaries, matching the ICO DIB
 * layout. A set bit means transparent.
 *
 * @param force When non-zero, overwrite alpha even if some pixels already have
 *   non-zero alpha. When zero, only used by the identically-zero alpha fallback.
 */
GIMG_Result gimg_ico_apply_and_mask(GIMG_Raster * raster,
    const unsigned char * mask, size_t mask_size, int force);

/** Directory width/height byte → real dimension (0 means 256). */
static inline uint32_t gimg_ico_dir_dim(uint8_t byte) {
  return byte == 0u ? 256u : (uint32_t)byte;
}

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_SRC_CODEC_ICO_ICO_INTERNAL_H
