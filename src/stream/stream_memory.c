/**
 * @file
 *
 * Memory stream implementation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/stream.h>
#include <string.h>

#include "../core/alloc_internal.h"
#include "stream_internal.h"

GIMG_API GIMG_Result gimg_stream_create_memory(
    const void * data, size_t size, GIMG_Stream ** out_stream) {
  return gimg_stream_create_memory_with_allocator(NULL, data, size,
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
  s->data = (const unsigned char *)data;
  s->size = size;
  s->position = 0;
  s->error = GIMG_OK;
  s->can_seek = true;
  *out_stream = s;
  return GIMG_OK;
}

GIMG_API void gimg_stream_destroy(GIMG_Stream * stream) {
  if (!stream) {
    return;
  }
  gimg_free(stream->allocator, stream);
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
  return stream ? stream->position : (size_t)-1;
}

GIMG_API size_t gimg_stream_size(const GIMG_Stream * stream) {
  return stream ? stream->size : (size_t)-1;
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
