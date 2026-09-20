/**
 * @file
 *
 * Physical resolution conversion between dots per inch and pixels per metre.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>

#include "resolution_internal.h"

uint32_t gimg_dpi_to_pixels_per_meter(uint32_t dpi) {
  if (dpi == 0) {
    return 0; // "not stated" on both sides
  }
  uint64_t ppm = ((uint64_t)dpi * 5000u + 63u) / 127u;
  return (ppm > UINT32_MAX) ? UINT32_MAX : (uint32_t)ppm;
}

uint32_t gimg_pixels_per_meter_to_dpi(uint32_t ppm) {
  if (ppm == 0) {
    return 0;
  }
  return (uint32_t)(((uint64_t)ppm * 127u + 2500u) / 5000u);
}
