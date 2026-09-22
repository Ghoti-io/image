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
 * Exif (eXIf / TIFF-IFD) parse, strip GPS, and normalize.
 *
 * - Parse: read orientation (and other tags) from IFD0 for metadata common.
 * - STRIP_GPS: remove only the GPS IFD (tag 0x8825) and its payload;
 * re-serialize the rest so non-GPS Exif (orientation, datetime, etc.) is
 * preserved. Used by GIMG_META_STRIP_GPS on save.
 * - NORMALIZE_EXIF: copy blob and set orientation tag to 1 (normal) if present;
 *   used by GIMG_META_NORMALIZE_EXIF so saved eXIf has canonical orientation.
 */

#include <stdbool.h>
#include <string.h>

#include <ghoti.io/image/macros.h>
#include "../core/alloc_internal.h"
#include "exif_internal.h"

// Minimum size: TIFF header (8) + IFD at least 2 + 0 entries + 4 = 14.
#define GIMG_EXIF_MIN_SIZE 14u

/**
 * Does a run of @a length bytes at @a offset lie inside a buffer of @a size?
 *
 * Both the offset and the entry count come out of the file, and an IFD offset
 * is a full 32 bits. Adding them in 32-bit arithmetic lets a large offset wrap
 * to a small one and pass a bounds test it should fail - which is how a
 * crafted offset reached a long way past the end of the buffer and segfaulted
 * in read_u16. uint64_t holds every sum these callers can form (an offset
 * below 2^32 plus at most 65535 entries of twelve bytes), so nothing wraps.
 */
static bool exif_region_fits(uint64_t offset, uint64_t length, size_t size) {
  return offset + length <= (uint64_t)size;
}

static uint16_t read_u16(const unsigned char * p, int little) {
  if (little) {
    return (uint16_t)(p[0] | (p[1] << 8));
  }
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_u32(const unsigned char * p, int little) {
  // Each byte is widened before it is shifted.  An unsigned char promotes to
  // int, so p[x] << 24 is a signed shift, and 255 << 24 does not fit an int:
  // that is undefined behavior, not merely a wrap, and every one of the four
  // fuzz harnesses reported it - any file with 0xFF in the top byte of any
  // 32-bit EXIF field reaches here, which includes every EXIF block a fuzzer
  // has touched.  The value produced is the same on any compiler anyone uses;
  // what changes is that the arithmetic is now defined.  read_u16 needs no
  // such care: its widest shift is 255 << 8, which fits.
  if (little) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
        ((uint32_t)p[3] << 24);
  }
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
      ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/**
 * Read a TIFF header's byte-order mark and magic number, together.
 *
 * TIFF 6.0 section 2 gives the first two bytes exactly two legal values, "II"
 * for little-endian and "MM" for big-endian, and the 16-bit 42 that follows is
 * written in *that* order - 2A 00 one way round, 00 2A the other.
 *
 * Every entry point here used to test the magic before establishing the order,
 * and test it only in its little-endian spelling, which rejected every
 * big-endian file at the door however carefully the code below carried the
 * flag around. The two questions are one question and are answered here once.
 *
 * A mark that is neither "II" nor "MM" is refused rather than assumed. The
 * old code treated anything that was not "II" as big-endian, so a garbage mark
 * followed by the little-endian spelling of 42 was accepted and then had every
 * one of its fields read backwards.
 *
 * @return false if this is not a TIFF header; otherwise true, with @p out_le
 *   set to 1 for little-endian and 0 for big-endian.
 */
static bool exif_read_header(const unsigned char * h, int * out_le) {
  int le;
  if (h[0] == 0x49u && h[1] == 0x49u) {
    le = 1; // "II"
  }
  else if (h[0] == 0x4Du && h[1] == 0x4Du) {
    le = 0; // "MM"
  }
  else {
    return false;
  }
  if (read_u16(h + 2, le) != 42u) {
    return false;
  }
  *out_le = le;
  return true;
}

static void write_u16(unsigned char * p, uint16_t v, int little) {
  if (little) {
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
  }
  else {
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v);
  }
}

static void write_u32(unsigned char * p, uint32_t v, int little) {
  if (little) {
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
  }
  else {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)(v);
  }
}

