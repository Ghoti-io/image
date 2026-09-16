/**
 * @file
 *
 * The abbreviated formats of ITU-T T.81 / ISO 10918-1 B.4.
 *
 * B.4 describes two streams that are not complete JPEGs on their own and only
 * mean anything as a pair.  One carries table-specification data and no frame:
 * SOI, then DQT, DHT, DAC and DRI segments (and any miscellaneous ones), then
 * EOI.  The other carries a frame whose table-specification segments are
 * absent, to be read with the tables the first one installed.  Together they
 * let a set of images share one copy of its tables, which is what the format
 * exists for.
 *
 * Neither half is a picture, so neither can go through gimg_doc_load: a
 * table-specification stream has nothing to decode, and an abbreviated image
 * cannot be read without being told which tables to use.  Hence the small API
 * of its own - gimg_jpeg_tables_load, GIMG_Load_Options.jpeg_tables - rather
 * than a heuristic inside the loader.
 *
 * The tables are kept as the segments themselves rather than as parsed tables.
 * Installing them is then the same code that installs a table segment found in
 * an ordinary file, so there is no second reading of B.2.4 to drift from the
 * first.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/stream.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "jpeg_internal.h"

/** Read a big-endian 16-bit segment length. */
static GIMG_Result jpeg_tables_read_u16(GIMG_Stream * stream, uint16_t * out) {
  unsigned char b[2];
  size_t n = 0;
  GIMG_Result r = gimg_stream_read(stream, b, 2, &n);
  if (r != GIMG_OK || n != 2) {
    return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
  }
  *out = (uint16_t)((b[0] << 8) | b[1]);
  return GIMG_OK;
}

GIMG_API GIMG_Result gimg_jpeg_tables_load(
    GIMG_Stream * stream, GIMG_JPEG_Tables ** out_tables) {
  if (!stream || !out_tables) {
    return GIMG_ERR_INTERNAL;
  }
  *out_tables = NULL;
  const GIMG_Allocator * alloc = gimg_alloc_or_default(NULL);
  GIMG_Result r = gimg_jpeg_verify_soi(stream);
  if (r != GIMG_OK) {
    return r;
  }
  unsigned char * buf = NULL;
  size_t len = 0, cap = 0;
  for (;;) {
    uint8_t marker = 0;
    r = gimg_jpeg_read_marker(stream, &marker);
    if (r != GIMG_OK) {
      // Running out of stream before EOI is a truncated file, not an empty
      // table set: B.4 ends a table-specification stream with EOI.
      gimg_free(alloc, buf);
      return GIMG_ERR_FORMAT;
    }
    if (marker == GIMG_JPEG_MARKER_EOI) {
      break;
    }
    // A frame header, a scan, or anything else that belongs to a picture means
    // this is not a table-specification stream (B.4).
    if (marker == GIMG_JPEG_MARKER_SOS || marker == GIMG_JPEG_MARKER_DHP ||
        marker == GIMG_JPEG_MARKER_SOI ||
        jpeg_marker_is_sof(marker)) {
      gimg_free(alloc, buf);
      return GIMG_ERR_FORMAT;
    }
    if (gimg_jpeg_marker_has_no_length(marker)) {
      continue; // TEM and the RSTn markers carry nothing (B.1.1.3).
    }
    uint16_t seg_len = 0;
    r = jpeg_tables_read_u16(stream, &seg_len);
    if (r != GIMG_OK || seg_len < 2u) {
      gimg_free(alloc, buf);
      return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
    }
    size_t payload_size = (size_t)seg_len - 2u;
    if (payload_size > GIMG_JPEG_DEFAULT_MAX_SEGMENT_PAYLOAD) {
      gimg_free(alloc, buf);
      return GIMG_ERR_LIMIT;
    }
    int is_table = (marker == GIMG_JPEG_MARKER_DQT ||
        marker == GIMG_JPEG_MARKER_DHT || marker == GIMG_JPEG_MARKER_DAC ||
        marker == GIMG_JPEG_MARKER_DRI);
    unsigned char * payload = NULL;
    if (payload_size > 0) {
      payload = (unsigned char *)gimg_malloc(alloc, payload_size);
      if (!payload) {
        gimg_free(alloc, buf);
        return GIMG_ERR_OOM;
      }
      size_t got = 0;
      r = gimg_stream_read(stream, payload, payload_size, &got);
      if (r != GIMG_OK || got != payload_size) {
        gimg_free(alloc, payload);
        gimg_free(alloc, buf);
        return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
      }
    }
    if (!is_table) {
      // B.4 allows the miscellaneous segments here too; they carry no tables,
      // so they are read past and dropped.
      gimg_free(alloc, payload);
      continue;
    }
    size_t need = 0;
    if ((need = len + payload_size + 3u, need < len)) {
      gimg_free(alloc, payload);
      gimg_free(alloc, buf);
      return GIMG_ERR_LIMIT;
    }
    if (need > cap) {
      size_t want = cap ? cap * 2u : 256u;
      if (want < need) {
        want = need;
      }
      unsigned char * grown =
          (unsigned char *)gimg_realloc(alloc, buf, want);
      if (!grown) {
        gimg_free(alloc, payload);
        gimg_free(alloc, buf);
        return GIMG_ERR_OOM;
      }
      buf = grown;
      cap = want;
    }
    buf[len++] = marker;
    buf[len++] = (unsigned char)((payload_size >> 8) & 0xFFu);
    buf[len++] = (unsigned char)(payload_size & 0xFFu);
    if (payload_size > 0) {
      memcpy(buf + len, payload, payload_size);
      len += payload_size;
    }
    gimg_free(alloc, payload);
  }
  GIMG_JPEG_Tables * t =
      (GIMG_JPEG_Tables *)gimg_malloc(alloc, sizeof(GIMG_JPEG_Tables));
  if (!t) {
    gimg_free(alloc, buf);
    return GIMG_ERR_OOM;
  }
  t->allocator = alloc;
  t->segments = buf;
  t->size = len;
  *out_tables = t;
  return GIMG_OK;
}

