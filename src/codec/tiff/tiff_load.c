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
 * Read a TIFF's header and image file directories (TIFF 6.0 sections 2, 8).
 *
 * A TIFF is a header naming the offset of the first IFD, and each IFD is a
 * count, that many twelve-byte entries, and the offset of the next one.  An
 * entry is a tag, a type, a count, and four bytes that hold the value when it
 * fits and an offset to it when it does not.  Everything else in the format -
 * where the pixels are, how they are compressed, what the samples mean - is a
 * tag, which is why this file is mostly about reading one entry correctly and
 * the rest is a table.
 *
 * Every IFD becomes an item.  A multi-page TIFF is several pictures in one
 * file, which is what items are for and what the GIF, APNG and BMP array
 * loaders already do with them.
 */

#include <ghoti.io/image/macros.h>

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/limits_internal.h"
#include "../../core/resolution_internal.h"
#include "../../core/safe_math_internal.h"
#include "../codec_internal.h"
#include "tiff_internal.h"

/** Append a load diagnostic tagged with the codec name and a file offset. */
static void tiff_diag(
    GIMG_Diagnostics * d, size_t offset, const char * action) {
  if (!d) {
    return;
  }
  (void)gimg_diagnostics_append(
      d, "tiff", offset, 0u, GIMG_DIAG_ERROR, action);
}

// ---------------------------------------------------------------------------
// Byte-order-aware readers
//
// Every multi-byte value in a TIFF is written in the order the header
// declared, which is not necessarily this machine's.  Nothing below reads a
// value any other way.
// ---------------------------------------------------------------------------

static uint16_t tiff_u16(const unsigned char * p, bool be) {
  return be ? (uint16_t)(((uint16_t)p[0] << 8) | p[1])
            : (uint16_t)(((uint16_t)p[1] << 8) | p[0]);
}

