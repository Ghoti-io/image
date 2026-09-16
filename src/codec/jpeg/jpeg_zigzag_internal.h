/**
 * @file
 *
 * Zigzag and inverse-zigzag order tables for JPEG (T.81). Stream and DQT use
 * zigzag order; decode uses inv_zigzag for natural-order indexing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_JPEG_ZIGZAG_INTERNAL_H
#define GHOTI_IO_GIMG_JPEG_ZIGZAG_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Zigzag order (stream index -> 8x8 row-major position). ITU-T T.81. */
extern const uint8_t gimg_jpeg_zigzag[64];
/** Row-major index -> zigzag (DQT) index. Decode-only. */
extern const uint8_t gimg_jpeg_inv_zigzag[64];

#ifdef GIMG_JPEG_ZIGZAG_DEFINE
// clang-format off
const uint8_t gimg_jpeg_zigzag[64] = {
    0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6,  7,  14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};
const uint8_t gimg_jpeg_inv_zigzag[64] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42,
    3,  8,  12, 17, 25, 30, 41, 43, 9,  11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63,
};
// clang-format on
#endif

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_JPEG_ZIGZAG_INTERNAL_H
