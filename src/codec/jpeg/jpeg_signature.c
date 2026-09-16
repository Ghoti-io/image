/**
 * @file
 *
 * JPEG SOI signature for probe (0xFF 0xD8).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include "jpeg_internal.h"

const unsigned char gimg_jpeg_signature[GIMG_JPEG_SIGNATURE_LEN] = {
    0xFF, 0xD8};