static uint32_t tiff_u32(const unsigned char * p, bool be) {
  return be ? (((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                  ((uint32_t)p[2] << 8) | (uint32_t)p[3])
            : (((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) |
                  ((uint32_t)p[1] << 8) | (uint32_t)p[0]);
}

/** Bytes one value of @p type occupies, or 0 for a type this codec does not
 * know.  An unknown type is not an error by itself - a file may carry tags
 * this codec ignores - but its length cannot be computed, so such an entry is
 * skipped rather than guessed at. */
static size_t tiff_type_size(uint16_t type) {
  switch (type) {
  case GIMG_TIFF_TYPE_BYTE:
  case GIMG_TIFF_TYPE_ASCII:
  case GIMG_TIFF_TYPE_SBYTE:
  case GIMG_TIFF_TYPE_UNDEFINED:
    return 1u;
  case GIMG_TIFF_TYPE_SHORT:
  case GIMG_TIFF_TYPE_SSHORT:
    return 2u;
  case GIMG_TIFF_TYPE_LONG:
  case GIMG_TIFF_TYPE_SLONG:
  case GIMG_TIFF_TYPE_FLOAT:
    return 4u;
  case GIMG_TIFF_TYPE_RATIONAL:
  case GIMG_TIFF_TYPE_SRATIONAL:
  case GIMG_TIFF_TYPE_DOUBLE:
    return 8u;
  default:
    return 0u;
  }
}

/** One directory entry, resolved to where its values actually are. */
typedef struct {
  uint16_t tag;
  uint16_t type;
  uint32_t count;
  const unsigned char * values; ///< NULL when the entry is out of bounds.
  size_t value_bytes;
} tiff_entry_t;

/**
 * Point @p out at an entry's values, wherever they are.
 *
 * Four bytes or fewer live in the entry itself, left-justified, in the file's
 * byte order (TIFF 6.0 section 2, "IFD Entry").  Anything longer is at the
 * offset those four bytes hold, and that offset is checked against the end of
 * the file here rather than by every caller.
 */
static bool tiff_entry_at(const gimg_tiff_doc_state_t * st,
    const unsigned char * entry, tiff_entry_t * out) {
  out->tag = tiff_u16(entry + 0, st->big_endian);
  out->type = tiff_u16(entry + 2, st->big_endian);
  out->count = tiff_u32(entry + 4, st->big_endian);
  out->values = NULL;
  out->value_bytes = 0;

  const size_t unit = tiff_type_size(out->type);
  if (unit == 0u) {
    return false; // A type this codec cannot size; the caller skips the tag.
  }
  size_t total = 0;
  if (!gcu_safe_mul_size(unit, (size_t)out->count, &total)) {
    return false;
  }
  if (total <= 4u) {
    out->values = entry + 8;
    out->value_bytes = total;
    return true;
  }
  const uint32_t at = tiff_u32(entry + 8, st->big_endian);
  size_t end = 0;
  if (!gcu_safe_add_size((size_t)at, total, &end) || end > st->file_size) {
    return false;
  }
  out->values = st->file + at;
  out->value_bytes = total;
  return true;
}

/** The @p index'th value of an entry, widened to 64 bits. Signed types are
 * read as their unsigned counterparts: no tag this codec reads is negative,
 * and a file that makes one negative is saying something this codec would
 * refuse anyway. */
static uint64_t tiff_value(
    const gimg_tiff_doc_state_t * st, const tiff_entry_t * e, size_t index) {
  const size_t unit = tiff_type_size(e->type);
  const unsigned char * p = e->values + (index * unit);
  switch (unit) {
  case 1u:
    return (uint64_t)*p;
  case 2u:
    return (uint64_t)tiff_u16(p, st->big_endian);
  case 4u:
    return (uint64_t)tiff_u32(p, st->big_endian);
  case 8u:
    // A RATIONAL is two LONGs; callers that want both read them directly.
    return (uint64_t)tiff_u32(p, st->big_endian);
  default:
    return 0u;
  }
}

/** Copy an entry's values into a freshly allocated uint64_t array. */
static GIMG_Result tiff_value_array(const gimg_tiff_doc_state_t * st,
    const tiff_entry_t * e, uint64_t ** out_values, size_t * out_count) {
  *out_values = NULL;
  *out_count = 0;
  if (e->count == 0u) {
    return GIMG_OK;
  }
  size_t bytes = 0;
  if (!gcu_safe_mul_size((size_t)e->count, sizeof(uint64_t), &bytes)) {
    return GIMG_ERR_LIMIT;
  }
  uint64_t * v = (uint64_t *)gimg_malloc(st->allocator, bytes);
  if (!v) {
    return GIMG_ERR_OOM;
  }
  for (size_t i = 0; i < e->count; i++) {
    v[i] = tiff_value(st, e, i);
  }
  *out_values = v;
  *out_count = e->count;
  return GIMG_OK;
}

static void tiff_free_ifd(const GIMG_Allocator * a, gimg_tiff_ifd_t * ifd) {
  gimg_free(a, ifd->block_offsets);
  gimg_free(a, ifd->block_byte_counts);
  gimg_free(a, ifd->color_map);
  gimg_free(a, ifd->icc);
  gimg_free(a, ifd->jpeg_tables);
  ifd->jpeg_tables = NULL;
  ifd->jpeg_tables_size = 0;
  gimg_free(a, ifd->description);
  gimg_free(a, ifd->xmp);
  gimg_free(a, ifd->sub_ifds);
  ifd->sub_ifds = NULL;
  ifd->sub_ifd_count = 0;
  ifd->icc = NULL;
  ifd->icc_size = 0;
  ifd->description = NULL;
  ifd->xmp = NULL;
  ifd->xmp_size = 0;
  ifd->block_offsets = NULL;
  ifd->block_byte_counts = NULL;
  ifd->color_map = NULL;
  ifd->block_count = 0;
  ifd->color_map_count = 0;
}

void gimg_tiff_free_doc_state(GIMG_Codec * codec, void * codec_private) {
  (void)codec;
  gimg_tiff_doc_state_t * st = (gimg_tiff_doc_state_t *)codec_private;
  if (!st) {
    return;
  }
  const GIMG_Allocator * a = st->allocator;
  for (size_t i = 0; i < st->ifd_count; i++) {
    tiff_free_ifd(a, &st->ifds[i]);
  }
  gimg_free(a, st->ifds);
  gimg_free(a, st->file);
  gimg_free(a, st);
}

// ---------------------------------------------------------------------------
// One directory
// ---------------------------------------------------------------------------

/** Install the TIFF 6.0 defaults, so a tag's absence means what the
 * specification says it means rather than zero. */
static void tiff_ifd_defaults(gimg_tiff_ifd_t * ifd) {
  memset(ifd, 0, sizeof(*ifd));
  ifd->bits_per_sample = 1u;                  // Section 8.
  ifd->compression = GIMG_TIFF_COMPRESSION_NONE;
  ifd->samples_per_pixel = 1u;
  ifd->planar_config = 1u;
  ifd->resolution_unit = 2u;                  // Inches.
  ifd->sample_format = 1u;                    // Unsigned integer.
  ifd->predictor = 1u;                        // No differencing.
  ifd->role = GIMG_ITEM_IMAGE;
  // CCIR 601-1, which section 21 gives as the default and which is the same
  // set JPEG uses.
  ifd->luma_red = 0.299;
  ifd->luma_green = 0.587;
  ifd->luma_blue = 0.114;
  ifd->ycbcr_h = 2u;
  ifd->ycbcr_v = 2u;
  // The default for YCbCr: luma over its full range, chroma centred on 128.
  // With these the conversion below is the ordinary JPEG one.
  ifd->reference_black_white[0] = 0.0;
  ifd->reference_black_white[1] = 255.0;
  ifd->reference_black_white[2] = 128.0;
  ifd->reference_black_white[3] = 255.0;
  ifd->reference_black_white[4] = 128.0;
  ifd->reference_black_white[5] = 255.0;
  ifd->rows_per_strip = 0xFFFFFFFFu;          // "The whole image in one strip".
  ifd->photometric = 0xFFFFu;                 // No default; absence is an error.
}

/**
 * Read one IFD at @p at into @p ifd.
 *
 * @param out_next Offset of the following IFD, or 0 at the end of the chain.
 */
static GIMG_Result tiff_read_ifd(gimg_tiff_doc_state_t * st, uint32_t at,
    const GIMG_Limits * limits, GIMG_Diagnostics * diag, gimg_tiff_ifd_t * ifd,
    uint32_t * out_next) {
  tiff_ifd_defaults(ifd);
  ifd->file_big_endian = st->big_endian;
  *out_next = 0u;

  if ((size_t)at + 2u > st->file_size) {
    tiff_diag(diag, at, "the directory begins past the end of the file");
    return GIMG_ERR_CORRUPT;
  }
  const uint32_t count = tiff_u16(st->file + at, st->big_endian);
  size_t after = 0;
  if (!gcu_safe_mul_size((size_t)count, 12u, &after) ||
      !gcu_safe_add_size(after, (size_t)at + 2u, &after) ||
      after + 4u > st->file_size) {
    tiff_diag(diag, at, "the directory runs past the end of the file");
    return GIMG_ERR_CORRUPT;
  }
  *out_next = tiff_u32(st->file + after, st->big_endian);

  uint64_t * strip_offsets = NULL;
  size_t strip_offset_count = 0;
  uint64_t * strip_counts = NULL;
  size_t strip_count_count = 0;
  bool have_tile_width = false, have_tile_height = false;
  GIMG_Result r = GIMG_OK;

  for (uint32_t i = 0; i < count && r == GIMG_OK; i++) {
    tiff_entry_t e;
    if (!tiff_entry_at(st, st->file + at + 2u + ((size_t)i * 12u), &e)) {
      // A type this codec cannot size, or values outside the file.  Skipping
      // is right for a tag nobody reads and wrong for one that decides how to
      // decode; the check after the loop is what separates them, because a
      // required tag that was skipped is a required tag that is missing.
      continue;
    }
    if (e.count == 0u) {
      continue;
    }
    switch (e.tag) {
    case GIMG_TIFF_TAG_IMAGE_WIDTH:
      ifd->width = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_IMAGE_LENGTH:
      ifd->height = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_BITS_PER_SAMPLE: {
      // One value per sample, and this codec reads only files where they
      // agree.  A file that stores eight bits of red beside four of green is
      // legal and is refused below by name, rather than by reading the first
      // value and quietly getting the rest wrong.
      ifd->bits_per_sample = (uint16_t)tiff_value(st, &e, 0);
      for (size_t k = 1; k < e.count; k++) {
        if ((uint16_t)tiff_value(st, &e, k) != ifd->bits_per_sample) {
          ifd->bits_per_sample = 0u; // Refused by the support check.
          break;
        }
      }
      break;
    }
    case GIMG_TIFF_TAG_COMPRESSION:
      ifd->compression = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_PHOTOMETRIC:
      ifd->photometric = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_SAMPLES_PER_PIXEL:
      ifd->samples_per_pixel = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_ROWS_PER_STRIP:
      ifd->rows_per_strip = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_PREDICTOR:
      ifd->predictor = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_PLANAR_CONFIG:
      ifd->planar_config = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_FILL_ORDER:
      ifd->fill_order = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_T4_OPTIONS:
      ifd->t4_options = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_T6_OPTIONS:
      ifd->t6_options = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_SAMPLE_FORMAT:
      ifd->sample_format = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_RESOLUTION_UNIT:
      ifd->resolution_unit = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_EXTRA_SAMPLES:
      ifd->extra_samples = (uint16_t)tiff_value(st, &e, 0);
      ifd->has_extra_samples = true;
      break;
    case GIMG_TIFF_TAG_TILE_WIDTH:
      ifd->tile_width = (uint32_t)tiff_value(st, &e, 0);
      have_tile_width = true;
      break;
    case GIMG_TIFF_TAG_TILE_LENGTH:
      ifd->tile_height = (uint32_t)tiff_value(st, &e, 0);
      have_tile_height = true;
      break;
    case GIMG_TIFF_TAG_STRIP_OFFSETS:
    case GIMG_TIFF_TAG_TILE_OFFSETS:
      gimg_free(st->allocator, strip_offsets);
      r = tiff_value_array(st, &e, &strip_offsets, &strip_offset_count);
      break;
    case GIMG_TIFF_TAG_STRIP_BYTE_COUNTS:
    case GIMG_TIFF_TAG_TILE_BYTE_COUNTS:
      gimg_free(st->allocator, strip_counts);
      r = tiff_value_array(st, &e, &strip_counts, &strip_count_count);
      break;
    case GIMG_TIFF_TAG_X_RESOLUTION:
      if (e.value_bytes >= 8u) {
        ifd->x_res_num = tiff_u32(e.values, st->big_endian);
        ifd->x_res_den = tiff_u32(e.values + 4, st->big_endian);
        ifd->has_x_res = ifd->x_res_den != 0u;
      }
      break;
    case GIMG_TIFF_TAG_Y_RESOLUTION:
      if (e.value_bytes >= 8u) {
        ifd->y_res_num = tiff_u32(e.values, st->big_endian);
        ifd->y_res_den = tiff_u32(e.values + 4, st->big_endian);
        ifd->has_y_res = ifd->y_res_den != 0u;
      }
      break;
    case GIMG_TIFF_TAG_WHITE_POINT:
      // Two RATIONALs: x then y.  A zero denominator is a division this will
      // not do, and a file that writes one has said nothing.
      if (e.count >= 2u && e.value_bytes >= 16u) {
        uint32_t xn = tiff_u32(e.values, st->big_endian);
        uint32_t xd = tiff_u32(e.values + 4, st->big_endian);
        uint32_t yn = tiff_u32(e.values + 8, st->big_endian);
        uint32_t yd = tiff_u32(e.values + 12, st->big_endian);
        if (xd != 0u && yd != 0u) {
          ifd->white_point.x = (double)xn / (double)xd;
          ifd->white_point.y = (double)yn / (double)yd;
          ifd->has_white_point = true;
        }
      }
      break;
    case GIMG_TIFF_TAG_PRIMARY_CHROMATICITIES:
      // Six RATIONALs: red x,y then green then blue.
      if (e.count >= 6u && e.value_bytes >= 48u) {
        bool ok = true;
        GCOL_Chromaticity got[3];
        for (unsigned int i = 0; i < 3u && ok; i++) {
          const unsigned char * v = e.values + ((size_t)i * 16u);
          uint32_t xn = tiff_u32(v, st->big_endian);
          uint32_t xd = tiff_u32(v + 4, st->big_endian);
          uint32_t yn = tiff_u32(v + 8, st->big_endian);
          uint32_t yd = tiff_u32(v + 12, st->big_endian);
          if (xd == 0u || yd == 0u) {
            ok = false;
            break;
          }
          got[i].x = (double)xn / (double)xd;
          got[i].y = (double)yn / (double)yd;
        }
        if (ok) {
          ifd->primaries[0] = got[0];
          ifd->primaries[1] = got[1];
          ifd->primaries[2] = got[2];
          ifd->has_primaries = true;
        }
      }
      break;
    case GIMG_TIFF_TAG_NEW_SUBFILE_TYPE:
      ifd->subfile_type = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_SUB_IFDS:
      // A pyramid's levels hang off the full-size image rather than sitting
      // in the main chain (TIFF Technical Note 1). Both spellings exist and
      // both are read; this is the one that says "these belong to me".
      gimg_free(st->allocator, ifd->sub_ifds);
      r = tiff_value_array(st, &e, &ifd->sub_ifds, &ifd->sub_ifd_count);
      break;
    case GIMG_TIFF_TAG_YCBCR_SUBSAMPLING:
      if (e.count >= 2u) {
        ifd->ycbcr_h = (uint16_t)tiff_value(st, &e, 0);
        ifd->ycbcr_v = (uint16_t)tiff_value(st, &e, 1);
      }
      break;
    case GIMG_TIFF_TAG_YCBCR_COEFFICIENTS:
      if (e.count >= 3u && e.value_bytes >= 24u) {
        double * into[3] = {
            &ifd->luma_red, &ifd->luma_green, &ifd->luma_blue};
        for (size_t k = 0; k < 3u; k++) {
          const uint32_t num = tiff_u32(e.values + (k * 8u), st->big_endian);
          const uint32_t den =
              tiff_u32(e.values + (k * 8u) + 4u, st->big_endian);
          if (den != 0u) {
            *into[k] = (double)num / (double)den;
          }
        }
      }
      break;
    case GIMG_TIFF_TAG_REFERENCE_BLACK_WHITE:
      if (e.count >= 6u && e.value_bytes >= 48u) {
        for (size_t k = 0; k < 6u; k++) {
          const uint32_t num = tiff_u32(e.values + (k * 8u), st->big_endian);
          const uint32_t den =
              tiff_u32(e.values + (k * 8u) + 4u, st->big_endian);
          if (den != 0u) {
            ifd->reference_black_white[k] = (double)num / (double)den;
          }
        }
      }
      break;
    case GIMG_TIFF_TAG_ORIENTATION:
      ifd->orientation = (uint16_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_IMAGE_DESCRIPTION: {
      // ASCII, NUL-terminated in a well-formed file and not always in a real
      // one, so it is copied with a terminator of our own rather than
      // trusted to carry one.
      {
        // Dropped rather than refused when it is merely implausible, which is
        // what the three tags below do and for the same reason: an oversized
        // description says nothing about whether the picture decodes. Before
        // this check it was the one metadata tag here with no bound at all.
        const gimg_metadata_verdict_t v =
            gimg_metadata_verdict(limits, e.value_bytes);
        if (v == GIMG_METADATA_REFUSED) {
          r = GIMG_ERR_LIMIT;
          break;
        }
        if (v == GIMG_METADATA_IMPLAUSIBLE) {
          break;
        }
      }
      gimg_free(st->allocator, ifd->description);
      ifd->description = (char *)gimg_malloc(st->allocator, e.value_bytes + 1u);
      if (!ifd->description) {
        r = GIMG_ERR_OOM;
        break;
      }
      memcpy(ifd->description, e.values, e.value_bytes);
      ifd->description[e.value_bytes] = '\0';
      break;
    }
    case GIMG_TIFF_TAG_ICC_PROFILE: {
      {
        // Past what any real profile is, so the file is describing something
        // other than its own colour. Untagged rather than refused, which is
        // what the BMP loader does with the same case and for the same
        // reason: the picture is not wrong. A cap the caller set is different:
        // they asked to be told.
        const gimg_metadata_verdict_t v =
            gimg_metadata_verdict(limits, e.value_bytes);
        if (v == GIMG_METADATA_REFUSED) {
          r = GIMG_ERR_LIMIT;
          break;
        }
        if (v == GIMG_METADATA_IMPLAUSIBLE) {
          break;
        }
      }
      gimg_free(st->allocator, ifd->icc);
      ifd->icc = (unsigned char *)gimg_malloc(st->allocator, e.value_bytes);
      if (!ifd->icc) {
        r = GIMG_ERR_OOM;
        break;
      }
      memcpy(ifd->icc, e.values, e.value_bytes);
      ifd->icc_size = e.value_bytes;
      break;
    }
    case GIMG_TIFF_TAG_JPEG_TABLES: {
      {
        // Larger than any table stream is; treated as absent.
        const gimg_metadata_verdict_t v =
            gimg_metadata_verdict(limits, e.value_bytes);
        if (v == GIMG_METADATA_REFUSED) {
          r = GIMG_ERR_LIMIT;
          break;
        }
        if (v == GIMG_METADATA_IMPLAUSIBLE) {
          break;
        }
      }
      gimg_free(st->allocator, ifd->jpeg_tables);
      ifd->jpeg_tables =
          (unsigned char *)gimg_malloc(st->allocator, e.value_bytes);
      if (!ifd->jpeg_tables) {
        r = GIMG_ERR_OOM;
        break;
      }
      memcpy(ifd->jpeg_tables, e.values, e.value_bytes);
      ifd->jpeg_tables_size = e.value_bytes;
      break;
    }
    case GIMG_TIFF_TAG_JPEG_PROC:
      ifd->jpeg_proc = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_JPEG_INTERCHANGE_FORMAT:
      ifd->jpeg_interchange_offset = tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_JPEG_INTERCHANGE_LENGTH:
      ifd->jpeg_interchange_size = tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_JPEG_RESTART_INTERVAL:
      ifd->jpeg_restart_interval = (uint32_t)tiff_value(st, &e, 0);
      break;
    case GIMG_TIFF_TAG_JPEG_Q_TABLES:
    case GIMG_TIFF_TAG_JPEG_DC_TABLES:
    case GIMG_TIFF_TAG_JPEG_AC_TABLES: {
      uint64_t * slot = e.tag == GIMG_TIFF_TAG_JPEG_Q_TABLES
          ? ifd->jpeg_q_tables
          : (e.tag == GIMG_TIFF_TAG_JPEG_DC_TABLES ? ifd->jpeg_dc_tables
                                                   : ifd->jpeg_ac_tables);
      uint8_t * count = e.tag == GIMG_TIFF_TAG_JPEG_Q_TABLES
          ? &ifd->jpeg_q_count
          : (e.tag == GIMG_TIFF_TAG_JPEG_DC_TABLES ? &ifd->jpeg_dc_count
                                                   : &ifd->jpeg_ac_count);
      const size_t n = e.count < GIMG_TIFF_JPEG_MAX_TABLES
          ? e.count
          : GIMG_TIFF_JPEG_MAX_TABLES;
      for (size_t k = 0; k < n; k++) {
        slot[k] = tiff_value(st, &e, k);
      }
      *count = (uint8_t)n;
      break;
    }
    case GIMG_TIFF_TAG_XMP: {
      {
        const gimg_metadata_verdict_t v =
            gimg_metadata_verdict(limits, e.value_bytes);
        if (v == GIMG_METADATA_REFUSED) {
          r = GIMG_ERR_LIMIT;
          break;
        }
        if (v == GIMG_METADATA_IMPLAUSIBLE) {
          break;
        }
      }
      gimg_free(st->allocator, ifd->xmp);
      ifd->xmp = (unsigned char *)gimg_malloc(st->allocator, e.value_bytes);
      if (!ifd->xmp) {
        r = GIMG_ERR_OOM;
        break;
      }
      memcpy(ifd->xmp, e.values, e.value_bytes);
      ifd->xmp_size = e.value_bytes;
      break;
    }
    case GIMG_TIFF_TAG_COLOR_MAP: {
      // Three runs of 2^BitsPerSample entries: all reds, then greens, then
      // blues, each a 16-bit value (section 8).
      size_t bytes = 0;
      if (!gcu_safe_mul_size((size_t)e.count, sizeof(uint16_t), &bytes)) {
        r = GIMG_ERR_LIMIT;
        break;
      }
      gimg_free(st->allocator, ifd->color_map);
      ifd->color_map = (uint16_t *)gimg_malloc(st->allocator, bytes);
      if (!ifd->color_map) {
        r = GIMG_ERR_OOM;
        break;
      }
      for (size_t k = 0; k < e.count; k++) {
        ifd->color_map[k] = (uint16_t)tiff_value(st, &e, k);
      }
      ifd->color_map_count = e.count;
      // TIFF 6.0 section 8 says a ColorMap entry is sixteen bits, and a great
      // many writers store an eight-bit value in the field anyway. Read
      // strictly, such a map makes every colour 1/257th of what was meant and
      // the picture comes back essentially black - not subtly wrong, grossly
      // wrong, and wrong in a way the file gives no other clue about.
      //
      // libtiff guesses, and this codec now guesses the same way, because
      // reading the specification and reading the files disagree here and the
      // files are what a caller has. Measured 2026-09-26 against libtiff
      // 4.7.0 in the pinned image: given a map of 0..255 it answers 255 where
      // this codec answered 1.
      //
      // The guess is safe in the one direction that matters. A map that is
      // genuinely sixteen-bit *and* has every entry below 256 describes an
      // image whose brightest colour is 0.39% of full scale - black to any
      // eye - so misreading it costs a picture that was already black and
      // gains every picture written by the majority of encoders.
      ifd->color_map_is_8bit = true;
      for (size_t k = 0; k < ifd->color_map_count; k++) {
        if (ifd->color_map[k] > 255u) {
          ifd->color_map_is_8bit = false;
          break;
        }
      }
      break;
    }
    default:
      break; // A tag this codec does not read.
    }
  }

  ifd->tiled = have_tile_width && have_tile_height;

  if (r == GIMG_OK) {
    // A strip and a tile are the same thing to everything downstream: a block
    // of pixels at an offset, with a length.  They are kept in one pair of
    // arrays so that assembling the image is one loop rather than two.
    if (strip_offset_count == 0u || strip_count_count != strip_offset_count) {
      tiff_diag(diag, at,
          "the offsets and the byte counts do not describe the same blocks");
      r = GIMG_ERR_CORRUPT;
    }
    else {
      ifd->block_offsets = strip_offsets;
      ifd->block_byte_counts = strip_counts;
      ifd->block_count = strip_offset_count;
      strip_offsets = NULL;
      strip_counts = NULL;
    }
  }
  gimg_free(st->allocator, strip_offsets);
  gimg_free(st->allocator, strip_counts);
  if (r != GIMG_OK) {
    tiff_free_ifd(st->allocator, ifd);
  }
  return r;
}

// ---------------------------------------------------------------------------
// What this codec can decode
//
// Refused at load rather than at decode, because gimg_item_decode() has no
// diagnostics parameter and a file rejected for its structure should say
// which rule it broke.
// ---------------------------------------------------------------------------

/**
 * How many blocks the image's geometry implies, so a file that names a
 * different number is saying something inconsistent about itself.
 *
 * PlanarConfiguration 2 stores one whole set of strips or tiles per sample
 * (section 8), so the count multiplies by SamplesPerPixel - which is also
 * what makes the block index enough to say which channel a block carries.
 */
static bool tiff_expected_block_count(
    gimg_tiff_ifd_t * ifd, size_t * out_count) {
  size_t per_plane = 0;
  if (ifd->tiled) {
    if (ifd->tile_width == 0u || ifd->tile_height == 0u) {
      return false;
    }
    const size_t across =
        ((size_t)ifd->width + ifd->tile_width - 1u) / ifd->tile_width;
    const size_t down =
        ((size_t)ifd->height + ifd->tile_height - 1u) / ifd->tile_height;
    if (!gcu_safe_mul_size(across, down, &per_plane)) {
      return false;
    }
  }
  else {
    if (ifd->rows_per_strip == 0u) {
      return false;
    }
    // A strip cannot be taller than the image. The default is 2^32-1, which
    // means "the whole thing in one strip", and a hostile file can say
    // anything at all; clamping here means nothing downstream has to wonder
    // whether RowsPerStrip or the height is the real bound.
    if (ifd->rows_per_strip > ifd->height) {
      ifd->rows_per_strip = ifd->height;
    }
    const uint32_t rows = ifd->rows_per_strip;
    per_plane = ((size_t)ifd->height + rows - 1u) / rows;
  }
  ifd->blocks_per_plane = per_plane;
  if (ifd->planar_config != 2u) {
    *out_count = per_plane;
    return true;
  }
  return gcu_safe_mul_size(per_plane, ifd->samples_per_pixel, out_count);
}

static GIMG_Result tiff_check_supported(const gimg_tiff_doc_state_t * st,
    gimg_tiff_ifd_t * ifd, const GIMG_Limits * limits,
    GIMG_Diagnostics * diag, size_t which) {
  if (ifd->width == 0u || ifd->height == 0u) {
    tiff_diag(diag, which, "an image of zero width or height");
    return GIMG_ERR_CORRUPT;
  }
  if (ifd->photometric == 0xFFFFu) {
    tiff_diag(diag, which,
        "no PhotometricInterpretation, which has no default (TIFF 6.0 8)");
    return GIMG_ERR_CORRUPT;
  }
  if (!gimg_tiff_compression_known(ifd->compression)) {
    tiff_diag(diag, which,
        "a compression method this codec does not undo yet");
    return GIMG_ERR_UNSUPPORTED;
  }
  if (ifd->compression == GIMG_TIFF_COMPRESSION_JPEG ||
      ifd->compression == GIMG_TIFF_COMPRESSION_JPEG_OLD) {
    if (ifd->compression == GIMG_TIFF_COMPRESSION_JPEG_OLD &&
        ifd->jpeg_proc != 0u && ifd->jpeg_proc != 1u) {
      // JPEGProc 14 is lossless, which the 1992 tags describe differently
      // again - a different scan header, no quantization, a predictor
      // selector. Nothing in the sample set uses it and this does not guess.
      tiff_diag(diag, which,
          "old-style JPEG with a JPEGProc other than baseline");
      return GIMG_ERR_UNSUPPORTED;
    }
    // The frame header inside each strip carries the depth, the component
    // count and the subsampling, and the JPEG decoder reads all three. What
    // the TIFF tags have to do is agree about the shape of the result, since
    // that is what the raster was sized from.
    if (ifd->bits_per_sample != 8u) {
      tiff_diag(diag, which,
          "JPEG-in-TIFF at a depth other than eight bits; the technical note "
          "that defines compression 7 gives it eight");
      return GIMG_ERR_UNSUPPORTED;
    }
    if (ifd->planar_config != 1u) {
      // PlanarConfiguration 2 with JPEG means one frame per plane, which the
      // technical note allows and nothing writes.
      tiff_diag(diag, which,
          "JPEG-in-TIFF with PlanarConfiguration 2, which this codec does "
          "not undo");
      return GIMG_ERR_UNSUPPORTED;
    }
  }
  if (ifd->compression == GIMG_TIFF_COMPRESSION_THUNDERSCAN &&
      (ifd->bits_per_sample != 4u || ifd->samples_per_pixel != 1u)) {
    // ThunderScan is four-bit greyscale and nothing else; the coding has no
    // way to say anything wider.
    tiff_diag(diag, which,
        "ThunderScan on something other than four bits of one sample");
    return GIMG_ERR_CORRUPT;
  }
  const bool ccitt = ifd->compression == GIMG_TIFF_COMPRESSION_CCITT_RLE ||
      ifd->compression == GIMG_TIFF_COMPRESSION_CCITT_T4 ||
      ifd->compression == GIMG_TIFF_COMPRESSION_CCITT_T6;
  if (ccitt) {
    // CCITT codes runs of black and white and nothing else, so a file that
    // declares it beside anything but one bit of one sample is describing
    // something that cannot exist. Saying so by name beats handing the fax
    // decoder a geometry it would fill with the wrong number of rows.
    if (ifd->bits_per_sample != 1u || ifd->samples_per_pixel != 1u) {
      tiff_diag(diag, which,
          "CCITT compression on something other than one bit of one sample, "
          "which the coding cannot describe (TIFF 6.0 10)");
      return GIMG_ERR_CORRUPT;
    }
    // Uncompressed mode is an escape inside the coded data that switches to
    // literal bits. Nothing writes it - libtiff has never emitted it - and a
    // decoder that skipped the escape would produce plausible-looking
    // rubbish rather than stop, so it is refused where it is declared.
    const uint32_t uncompressed =
        ifd->compression == GIMG_TIFF_COMPRESSION_CCITT_T6
        ? (ifd->t6_options & 2u)
        : (ifd->t4_options & 2u);
    if (uncompressed) {
      tiff_diag(diag, which,
          "CCITT uncompressed mode, which this codec does not undo");
      return GIMG_ERR_UNSUPPORTED;
    }
  }
  if (ifd->predictor != 1u && ifd->predictor != 2u) {
    tiff_diag(diag, which, "a Predictor other than 1 or 2; not read yet");
    return GIMG_ERR_UNSUPPORTED;
  }
  if (ifd->predictor == 2u && ifd->bits_per_sample != 8u &&
      ifd->bits_per_sample != 16u) {
    // Horizontal differencing is defined for whole-byte samples. Leaving it
    // undone at another depth would decode as a gradient of noise, which
    // reads as a corrupt file rather than as a missing feature.
    tiff_diag(diag, which,
        "horizontal differencing at a depth it is not defined for");
    return GIMG_ERR_UNSUPPORTED;
  }
  if (ifd->planar_config != 1u && ifd->planar_config != 2u) {
    tiff_diag(diag, which,
        "PlanarConfiguration is neither 1 nor 2 (TIFF 6.0 8)");
    return GIMG_ERR_CORRUPT;
  }
  if (ifd->sample_format != 1u) {
    tiff_diag(diag, which,
        "SampleFormat names something other than an unsigned integer");
    return GIMG_ERR_UNSUPPORTED;
  }
  // Any depth from one to thirty-two. TIFF 6.0 puts no list in section 8 -
  // BitsPerSample is a number - and the reader is a bit-packed one, so six,
  // ten, twelve and fourteen cost nothing beside two and four. Thirty-two is
  // where it stops because that is what one sample fits in here; a file
  // above it is saying something this codec would have to widen its
  // arithmetic for rather than something it is declining to read.
  if (ifd->bits_per_sample == 0u || ifd->bits_per_sample > 32u) {
    // Zero is what the parser leaves when the samples disagree with each
    // other, which is a different statement from "a depth this codec does not
    // read", and the caller is told which.
    tiff_diag(diag, which,
        ifd->bits_per_sample == 0u
            ? "BitsPerSample differs between samples; not read yet"
            : "a bit depth above thirty-two; not read yet");
    return GIMG_ERR_UNSUPPORTED;
  }
  switch (ifd->photometric) {
  case GIMG_TIFF_PHOTOMETRIC_WHITE_IS_ZERO:
  case GIMG_TIFF_PHOTOMETRIC_BLACK_IS_ZERO:
    if (ifd->samples_per_pixel != 1u) {
      tiff_diag(diag, which, "a grayscale image with more than one sample");
      return GIMG_ERR_UNSUPPORTED;
    }
    break;
  case GIMG_TIFF_PHOTOMETRIC_PALETTE: {
    if (ifd->samples_per_pixel != 1u) {
      tiff_diag(diag, which, "a palette image with more than one sample");
      return GIMG_ERR_UNSUPPORTED;
    }
    if (ifd->bits_per_sample > 16u) {
      // 3 * 2**BitsPerSample entries, which above sixteen bits is a colour
      // map larger than any file holds - and the index would be wider than
      // the map this codec can build.
      tiff_diag(diag, which,
          "a palette deeper than sixteen bits; not read yet");
      return GIMG_ERR_UNSUPPORTED;
    }
    // 3 * 2**BitsPerSample entries (section 8). At 16 bits that is 196,608
    // of them, which is legal and is what depth/flower-palette-16.tif has.
    const size_t want = (size_t)3u << ifd->bits_per_sample;
    if (ifd->color_map_count < want) {
      tiff_diag(diag, which,
          "the ColorMap is shorter than the bit depth needs");
      return GIMG_ERR_CORRUPT;
    }
    break;
  }
  case GIMG_TIFF_PHOTOMETRIC_RGB:
    if (ifd->samples_per_pixel != 3u && ifd->samples_per_pixel != 4u) {
      tiff_diag(diag, which, "an RGB image with neither three nor four samples");
      return GIMG_ERR_UNSUPPORTED;
    }
    break;
  case GIMG_TIFF_PHOTOMETRIC_YCBCR:
    if (ifd->samples_per_pixel != 3u) {
      tiff_diag(diag, which, "a YCbCr image without three samples");
      return GIMG_ERR_UNSUPPORTED;
    }
    if (ifd->bits_per_sample != 8u) {
      tiff_diag(diag, which, "YCbCr at a depth other than eight bits");
      return GIMG_ERR_UNSUPPORTED;
    }
    if (ifd->planar_config != 1u) {
      tiff_diag(diag, which, "YCbCr with its channels stored apart");
      return GIMG_ERR_UNSUPPORTED;
    }
    if (ifd->ycbcr_h == 0u || ifd->ycbcr_v == 0u || ifd->ycbcr_h > 4u ||
        ifd->ycbcr_v > 4u) {
      tiff_diag(diag, which, "a YCbCr subsampling the format does not define");
      return GIMG_ERR_CORRUPT;
    }
    // A strip has to hold whole subsampling units, or the last row of one
    // would be split across two strips with nothing to say so (section 21).
    if (!ifd->tiled && (ifd->rows_per_strip % ifd->ycbcr_v) != 0u &&
        ifd->rows_per_strip < ifd->height) {
      tiff_diag(diag, which,
          "RowsPerStrip does not hold whole YCbCr subsampling units");
      return GIMG_ERR_CORRUPT;
    }
    break;
  case GIMG_TIFF_PHOTOMETRIC_CMYK:
    // "Separated" in the specification's words (section 16), and CMYK in
    // every file that uses it. More than four samples means inks this
    // library's CMYK formats have no channel for - a fifth spot colour, or
    // an alpha - and dropping one silently would be a worse answer than
    // saying so.
    if (ifd->samples_per_pixel != 4u) {
      tiff_diag(diag, which,
          "a separated image with other than four inks; not read yet");
      return GIMG_ERR_UNSUPPORTED;
    }
    if (ifd->bits_per_sample != 8u && ifd->bits_per_sample != 16u) {
      // CMYK8 and CMYK16 are the only separated rasters this library has, and
      // unlike grey and RGB there is no widening here that would be exact -
      // an ink at twelve bits is not a CMYK16 sample with the low bits zero.
      tiff_diag(diag, which,
          "a separated image at a depth with no CMYK raster; not read yet");
      return GIMG_ERR_UNSUPPORTED;
    }
    break;
  default:
    tiff_diag(diag, which,
        "a PhotometricInterpretation this codec does not read yet");
    return GIMG_ERR_UNSUPPORTED;
  }

  size_t want_blocks = 0;
  if (!tiff_expected_block_count(ifd, &want_blocks)) {
    tiff_diag(diag, which, "the strip or tile geometry does not add up");
    return GIMG_ERR_CORRUPT;
  }
  if (ifd->block_count != want_blocks) {
    tiff_diag(diag, which,
        "the image names a different number of blocks than its geometry has");
    return GIMG_ERR_CORRUPT;
  }
  // Every block has to lie inside the file.  Checked here, once, so that
  // decode can read it without re-deciding whether the bytes are there. This
  // is the *stored* extent, which for a compressed block is shorter than the
  // pixels it expands to.
  for (size_t i = 0; i < ifd->block_count; i++) {
    size_t end = 0;
    if (!gcu_safe_add_size((size_t)ifd->block_offsets[i],
            (size_t)ifd->block_byte_counts[i], &end) ||
        end > st->file_size) {
      tiff_diag(diag, which, "a strip or tile lies outside the file");
      return GIMG_ERR_CORRUPT;
    }
  }

  size_t pixels = 0;
  if (gimg_safe_pixel_count(ifd->width, ifd->height, &pixels) != GIMG_OK) {
    tiff_diag(diag, which, "the pixel count overflows");
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_decoded_pixels &&
      pixels > limits->max_decoded_pixels) {
    tiff_diag(diag, which, "increase max_decoded_pixels");
    return GIMG_ERR_LIMIT;
  }
  return GIMG_OK;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------

/**
 * Read one directory into a new slot, check it, and say where it landed.
 *
 * Grows the array, reads, and validates - the three things that used to be
 * spelled inline in the walk and had to be spelled a second time the moment
 * a SubIFD could also be a directory. One function, so a check added to it
 * cannot be added to only one kind of directory.
 */
static GIMG_Result tiff_append_ifd(GIMG_Codec * codec,
    gimg_tiff_doc_state_t * st, uint32_t at, const GIMG_Limits * limits,
    GIMG_Diagnostics * diagnostics, size_t * out_index, uint32_t * out_next) {
  *out_index = 0;
  *out_next = 0u;
  if (st->ifd_count >= GIMG_TIFF_MAX_IFDS) {
    tiff_diag(diagnostics, at, "more directories than this codec chains");
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_frame_count &&
      st->ifd_count >= limits->max_frame_count) {
    tiff_diag(diagnostics, at, "increase max_frame_count");
    return GIMG_ERR_LIMIT;
  }
  size_t bytes = 0;
  if (!gcu_safe_mul_size(st->ifd_count + 1u, sizeof(gimg_tiff_ifd_t),
          &bytes)) {
    return GIMG_ERR_LIMIT;
  }
  gimg_tiff_ifd_t * grown =
      (gimg_tiff_ifd_t *)gimg_realloc(st->allocator, st->ifds, bytes);
  if (!grown) {
    return GIMG_ERR_OOM;
  }
  st->ifds = grown;

  GIMG_Result r = tiff_read_ifd(
      st, at, limits, diagnostics, &st->ifds[st->ifd_count], out_next);
  if (r != GIMG_OK) {
    // The slot owns nothing: tiff_read_ifd frees what it took before
    // returning a failure, and ifd_count still excludes it.
    return r;
  }
  st->ifd_count++;
  *out_index = st->ifd_count - 1u;
  r = tiff_check_supported(st, &st->ifds[*out_index], limits, diagnostics,
      *out_index);
  (void)codec;
  return r;
}

GIMG_Result gimg_tiff_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(codec->allocator);
  const GIMG_Limits * limits = options ? options->limits : NULL;

  const size_t size = gimg_stream_size(stream);
  if (size == GIMG_STREAM_SIZE_UNKNOWN) {
    // Everything in a TIFF is found by absolute offset and an offset may
    // point backwards, so the file cannot be read as it arrives.
    tiff_diag(diagnostics, 0u, "a TIFF requires a sized stream");
    return GIMG_ERR_UNSUPPORTED;
  }
  if (size < 8u) {
    tiff_diag(diagnostics, 0u, "truncated header");
    return GIMG_ERR_CORRUPT;
  }

  gimg_tiff_doc_state_t * st = (gimg_tiff_doc_state_t *)gimg_calloc(
      alloc, 1u, sizeof(gimg_tiff_doc_state_t));
  if (!st) {
    return GIMG_ERR_OOM;
  }
  st->allocator = alloc;
  st->file_size = size;
  st->file = (unsigned char *)gimg_malloc(alloc, size);
  if (!st->file) {
    gimg_tiff_free_doc_state(codec, st);
    return GIMG_ERR_OOM;
  }
  GIMG_Result r = gimg_stream_seek(stream, 0u);
  if (r == GIMG_OK) {
    r = gimg_stream_read_exact(stream, st->file, size);
  }
  if (r != GIMG_OK) {
    tiff_diag(diagnostics, 0u, "the file could not be read");
    gimg_tiff_free_doc_state(codec, st);
    return r;
  }

  r = gimg_tiff_read_header(st->file, size, &st->big_endian);
  if (r != GIMG_OK) {
    tiff_diag(diagnostics, 0u,
        r == GIMG_ERR_UNSUPPORTED
            ? "BigTIFF (version 43) is a different format; not read"
            : "the file does not begin with a TIFF header");
    gimg_tiff_free_doc_state(codec, st);
    return r;
  }

  // Walk the chain, refusing one that does not move forward.  An offset that
  // pointed backwards or at itself would loop here for ever.
  //
  // Each directory in the chain is a page. Each SubIFD hanging off one is a
  // reduced-resolution version of it, which the document model calls a
  // GIMG_ITEM_LEVEL - the role that existed for this and had nothing setting
  // it until now. A directory in the *chain* whose NewSubfileType says
  // "reduced resolution" is the older spelling of the same thing and belongs
  // to the last full-size page seen.
  uint32_t at = tiff_u32(st->file + 4, st->big_endian);
  uint32_t previous = 0u;
  size_t last_full_size = 0;
  while (at != 0u) {
    if (st->ifd_count > 0u && at <= previous) {
      tiff_diag(diagnostics, at, "the directory chain does not advance");
      gimg_tiff_free_doc_state(codec, st);
      return GIMG_ERR_CORRUPT;
    }
    size_t index = 0;
    uint32_t next = 0u;
    r = tiff_append_ifd(
        codec, st, at, limits, diagnostics, &index, &next);
    if (r != GIMG_OK) {
      gimg_tiff_free_doc_state(codec, st);
      return r;
    }
    if ((st->ifds[index].subfile_type & 1u) != 0u && st->ifd_count > 1u) {
      st->ifds[index].role = GIMG_ITEM_LEVEL;
      st->ifds[index].role_subject = last_full_size;
    }
    else {
      last_full_size = index;
    }

    // The levels this page names, if any. They are read after the page so
    // that the page's own index is the subject they point at, and their own
    // "next" offsets are ignored: a SubIFD chain is a list held by the tag,
    // not a continuation of the file's main chain.
    const size_t levels = st->ifds[index].sub_ifd_count;
    for (size_t k = 0; k < levels && r == GIMG_OK; k++) {
      const uint64_t where = st->ifds[index].sub_ifds[k];
      if (where == 0u || where > 0xFFFFFFFFu) {
        continue;
      }
      size_t level_index = 0;
      uint32_t ignored = 0u;
      r = tiff_append_ifd(codec, st, (uint32_t)where, limits, diagnostics,
          &level_index, &ignored);
      if (r == GIMG_OK) {
        st->ifds[level_index].role = GIMG_ITEM_LEVEL;
        st->ifds[level_index].role_subject = index;
      }
    }
    if (r != GIMG_OK) {
      gimg_tiff_free_doc_state(codec, st);
      return r;
    }
    previous = at;
    at = next;
  }

  if (st->ifd_count == 0u) {
    tiff_diag(diagnostics, 4u, "no image directories in the file");
    gimg_tiff_free_doc_state(codec, st);
    return GIMG_ERR_CORRUPT;
  }

  GIMG_Doc * doc = NULL;
  r = gimg_doc_create_with_allocator(alloc, &doc);
  if (r != GIMG_OK) {
    gimg_tiff_free_doc_state(codec, st);
    return r;
  }
  r = gimg_doc_set_item_count(doc, st->ifd_count);
  if (r != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_tiff_free_doc_state(codec, st);
    return r;
  }
  // A page is a picture in its own right and keeps the default
  // GIMG_ITEM_IMAGE; a reduced-resolution subfile is a GIMG_ITEM_LEVEL of the
  // page it belongs to. See the note on the decode model: one item is one
  // picture, and a pyramid level is not another picture.
  for (size_t i = 0; i < st->ifd_count; i++) {
    if (st->ifds[i].role == GIMG_ITEM_LEVEL) {
      gimg_item_set_role(gimg_doc_item(doc, i), GIMG_ITEM_LEVEL,
          st->ifds[i].role_subject);
    }
  }

  const gimg_tiff_ifd_t * first = &st->ifds[0];

  // Metadata comes from the first directory, which is the one a caller
  // showing a single image sees.
  //
  // Orientation, the description and XMP reach the document; the ICC profile
  // does not, because a profile describes one picture's colours and a
  // multi-page TIFF's pages may each have their own. It is applied to the
  // raster at decode instead, beside the colour info it belongs to.
  if (first->orientation >= 1u && first->orientation <= 8u) {
    GIMG_Meta_Common * common = NULL;
    if (gimg_doc_ensure_meta_common(doc, &common) == GIMG_OK && common) {
      gimg_meta_common_set_orientation(
          common, (GIMG_Orientation)first->orientation);
    }
  }
  if (first->description && first->description[0] != '\0') {
    GIMG_Meta_Common * common = NULL;
    if (gimg_doc_ensure_meta_common(doc, &common) == GIMG_OK && common) {
      (void)gimg_meta_common_set_description(common, first->description);
    }
  }
  if (first->xmp && first->xmp_size > 0u) {
    GIMG_Meta_Raw * raw = NULL;
    if (gimg_doc_ensure_meta_raw(doc, &raw) == GIMG_OK && raw) {
      (void)gimg_meta_raw_attach(
          raw, "tiff", GIMG_TIFF_TAG_XMP, first->xmp, first->xmp_size);
    }
  }

  // Only inches convert to DPI; centimetres and "no unit" say something this
  // field cannot carry.
  if (first->resolution_unit == 2u && first->has_x_res && first->has_y_res) {
    GIMG_Meta_Common * common = NULL;
    if (gimg_doc_ensure_meta_common(doc, &common) == GIMG_OK && common) {
      gimg_meta_common_set_dpi(common,
          first->x_res_num / first->x_res_den,
          first->y_res_num / first->y_res_den);
    }
  }

  doc->loaded_by_codec = codec;
  doc->codec_private = st;
  *out_doc = doc;
  return GIMG_OK;
}
