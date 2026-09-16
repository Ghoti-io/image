/**
 * @file
 *
 * Color model: primaries, transfer, ICC stub (spec §4.2).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_COLOR_H
#define GHOTI_IO_GIMG_COLOR_H

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
} GIMG_Primaries;

/**
 * @brief Transfer function.
 */
typedef enum {
  GIMG_TRANSFER_UNKNOWN = 0,
  GIMG_TRANSFER_LINEAR,
  GIMG_TRANSFER_SRGB,
  GIMG_TRANSFER_GAMMA, ///< Use gamma_value in struct.
  GIMG_TRANSFER_COUNT
} GIMG_Transfer;

/**
 * @brief Rendering intent (when ICC present).
 */
typedef enum {
  GIMG_INTENT_PERCEPTUAL = 0,
  GIMG_INTENT_RELATIVE_COLORIMETRIC,
  GIMG_INTENT_SATURATION,
  GIMG_INTENT_ABSOLUTE_COLORIMETRIC,
  GIMG_INTENT_COUNT
} GIMG_Rendering_Intent;

/**
 * @brief CMYK channel interpretation (only relevant when raster format is CMYK).
 * Raster pixels are stored as raw values; this describes how to interpret them.
 */
typedef enum {
  GIMG_CMYK_POLARITY_UNKNOWN = 0,
  /** 0 = full ink, 255 = no ink (Adobe / JPEG file convention). */
  GIMG_CMYK_POLARITY_INK,
  /** 0 = no ink, 255 = full ink (reflection; e.g. many design-tool APIs). */
  GIMG_CMYK_POLARITY_REFLECTION,
  GIMG_CMYK_POLARITY_COUNT
} GIMG_CMYK_Polarity;

/**
 * @brief Color info attached to raster (spec §4.2).
 */
typedef struct {
  GIMG_Primaries primaries;
  GIMG_Primaries white_point;
  GIMG_Transfer transfer;
  double gamma_value; ///< Used when transfer == GIMG_TRANSFER_GAMMA.
  GIMG_Rendering_Intent intent;
  const void * icc_bytes; ///< Opaque; library does not take ownership.
  size_t icc_size;        ///< ICC profile size in bytes.
  GIMG_CMYK_Polarity cmyk_polarity; ///< Interpretation of CMYK channels; use when
                                    ///< raster format is GIMG_PIXEL_CMYK8.
  uint8_t _reserved[7];
} GIMG_Color_Info;

/**
 * @brief Initialize color info to unknown/default.
 */
GIMG_API void gimg_color_info_default(GIMG_Color_Info * info);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_COLOR_H
