/**
 * @file
 *
 * Internal Exif (eXIf / TIFF-IFD) parsing and re-serialization.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_EXIF_INTERNAL_H
#define GHOTI_IO_GIMG_EXIF_INTERNAL_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/meta.h>
#include <stddef.h>
#include <stdint.h>

/** TIFF/Exif orientation tag (IFD0). Type SHORT (3), count 1. */
#define GIMG_EXIF_TAG_ORIENTATION UINT16_C(0x0112)
/** GPS IFD pointer tag (IFD0). Type LONG (4), count 1; value = offset to GPS
 * IFD. */
#define GIMG_EXIF_TAG_GPS_IFD UINT16_C(0x8825)

/** TIFF type SHORT (16-bit). */
#define GIMG_EXIF_TYPE_SHORT 3
/** TIFF type LONG (32-bit). */
#define GIMG_EXIF_TYPE_LONG 4

/**
 * Parse orientation from an Exif (eXIf) blob.
 * @param exif Exif blob (TIFF-like: II/MM, 42, IFD0 offset, ...).
 * @param size Size of exif.
 * @param out On success, set to orientation (1-8) or GIMG_ORIENTATION_UNKNOWN.
 * @return GIMG_OK if parsed (even when orientation missing/unknown),
 * GIMG_ERR_CORRUPT if not valid TIFF/Exif.
 */
GIMG_Result gimg_exif_parse_orientation(
    const void * exif, size_t size, GIMG_Orientation * out);

/**
 * Produce a new Exif blob with GPS IFD and tag 0x8825 removed; rest preserved.
 * Caller must free *out with the same allocator.
 * @param allocator Allocator for output (NULL = default).
 * @param exif Input Exif blob.
 * @param size Input size.
 * @param out On success, set to new blob (allocated).
 * @param out_size On success, set to new blob size.
 * @return GIMG_OK or GIMG_ERR_CORRUPT / GIMG_ERR_OOM.
 */
GIMG_Result gimg_exif_strip_gps(const GIMG_Allocator * allocator,
    const void * exif, size_t size, void ** out, size_t * out_size);

/**
 * Produce a normalized Exif blob (e.g. orientation set to 1, duplicate tags
 * removed). Caller must free *out with the same allocator.
 * @param allocator Allocator for output (NULL = default).
 * @param exif Input Exif blob.
 * @param size Input size.
 * @param out On success, set to new blob (allocated).
 * @param out_size On success, set to new blob size.
 * @return GIMG_OK or GIMG_ERR_CORRUPT / GIMG_ERR_OOM.
 */
GIMG_Result gimg_exif_normalize(const GIMG_Allocator * allocator,
    const void * exif, size_t size, void ** out, size_t * out_size);

#endif  // GHOTI_IO_GIMG_EXIF_INTERNAL_H
