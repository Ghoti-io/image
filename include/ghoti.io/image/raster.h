/**
 * @file
 *
 * Raster image type, pixel format descriptors, and accessors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_RASTER_H
#define GHOTI_IO_GIMG_RASTER_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/color.h>
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
  /**
   * Channels with no color meaning attached: some number of samples per
   * pixel, in the order the file gave them.
   *
   * A JPEG frame is the case this exists for.  ISO/IEC 10918-1 (T.81) B.2.2
   * lets a frame carry from 1 to 255 components and says nothing anywhere
   * about what they mean; one, three and four have conventions attached to
   * them (JFIF, the Adobe APP14 marker, the component identifiers) and no
   * other count has any.  libjpeg calls the same thing JCS_UNKNOWN and hands
   * the components back untouched, which is what a raster in this model
   * holds.
   */
  GIMG_CHANNEL_UNKNOWN,
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
  uint8_t channel_count;            ///< Number of channels, 1 to 255.
  /**
   * Bits per channel; 0 marks an unused entry.
   *
   * A format with more than eight channels gives every channel the depth in
   * bits_per_channel[0] and leaves the rest of the array zero.  That is not a
   * loss: a format wide enough to need the rule is one whose samples all came
   * from the same place, such as a JPEG frame, where T.81 B.2.2 gives the
   * whole frame a single sample precision.  Use
   * gimg_pixel_format_multichannel() to build one rather than filling this in
   * by hand.
   */
  uint8_t bits_per_channel[8];
  uint8_t alignment;                ///< Row alignment in bytes (e.g. 16).
  uint8_t _reserved[5];
} GIMG_Pixel_Format;

/**
 * @brief Bits in one channel of @p format, for any channel count.
 *
 * Reads bits_per_channel[index] for the first eight channels and
 * bits_per_channel[0] beyond them; see the note on that field.
 */
GIMG_API uint8_t gimg_pixel_format_channel_bits(
    const GIMG_Pixel_Format * format, uint8_t index);

/**
 * @brief Build a format for @p channel_count channels of @p bits each.
 *
 * The model is chosen from the count where there is a convention for it -
 * GRAY for one, CMYK for four - and GIMG_CHANNEL_UNKNOWN otherwise, which is
 * what a JPEG frame of two, or of five or more, components carries.  Three
 * channels are ambiguous (RGB and YCbCr are both three) and are not guessed:
 * they come back as GIMG_CHANNEL_UNKNOWN too, so a caller that knows they are
 * color should name GIMG_PIXEL_RGBA8 or its kin instead.
 *
 * @param channel_count 1 to 255.
 * @param bits 8, 12 or 16.  A 12-bit sample occupies a uint16_t and runs
 *   0..4095; a JPEG frame at P=12 decodes to 16 bits, left-justified.
 * @return GIMG_ERR_UNSUPPORTED for any other count or depth.
 */
GIMG_API GIMG_Result gimg_pixel_format_multichannel(
    uint8_t channel_count, uint8_t bits, GIMG_Pixel_Format * out_format);

/** @brief Canonical RGBA 8-bit per channel (sRGB). */
extern const GIMG_Pixel_Format GIMG_PIXEL_RGBA8;
/** @brief Canonical RGBA 16-bit per channel. */
extern const GIMG_Pixel_Format GIMG_PIXEL_RGBA16;
/** @brief Canonical grayscale 8-bit. */
extern const GIMG_Pixel_Format GIMG_PIXEL_GRAY8;
/** @brief Canonical grayscale 16-bit. */
extern const GIMG_Pixel_Format GIMG_PIXEL_GRAY16;
/** @brief Grayscale 12-bit per channel (uint16_t per sample, value 0..4095). */
extern const GIMG_Pixel_Format GIMG_PIXEL_GRAY12;
/** @brief RGBA 12-bit per channel (uint16_t per sample, value 0..4095). */
extern const GIMG_Pixel_Format GIMG_PIXEL_RGBA12;
/** @brief Canonical CMYK 8-bit per channel (C, M, Y, K interleaved). */
extern const GIMG_Pixel_Format GIMG_PIXEL_CMYK8;

/** @brief CMYK 12 bits per channel (uint16_t per sample, value 0..4095). */
extern const GIMG_Pixel_Format GIMG_PIXEL_CMYK12;

/** @brief CMYK 16 bits per channel; carries 12-bit JPEG samples left-justified.
 */
extern const GIMG_Pixel_Format GIMG_PIXEL_CMYK16;

/**
 * @brief Ownership of the pixel buffer.
 *
 * With GIMG_RASTER_OWNED, @a buffer may be NULL (library allocates and zeros)
 * or non-NULL (caller transfers ownership; must have been allocated with the
 * same allocator passed to create; that allocator frees it on destroy).
 * Caller must not free or use the buffer after a successful return.
 */
typedef enum {
  GIMG_RASTER_OWNED = 0, ///< Library owns buffer; freed with raster.
  GIMG_RASTER_BORROWED,  ///< Caller-owned; not freed by raster. buffer non-NULL.
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
 * @param allocator Allocator for the raster struct and, when OWNED and @a buffer is NULL,
 * for the pixel buffer. When OWNED and @a buffer is non-NULL, @a buffer must have been
 * allocated with this same allocator (freed with it on destroy). NULL = default allocator.
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

/**
 * @brief Get color info (primaries, transfer, ICC). Returns default (unknown) if never set.
 */
GIMG_API const GIMG_Color_Info * gimg_raster_color_info_const(
    const GIMG_Raster * raster);

/**
 * @brief Set color info on the raster. If @a info->icc_size > 0, the library
 * copies the ICC bytes and owns them (freed with the raster).
 */
GIMG_API GIMG_Result gimg_raster_set_color_info(GIMG_Raster * raster,
    const GIMG_Color_Info * info);

/**
 * @brief Copy a raster: same dimensions and format, new owned buffer (library
 * stride). Pixel data is copied row-by-row (source stride respected). Color
 * info is copied if set on the source. Uses default allocator.
 * @param src Source raster (not modified).
 * @param out_raster On success, new raster; caller owns it.
 * @return GIMG_OK, GIMG_ERR_UNSUPPORTED (e.g. planar format), GIMG_ERR_OOM.
 */
GIMG_API GIMG_Result gimg_raster_copy(const GIMG_Raster * src,
    GIMG_Raster ** out_raster);

/**
 * @brief Copy a raster with a specific allocator. Same semantics as
 * gimg_raster_copy; the new raster and its buffer use @a allocator (NULL =
 * default).
 */
GIMG_API GIMG_Result gimg_raster_copy_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Raster * src,
    GIMG_Raster ** out_raster);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_RASTER_H
