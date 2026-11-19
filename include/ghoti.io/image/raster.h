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
 * Raster image type, pixel format descriptors, and accessors.
 */

#ifndef GHOTI_IO_GIMG_RASTER_H
#define GHOTI_IO_GIMG_RASTER_H

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/color/color.h>
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
 * @brief How a format's alpha channel relates to its color channels.
 *
 * Bytes alone do not say whether an RGBA pixel has been multiplied by its own
 * alpha, and treating a premultiplied buffer as straight (or the reverse) is
 * an error nothing downstream can detect.  The form is therefore recorded
 * with the format, and each operation states the form it accepts.
 */
typedef enum {
  /** No alpha channel; the model is gray, CMYK, and so on.  Zero, so a format
   *  built before this field existed reads as "unsaid", which raster
   *  creation resolves (see GIMG_Pixel_Format::alpha_form). */
  GIMG_ALPHA_NONE = 0,
  /** Color channels are independent of alpha. */
  GIMG_ALPHA_STRAIGHT,
  /** Color channels have been multiplied by alpha (and rounded as
   *  gimg_alpha_premultiply() rounds). */
  GIMG_ALPHA_PREMULTIPLIED
} GIMG_Alpha_Form;

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
  /**
   * A GIMG_Alpha_Form.  Built-in RGBA constants are STRAIGHT and all others
   * NONE.
   *
   * A struct built before this field existed carries zero.  On an alpha-bearing
   * model (GIMG_CHANNEL_RGBA) gimg_raster_create() reads zero as STRAIGHT and
   * stores that in the raster's own copy of the format; the caller's struct is
   * not touched.  On a model with no alpha channel, STRAIGHT or PREMULTIPLIED
   * is refused with GIMG_ERR_INVALID, as is any value that is not a
   * GIMG_Alpha_Form.
   */
  uint8_t alpha_form;
  uint8_t _reserved[4]; ///< Zero; room to grow.
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

/**
 * Byte order of a sample wider than eight bits.
 *
 * A raster whose format has 12- or 16-bit channels (GIMG_PIXEL_GRAY16,
 * GIMG_PIXEL_RGBA16, the 12-bit and CMYK formats) holds each sample as one
 * `uint16_t` in the **host's** byte order, and a raster of 32-bit channels
 * holds one 32-bit value per sample, also in host order.  A sample is
 * therefore read and written through a `uint16_t` (or `uint32_t`) object, and
 * a buffer lent to gimg_raster_create_with_allocator() as GIMG_RASTER_BORROWED
 * must be aligned for that type (the call refuses one that is not).  It is
 * never a pair of bytes in a fixed order.  A codec converts between this and
 * its file format's byte order at its own boundary (PNG stores 16-bit samples
 * big-endian), so one file decodes to the same sample values on a big- and on
 * a little-endian host, and what a codec writes is a function of the sample
 * values and never of the host.
 */

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
 * @brief RGBA, one C float per channel, linear light.
 *
 * For samples an integer cannot hold: what a TIFF LogLuv file decodes to.
 * Values are not clamped - above 1 is brighter than the reference white,
 * below 0 is out of gamut - and the raster's GCOL_Color_Info says what 1
 * means.  Geometry, crop, copy, GIMG_COMPOSITE_SOURCE and conversion to the
 * same format move the bytes untouched; every operation that reads a sample
 * as a number, and every encoder, refuses it with GIMG_ERR_UNSUPPORTED.  A
 * float format at any width but 32 is refused by gimg_raster_create().
 */
extern const GIMG_Pixel_Format GIMG_PIXEL_RGBAF32;

/** @brief Grayscale, one C float, linear light; see GIMG_PIXEL_RGBAF32. */
extern const GIMG_Pixel_Format GIMG_PIXEL_GRAYF32;

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
 *
 * @return GIMG_ERR_UNSUPPORTED for a format with no whole-byte pixel, or a
 *   GIMG_CHANNEL_FLOAT format with any channel not 32 bits wide.
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
GIMG_API const GCOL_Color_Info * gimg_raster_color_info_const(
    const GIMG_Raster * raster);

/**
 * @brief Set color info on the raster. If @a info->icc_size > 0, the library
 * copies the ICC bytes and owns them (freed with the raster).
 */
GIMG_API GIMG_Result gimg_raster_set_color_info(GIMG_Raster * raster,
    const GCOL_Color_Info * info);

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
