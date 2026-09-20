/**
 * @file
 *
 * Physical resolution conversion, shared by the codecs that state one
 * (internal).
 *
 * The common metadata records a resolution in dots per inch.  PNG's pHYs
 * (11.3.4.3) and BMP's biXPelsPerMeter both state pixels per metre instead,
 * and JFIF can state either, so the same conversion is wanted in more than one
 * codec and lives here rather than in whichever one grew it first.
 *
 * An inch is exactly 0.0254 m, so this is integer arithmetic - 5000/127 and
 * back - and every resolution from 1 to 1200 dpi survives the round trip
 * exactly.  Zero means "not stated" on both sides, and is preserved as such
 * rather than turning into a resolution of zero.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SRC_CORE_RESOLUTION_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_CORE_RESOLUTION_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Dots per inch to pixels per metre, rounded.
 * @param dpi Dots per inch; 0 means "not stated".
 * @return Pixels per metre, 0 for a dpi of 0, saturated at UINT32_MAX.
 */
uint32_t gimg_dpi_to_pixels_per_meter(uint32_t dpi);

/**
 * @brief Pixels per metre to dots per inch, rounded.
 * @param ppm Pixels per metre; 0 means "not stated".
 * @return Dots per inch, 0 for a ppm of 0.
 */
uint32_t gimg_pixels_per_meter_to_dpi(uint32_t ppm);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_SRC_CORE_RESOLUTION_INTERNAL_H
