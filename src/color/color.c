/**
 * @file
 *
 * Color info default (stub; sRGB/linear conversion in ops).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/color.h>

GIMG_API void gimg_color_info_default(GIMG_COLOR_INFO * info) {
  if (!info) {
    return;
  }
  *info = (GIMG_COLOR_INFO){
      .primaries = GIMG_PRIMARIES_UNKNOWN,
      .white_point = GIMG_PRIMARIES_UNKNOWN,
      .transfer = GIMG_TRANSFER_UNKNOWN,
      .gamma_value = 0.0,
      .intent = GIMG_INTENT_PERCEPTUAL,
      .icc_bytes = NULL,
      .icc_size = 0,
  };
}
