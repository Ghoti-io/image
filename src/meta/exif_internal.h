/**
 * @file
 *
 * Internal Exif (eXIf / TIFF-IFD) parsing and re-serialization.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SRC_META_EXIF_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_META_EXIF_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/meta.h>
#include <stddef.h>
#include <stdint.h>

// Usable from C++ (the unit tests reach these directly), as png_internal.h is.
#ifdef __cplusplus
extern "C" {
#endif

/** TIFF/Exif orientation tag (IFD0). Type SHORT (3), count 1. */
#define GIMG_EXIF_TAG_ORIENTATION UINT16_C(0x0112)
/** GPS IFD pointer tag (IFD0). Type LONG (4), count 1; value = offset to GPS
 * IFD. */
#define GIMG_EXIF_TAG_GPS_IFD UINT16_C(0x8825)

/** Compression (IFD1 thumbnail). Type SHORT; 1 = none, 6 = JPEG, 7 = TIFF
 * TechNote 2 JPEG. */
#define GIMG_EXIF_TAG_COMPRESSION UINT16_C(0x0103)
/** JPEG thumbnail offset (IFD1). Type LONG; offset from start of TIFF. */
#define GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT UINT16_C(0x0201)
/** JPEG thumbnail length (IFD1). Type LONG. */
#define GIMG_EXIF_TAG_JPEG_INTERCHANGE_FORMAT_LENGTH UINT16_C(0x0202)
/** Image width (IFD1 uncompressed). Type LONG or SHORT. */
#define GIMG_EXIF_TAG_IMAGE_WIDTH UINT16_C(0x0100)
/** Image length / height (IFD1 uncompressed). Type LONG or SHORT. */
#define GIMG_EXIF_TAG_IMAGE_LENGTH UINT16_C(0x0101)
/** Bits per sample (IFD1). Type SHORT; count = samples per pixel. */
#define GIMG_EXIF_TAG_BITS_PER_SAMPLE UINT16_C(0x0102)
/** Photometric interpretation (IFD1). Type SHORT; 0=WhiteIsZero, 1=BlackIsZero,
 * 2=RGB. */
#define GIMG_EXIF_TAG_PHOTOMETRIC_INTERPRETATION UINT16_C(0x0106)
/** Strip offsets (IFD1 uncompressed / Compress 7). Type LONG or SHORT array. */
#define GIMG_EXIF_TAG_STRIP_OFFSETS UINT16_C(0x0111)
/** Strip byte counts (IFD1 uncompressed / Compress 7). Type LONG or SHORT
 * array. */
#define GIMG_EXIF_TAG_STRIP_BYTE_COUNTS UINT16_C(0x0117)
/** JPEG tables (IFD1 Compression=7). Type UNDEFINED; optional. */
#define GIMG_EXIF_TAG_JPEG_TABLES UINT16_C(0x015B)

/** TIFF type SHORT (16-bit). */
#define GIMG_EXIF_TYPE_SHORT 3
/** TIFF type LONG (32-bit). */
#define GIMG_EXIF_TYPE_LONG 4
/** TIFF type UNDEFINED (opaque bytes). */
#define GIMG_EXIF_TYPE_UNDEFINED 7

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

/**
 * Get embedded JPEG thumbnail from Exif (IFD1) if present.
 * @param tiff TIFF blob (e.g. Exif APP1 payload after "Exif\\0\\0", so first
 *   byte is II/MM).
 * @param size Size of tiff.
 * @param out_data On success, set to pointer into tiff to JPEG bytes (caller
 *   must keep tiff valid while using).
 * @param out_size On success, set to JPEG byte count.
 * @return GIMG_OK if IFD1 has Compression=6 and valid 0x0201/0x0202;
 *   GIMG_ERR_CORRUPT if invalid; no thumbnail is not an error (caller checks
 *   out_size).
 */
GIMG_Result gimg_exif_embedded_thumbnail_jpeg(
    const void * tiff, size_t size, const void ** out_data, size_t * out_size);

