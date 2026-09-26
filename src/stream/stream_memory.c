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
 * Memory stream implementation.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "stream_internal.h"

#define OUTPUT_INITIAL_CAP 4096

GIMG_API GIMG_Result gimg_stream_create_memory(
    const void * data, size_t size, GIMG_Stream ** out_stream) {
  return gimg_stream_create_memory_with_allocator(NULL, data, size,
      out_stream);
}

GIMG_API GIMG_Result gimg_stream_create_memory_no_seek(
    const void * data, size_t size, GIMG_Stream ** out_stream) {
  return gimg_stream_create_memory_no_seek_with_allocator(NULL, data, size,
      out_stream);
}

GIMG_API GIMG_Result gimg_stream_create_memory_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    GIMG_Stream ** out_stream) {
  if (!out_stream) {
    return GIMG_ERR_INTERNAL;
  }
  if (data == NULL && size != 0) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Stream * s =
      (GIMG_Stream *)gimg_malloc(allocator, sizeof(GIMG_Stream));
  if (!s) {
    return GIMG_ERR_OOM;
  }
  s->allocator = allocator;
  s->data = (unsigned char *)data;
  s->size = size;
  s->position = 0;
  s->error = GIMG_OK;
  s->can_seek = true;
  s->writable = false;
  s->max_read_chunk = 0;
  *out_stream = s;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_create_memory_no_seek_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    GIMG_Stream ** out_stream) {
  if (!out_stream) {
    return GIMG_ERR_INTERNAL;
  }
  if (data == NULL && size != 0) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Stream * s =
      (GIMG_Stream *)gimg_malloc(allocator, sizeof(GIMG_Stream));
  if (!s) {
    return GIMG_ERR_OOM;
  }
  s->allocator = allocator;
  s->data = (unsigned char *)data;
  s->size = size;
  s->position = 0;
  s->error = GIMG_OK;
  s->can_seek = false;
  s->writable = false;
  s->max_read_chunk = 0;
  *out_stream = s;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_create_memory_chunked(
    const void * data, size_t size, size_t max_bytes_per_read,
    GIMG_Stream ** out_stream) {
  return gimg_stream_create_memory_chunked_with_allocator(
      NULL, data, size, max_bytes_per_read, out_stream);
}

GIMG_API GIMG_Result gimg_stream_create_memory_chunked_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    size_t max_bytes_per_read, GIMG_Stream ** out_stream) {
  if (!out_stream) {
    return GIMG_ERR_INTERNAL;
  }
  if (data == NULL && size != 0) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Stream * s =
      (GIMG_Stream *)gimg_malloc(allocator, sizeof(GIMG_Stream));
  if (!s) {
    return GIMG_ERR_OOM;
  }
  s->allocator = allocator;
  s->data = (unsigned char *)data;
  s->size = size;
  s->position = 0;
  s->error = GIMG_OK;
  s->can_seek = true;
  s->writable = false;
  s->max_read_chunk = max_bytes_per_read;
  *out_stream = s;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_create_memory_no_seek_chunked(
    const void * data, size_t size, size_t max_bytes_per_read,
    GIMG_Stream ** out_stream) {
  return gimg_stream_create_memory_no_seek_chunked_with_allocator(
      NULL, data, size, max_bytes_per_read, out_stream);
}

GIMG_API GIMG_Result gimg_stream_create_memory_no_seek_chunked_with_allocator(
    const GIMG_Allocator * allocator, const void * data, size_t size,
    size_t max_bytes_per_read, GIMG_Stream ** out_stream) {
  if (!out_stream) {
    return GIMG_ERR_INTERNAL;
  }
  if (data == NULL && size != 0) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Stream * s =
      (GIMG_Stream *)gimg_malloc(allocator, sizeof(GIMG_Stream));
  if (!s) {
    return GIMG_ERR_OOM;
  }
  s->allocator = allocator;
  s->data = (unsigned char *)data;
  s->size = size;
  s->position = 0;
  s->error = GIMG_OK;
  s->can_seek = false;
  s->writable = false;
  s->max_read_chunk = max_bytes_per_read;
  *out_stream = s;
  return GIMG_OK;
}

static GIMG_Result grow_output(GIMG_Stream * s, size_t need) {
  size_t new_cap = s->size ? s->size : OUTPUT_INITIAL_CAP;
  while (new_cap < need) {
    if (new_cap > (size_t)-1 / 2) {
      return GIMG_ERR_OOM;
    }
    new_cap *= 2;
  }
  unsigned char * new_buf =
      (unsigned char *)gimg_realloc(s->allocator, s->data, new_cap);
  if (!new_buf) {
    return GIMG_ERR_OOM;
  }
  s->data = new_buf;
  s->size = new_cap;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_create_memory_output(
    GIMG_Stream ** out_stream) {
  return gimg_stream_create_memory_output_with_allocator(NULL, out_stream);
}

GIMG_API GIMG_Result gimg_stream_create_memory_output_with_allocator(
    const GIMG_Allocator * allocator, GIMG_Stream ** out_stream) {
  if (!out_stream) {
    return GIMG_ERR_INTERNAL;
  }
  allocator = gimg_alloc_or_default(allocator);
  GIMG_Stream * s =
      (GIMG_Stream *)gimg_malloc(allocator, sizeof(GIMG_Stream));
  if (!s) {
    return GIMG_ERR_OOM;
  }
  s->allocator = allocator;
  s->data = NULL;
  s->size = 0;
  s->position = 0;
  s->error = GIMG_OK;
  s->can_seek = true;
  s->writable = true;
  s->max_read_chunk = 0;
  *out_stream = s;
  return GIMG_OK;
}

GIMG_API void gimg_stream_output_buffer(const GIMG_Stream * stream,
    const void ** out_data, size_t * out_size) {
  if (!stream || !out_data || !out_size) {
    return;
  }
  *out_data = stream->writable ? stream->data : NULL;
  *out_size = stream->writable ? stream->position : 0;
}

GIMG_API void gimg_stream_destroy(GIMG_Stream * stream) {
  if (!stream) {
    return;
  }
  if (stream->writable && stream->data) {
    gimg_free(stream->allocator, stream->data);
    stream->data = NULL;
  }
  gimg_free(stream->allocator, stream);
}

GIMG_API GIMG_Result gimg_stream_write(GIMG_Stream * stream,
    const void * buffer, size_t size, size_t * out_bytes_written) {
  if (!stream || !out_bytes_written) {
    return GIMG_ERR_INTERNAL;
  }
  *out_bytes_written = 0;
  if (!stream->writable) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (stream->error != GIMG_OK) {
    return stream->error;
  }
  if (size == 0) {
    return GIMG_OK;
  }
  if (!buffer) {
    return GIMG_ERR_INTERNAL;
  }
  size_t need = stream->position + size;
  if (need > stream->size) {
    GIMG_Result r = grow_output(stream, need);
    if (r != GIMG_OK) {
      stream->error = r;
      return r;
    }
  }
  memcpy(stream->data + stream->position, buffer, size);
  stream->position += size;
  *out_bytes_written = size;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_read(
    GIMG_Stream * stream, void * buffer, size_t size, size_t * out_bytes_read) {
  if (!stream || !buffer || !out_bytes_read) {
    return GIMG_ERR_INTERNAL;
  }
  if (stream->error != GIMG_OK) {
    *out_bytes_read = 0;
    return stream->error;
  }
  if (stream->writable) {
    *out_bytes_read = 0;
    return GIMG_OK;
  }
  if (stream->max_read_chunk > 0 && size > stream->max_read_chunk) {
    size = stream->max_read_chunk;
  }
  size_t avail = stream->size - stream->position;
  if (size > avail) {
    size = avail;
  }
  if (size > 0 && stream->data) {
    memcpy(buffer, stream->data + stream->position, size);
  }
  stream->position += size;
  *out_bytes_read = size;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_read_exact(
    GIMG_Stream * stream, void * buffer, size_t size) {
  if (!stream || !buffer) {
    return GIMG_ERR_INTERNAL;
  }
  unsigned char * p = (unsigned char *)buffer;
  size_t total = 0;
  while (total < size) {
    size_t n = 0;
    GIMG_Result r =
        gimg_stream_read(stream, p + total, size - total, &n);
    if (r != GIMG_OK) {
      return r;
    }
    if (n == 0) {
      return GIMG_ERR_FORMAT;
    }
    total += n;
  }
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_peek(GIMG_Stream * stream, void * buffer,
    size_t size, size_t * out_bytes_available) {
  if (!stream || !buffer || !out_bytes_available) {
    return GIMG_ERR_INTERNAL;
  }
  if (stream->error != GIMG_OK) {
    *out_bytes_available = 0;
    return stream->error;
  }
  size_t avail = stream->size - stream->position;
  if (size > avail) {
    size = avail;
  }
  *out_bytes_available = size;
  if (size > 0 && stream->data) {
    memcpy(buffer, stream->data + stream->position, size);
  }
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_skip(
    GIMG_Stream * stream, size_t count, size_t * out_bytes_skipped) {
  if (!stream || !out_bytes_skipped) {
    return GIMG_ERR_INTERNAL;
  }
  if (stream->error != GIMG_OK) {
    *out_bytes_skipped = 0;
    return stream->error;
  }
  size_t avail = stream->size - stream->position;
  if (count > avail) {
    count = avail;
  }
  stream->position += count;
  *out_bytes_skipped = count;
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_stream_seek(GIMG_Stream * stream, size_t offset) {
  if (!stream) {
    return GIMG_ERR_INTERNAL;
  }
  if (!stream->can_seek) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (offset > stream->size) {
    stream->error = GIMG_ERR_IO;
    return GIMG_ERR_IO;
  }
  stream->position = offset;
  return GIMG_OK;
}

GIMG_API size_t gimg_stream_tell(const GIMG_Stream * stream) {
  if (!stream || !stream->can_seek) {
    return (size_t)-1;
  }
  return stream->position;
}

GIMG_API size_t gimg_stream_size(const GIMG_Stream * stream) {
  // A stream that cannot seek does not know where its end is.  The memory
  // stream behind a non-seekable one was handed a length, but answering it
  // would make this a poor model of the pipe it stands for - and it is the
  // only model the codecs are ever tested against.
  if (!stream || !stream->can_seek) {
    return GIMG_STREAM_SIZE_UNKNOWN;
  }
  return stream->size;
}

GIMG_API GIMG_Result gimg_stream_error(const GIMG_Stream * stream) {
  return stream ? stream->error : GIMG_ERR_INTERNAL;
}

GIMG_API void gimg_limits_default(GIMG_Limits * limits) {
  if (!limits) {
    return;
  }
  *limits = (GIMG_Limits){0};
}
