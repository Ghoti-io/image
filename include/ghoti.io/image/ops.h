/**
 * @file
 *
 * Basic operations: orientation, pixel format conversion, alpha (spec §8).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_OPS_H
#define GHOTI_IO_IMAGE_OPS_H

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Apply orientation to raster (from metadata or explicit value).
 * @param raster Raster to transform (in-place).
 * @param orientation Source orientation (e.g. from GIMG_META_COMMON).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED for unimplemented orientation.
 */
GIMG_API GIMG_RESULT gimg_ops_apply_orientation(
    GIMG_RASTER * raster, GIMG_ORIENTATION orientation);

/**
 * @brief Convert pixel format (stub: same-format copy or UNSUPPORTED).
 * @param src Source raster.
 * @param dst_format Target format descriptor.
 * @param out_raster On success, new raster in target format.
 */
GIMG_API GIMG_RESULT gimg_ops_convert_pixel_format(const GIMG_RASTER * src,
    const GIMG_PIXEL_FORMAT * dst_format, GIMG_RASTER ** out_raster);

/**
 * @brief Premultiply alpha (straight -> premultiplied) (spec §4.4).
 * @param raster RGBA raster (in-place).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED if format not supported.
 */
GIMG_API GIMG_RESULT gimg_alpha_premultiply(GIMG_RASTER * raster);

/**
 * @brief Unpremultiply alpha (premultiplied -> straight).
 * @param raster RGBA raster (in-place).
 * @return GIMG_OK or GIMG_ERR_UNSUPPORTED if format not supported.
 */
GIMG_API GIMG_RESULT gimg_alpha_unpremultiply(GIMG_RASTER * raster);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_OPS_H
