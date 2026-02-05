/**
 * @file
 *
 * Raster image type, pixel format descriptors, and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_RASTER_H
#define GHOTI_IO_IMAGE_RASTER_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Opaque raster image. */
typedef struct GIMG_Raster GIMG_Raster;

/**
 * @brief Channel model for pixel format.
 */
typedef enum {
  GIMG_CHANNEL_GRAY = 0,
  GIMG_CHANNEL_RGB,
  GIMG_CHANNEL_RGBA,
  GIMG_CHANNEL_CMYK,
  GIMG_CHANNEL_YCBCR,
  GIMG_CHANNEL_LAB,
  GIMG_CHANNEL_INDEXED,
  GIMG_CHANNEL_COUNT
} GIMG_Channel_Model;

/**
 * @brief Channel value type.
 */
typedef enum {
  GIMG_CHANNEL_UNORM = 0,
  GIMG_CHANNEL_SNORM,
  GIMG_CHANNEL_UINT,
  GIMG_CHANNEL_SINT,
  GIMG_CHANNEL_FLOAT,
  GIMG_CHANNEL_TYPE_COUNT
} GIMG_Channel_Type;

/**
 * @brief Layout: interleaved (e.g. RGBA RGBA) vs planar (R plane, G plane,
 * ...).
 */
typedef enum {
  GIMG_LAYOUT_INTERLEAVED = 0,
  GIMG_LAYOUT_PLANAR,
  GIMG_LAYOUT_COUNT
} GIMG_Pixel_Layout;

/**
 * @brief Pixel format descriptor (spec §4.1).
 */
typedef struct {
  GIMG_Channel_Model channel_model; ///< Gray, RGB, RGBA, etc.
  GIMG_Channel_Type channel_type;   ///< UNORM, UINT, FLOAT, etc.
  GIMG_Pixel_Layout layout;         ///< Interleaved or planar.
  uint8_t channel_count;            ///< Number of channels.
  uint8_t bits_per_channel[8];      ///< Bits per channel (0 = unused).
  uint8_t alignment;                ///< Row alignment in bytes (e.g. 16).
  uint8_t _reserved[5];
} GIMG_Pixel_Format;

/** @brief Canonical RGBA 8-bit per channel (sRGB). */
extern const GIMG_Pixel_Format GIMG_PIXEL_RGBA8;
/** @brief Canonical RGBA 16-bit per channel. */
extern const GIMG_Pixel_Format GIMG_PIXEL_RGBA16;
/** @brief Canonical grayscale 8-bit. */
extern const GIMG_Pixel_Format GIMG_PIXEL_GRAY8;
/** @brief Canonical grayscale 16-bit. */
extern const GIMG_Pixel_Format GIMG_PIXEL_GRAY16;

/**
 * @brief Ownership of the pixel buffer.
 */
typedef enum {
  GIMG_RASTER_OWNED = 0, ///< Library owns buffer; destroyed with raster.
  GIMG_RASTER_BORROWED,  ///< Caller-owned view; raster does not free buffer.
  GIMG_RASTER_OWNERSHIP_COUNT
} GIMG_Raster_Ownership;

/**
 * @brief Create a raster image (uses default allocator).
 */
GIMG_API GIMG_Result gimg_raster_create(uint32_t width, uint32_t height,
    const GIMG_Pixel_Format * format, GIMG_Raster_Ownership ownership,
    void * buffer, size_t stride_bytes, GIMG_Raster ** out_raster);

/**
 * @brief Create a raster image with a specific allocator.
 * @param allocator Allocator for raster and owned pixel buffer (NULL =
 * default).
 */
GIMG_API GIMG_Result gimg_raster_create_with_allocator(
    const GIMG_Allocator * allocator, uint32_t width, uint32_t height,
    const GIMG_Pixel_Format * format, GIMG_Raster_Ownership ownership,
    void * buffer, size_t stride_bytes, GIMG_Raster ** out_raster);

/**
 * @brief Destroy a raster and, if owned, its buffer.
 * @param raster Raster to destroy (no-op if NULL).
 */
GIMG_API void gimg_raster_destroy(GIMG_Raster * raster);

/**
 * @brief Width in pixels.
 */
GIMG_API uint32_t gimg_raster_width(const GIMG_Raster * raster);
/**
 * @brief Height in pixels.
 */
GIMG_API uint32_t gimg_raster_height(const GIMG_Raster * raster);
/**
 * @brief Row stride in bytes.
 */
GIMG_API size_t gimg_raster_stride_bytes(const GIMG_Raster * raster);
/**
 * @brief Pixel format descriptor (read-only).
 */
GIMG_API const GIMG_Pixel_Format * gimg_raster_format(
    const GIMG_Raster * raster);
/**
 * @brief Ownership of the pixel buffer.
 */
GIMG_API GIMG_Raster_Ownership gimg_raster_ownership(
    const GIMG_Raster * raster);
/**
 * @brief Pointer to the first pixel (row 0). Do not free when borrowed.
 */
GIMG_API void * gimg_raster_pixels(GIMG_Raster * raster);
GIMG_API const void * gimg_raster_pixels_const(const GIMG_Raster * raster);

/**
 * @brief Bytes per pixel for packed/interleaved format.
 * @return Bytes per pixel or 0 if format is invalid or planar.
 */
GIMG_API size_t gimg_raster_bytes_per_pixel(const GIMG_Pixel_Format * format);

/**
 * @brief Allocator used by this raster (for shared allocations, e.g. in ops).
 */
GIMG_API const GIMG_Allocator * gimg_raster_allocator(const GIMG_Raster * raster);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_RASTER_H