GIMG_API void gimg_jpeg_tables_destroy(GIMG_JPEG_Tables * tables) {
  if (!tables) {
    return;
  }
  const GIMG_Allocator * alloc = gimg_alloc_or_default(tables->allocator);
  gimg_free(alloc, tables->segments);
  gimg_free(alloc, tables);
}

GIMG_Result gimg_jpeg_tables_install(const GIMG_JPEG_Tables * tables,
    gimg_jpeg_doc_state_t * state, const GIMG_Allocator * alloc,
    const char ** out_why) {
  if (out_why) {
    *out_why = NULL;
  }
  if (!tables || !state) {
    return GIMG_OK;
  }
  size_t off = 0;
  while (off + 3u <= tables->size) {
    uint8_t marker = tables->segments[off];
    size_t payload_size = (size_t)((tables->segments[off + 1] << 8) |
        tables->segments[off + 2]);
    off += 3u;
    if (off + payload_size > tables->size) {
      if (out_why) {
        *out_why = "truncated table-specification data";
      }
      return GIMG_ERR_FORMAT;
    }
    const unsigned char * payload = tables->segments + off;
    off += payload_size;
    GIMG_Result r = GIMG_OK;
    switch (marker) {
      case GIMG_JPEG_MARKER_DQT:
        // No frame header has been read yet, so the Pq-against-precision rule
        // of B.2.4.1 cannot be checked here; the frame's own DQT segments are
        // checked against it in the ordinary way.
        r = gimg_jpeg_apply_dqt(state, payload, payload_size, false, out_why);
        break;
      case GIMG_JPEG_MARKER_DHT:
        jpeg_apply_dht_payload(state, payload, payload_size, alloc);
        break;
      case GIMG_JPEG_MARKER_DAC:
        r = gimg_jpeg_apply_dac(state, payload, payload_size, out_why);
        break;
      case GIMG_JPEG_MARKER_DRI:
        r = gimg_jpeg_apply_dri(state, payload, payload_size, out_why);
        break;
      default:
        break;
    }
    if (r != GIMG_OK) {
      return r;
    }
  }
  return GIMG_OK;
}