GIMG_Result gimg_exif_parse_orientation(
    const void * exif, size_t size, GIMG_Orientation * out) {
  if (!exif || size < GIMG_EXIF_MIN_SIZE || !out) {
    return GIMG_ERR_INTERNAL;
  }
  *out = GIMG_ORIENTATION_UNKNOWN;
  const unsigned char * buf = (const unsigned char *)exif;
  int le = 0;
  if (!exif_read_header(buf, &le)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd0 = read_u32(buf + 4, le);
  if (!exif_region_fits(ifd0, 2u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num_entries = read_u16(buf + ifd0, le);
  if (!exif_region_fits(ifd0, 2u + (uint64_t)num_entries * 12u + 4u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  for (uint16_t i = 0; i < num_entries; i++) {
    size_t off = ifd0 + 2 + (size_t)i * 12;
    uint16_t tag = read_u16(buf + off, le);
    if (tag != GIMG_EXIF_TAG_ORIENTATION) {
      continue;
    }
    uint16_t type = read_u16(buf + off + 2, le);
    uint32_t count = read_u32(buf + off + 4, le);
    if (type != GIMG_EXIF_TYPE_SHORT || count != 1) {
      continue;
    }
    uint16_t val = read_u16(buf + off + 8, le);
    if (val >= 1 && val <= 8) {
      *out = (GIMG_Orientation)val;
    }
    return GIMG_OK;
  }
  return GIMG_OK;
}


/** TIFF field type sizes in bytes; 0 for a type this code does not know. */
static size_t exif_type_size(uint16_t type) {
  switch (type) {
    case 1: /* BYTE */
    case 2: /* ASCII */
    case 6: /* SBYTE */
    case 7: /* UNDEFINED */
      return 1u;
    case 3: /* SHORT */
    case 8: /* SSHORT */
      return 2u;
    case 4: /* LONG */
    case 9: /* SLONG */
    case 11: /* FLOAT */
      return 4u;
    case 5: /* RATIONAL */
    case 10: /* SRATIONAL */
    case 12: /* DOUBLE */
      return 8u;
    default:
      return 0u;
  }
}

/** The IFDs a strip walks, in the order they are written. */
enum {
  EXIF_IFD_0 = 0,
  EXIF_IFD_EXIF,
  EXIF_IFD_INTEROP,
  EXIF_IFD_1,
  EXIF_IFD_COUNT
};

typedef struct {
  uint32_t src; /**< Offset of this IFD in the source blob. */
  uint32_t dst; /**< Offset it is being written to. */
  uint16_t n_src;
  uint16_t n_dst;
  bool present;
} exif_ifd_plan_t;

/** Read an IFD header, checking the whole directory lies inside the blob. */
static bool exif_plan_ifd(const unsigned char * buf, size_t size, int le,
    uint32_t off, exif_ifd_plan_t * plan) {
  if (off == 0u || !exif_region_fits(off, 2u, size)) {
    return false;
  }
  const uint16_t n = read_u16(buf + off, le);
  if (!exif_region_fits(off, 2u + (uint64_t)n * 12u + 4u, size)) {
    return false;
  }
  plan->src = off;
  plan->dst = 0u;
  plan->n_src = n;
  plan->n_dst = n;
  plan->present = true;
  return true;
}

/** Value of a SHORT or LONG tag with count 1, or `def` when absent. */
static uint32_t exif_find_uint(const unsigned char * buf, int le,
    const exif_ifd_plan_t * ifd, uint16_t tag, uint32_t def) {
  for (uint16_t i = 0; i < ifd->n_src; i++) {
    const size_t off = ifd->src + 2u + (size_t)i * 12u;
    if (read_u16(buf + off, le) != tag) {
      continue;
    }
    const uint16_t type = read_u16(buf + off + 2, le);
    if (read_u32(buf + off + 4, le) != 1u) {
      continue;
    }
    if (type == GIMG_EXIF_TYPE_LONG) {
      return read_u32(buf + off + 8, le);
    }
    if (type == GIMG_EXIF_TYPE_SHORT) {
      return read_u16(buf + off + 8, le);
    }
  }
  return def;
}

/**
 * Bytes this entry keeps outside the 4-byte value field, and where they are.
 *
 * Returns 0 for an entry whose value is inline. The thumbnail pointer is the
 * one entry whose payload size is not derivable from its own type and count -
 * it is a LONG, so it looks inline - and the caller passes its length in.
 */
static size_t exif_entry_payload(const unsigned char * buf, size_t size,
    int le, size_t ent, uint32_t * out_src) {
  const uint16_t type = read_u16(buf + ent + 2, le);
  const uint32_t count = read_u32(buf + ent + 4, le);
  const size_t ts = exif_type_size(type);
  if (ts == 0u) {
    return 0u; // Unknown type: keep the four value bytes as they stand.
  }
  const uint64_t bytes = (uint64_t)ts * count;
  if (bytes <= 4u) {
    return 0u;
  }
  const uint32_t src = read_u32(buf + ent + 8, le);
  if (!exif_region_fits(src, bytes, size)) {
    return 0u; // Out of bounds: treated as inline rather than trusted.
  }
  *out_src = src;
  return (size_t)bytes;
}

GIMG_Result gimg_exif_strip_gps(const GIMG_Allocator * allocator,
    const void * exif, size_t size, void ** out, size_t * out_size) {
  if (!exif || !out || !out_size) {
    return GIMG_ERR_INTERNAL; // A null argument is the caller's bug.
  }
  *out = NULL;
  *out_size = 0;
  if (size < GIMG_EXIF_MIN_SIZE) {
    // Too short to hold even a TIFF header, so there is nothing here that can
    // be parsed and nothing that can be asserted about it.  Corrupt rather
    // than internal: this is a statement about the data, and it is what this
    // function's documented contract already said it would return.
    return GIMG_ERR_CORRUPT;
  }
  const unsigned char * buf = (const unsigned char *)exif;
  int le = 0;
  if (!exif_read_header(buf, &le)) {
    return GIMG_ERR_CORRUPT;
  }
  const uint32_t ifd0_off = read_u32(buf + 4, le);

  exif_ifd_plan_t ifd[EXIF_IFD_COUNT];
  memset(ifd, 0, sizeof(ifd));
  if (!exif_plan_ifd(buf, size, le, ifd0_off, &ifd[EXIF_IFD_0])) {
    return GIMG_ERR_CORRUPT;
  }

  // Is there a GPS pointer to remove at all?
  const uint32_t gps_off = exif_find_uint(
      buf, le, &ifd[EXIF_IFD_0], GIMG_EXIF_TAG_GPS_IFD, 0u);
  if (gps_off == 0u) {
    // Nothing to strip: hand back a copy, as the contract promises.
    allocator = gimg_alloc_or_default(allocator);
    unsigned char * copy = (unsigned char *)gimg_malloc(allocator, size);
    if (!copy) {
      return GIMG_ERR_OOM;
    }
    memcpy(copy, exif, size);
    *out = copy;
    *out_size = size;
    return GIMG_OK;
  }
  ifd[EXIF_IFD_0].n_dst = (uint16_t)(ifd[EXIF_IFD_0].n_src - 1u);

  // The rest of the standard tree. Each is optional; a pointer that does not
  // resolve is dropped rather than followed, which loses the sub-IFD but
  // cannot produce a blob pointing outside itself.
  const uint32_t exif_sub =
      exif_find_uint(buf, le, &ifd[EXIF_IFD_0], GIMG_EXIF_TAG_EXIF_IFD, 0u);
  if (exif_sub != 0u) {
    (void)exif_plan_ifd(buf, size, le, exif_sub, &ifd[EXIF_IFD_EXIF]);
  }
  if (ifd[EXIF_IFD_EXIF].present) {
    const uint32_t interop = exif_find_uint(
        buf, le, &ifd[EXIF_IFD_EXIF], GIMG_EXIF_TAG_INTEROP_IFD, 0u);
    if (interop != 0u) {
      (void)exif_plan_ifd(buf, size, le, interop, &ifd[EXIF_IFD_INTEROP]);
    }
  }
  const uint32_t ifd1_off = read_u32(
      buf + ifd[EXIF_IFD_0].src + 2u + (size_t)ifd[EXIF_IFD_0].n_src * 12u, le);
  if (ifd1_off != 0u) {
    (void)exif_plan_ifd(buf, size, le, ifd1_off, &ifd[EXIF_IFD_1]);
  }

  // Thumbnail bytes are found through a length in a second tag, so they are
  // measured here and carried to the emit pass.
  uint32_t thumb_src = 0u;
  uint32_t thumb_len = 0u;
  if (ifd[EXIF_IFD_1].present) {
    thumb_src = exif_find_uint(
        buf, le, &ifd[EXIF_IFD_1], GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT, 0u);
    thumb_len = exif_find_uint(
        buf, le, &ifd[EXIF_IFD_1], GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH, 0u);
    if (thumb_src == 0u || thumb_len == 0u ||
        !exif_region_fits(thumb_src, thumb_len, size)) {
      thumb_src = 0u;
      thumb_len = 0u;
    }
  }

  // Lay the directories out first: every payload offset is relative to the
  // end of the last one, so the sizes have to be known before anything can
  // be written.
  uint64_t cursor = 8u;
  for (int k = 0; k < EXIF_IFD_COUNT; k++) {
    if (!ifd[k].present) {
      continue;
    }
    ifd[k].dst = (uint32_t)cursor;
    cursor += 2u + (uint64_t)ifd[k].n_dst * 12u + 4u;
  }
  const uint64_t payload_base = cursor;

  // Measure the payloads that survive.
  uint64_t payload_bytes = 0u;
  for (int k = 0; k < EXIF_IFD_COUNT; k++) {
    if (!ifd[k].present) {
      continue;
    }
    for (uint16_t i = 0; i < ifd[k].n_src; i++) {
      const size_t ent = ifd[k].src + 2u + (size_t)i * 12u;
      const uint16_t tag = read_u16(buf + ent, le);
      if (k == EXIF_IFD_0 && tag == GIMG_EXIF_TAG_GPS_IFD) {
        continue;
      }
      if (k == EXIF_IFD_1 && tag == GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT) {
        payload_bytes += thumb_len;
        continue;
      }
      uint32_t psrc = 0u;
      payload_bytes += exif_entry_payload(buf, size, le, ent, &psrc);
    }
  }
  const uint64_t total = payload_base + payload_bytes;
  if (total > (uint64_t)UINT32_MAX) {
    return GIMG_ERR_LIMIT;
  }

  allocator = gimg_alloc_or_default(allocator);
  unsigned char * dst = (unsigned char *)gimg_malloc(allocator, (size_t)total);
  if (!dst) {
    return GIMG_ERR_OOM;
  }
  memset(dst, 0, (size_t)total);

  // Header: same byte order as the source, IFD0 wherever it landed.
  dst[0] = buf[0];
  dst[1] = buf[1];
  write_u16(dst + 2, 42u, le);
  write_u32(dst + 4, ifd[EXIF_IFD_0].dst, le);

  uint64_t pay = payload_base;
  for (int k = 0; k < EXIF_IFD_COUNT; k++) {
    if (!ifd[k].present) {
      continue;
    }
    unsigned char * d = dst + ifd[k].dst;
    write_u16(d, ifd[k].n_dst, le);
    size_t w = 2u;
    for (uint16_t i = 0; i < ifd[k].n_src; i++) {
      const size_t ent = ifd[k].src + 2u + (size_t)i * 12u;
      const uint16_t tag = read_u16(buf + ent, le);
      if (k == EXIF_IFD_0 && tag == GIMG_EXIF_TAG_GPS_IFD) {
        continue; // The entry this function exists to remove.
      }
      memcpy(d + w, buf + ent, 12u);
      if (tag == GIMG_EXIF_TAG_EXIF_IFD && ifd[EXIF_IFD_EXIF].present) {
        write_u32(d + w + 8, ifd[EXIF_IFD_EXIF].dst, le);
      }
      else if (tag == GIMG_EXIF_TAG_INTEROP_IFD &&
          ifd[EXIF_IFD_INTEROP].present) {
        write_u32(d + w + 8, ifd[EXIF_IFD_INTEROP].dst, le);
      }
      else if (k == EXIF_IFD_1 && tag == GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT) {
        if (thumb_len != 0u) {
          memcpy(dst + pay, buf + thumb_src, thumb_len);
          write_u32(d + w + 8, (uint32_t)pay, le);
          pay += thumb_len;
        }
        else {
          write_u32(d + w + 8, 0u, le);
        }
      }
      else {
        uint32_t psrc = 0u;
        const size_t n = exif_entry_payload(buf, size, le, ent, &psrc);
        if (n != 0u) {
          memcpy(dst + pay, buf + psrc, n);
          write_u32(d + w + 8, (uint32_t)pay, le);
          pay += n;
        }
      }
      w += 12u;
    }
    // Only IFD0 chains onward, and only to IFD1.
    const uint32_t next =
        (k == EXIF_IFD_0 && ifd[EXIF_IFD_1].present) ? ifd[EXIF_IFD_1].dst : 0u;
    write_u32(d + w, next, le);
  }

  *out = dst;
  *out_size = (size_t)total;
  return GIMG_OK;
}

GIMG_Result gimg_exif_normalize(const GIMG_Allocator * allocator,
    const void * exif, size_t size, void ** out, size_t * out_size) {
  if (!exif || !out || !out_size) {
    return GIMG_ERR_INTERNAL; // A null argument is the caller's bug.
  }
  *out = NULL;
  *out_size = 0;
  if (size < GIMG_EXIF_MIN_SIZE) {
    return GIMG_ERR_CORRUPT; // See gimg_exif_strip_gps().
  }
  const unsigned char * buf = (const unsigned char *)exif;
  int le = 0;
  if (!exif_read_header(buf, &le)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd0 = read_u32(buf + 4, le);
  if (!exif_region_fits(ifd0, 2u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num_entries = read_u16(buf + ifd0, le);
  if (!exif_region_fits(ifd0, 2u + (uint64_t)num_entries * 12u + 4u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  allocator = gimg_alloc_or_default(allocator);
  unsigned char * dst = (unsigned char *)gimg_malloc(allocator, size);
  if (!dst) {
    return GIMG_ERR_OOM;
  }
  memcpy(dst, exif, size);
  // Set orientation tag (0x0112) to 1 (normal) if present.
  for (uint16_t i = 0; i < num_entries; i++) {
    size_t off = ifd0 + 2 + (size_t)i * 12;
    if (read_u16(dst + off, le) != GIMG_EXIF_TAG_ORIENTATION) {
      continue;
    }
    if (read_u16(dst + off + 2, le) == GIMG_EXIF_TYPE_SHORT &&
        read_u32(dst + off + 4, le) == 1) {
      if (le) {
        dst[off + 8] = 1;
        dst[off + 9] = 0;
      }
      else {
        dst[off + 8] = 0;
        dst[off + 9] = 1;
      }
    }
    break;
  }
  *out = dst;
  *out_size = size;
  return GIMG_OK;
}

GIMG_Result gimg_exif_embedded_thumbnail_jpeg(
    const void * tiff, size_t size, const void ** out_data, size_t * out_size) {
  if (!tiff || !out_data || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_data = NULL;
  *out_size = 0;
  if (size < GIMG_EXIF_MIN_SIZE) {
    return GIMG_OK;
  }
  const unsigned char * buf = (const unsigned char *)tiff;
  int le = 0;
  if (!exif_read_header(buf, &le)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd0 = read_u32(buf + 4, le);
  if (!exif_region_fits(ifd0, 2u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num0 = read_u16(buf + ifd0, le);
  if (!exif_region_fits(ifd0, 2u + (uint64_t)num0 * 12u + 4u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd1 = read_u32(buf + ifd0 + 2 + (size_t)num0 * 12, le);
  if (ifd1 == 0) {
    return GIMG_OK;
  }
  if (!exif_region_fits(ifd1, 2u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num1 = read_u16(buf + ifd1, le);
  if (!exif_region_fits(ifd1, 2u + (uint64_t)num1 * 12u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  int has_compression_jpeg = 0;
  uint32_t jpeg_offset = 0;
  uint32_t jpeg_length = 0;
  for (uint16_t i = 0; i < num1; i++) {
    size_t off = ifd1 + 2 + (size_t)i * 12;
    uint16_t tag = read_u16(buf + off, le);
    uint16_t type = read_u16(buf + off + 2, le);
    uint32_t count = read_u32(buf + off + 4, le);
    if (tag == GIMG_EXIF_TAG_COMPRESSION) {
      if (type == GIMG_EXIF_TYPE_SHORT && count == 1) {
        uint16_t val = read_u16(buf + off + 8, le);
        if (val == 6) {
          has_compression_jpeg = 1;
        }
      }
    }
    else if (tag == GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT) {
      if (type == GIMG_EXIF_TYPE_LONG && count == 1) {
        jpeg_offset = read_u32(buf + off + 8, le);
      }
    }
    else if (tag == GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH) {
      if (type == GIMG_EXIF_TYPE_LONG && count == 1) {
        jpeg_length = read_u32(buf + off + 8, le);
      }
    }
  }
  if (!has_compression_jpeg || jpeg_length == 0) {
    return GIMG_OK;
  }
  if (jpeg_offset > size || jpeg_length > size ||
      jpeg_offset + jpeg_length > size) {
    return GIMG_ERR_CORRUPT;
  }
  *out_data = (const void *)(buf + jpeg_offset);
  *out_size = (size_t)jpeg_length;
  return GIMG_OK;
}

// Resolve TIFF tag value: for count*type_size <= 4, value is inline at off+8;
// otherwise value at off+8 is offset to data.
static uint32_t tag_value_or_offset(const unsigned char * buf,
    GIMG_MAYBE_UNUSED(size_t size), size_t ent_off, int le, uint16_t type,
    uint32_t count) {
  size_t type_size = (type == GIMG_EXIF_TYPE_SHORT) ? 2u : 4u;
  if (type == GIMG_EXIF_TYPE_UNDEFINED) {
    type_size = 1u;
  }
  if (count * type_size <= 4) {
    if (type == GIMG_EXIF_TYPE_SHORT && count >= 1) {
      return (uint32_t)read_u16(buf + ent_off + 8, le);
    }
    if (type == GIMG_EXIF_TYPE_LONG && count >= 1) {
      return read_u32(buf + ent_off + 8, le);
    }
    return 0;
  }
  return read_u32(buf + ent_off + 8, le);
}

// Get pointer to tag data: if inline, return buf+ent_off+8; else return
// buf+offset.
static const unsigned char * tag_data_ptr(const unsigned char * buf,
    size_t ent_off, int le, uint16_t type, uint32_t count) {
  size_t type_size = (type == GIMG_EXIF_TYPE_SHORT) ? 2u : 4u;
  if (type == GIMG_EXIF_TYPE_UNDEFINED) {
    type_size = 1u;
  }
  if (count * type_size <= 4) {
    return buf + ent_off + 8;
  }
  uint32_t off = read_u32(buf + ent_off + 8, le);
  return buf + off;
}

GIMG_Result gimg_exif_embedded_thumbnail_uncompressed(
    const GIMG_Allocator * allocator, const void * tiff, size_t size,
    uint32_t * out_width, uint32_t * out_height, uint8_t * out_bits_per_sample,
    uint16_t * out_photometric, void ** out_data, size_t * out_size) {
  if (!tiff || size < GIMG_EXIF_MIN_SIZE || !out_width || !out_height ||
      !out_bits_per_sample || !out_photometric || !out_data || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_width = 0;
  *out_height = 0;
  *out_bits_per_sample = 0;
  *out_photometric = 0xFFFF;
  *out_data = NULL;
  *out_size = 0;
  const unsigned char * buf = (const unsigned char *)tiff;
  int le = 0;
  if (!exif_read_header(buf, &le)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd0 = read_u32(buf + 4, le);
  if (!exif_region_fits(ifd0, 2u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num0 = read_u16(buf + ifd0, le);
  if (!exif_region_fits(ifd0, 2u + (uint64_t)num0 * 12u + 4u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd1 = read_u32(buf + ifd0 + 2 + (size_t)num0 * 12, le);
  if (ifd1 == 0 || !exif_region_fits(ifd1, 2u, size)) {
    return GIMG_OK;
  }
  uint16_t num1 = read_u16(buf + ifd1, le);
  if (!exif_region_fits(ifd1, 2u + (uint64_t)num1 * 12u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  int compression_1 = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t bits_per_sample = 0;
  uint32_t strip_offsets_val = 0; // inline value or offset to array
  uint32_t strip_byte_counts_val = 0;
  uint16_t strip_count = 0;
  int strip_offsets_long = 0;
  int strip_counts_long = 0;
  uint16_t photometric = 0xFFFF;
  for (uint16_t i = 0; i < num1; i++) {
    size_t off = ifd1 + 2 + (size_t)i * 12;
    uint16_t tag = read_u16(buf + off, le);
    uint16_t type = read_u16(buf + off + 2, le);
    uint32_t count = read_u32(buf + off + 4, le);
    if (tag == GIMG_EXIF_TAG_COMPRESSION) {
      if (type == GIMG_EXIF_TYPE_SHORT && count == 1) {
        if (read_u16(buf + off + 8, le) == 1) {
          compression_1 = 1;
        }
      }
    }
    else if (tag == GIMG_EXIF_TAG_IMAGE_WIDTH) {
      if ((type == GIMG_EXIF_TYPE_LONG || type == GIMG_EXIF_TYPE_SHORT) &&
          count == 1) {
        width = tag_value_or_offset(buf, size, off, le, type, count);
      }
    }
    else if (tag == GIMG_EXIF_TAG_IMAGE_LENGTH) {
      if ((type == GIMG_EXIF_TYPE_LONG || type == GIMG_EXIF_TYPE_SHORT) &&
          count == 1) {
        height = tag_value_or_offset(buf, size, off, le, type, count);
      }
    }
    else if (tag == GIMG_EXIF_TAG_BITS_PER_SAMPLE) {
      if (type == GIMG_EXIF_TYPE_SHORT && count >= 1) {
        bits_per_sample =
            (uint16_t)read_u16(tag_data_ptr(buf, off, le, type, count), le);
      }
    }
    else if (tag == GIMG_EXIF_TAG_STRIP_OFFSETS) {
      if ((type == GIMG_EXIF_TYPE_LONG || type == GIMG_EXIF_TYPE_SHORT) &&
          count >= 1 && count <= 65535u) {
        strip_count = (uint16_t)count;
        strip_offsets_long = (type == GIMG_EXIF_TYPE_LONG);
        strip_offsets_val =
            tag_value_or_offset(buf, size, off, le, type, count);
      }
    }
    else if (tag == GIMG_EXIF_TAG_STRIP_BYTE_COUNTS) {
      if ((type == GIMG_EXIF_TYPE_LONG || type == GIMG_EXIF_TYPE_SHORT) &&
          count >= 1 && count <= 65535u) {
        if (count == strip_count) {
          strip_counts_long = (type == GIMG_EXIF_TYPE_LONG);
          strip_byte_counts_val =
              tag_value_or_offset(buf, size, off, le, type, count);
        }
      }
    }
    else if (tag == GIMG_EXIF_TAG_PHOTOMETRIC_INTERPRETATION) {
      if (type == GIMG_EXIF_TYPE_SHORT && count == 1) {
        photometric = read_u16(buf + off + 8, le);
      }
    }
  }
  if (!compression_1 || width == 0 || height == 0 || strip_count == 0 ||
      bits_per_sample == 0 || bits_per_sample > 16) {
    return GIMG_OK;
  }
  if (photometric > 2) {
    return GIMG_OK;
  }
  size_t total_bytes = 0;
  for (uint16_t s = 0; s < strip_count; s++) {
    uint32_t so, sc;
    if (strip_count == 1) {
      so = strip_offsets_val;
      sc = strip_byte_counts_val;
    }
    else {
      if (strip_offsets_long) {
        if (strip_offsets_val + 4u * (s + 1) > size) {
          return GIMG_ERR_CORRUPT;
        }
        so = read_u32(buf + strip_offsets_val + (size_t)s * 4, le);
      }
      else {
        if (strip_offsets_val + 2u * (s + 1) > size) {
          return GIMG_ERR_CORRUPT;
        }
        so = (uint32_t)read_u16(buf + strip_offsets_val + (size_t)s * 2, le);
      }
      if (strip_counts_long) {
        if (strip_byte_counts_val + 4u * (s + 1) > size) {
          return GIMG_ERR_CORRUPT;
        }
        sc = read_u32(buf + strip_byte_counts_val + (size_t)s * 4, le);
      }
      else {
        if (strip_byte_counts_val + 2u * (s + 1) > size) {
          return GIMG_ERR_CORRUPT;
        }
        sc =
            (uint32_t)read_u16(buf + strip_byte_counts_val + (size_t)s * 2, le);
      }
    }
    if (so > size || sc > size || so + sc > size) {
      return GIMG_ERR_CORRUPT;
    }
    total_bytes += sc;
  }
  allocator = gimg_alloc_or_default(allocator);
  unsigned char * out = (unsigned char *)gimg_malloc(allocator, total_bytes);
  if (!out) {
    return GIMG_ERR_OOM;
  }
  size_t written = 0;
  for (uint16_t s = 0; s < strip_count; s++) {
    uint32_t so, sc;
    if (strip_count == 1) {
      so = strip_offsets_val;
      sc = strip_byte_counts_val;
    }
    else {
      if (strip_offsets_long) {
        so = read_u32(buf + strip_offsets_val + (size_t)s * 4, le);
        sc = read_u32(buf + strip_byte_counts_val + (size_t)s * 4, le);
      }
      else {
        so = (uint32_t)read_u16(buf + strip_offsets_val + (size_t)s * 2, le);
        sc =
            (uint32_t)read_u16(buf + strip_byte_counts_val + (size_t)s * 2, le);
      }
    }
    memcpy(out + written, buf + so, sc);
    written += sc;
  }
  *out_width = width;
  *out_height = height;
  *out_bits_per_sample = (uint8_t)bits_per_sample;
  *out_photometric = photometric;
  *out_data = out;
  *out_size = total_bytes;
  return GIMG_OK;
}

GIMG_Result gimg_exif_embedded_thumbnail_tiff_jpeg(
    const GIMG_Allocator * allocator, const void * tiff, size_t size,
    void ** out_data, size_t * out_size) {
  if (!tiff || !out_data || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out_data = NULL;
  *out_size = 0;
  if (size < GIMG_EXIF_MIN_SIZE) {
    return GIMG_OK;
  }
  const unsigned char * buf = (const unsigned char *)tiff;
  int le = 0;
  if (!exif_read_header(buf, &le)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd0 = read_u32(buf + 4, le);
  if (!exif_region_fits(ifd0, 2u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num0 = read_u16(buf + ifd0, le);
  if (!exif_region_fits(ifd0, 2u + (uint64_t)num0 * 12u + 4u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  uint32_t ifd1 = read_u32(buf + ifd0 + 2 + (size_t)num0 * 12, le);
  if (ifd1 == 0 || !exif_region_fits(ifd1, 2u, size)) {
    return GIMG_OK;
  }
  uint16_t num1 = read_u16(buf + ifd1, le);
  if (!exif_region_fits(ifd1, 2u + (uint64_t)num1 * 12u, size)) {
    return GIMG_ERR_CORRUPT;
  }
  int compression_7 = 0;
  uint32_t strip_offset = 0;
  uint32_t strip_length = 0;
  const unsigned char * jpeg_tables = NULL;
  size_t jpeg_tables_len = 0;
  for (uint16_t i = 0; i < num1; i++) {
    size_t off = ifd1 + 2 + (size_t)i * 12;
    uint16_t tag = read_u16(buf + off, le);
    uint16_t type = read_u16(buf + off + 2, le);
    uint32_t count = read_u32(buf + off + 4, le);
    if (tag == GIMG_EXIF_TAG_COMPRESSION) {
      if (type == GIMG_EXIF_TYPE_SHORT && count == 1) {
        if (read_u16(buf + off + 8, le) == 7) {
          compression_7 = 1;
        }
      }
    }
    else if (tag == GIMG_EXIF_TAG_STRIP_OFFSETS) {
      if ((type == GIMG_EXIF_TYPE_LONG || type == GIMG_EXIF_TYPE_SHORT) &&
          count == 1) {
        strip_offset = tag_value_or_offset(buf, size, off, le, type, count);
      }
    }
    else if (tag == GIMG_EXIF_TAG_STRIP_BYTE_COUNTS) {
      if ((type == GIMG_EXIF_TYPE_LONG || type == GIMG_EXIF_TYPE_SHORT) &&
          count == 1) {
        strip_length = tag_value_or_offset(buf, size, off, le, type, count);
      }
    }
    else if (tag == GIMG_EXIF_TAG_JPEG_TABLES) {
      if (type == GIMG_EXIF_TYPE_UNDEFINED && count > 0) {
        if (count <= 4) {
          jpeg_tables = buf + off + 8;
          jpeg_tables_len = count;
        }
        else {
          uint32_t tab_off = read_u32(buf + off + 8, le);
          if (tab_off > size || count > size || tab_off + count > size) {
            return GIMG_ERR_CORRUPT;
          }
          jpeg_tables = buf + tab_off;
          jpeg_tables_len = count;
        }
      }
    }
  }
  if (!compression_7 || strip_length == 0) {
    return GIMG_OK;
  }
  if (strip_offset > size || strip_length > size ||
      strip_offset + strip_length > size) {
    return GIMG_ERR_CORRUPT;
  }
  allocator = gimg_alloc_or_default(allocator);
  if (jpeg_tables && jpeg_tables_len >= 2) {
    // Reassemble: SOI + (JPEGTables without leading SOI and trailing EOI) +
    // strip. If strip starts with SOI (0xFF 0xD8), skip it so we don't
    // duplicate SOI.
    size_t tables_skip_lead =
        (jpeg_tables[0] == 0xFF && jpeg_tables[1] == 0xD8) ? 2u : 0u;
    size_t tables_skip_tail = 0u;
    if (jpeg_tables_len >= 4 && jpeg_tables[jpeg_tables_len - 2] == 0xFF &&
        jpeg_tables[jpeg_tables_len - 1] == 0xD9) {
      tables_skip_tail = 2u;
    }
    size_t tables_body = jpeg_tables_len - tables_skip_lead - tables_skip_tail;
    if (tables_body > jpeg_tables_len) {
      return GIMG_ERR_CORRUPT;
    }
    const unsigned char * strip_start = buf + strip_offset;
    size_t strip_use = strip_length;
    if (strip_length >= 2 && strip_start[0] == 0xFF && strip_start[1] == 0xD8) {
      strip_start += 2;
      strip_use -= 2;
    }
    size_t out_len = 2 + tables_body + strip_use;
    unsigned char * out_buf = (unsigned char *)gimg_malloc(allocator, out_len);
    if (!out_buf) {
      return GIMG_ERR_OOM;
    }
    out_buf[0] = 0xFF;
    out_buf[1] = 0xD8;
    memcpy(out_buf + 2, jpeg_tables + tables_skip_lead, tables_body);
    memcpy(out_buf + 2 + tables_body, strip_start, strip_use);
    *out_data = out_buf;
    *out_size = out_len;
  }
  else {
    unsigned char * out_buf =
        (unsigned char *)gimg_malloc(allocator, strip_length);
    if (!out_buf) {
      return GIMG_ERR_OOM;
    }
    memcpy(out_buf, buf + strip_offset, strip_length);
    *out_data = out_buf;
    *out_size = strip_length;
  }
  return GIMG_OK;
}

// Parse width and height from JPEG SOF (0xFF 0xC0 or 0xC1). Returns 0,0 if not
// found.
static void exif_jpeg_sof_dimensions(const unsigned char * jpeg,
    size_t jpeg_size, uint32_t * out_w, uint32_t * out_h) {
  *out_w = 0;
  *out_h = 0;
  if (!jpeg || jpeg_size < 12) {
    return;
  }
  for (size_t i = 0; i + 12 <= jpeg_size; i++) {
    if (jpeg[i] != 0xFF || (jpeg[i + 1] != 0xC0 && jpeg[i + 1] != 0xC1)) {
      if (jpeg[i] == 0xFF && jpeg[i + 1] >= 0xC2 && jpeg[i + 1] <= 0xFE) {
        size_t seg_len = (size_t)((jpeg[i + 2] << 8) | jpeg[i + 3]);
        if (seg_len >= 2 && i + 2 + seg_len <= jpeg_size) {
          i += 1 + seg_len;
          continue;
        }
      }
      continue;
    }
    // SOF: +2 length, +1 precision, +2 height, +2 width
    *out_h = (uint32_t)((jpeg[i + 5] << 8) | jpeg[i + 6]);
    *out_w = (uint32_t)((jpeg[i + 7] << 8) | jpeg[i + 8]);
    return;
  }
}

GIMG_Result gimg_exif_build_with_thumbnail_jpeg(
    const GIMG_Allocator * allocator, const void * base_exif, size_t base_size,
    const void * jpeg_data, size_t jpeg_size, void ** out, size_t * out_size) {
  if (!jpeg_data || jpeg_size == 0 || !out || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out = NULL;
  *out_size = 0;
  uint32_t thumb_w = 0, thumb_h = 0;
  exif_jpeg_sof_dimensions(
      (const unsigned char *)jpeg_data, jpeg_size, &thumb_w, &thumb_h);
  if (thumb_w == 0) {
    thumb_w = 160;
  }
  if (thumb_h == 0) {
    thumb_h = 120;
  }

  GIMG_Orientation orientation = GIMG_ORIENTATION_UNKNOWN;
  if (base_exif && base_size >= GIMG_EXIF_MIN_SIZE) {
    (void)gimg_exif_parse_orientation(base_exif, base_size, &orientation);
  }
  int le = 1; // Little-endian (II)
  allocator = gimg_alloc_or_default(allocator);

  // IFD0: 0 or 1 entry (orientation). Next IFD = IFD1 offset.
  uint16_t ifd0_entries = (orientation >= 1 && orientation <= 8) ? 1 : 0;
  uint32_t ifd0_off = 8;
  uint32_t ifd1_off = (uint32_t)(ifd0_off + 2 + (size_t)ifd0_entries * 12 + 4);
  uint32_t jpeg_off = ifd1_off + 2 + 5 * 12 + 4; // IFD1: 5 entries, then next

  size_t total = jpeg_off + jpeg_size;
  unsigned char * dst = (unsigned char *)gimg_malloc(allocator, total);
  if (!dst) {
    return GIMG_ERR_OOM;
  }
  memset(dst, 0, total);

  // TIFF header
  dst[0] = 0x49;
  dst[1] = 0x49;
  dst[2] = 42;
  dst[3] = 0;
  write_u32(dst + 4, ifd0_off, le);

  // IFD0
  write_u16(dst + ifd0_off, ifd0_entries, le);
  size_t ent_off = ifd0_off + 2;
  if (ifd0_entries == 1) {
    write_u16(dst + ent_off, GIMG_EXIF_TAG_ORIENTATION, le);
    write_u16(dst + ent_off + 2, GIMG_EXIF_TYPE_SHORT, le);
    write_u32(dst + ent_off + 4, 1, le);
    write_u16(dst + ent_off + 8, (uint16_t)orientation, le);
    ent_off += 12;
  }
  write_u32(dst + ent_off, ifd1_off, le);
  ent_off += 4;

  // IFD1: Compression(6), ImageWidth, ImageLength, JPEGInterchangeFormat,
  // JPEGInterchangeFormatLength
  write_u16(dst + ifd1_off, 5, le);
  unsigned char * e = dst + ifd1_off + 2;
  write_u16(e, GIMG_EXIF_TAG_COMPRESSION, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_SHORT, le);
  write_u32(e + 4, 1, le);
  write_u16(e + 8, 6, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_IMAGE_WIDTH, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, thumb_w, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_IMAGE_LENGTH, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, thumb_h, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, jpeg_off, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, (uint32_t)jpeg_size, le);
  e += 12;
  write_u32(e, 0, le); // next IFD

  memcpy(dst + jpeg_off, jpeg_data, jpeg_size);

  *out = dst;
  *out_size = total;
  return GIMG_OK;
}

GIMG_Result gimg_exif_build_with_thumbnail_uncompressed(
    const GIMG_Allocator * allocator, const void * base_exif, size_t base_size,
    const void * strip_data, size_t strip_size, uint32_t width, uint32_t height,
    uint16_t samples_per_pixel, uint8_t bits_per_sample, void ** out,
    size_t * out_size) {
  if (!strip_data || strip_size == 0 || !out || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  if (samples_per_pixel != 1 && samples_per_pixel != 3) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (bits_per_sample != 8) {
    return GIMG_ERR_UNSUPPORTED;
  }
  size_t expected_strip =
      (size_t)width * (size_t)height * (size_t)samples_per_pixel;
  if (expected_strip == 0 || strip_size != expected_strip) {
    return GIMG_ERR_INTERNAL;
  }
  *out = NULL;
  *out_size = 0;

  GIMG_Orientation orientation = GIMG_ORIENTATION_UNKNOWN;
  if (base_exif && base_size >= GIMG_EXIF_MIN_SIZE) {
    (void)gimg_exif_parse_orientation(base_exif, base_size, &orientation);
  }
  int le = 1;
  allocator = gimg_alloc_or_default(allocator);

  uint16_t ifd0_entries = (orientation >= 1 && orientation <= 8) ? 1 : 0;
  uint32_t ifd0_off = 8;
  uint32_t ifd1_off =
      (uint32_t)(ifd0_off + 2 + (size_t)ifd0_entries * 12 + 4);
  // IFD1: 7 entries. For RGB, BitsPerSample count=3 stored at offset (6 bytes).
  size_t bps_extra = (samples_per_pixel == 3) ? 6u : 0u;
  uint32_t strip_off =
      (uint32_t)(ifd1_off + 2 + 7 * 12 + 4 + bps_extra);
  size_t total = (size_t)strip_off + strip_size;

  unsigned char * dst = (unsigned char *)gimg_malloc(allocator, total);
  if (!dst) {
    return GIMG_ERR_OOM;
  }
  memset(dst, 0, total);

  dst[0] = 0x49;
  dst[1] = 0x49;
  dst[2] = 42;
  dst[3] = 0;
  write_u32(dst + 4, ifd0_off, le);

  write_u16(dst + ifd0_off, ifd0_entries, le);
  size_t ent_off = ifd0_off + 2;
  if (ifd0_entries == 1) {
    write_u16(dst + ent_off, GIMG_EXIF_TAG_ORIENTATION, le);
    write_u16(dst + ent_off + 2, GIMG_EXIF_TYPE_SHORT, le);
    write_u32(dst + ent_off + 4, 1, le);
    write_u16(dst + ent_off + 8, (uint16_t)orientation, le);
    ent_off += 12;
  }
  write_u32(dst + ent_off, ifd1_off, le);

  // IFD1: Compression(1), ImageWidth, ImageLength, BitsPerSample,
  // PhotometricInterpretation, StripOffsets, StripByteCounts
  write_u16(dst + ifd1_off, 7, le);
  unsigned char * e = dst + ifd1_off + 2;
  write_u16(e, GIMG_EXIF_TAG_COMPRESSION, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_SHORT, le);
  write_u32(e + 4, 1, le);
  write_u16(e + 8, 1, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_IMAGE_WIDTH, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, width, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_IMAGE_LENGTH, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, height, le);
  e += 12;
  if (samples_per_pixel == 1) {
    write_u16(e, GIMG_EXIF_TAG_BITS_PER_SAMPLE, le);
    write_u16(e + 2, GIMG_EXIF_TYPE_SHORT, le);
    write_u32(e + 4, 1, le);
    write_u16(e + 8, (uint16_t)bits_per_sample, le);
    e += 12;
  }
  else {
    write_u16(e, GIMG_EXIF_TAG_BITS_PER_SAMPLE, le);
    write_u16(e + 2, GIMG_EXIF_TYPE_SHORT, le);
    write_u32(e + 4, 3, le);
    write_u32(e + 8, (uint32_t)(ifd1_off + 2 + 7 * 12 + 4), le);
    e += 12;
  }
  write_u16(e, GIMG_EXIF_TAG_PHOTOMETRIC_INTERPRETATION, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_SHORT, le);
  write_u32(e + 4, 1, le);
  write_u16(e + 8, samples_per_pixel == 1 ? 1 : 2, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_STRIP_OFFSETS, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, strip_off, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_STRIP_BYTE_COUNTS, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, (uint32_t)strip_size, le);
  e += 12;
  write_u32(e, 0, le);

  if (samples_per_pixel == 3) {
    uint32_t bps_off = (uint32_t)(ifd1_off + 2 + 7 * 12 + 4);
    write_u16(dst + bps_off, (uint16_t)bits_per_sample, le);
    write_u16(dst + bps_off + 2, (uint16_t)bits_per_sample, le);
    write_u16(dst + bps_off + 4, (uint16_t)bits_per_sample, le);
  }
  memcpy(dst + strip_off, strip_data, strip_size);

  *out = dst;
  *out_size = total;
  return GIMG_OK;
}

GIMG_Result gimg_exif_build_with_thumbnail_tiff_jpeg(
    const GIMG_Allocator * allocator, const void * base_exif, size_t base_size,
    const void * jpeg_data, size_t jpeg_size, void ** out, size_t * out_size) {
  if (!jpeg_data || jpeg_size == 0 || !out || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out = NULL;
  *out_size = 0;

  GIMG_Orientation orientation = GIMG_ORIENTATION_UNKNOWN;
  if (base_exif && base_size >= GIMG_EXIF_MIN_SIZE) {
    (void)gimg_exif_parse_orientation(base_exif, base_size, &orientation);
  }
  int le = 1;
  allocator = gimg_alloc_or_default(allocator);

  uint16_t ifd0_entries = (orientation >= 1 && orientation <= 8) ? 1 : 0;
  uint32_t ifd0_off = 8;
  uint32_t ifd1_off =
      (uint32_t)(ifd0_off + 2 + (size_t)ifd0_entries * 12 + 4);
  // IFD1: Compression(7), StripOffsets, StripByteCounts — 3 entries.
  uint32_t strip_off = (uint32_t)(ifd1_off + 2 + 3 * 12 + 4);

  size_t total = (size_t)strip_off + jpeg_size;
  unsigned char * dst = (unsigned char *)gimg_malloc(allocator, total);
  if (!dst) {
    return GIMG_ERR_OOM;
  }
  memset(dst, 0, total);

  dst[0] = 0x49;
  dst[1] = 0x49;
  dst[2] = 42;
  dst[3] = 0;
  write_u32(dst + 4, ifd0_off, le);

  write_u16(dst + ifd0_off, ifd0_entries, le);
  size_t ent_off = ifd0_off + 2;
  if (ifd0_entries == 1) {
    write_u16(dst + ent_off, GIMG_EXIF_TAG_ORIENTATION, le);
    write_u16(dst + ent_off + 2, GIMG_EXIF_TYPE_SHORT, le);
    write_u32(dst + ent_off + 4, 1, le);
    write_u16(dst + ent_off + 8, (uint16_t)orientation, le);
    ent_off += 12;
  }
  write_u32(dst + ent_off, ifd1_off, le);

  write_u16(dst + ifd1_off, 3, le);
  unsigned char * e = dst + ifd1_off + 2;
  write_u16(e, GIMG_EXIF_TAG_COMPRESSION, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_SHORT, le);
  write_u32(e + 4, 1, le);
  write_u16(e + 8, 7, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_STRIP_OFFSETS, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, strip_off, le);
  e += 12;
  write_u16(e, GIMG_EXIF_TAG_STRIP_BYTE_COUNTS, le);
  write_u16(e + 2, GIMG_EXIF_TYPE_LONG, le);
  write_u32(e + 4, 1, le);
  write_u32(e + 8, (uint32_t)jpeg_size, le);
  e += 12;
  write_u32(e, 0, le);

  memcpy(dst + strip_off, jpeg_data, jpeg_size);

  *out = dst;
  *out_size = total;
  return GIMG_OK;
}
