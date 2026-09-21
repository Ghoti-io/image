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
 * Raw metadata (round-trip) implementation.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/meta.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "meta_internal.h"

#define INITIAL_CAPACITY 8

GIMG_API GIMG_Result gimg_meta_raw_create(GIMG_Meta_Raw ** out_raw) {
  return gimg_meta_raw_create_with_allocator(NULL, out_raw);
}

GIMG_API GIMG_Result gimg_meta_raw_create_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Meta_Raw ** out_raw) {
  if (!out_raw) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Meta_Raw * r =
      (GIMG_Meta_Raw *)gimg_malloc(allocator, sizeof(GIMG_Meta_Raw));
  if (!r) {
    return GIMG_ERR_OOM;
  }
  r->allocator = allocator;
  r->blocks = NULL;
  r->count = 0;
  r->capacity = 0;
  *out_raw = r;
  return GIMG_OK;
}

GIMG_API void gimg_meta_raw_destroy(GIMG_Meta_Raw * raw) {
  if (!raw) {
    return;
  }
  const GIMG_Allocator * alloc = raw->allocator;
  for (size_t i = 0; i < raw->count; i++) {
    gimg_free(alloc, raw->blocks[i].format_id);
    gimg_free(alloc, raw->blocks[i].data);
  }
  gimg_free(alloc, raw->blocks);
  gimg_free(alloc, raw);
}

GIMG_API GIMG_Result gimg_meta_raw_attach(GIMG_Meta_Raw * raw,
    const char * format_id, uint32_t tag_or_chunk_id, const void * data,
    size_t size) {
  if (!raw || !format_id) {
    return GIMG_ERR_INTERNAL;
  }
  const GIMG_Allocator * alloc = raw->allocator;
  if (raw->count >= raw->capacity) {
    size_t new_cap = raw->capacity ? raw->capacity * 2 : INITIAL_CAPACITY;
    gimg_meta_raw_block_t * new_blocks = (gimg_meta_raw_block_t *)gimg_realloc(
        alloc, raw->blocks, new_cap * sizeof(gimg_meta_raw_block_t));
    if (!new_blocks) {
      return GIMG_ERR_OOM;
    }
    raw->blocks = new_blocks;
    raw->capacity = new_cap;
  }
  size_t id_len = strlen(format_id) + 1;
  char * id_copy = (char *)gimg_malloc(alloc, id_len);
  if (!id_copy) {
    return GIMG_ERR_OOM;
  }
  memcpy(id_copy, format_id, id_len);
  void * data_copy = NULL;
  if (data && size > 0) {
    data_copy = gimg_malloc(alloc, size);
    if (!data_copy) {
      gimg_free(alloc, id_copy);
      return GIMG_ERR_OOM;
    }
    memcpy(data_copy, data, size);
  }
  raw->blocks[raw->count].format_id = id_copy;
  raw->blocks[raw->count].tag_or_chunk_id = tag_or_chunk_id;
  raw->blocks[raw->count].data = data_copy;
  raw->blocks[raw->count].size = data ? size : 0;
  raw->count++;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_meta_raw_get(const GIMG_Meta_Raw * raw,
    const char * format_id, uint32_t tag_or_chunk_id, void * data,
    size_t * size) {
  if (!raw || !format_id || !size) {
    return GIMG_ERR_INTERNAL;
  }
  for (size_t i = 0; i < raw->count; i++) {
    if (raw->blocks[i].tag_or_chunk_id != tag_or_chunk_id) {
      continue;
    }
    if (strcmp(raw->blocks[i].format_id, format_id) != 0) {
      continue;
    }
    *size = raw->blocks[i].size;
    if (data && raw->blocks[i].size > 0 && raw->blocks[i].data) {
      memcpy(data, raw->blocks[i].data, raw->blocks[i].size);
    }
    return GIMG_OK;
  }
  return GIMG_ERR_UNSUPPORTED; // Not found
}

GIMG_API GIMG_Result gimg_meta_raw_copy(const GIMG_Meta_Raw * src,
    GIMG_Meta_Raw ** out_raw) {
  return gimg_meta_raw_copy_with_allocator(NULL, src, out_raw);
}

GIMG_API GIMG_Result gimg_meta_raw_copy_with_allocator(
    const GIMG_Allocator * allocator, const GIMG_Meta_Raw * src,
    GIMG_Meta_Raw ** out_raw) {
  if (!out_raw) {
    return GIMG_ERR_INTERNAL;
  }
  *out_raw = NULL;
  if (!src) {
    return GIMG_OK;
  }
  GIMG_Result r = gimg_meta_raw_create_with_allocator(allocator, out_raw);
  if (r != GIMG_OK) {
    return r;
  }
  for (size_t i = 0; i < src->count; i++) {
    const gimg_meta_raw_block_t * b = &src->blocks[i];
    r = gimg_meta_raw_attach(*out_raw, b->format_id, b->tag_or_chunk_id,
        b->data, b->size);
    if (r != GIMG_OK) {
      gimg_meta_raw_destroy(*out_raw);
      *out_raw = NULL;
      return r;
    }
  }
  return GIMG_OK;
}
