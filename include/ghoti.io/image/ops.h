/**
 * @file
 *
 * Basic operations: orientation, pixel format conversion, alpha (spec §8).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_OPS_H
#define GHOTI_IO_GIMG_OPS_H

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply orientation to raster (from metadata or explicit value).
 * @param raster Raster to transform (in-place).
 * @param orientation Source orientation (e.g. from GIMG_META_COMMON).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED for unimplemented orientation.
 */
GIMG_API GIMG_Result gimg_ops_apply_orientation(
    GIMG_Raster * raster, GIMG_Orientation orientation);

/**
 * @brief Convert pixel format: a same-format copy, or CMYK to RGBA.
 *
 * A **same-format** conversion copies the samples, and the result carries the
 * source's GIMG_Color_Info, profile included: copying samples does not change
 * what they mean.
 *
 * **CMYK to RGBA** at the same sample width (CMYK8 to RGBA8, CMYK12 to
 * RGBA12, CMYK16 to RGBA16) performs the naive conversion: each ink is taken
 * as an independent multiplicative filter over white, so a channel is the
 * product of its own colourant and the black, rounded to nearest. This exists
 * because neither PNG nor BMP has CMYK, so without it a four-component JPEG
 * could not be converted into anything at all.
 *
 * It is **not colorimetric**. A real conversion would run the samples through
 * the source profile and a destination profile, and this library has no colour
 * engine; the choice it offers is between the naive conversion and none. It is
 * what libjpeg-based tools do, and it agrees with Pillow exactly on every
 * pixel of every CMYK and YCCK fixture in tests/data/jpeg.
 *
 * The source's `cmyk_polarity` must say which way round the samples are:
 * GIMG_CMYK_POLARITY_UNKNOWN returns GIMG_ERR_UNSUPPORTED rather than a
 * guess, because the two readings are negatives of each other and the wrong
 * one gives a plausible but inverted picture. The JPEG decoder always states
 * it.
 *
 * The result is **opaque** and carries **no** GIMG_Color_Info: what the source
 * said described four ink amounts, and none of it - an embedded profile least
 * of all - is true of the three-channel result.
 *
 * No writer performs this conversion on your behalf. Saving a CMYK raster as
 * a PNG or a BMP still returns GIMG_ERR_UNSUPPORTED, so the library never
 * changes an image's colour without being asked.
 *
 * @param src Source raster.
 * @param dst_format Target format descriptor.
 * @param out_raster On success, new raster in target format.
 * @return GIMG_OK, or GIMG_ERR_UNSUPPORTED for any other pair of formats, for
 *   a CMYK source with no stated polarity, or for a width change (that is
 *   gimg_ops_convert_bit_depth's job).
 */
GIMG_API GIMG_Result gimg_ops_convert_pixel_format(const GIMG_Raster * src,
    const GIMG_Pixel_Format * dst_format, GIMG_Raster ** out_raster);

/**
 * @brief Convert raster bit depth (8, 12, or 16 bits per channel). Same
 * channel model and count; uses library bit-depth conversion (bitshift/clamp).
 *
 * Supported: GRAY, RGBA and CMYK at 8/12/16, and a raster of unnamed channels
 * (GIMG_CHANNEL_UNKNOWN) at any count. Other channel models, a source whose
 * channels are not all the same width, and any depth but 8, 12 or 16 return
 * GIMG_ERR_UNSUPPORTED.
 *
 * The result carries the source's GIMG_Color_Info, profile included: a sample
 * restated at a different precision still means what it meant.
 *
 * @param src Source raster (8-, 12-, or 16-bit per channel).
 * @param dst_bits Target bits per channel (8, 12, or 16).
 * @param out_raster On success, new raster in target bit depth; caller owns it.
 */
GIMG_API GIMG_Result gimg_ops_convert_bit_depth(const GIMG_Raster * src,
    uint8_t dst_bits, GIMG_Raster ** out_raster);

/**
 * @brief Premultiply alpha (straight -> premultiplied) (spec §4.4).
 * @param raster RGBA raster (in-place).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED if format not supported.
 */
GIMG_API GIMG_Result gimg_alpha_premultiply(GIMG_Raster * raster);

/**
 * @brief Unpremultiply alpha (premultiplied -> straight).
 * @param raster RGBA raster (in-place).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED if format not supported.
 */
GIMG_API GIMG_Result gimg_alpha_unpremultiply(GIMG_Raster * raster);

/**
 * @brief Compare two rasters: dimensions, format, and pixel data must match.
 * Strides may differ; comparison is row-by-row over pixel bytes. Color info is
 * not compared.
 * @return true if equal, false if either is NULL or dimensions/format/pixels
 * differ.
 */
GIMG_API bool gimg_ops_raster_equal(
    const GIMG_Raster * a, const GIMG_Raster * b);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_OPS_H