/**
 * Get embedded uncompressed thumbnail from Exif (IFD1 Compression=1) if
 * present. Parses ImageWidth, ImageLength, BitsPerSample, StripOffsets,
 * StripByteCounts, PhotometricInterpretation; concatenates strip(s) into one
 * buffer.
 * @param allocator Allocator for concatenated strip buffer (NULL = default).
 * @param tiff TIFF blob (Exif APP1 payload after "Exif\\0\\0").
 * @param size Size of tiff.
 * @param out_width On success, thumbnail width.
 * @param out_height On success, thumbnail height.
 * @param out_bits_per_sample On success, 8 or 16.
 * @param out_photometric On success, 0=WhiteIsZero, 1=BlackIsZero, 2=RGB.
 * @param out_data On success, allocated strip data (caller frees with
 * allocator).
 * @param out_size On success, byte count of out_data.
 * @return GIMG_OK if IFD1 has Compression=1 and valid tags; GIMG_ERR_CORRUPT if
 * invalid; no thumbnail is not an error (caller checks out_size).
 */
GIMG_Result gimg_exif_embedded_thumbnail_uncompressed(
    const GIMG_Allocator * allocator, const void * tiff, size_t size,
    uint32_t * out_width, uint32_t * out_height, uint8_t * out_bits_per_sample,
    uint16_t * out_photometric, void ** out_data, size_t * out_size);

/**
 * Get embedded TIFF TechNote 2 JPEG thumbnail (IFD1 Compression=7) if present.
 * When JPEGTables (tag 347) is present, reassembles one stream (SOI + tables +
 * strip) and allocates; otherwise copies strip to allocated buffer so caller
 * always frees.
 * @param allocator Allocator for output buffer (NULL = default).
 * @param tiff TIFF blob (Exif APP1 payload after "Exif\\0\\0").
 * @param size Size of tiff.
 * @param out_data On success, allocated JPEG bytes (caller frees with
 * allocator).
 * @param out_size On success, byte count.
 * @return GIMG_OK if IFD1 has Compression=7 and valid strip data;
 * GIMG_ERR_CORRUPT if invalid; no thumbnail is not an error.
 */
GIMG_Result gimg_exif_embedded_thumbnail_tiff_jpeg(
    const GIMG_Allocator * allocator, const void * tiff, size_t size,
    void ** out_data, size_t * out_size);

/**
 * Build an Exif (TIFF) blob with IFD1 containing a JPEG thumbnail
 * (Compression=6). When base_exif is non-NULL and valid, orientation is copied
 * to IFD0; otherwise minimal IFD0 (no tags) is used. IFD1 is always built with
 * Compression=6, ImageWidth/ImageLength (from JPEG SOF), JPEGInterchangeFormat,
 * JPEGInterchangeFormatLength. Caller must free *out with the same allocator.
 */
GIMG_Result gimg_exif_build_with_thumbnail_jpeg(
    const GIMG_Allocator * allocator, const void * base_exif, size_t base_size,
    const void * jpeg_data, size_t jpeg_size, void ** out, size_t * out_size);

/**
 * Build an Exif (TIFF) blob with IFD1 containing an uncompressed thumbnail
 * (Compression=1). strip_data is raw pixel data (one strip): grayscale 8-bit
 * or RGB 8-bit (R,G,B per pixel). samples_per_pixel 1 or 3; bits_per_sample 8.
 * Caller must free *out with the same allocator.
 */
GIMG_Result gimg_exif_build_with_thumbnail_uncompressed(
    const GIMG_Allocator * allocator, const void * base_exif, size_t base_size,
    const void * strip_data, size_t strip_size, uint32_t width, uint32_t height,
    uint16_t samples_per_pixel, uint8_t bits_per_sample, void ** out,
    size_t * out_size);

/**
 * Build an Exif (TIFF) blob with IFD1 containing a TIFF TechNote 2 JPEG
 * thumbnail (Compression=7). jpeg_data is a complete JPEG stream; it is stored
 * as a single strip (no JPEGTables). Caller must free *out with the same
 * allocator.
 */
GIMG_Result gimg_exif_build_with_thumbnail_tiff_jpeg(
    const GIMG_Allocator * allocator, const void * base_exif, size_t base_size,
    const void * jpeg_data, size_t jpeg_size, void ** out, size_t * out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#endif // GHOTI_IO_GIMG_SRC_META_EXIF_INTERNAL_H
