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
 * Internal metadata structures.
 */

#ifndef GHOTI_IO_GIMG_SRC_META_META_INTERNAL_H
#define GHOTI_IO_GIMG_SRC_META_META_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/allocator.h>
#include <ghoti.io/image/meta.h>
#include <stddef.h>
#include <stdint.h>

struct GIMG_Meta_Common {
  const GIMG_Allocator * allocator;
  GIMG_Orientation orientation;
  uint32_t x_dpi;
  uint32_t y_dpi;
  char * description; ///< Optional; UTF-8 null-terminated; used as comment/description.
};

typedef struct gimg_meta_raw_block {
  char * format_id;
  uint32_t tag_or_chunk_id;
  void * data;
  size_t size;
} gimg_meta_raw_block_t;

struct GIMG_Meta_Raw {
  const GIMG_Allocator * allocator;
  gimg_meta_raw_block_t * blocks;
  size_t count;
  size_t capacity;
};

#endif // GHOTI_IO_GIMG_SRC_META_META_INTERNAL_H
