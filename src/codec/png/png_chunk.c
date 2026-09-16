/**
 * @file
 *
 * PNG signature and chunk parsing with CRC verification.
 *
 * Copyright 2026 by Corey Pennycuff
 *
 * --- Internal algorithms and design ---
 *
 * CRC: PNG spec (5.5) requires CRC-32 over the 4-byte chunk type (big-endian)
 * followed by the payload. We emit the type as big-endian bytes before
 * hashing so the result is spec-compliant on any host endianness. The
 * compress library's gcomp_crc32_update/finalize are used; CRC is verified
 * after reading the payload and the 4-byte CRC that follows.
 *
 * Limits: When GIMG_Limits is provided and max_chunk_size is non-zero, we
 * reject chunks whose payload length exceeds it (bomb protection) before
 * reading any payload.
 *
 * Payload buffer: For small payloads (<= GIMG_PNG_CHUNK_READ_STACK_BUF) we use
 * a stack buffer
 * to avoid heap allocation when the caller does not provide a buffer; for
 * larger payloads we allocate so we can read in one shot and still verify
 * CRC. Caller-provided payload_buf is used when non-NULL.
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/stream.h>
#include <string.h>

#include <ghoti.io/compress/crc32.h>

#include "../../core/alloc_internal.h"
#include "png_internal.h"

const unsigned char gimg_png_signature[GIMG_PNG_SIGNATURE_LEN] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};

GIMG_Result gimg_png_verify_signature(GIMG_Stream * stream) {
  unsigned char buf[GIMG_PNG_SIGNATURE_LEN];
  size_t n = 0;
  GIMG_Result r = gimg_stream_read(stream, buf, GIMG_PNG_SIGNATURE_LEN, &n);
  if (r != GIMG_OK) {
    return r;
  }
  if (n != GIMG_PNG_SIGNATURE_LEN) {
    return GIMG_ERR_FORMAT;
  }
  if (memcmp(buf, gimg_png_signature, GIMG_PNG_SIGNATURE_LEN) != 0) {
    return GIMG_ERR_FORMAT;
  }
  return GIMG_OK;
}

GIMG_Result gimg_png_read_chunk_header(GIMG_Stream * stream,
    uint32_t * out_length, gimg_png_chunk_type_t * out_type) {
  unsigned char buf[GIMG_PNG_CHUNK_HEADER_LEN];
  size_t n = 0;
  GIMG_Result r = gimg_stream_read(stream, buf, GIMG_PNG_CHUNK_HEADER_LEN, &n);
  if (r != GIMG_OK) {
    return r;
  }
  if (n != GIMG_PNG_CHUNK_HEADER_LEN) {
    return GIMG_ERR_CORRUPT;
  }
  // PNG chunk length and type are big-endian; decode from bytes explicitly
  // (portable on any host endianness).
  uint32_t length = (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
      (uint32_t)buf[2] << 8 | (uint32_t)buf[3];
  gimg_png_chunk_type_t type = (gimg_png_chunk_type_t)buf[4] << 24 |
      (uint32_t)buf[5] << 16 | (uint32_t)buf[6] << 8 | (uint32_t)buf[7];
  *out_length = length;
  *out_type = type;
  return GIMG_OK;
}

GIMG_Result gimg_png_read_chunk_payload_and_crc(GIMG_Stream * stream,
    uint32_t length, gimg_png_chunk_type_t type, unsigned char * payload_buf,
    const GIMG_Limits * limits, const GIMG_Allocator * alloc) {
  if (limits && limits->max_chunk_size != 0 &&
      length > limits->max_chunk_size) {
    return GIMG_ERR_LIMIT;
  }

  const GIMG_Allocator * a = gimg_alloc_or_default(alloc);

  uint32_t crc = GCOMP_CRC32_INIT;
  // PNG CRC is over type (4 bytes) + payload; emit type as big-endian bytes
  // so CRC matches spec regardless of host endianness.
  unsigned char type_buf[4];
  type_buf[0] = (unsigned char)(type >> 24);
  type_buf[1] = (unsigned char)(type >> 16);
  type_buf[2] = (unsigned char)(type >> 8);
  type_buf[3] = (unsigned char)(type & 0xFF);
  crc = gcomp_crc32_update(crc, type_buf, 4);

  if (length > 0) {
    unsigned char * read_buf = payload_buf;
    unsigned char stack_buf[GIMG_PNG_CHUNK_READ_STACK_BUF];
    bool use_stack = length <= GIMG_PNG_CHUNK_READ_STACK_BUF;
    if (!payload_buf && !use_stack) {
      read_buf = (unsigned char *)gimg_malloc(a, length);
      if (!read_buf) {
        return GIMG_ERR_OOM;
      }
    }
    else if (!payload_buf) {
      read_buf = stack_buf;
    }

    size_t n = 0;
    GIMG_Result r = gimg_stream_read(stream, read_buf, length, &n);
    if (r != GIMG_OK) {
      if (read_buf != payload_buf && read_buf != stack_buf) {
        gimg_free(a, read_buf);
      }
      return r;
    }
    if (n != (size_t)length) {
      if (read_buf != payload_buf && read_buf != stack_buf) {
        gimg_free(a, read_buf);
      }
      return GIMG_ERR_CORRUPT;
    }

    crc = gcomp_crc32_update(crc, read_buf, length);

    if (payload_buf && payload_buf != read_buf) {
      memcpy(payload_buf, read_buf, length);
    }
    if (read_buf != payload_buf && read_buf != stack_buf) {
      gimg_free(a, read_buf);
    }
  }

  crc = gcomp_crc32_finalize(crc);

  unsigned char crc_buf[4];
  size_t crc_n = 0;
  GIMG_Result r = gimg_stream_read(stream, crc_buf, 4, &crc_n);
  if (r != GIMG_OK || crc_n != 4) {
    return r != GIMG_OK ? r : GIMG_ERR_CORRUPT;
  }
  // Chunk CRC is stored big-endian in the file; decode from bytes explicitly
  // (portable on any host endianness).
  uint32_t stored_crc = (uint32_t)crc_buf[0] << 24 |
      (uint32_t)crc_buf[1] << 16 | (uint32_t)crc_buf[2] << 8 |
      (uint32_t)crc_buf[3];
  if (stored_crc != crc) {
    return GIMG_ERR_CORRUPT;
  }
  return GIMG_OK;
}

GIMG_Result gimg_png_write_chunk(GIMG_Stream * stream,
    gimg_png_chunk_type_t type, const unsigned char * payload,
    size_t payload_size) {
  if (!stream) {
    return GIMG_ERR_INTERNAL;
  }
  uint32_t len = (uint32_t)(payload_size > 0x7FFFFFFFu ? 0x7FFFFFFFu
                                                       : payload_size);
  unsigned char header[8];
  header[0] = (unsigned char)(len >> 24);
  header[1] = (unsigned char)(len >> 16);
  header[2] = (unsigned char)(len >> 8);
  header[3] = (unsigned char)(len & 0xFF);
  header[4] = (unsigned char)(type >> 24);
  header[5] = (unsigned char)(type >> 16);
  header[6] = (unsigned char)(type >> 8);
  header[7] = (unsigned char)(type & 0xFF);
  size_t n = 0;
  GIMG_Result r = gimg_stream_write(stream, header, 8, &n);
  if (r != GIMG_OK || n != 8) {
    return r != GIMG_OK ? r : GIMG_ERR_IO;
  }
  uint32_t crc = GCOMP_CRC32_INIT;
  crc = gcomp_crc32_update(crc, header + 4, 4);
  if (len > 0 && payload) {
    r = gimg_stream_write(stream, payload, (size_t)len, &n);
    if (r != GIMG_OK || n != (size_t)len) {
      return r != GIMG_OK ? r : GIMG_ERR_IO;
    }
    crc = gcomp_crc32_update(crc, payload, (size_t)len);
  }
  crc = gcomp_crc32_finalize(crc);
  unsigned char crc_buf[4];
  crc_buf[0] = (unsigned char)(crc >> 24);
  crc_buf[1] = (unsigned char)(crc >> 16);
  crc_buf[2] = (unsigned char)(crc >> 8);
  crc_buf[3] = (unsigned char)(crc & 0xFF);
  r = gimg_stream_write(stream, crc_buf, 4, &n);
  if (r != GIMG_OK || n != 4) {
    return r != GIMG_OK ? r : GIMG_ERR_IO;
  }
  return GIMG_OK;
}

GIMG_Result gimg_png_parse_ihdr(const unsigned char * payload,
    gimg_png_ihdr_t * ihdr) {
  if (!payload || !ihdr) {
    return GIMG_ERR_INTERNAL;
  }
  // IHDR width and height are big-endian; decode from bytes explicitly
  // (portable on any host endianness).
  uint32_t width = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 |
      (uint32_t)payload[2] << 8 | (uint32_t)payload[3];
  uint32_t height = (uint32_t)payload[4] << 24 | (uint32_t)payload[5] << 16 |
      (uint32_t)payload[6] << 8 | (uint32_t)payload[7];
  uint8_t bit_depth = payload[8];
  uint8_t color_type = payload[9];
  uint8_t compression = payload[10];
  uint8_t filter = payload[11];
  uint8_t interlace = payload[12];

  if (width == 0 || height == 0) {
    return GIMG_ERR_FORMAT;
  }
  if (compression != 0 || filter != 0) {
    return GIMG_ERR_FORMAT;
  }
  if (interlace > 1) {
    return GIMG_ERR_FORMAT;
  }
  switch (color_type) {
  case 0: // Grayscale
    if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 &&
        bit_depth != 8 && bit_depth != 16) {
      return GIMG_ERR_FORMAT;
    }
    break;
  case 2: // RGB
    if (bit_depth != 8 && bit_depth != 16) {
      return GIMG_ERR_FORMAT;
    }
    break;
  case 3: // Palette
    if (bit_depth != 1 && bit_depth != 2 && bit_depth != 4 &&
        bit_depth != 8) {
      return GIMG_ERR_FORMAT;
    }
    break;
  case 4: // Grayscale + alpha
  case 6: // RGBA
    if (bit_depth != 8 && bit_depth != 16) {
      return GIMG_ERR_FORMAT;
    }
    break;
  default:
    return GIMG_ERR_FORMAT;
  }

  ihdr->width = width;
  ihdr->height = height;
  ihdr->bit_depth = bit_depth;
  ihdr->color_type = color_type;
  ihdr->compression_method = compression;
  ihdr->filter_method = filter;
  ihdr->interlace_method = interlace;
  return GIMG_OK;
}

GIMG_Result gimg_png_parse_actl(const unsigned char * payload,
    uint32_t * num_frames, uint32_t * num_plays) {
  if (!payload || !num_frames || !num_plays) {
    return GIMG_ERR_INTERNAL;
  }
  uint32_t nf = (uint32_t)payload[0] << 24 | (uint32_t)payload[1] << 16 |
      (uint32_t)payload[2] << 8 | (uint32_t)payload[3];
  uint32_t np = (uint32_t)payload[4] << 24 | (uint32_t)payload[5] << 16 |
      (uint32_t)payload[6] << 8 | (uint32_t)payload[7];
  if (nf == 0) {
    return GIMG_ERR_FORMAT;  // num_frames must not be 0.
  }
  *num_frames = nf;
  *num_plays = np;
  return GIMG_OK;
}

GIMG_Result gimg_png_parse_fctl(const unsigned char * payload,
    gimg_png_fctl_t * fctl) {
  if (!payload || !fctl) {
    return GIMG_ERR_INTERNAL;
  }
  fctl->sequence_number = (uint32_t)payload[0] << 24 |
      (uint32_t)payload[1] << 16 | (uint32_t)payload[2] << 8 |
      (uint32_t)payload[3];
  fctl->width = (uint32_t)payload[4] << 24 | (uint32_t)payload[5] << 16 |
      (uint32_t)payload[6] << 8 | (uint32_t)payload[7];
  fctl->height = (uint32_t)payload[8] << 24 | (uint32_t)payload[9] << 16 |
      (uint32_t)payload[10] << 8 | (uint32_t)payload[11];
  fctl->x_offset = (uint32_t)payload[12] << 24 | (uint32_t)payload[13] << 16 |
      (uint32_t)payload[14] << 8 | (uint32_t)payload[15];
  fctl->y_offset = (uint32_t)payload[16] << 24 | (uint32_t)payload[17] << 16 |
      (uint32_t)payload[18] << 8 | (uint32_t)payload[19];
  fctl->delay_num = (uint16_t)(((uint16_t)payload[20] << 8) | (uint16_t)payload[21]);
  fctl->delay_den = (uint16_t)(((uint16_t)payload[22] << 8) | (uint16_t)payload[23]);
  fctl->dispose_op = payload[24];
  fctl->blend_op = payload[25];
  if (fctl->dispose_op > 2) {
    return GIMG_ERR_FORMAT;
  }
  if (fctl->blend_op > 1) {
    return GIMG_ERR_FORMAT;
  }
  return GIMG_OK;
}
