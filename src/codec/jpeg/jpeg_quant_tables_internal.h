/**
 * @file
 *
 * Standard JPEG quantization tables (T.81 Annex K.1). Luminance and
 * chrominance 64-entry tables in natural/row-major order. Encode and save use
 * these; scaling by quality remains in jpeg_save.c.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_JPEG_QUANT_TABLES_INTERNAL_H
#define GHOTI_IO_GIMG_JPEG_QUANT_TABLES_INTERNAL_H

/** Standard luminance quant table (T.81 Annex K.1). 64 entries, row-major. */
extern const unsigned int gimg_jpeg_std_luminance_quant[64];
/** Standard chrominance quant table (T.81 Annex K.1). 64 entries, row-major. */
extern const unsigned int gimg_jpeg_std_chrominance_quant[64];

#ifdef GIMG_JPEG_QUANT_TABLES_DEFINE
// clang-format off
const unsigned int gimg_jpeg_std_luminance_quant[64] = {
    16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99
};
const unsigned int gimg_jpeg_std_chrominance_quant[64] = {
    17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99
};
// clang-format on
#endif

#endif // GHOTI_IO_GIMG_JPEG_QUANT_TABLES_INTERNAL_H
