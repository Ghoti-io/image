/**
 * @file
 *
 * Bit-depth conversion: 8↔12↔16 bits per sample (bitshift up/down, clamp).
 * Library first-class API; codecs use these when raster depth differs from
 * codec precision.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/bitdepth.h>

// 8 → 12: replicate bits so 0..255 maps to 0..4095. (v*4095+127)/255 or
// (v<<4)|(v>>4) gives good distribution.
GIMG_API uint16_t gimg_bitdepth_8_to_12(uint8_t v) {
  return (uint16_t)(((uint16_t)v << 4) | (uint16_t)(v >> 4));
}

// 8 → 16: replicate bits so 0..255 maps to 0..65535. (v<<8)|v.
GIMG_API uint16_t gimg_bitdepth_8_to_16(uint8_t v) {
  return (uint16_t)(((uint16_t)v << 8) | (uint16_t)v);
}

// 12 → 8: round and clamp. (v*255+2048)/4095, clamp to 255.
GIMG_API uint8_t gimg_bitdepth_12_to_8(uint16_t v) {
  if (v >= 4095u) {
    return 255;
  }
  return (uint8_t)((v * 255u + 2048u) / 4095u);
}

// 12 → 16: left-justify in 16-bit (value << 4).
GIMG_API uint16_t gimg_bitdepth_12_to_16(uint16_t v) {
  return (uint16_t)(v << 4);
}

// 16 → 8: round and clamp. (v+128)>>8, clamp to 255.
GIMG_API uint8_t gimg_bitdepth_16_to_8(uint16_t v) {
  uint32_t x = (uint32_t)v + 128u;
  if (x >= 65536u) {
    return 255;
  }
  return (uint8_t)(x >> 8);
}

// 16 → 12: round and clamp. (v+8)>>4, clamp to 4095.
GIMG_API uint16_t gimg_bitdepth_16_to_12(uint16_t v) {
  uint32_t x = (uint32_t)v + 8u;
  x >>= 4;
  return (uint16_t)(x > 4095u ? 4095u : x);
}
