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
 * @brief Convert pixel format (same-format copy; anything else UNSUPPORTED).
 *
 * The result carries the source's GIMG_Color_Info, profile included: copying
 * samples does not change what they mean.
 *
 * @param src Source raster.
 * @param dst_format Target format descriptor.
 * @param out_raster On success, new raster in target format.
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
