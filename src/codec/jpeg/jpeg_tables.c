/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Image.
 *
 * Ghoti.io Image is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Image is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Single definition of JPEG constant tables (zigzag, quant, Huffman). Include
 * the internal table headers with the appropriate DEFINE macro set so encode,
 * save, and decode share one source of truth.
 */

#define GIMG_JPEG_ZIGZAG_DEFINE
#define GIMG_JPEG_QUANT_TABLES_DEFINE
#define GIMG_JPEG_HUFFMAN_TABLES_DEFINE

#include <ghoti.io/image/macros.h>
#include "jpeg_zigzag_internal.h"
#include "jpeg_quant_tables_internal.h"
#include "jpeg_huffman_tables_internal.h"
