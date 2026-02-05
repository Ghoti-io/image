/**
 * @file
 *
 * Color model: primaries, transfer, ICC stub (spec §4.2).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_IMAGE_COLOR_H
#define GHOTI_IO_IMAGE_COLOR_H

#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Color primaries / white point (simplified; "unknown" when not set).
 */
typedef enum {
  GIMG_PRIMARIES_UNKNOWN = 0,
  GIMG_PRIMARIES_SRGB,
  GIMG_PRIMARIES_ADOBE_RGB,
  GIMG_PRIMARIES_COUNT
} GIMG_PRIMARIES;

/**
 * @brief Transfer function.
 */
typedef enum {
  GIMG_TRANSFER_UNKNOWN = 0,
  GIMG_TRANSFER_LINEAR,
  GIMG_TRANSFER_SRGB,
  GIMG_TRANSFER_GAMMA, ///< Use gamma_value in struct.
  GIMG_TRANSFER_COUNT
} GIMG_TRANSFER;

/**
 * @brief Rendering intent (when ICC present).
 */
typedef enum {
  GIMG_INTENT_PERCEPTUAL = 0,
  GIMG_INTENT_RELATIVE_COLORIMETRIC,
  GIMG_INTENT_SATURATION,
  GIMG_INTENT_ABSOLUTE_COLORIMETRIC,
  GIMG_INTENT_COUNT
} GIMG_RENDERING_INTENT;

/**
 * @brief Color info attached to raster (spec §4.2).
 */
typedef struct {
  GIMG_PRIMARIES primaries;
  GIMG_PRIMARIES white_point;
  GIMG_TRANSFER transfer;
  double gamma_value; ///< Used when transfer == GIMG_TRANSFER_GAMMA.
  GIMG_RENDERING_INTENT intent;
  const void * icc_bytes; ///< Opaque; library does not take ownership.
  size_t icc_size;        ///< ICC profile size in bytes.
  uint8_t _reserved[8];
} GIMG_COLOR_INFO;

/**
 * @brief Initialize color info to unknown/default.
 */
GIMG_API void gimg_color_info_default(GIMG_COLOR_INFO * info);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_IMAGE_COLOR_H
