/**
 * @file
 *
 * Single definition of JPEG constant tables (zigzag, quant, Huffman). Include
 * the internal table headers with the appropriate DEFINE macro set so encode,
 * save, and decode share one source of truth.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define GIMG_JPEG_ZIGZAG_DEFINE
#define GIMG_JPEG_QUANT_TABLES_DEFINE
#define GIMG_JPEG_HUFFMAN_TABLES_DEFINE

#include <ghoti.io/image/macros.h>
#include "jpeg_zigzag_internal.h"
#include "jpeg_quant_tables_internal.h"
#include "jpeg_huffman_tables_internal.h"
