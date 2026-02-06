/**
 * @file
 *
 * Exif (eXIf / TIFF-IFD) parse, strip GPS, and normalize.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string.h>

#include "../core/alloc_internal.h"
#include "exif_internal.h"

// Minimum size: TIFF header (8) + IFD at least 2 + 0 entries + 4 = 14.
#define GIMG_EXIF_MIN_SIZE 14u

static int is_little_endian(const unsigned char * h) {
  return h[0] == 0x49u && h[1] == 0x49u;  // "II"
}

static uint16_t read_u16(const unsigned char * p, int little) {
  if (little) {
    return (uint16_t)(p[0] | (p[1] << 8));
  }
  return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t read_u32(const unsigned char * p, int little) {
  if (little) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24));
  }
  return (uint32_t)((p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]);
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
  if (buf[2] != 42 || buf[3] != 0) {
    return GIMG_ERR_CORRUPT;
  }
  int le = is_little_endian(buf);
  uint32_t ifd0 = read_u32(buf + 4, le);
  if (ifd0 + 2 > size) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num_entries = read_u16(buf + ifd0, le);
  if (ifd0 + 2u + (uint32_t)num_entries * 12u + 4u > size) {
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

// Compute size of IFD at given offset (2 + 12*num_entries + 4). Return 0 if
// invalid.
static size_t exif_ifd_size(
    const unsigned char * buf, size_t size, uint32_t ifd_off, int le) {
  if (ifd_off + 2 > size) {
    return 0;
  }
  uint16_t n = read_u16(buf + ifd_off, le);
  if (ifd_off + 2 + (size_t)n * 12 + 4 > size) {
    return 0;
  }
  return 2 + (size_t)n * 12 + 4;
}

GIMG_Result gimg_exif_strip_gps(const GIMG_Allocator * allocator,
    const void * exif, size_t size, void ** out, size_t * out_size) {
  if (!exif || size < GIMG_EXIF_MIN_SIZE || !out || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  *out = NULL;
  *out_size = 0;
  const unsigned char * buf = (const unsigned char *)exif;
  if (buf[2] != 42 || buf[3] != 0) {
    return GIMG_ERR_CORRUPT;
  }
  int le = is_little_endian(buf);
  uint32_t ifd0 = read_u32(buf + 4, le);
  size_t ifd0_size = exif_ifd_size(buf, size, ifd0, le);
  if (ifd0_size == 0) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num_entries = read_u16(buf + ifd0, le);
  size_t gps_entry_index = (size_t)(-1);
  uint32_t gps_ifd_offset = 0;
  for (uint16_t i = 0; i < num_entries; i++) {
    size_t off = ifd0 + 2 + (size_t)i * 12;
    if (read_u16(buf + off, le) == GIMG_EXIF_TAG_GPS_IFD) {
      if (read_u16(buf + off + 2, le) != GIMG_EXIF_TYPE_LONG ||
          read_u32(buf + off + 4, le) != 1) {
        continue;
      }
      gps_entry_index = i;
      gps_ifd_offset = read_u32(buf + off + 8, le);
      break;
    }
  }
  if (gps_entry_index == (size_t)(-1)) {
    // No GPS IFD: return a copy.
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
  size_t gps_ifd_size = exif_ifd_size(buf, size, gps_ifd_offset, le);
  if (gps_ifd_size == 0 || gps_ifd_offset + gps_ifd_size > size) {
    return GIMG_ERR_CORRUPT;
  }
  size_t delta = 12 + gps_ifd_size;
  size_t new_size = size - delta;
  allocator = gimg_alloc_or_default(allocator);
  unsigned char * dst = (unsigned char *)gimg_malloc(allocator, new_size);
  if (!dst) {
    return GIMG_ERR_OOM;
  }
  // Copy [0, ifd0+2] (header + num_entries).
  memcpy(dst, buf, ifd0 + 2);
  dst[ifd0] = (unsigned char)((num_entries - 1) & 0xff);
  dst[ifd0 + 1] = (unsigned char)((num_entries - 1) >> 8);
  // Copy IFD entries skipping the GPS entry.
  size_t dst_ent = ifd0 + 2;
  for (uint16_t i = 0; i < num_entries; i++) {
    if (i == gps_entry_index) {
      continue;
    }
    memcpy(dst + dst_ent, buf + ifd0 + 2 + (size_t)i * 12, 12);
    dst_ent += 12;
  }
  // Copy next IFD pointer (4 bytes).
  memcpy(dst + dst_ent, buf + ifd0 + 2 + (size_t)num_entries * 12, 4);
  size_t ifd0_end_old = ifd0 + ifd0_size;
  size_t dst_after_ifd = dst_ent + 4;
  // Copy [ifd0_end, gps_ifd_offset].
  memcpy(
      dst + dst_after_ifd, buf + ifd0_end_old, gps_ifd_offset - ifd0_end_old);
  // Copy [gps_ifd_offset + gps_ifd_size, size].
  size_t gps_end = gps_ifd_offset + gps_ifd_size;
  memcpy(dst + dst_after_ifd + (gps_ifd_offset - ifd0_end_old), buf + gps_end,
      size - gps_end);
  // Fix 4-byte offsets in the new buffer: >= gps_end -> subtract delta; in
  // [gps_ifd_offset, gps_end) -> 0.
  for (size_t i = 0; i + 4 <= new_size; i += 4) {
    uint32_t v = read_u32(dst + i, le);
    if (v >= gps_ifd_offset && v < gps_end) {
      write_u32(dst + i, 0, le);
    }
    else if (v >= gps_end) {
      write_u32(dst + i, (uint32_t)(v - delta), le);
    }
  }
  *out = dst;
  *out_size = new_size;
  return GIMG_OK;
}

GIMG_Result gimg_exif_normalize(const GIMG_Allocator * allocator,
    const void * exif, size_t size, void ** out, size_t * out_size) {
  if (!exif || size < GIMG_EXIF_MIN_SIZE || !out || !out_size) {
    return GIMG_ERR_INTERNAL;
  }
  const unsigned char * buf = (const unsigned char *)exif;
  if (buf[2] != 42 || buf[3] != 0) {
    return GIMG_ERR_CORRUPT;
  }
  int le = is_little_endian(buf);
  uint32_t ifd0 = read_u32(buf + 4, le);
  if (ifd0 + 2 > size) {
    return GIMG_ERR_CORRUPT;
  }
  uint16_t num_entries = read_u16(buf + ifd0, le);
  if (ifd0 + 2u + (uint32_t)num_entries * 12u + 4u > size) {
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
