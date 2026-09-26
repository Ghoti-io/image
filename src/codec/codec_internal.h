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
 * Internal codec registry structures and load/save/decode callbacks.
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_CODEC_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CODEC_CODEC_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>

/**
 * @brief Magic signature for probing.
 */
typedef struct {
  const unsigned char * bytes;
  size_t length;
  size_t offset; ///< Offset in stream where magic appears (usually 0).
} gimg_codec_magic_t;

/**
 * @brief Codec load callback: stream -> document.
 */
typedef GIMG_Result (*gimg_codec_load_fn)(GIMG_Codec * codec,
    GIMG_Stream * stream, const GIMG_Load_Options * options,
    GIMG_Diagnostics * diagnostics, GIMG_Doc ** out_doc);

/**
 * @brief Codec save callback: document -> stream.
 */
typedef GIMG_Result (*gimg_codec_save_fn)(GIMG_Codec * codec,
    const GIMG_Doc * doc, GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report);

/**
 * @brief Codec decode callback: item -> raster.
 */
typedef GIMG_Result (*gimg_codec_decode_fn)(GIMG_Codec * codec,
    const GIMG_Item * item, const GIMG_Decode_Options * options,
    GIMG_Raster ** out_raster);

/**
 * @brief Codec callback to free doc->codec_private when document is destroyed.
 */
typedef void (*gimg_codec_free_doc_private_fn)(GIMG_Codec * codec,
    void * codec_private);

/**
 * @brief Codec descriptor (registry entry).
 */
struct GIMG_Codec {
  const GIMG_Allocator * allocator;
  char * name;
  gimg_codec_magic_t * magics;
  size_t magic_count;
  unsigned int capabilities;  ///< Read/write etc. (bitmask for later).
  gimg_codec_load_fn load_cb;
  gimg_codec_save_fn save_cb;
  gimg_codec_decode_fn decode_cb;
  gimg_codec_free_doc_private_fn free_doc_private;
};

/**
 * @brief Add a second (or third) magic to a codec already created.
 *
 * gimg_codec_create_stub_with_allocator() takes one, which is all a format
 * with one spelling of its header needs. TIFF has four - a byte order marker
 * crossed with a version number - and refusing three of them because the
 * constructor takes one would be the shape of a defect this codebase has
 * already had: a magic tested before the byte order rejects one whole
 * endianness.
 *
 * @return GIMG_OK, or GIMG_ERR_OOM with the codec's existing magics intact.
 */
GIMG_Result gimg_codec_add_magic(
    GIMG_Codec * codec, const void * bytes, size_t length, size_t offset);

/**
 * @brief Set load callback (internal; used by codec registration).
 */
void gimg_codec_set_load_cb(GIMG_Codec * codec, gimg_codec_load_fn fn);

/**
 * @brief Set save callback (internal; used by codec registration).
 */
void gimg_codec_set_save_cb(GIMG_Codec * codec, gimg_codec_save_fn fn);

/**
 * @brief Set decode callback (internal; used by codec registration).
 */
void gimg_codec_set_decode_cb(GIMG_Codec * codec, gimg_codec_decode_fn fn);

/**
 * @brief Set free_doc_private callback (internal; used by codec registration).
 */
void gimg_codec_set_free_doc_private(GIMG_Codec * codec,
    gimg_codec_free_doc_private_fn fn);

#endif // GHOTI_IO_GIMG_SRC_CODEC_CODEC_INTERNAL_H
