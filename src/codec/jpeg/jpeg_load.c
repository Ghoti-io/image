/**
 * @file
 *
 * JPEG load: verify SOI, parse segments (SOF0, DQT, DHT, SOS), enforce
 * limits, build doc with one item and codec-private state.
 *
 * Segment format: 0xFF + marker + length (big-endian 2 bytes where present) +
 * payload. Segment size limit (GIMG_Limits.max_chunk_size or internal default)
 * applies to payload size for bomb protection.
 *
 * Stream contract: Load consumes segments strictly in order and does not
 * require seek or tell. Non-seekable streams (e.g. pipes, socket, chunked
 * HTTP) are supported; gimg_stream_seek and gimg_stream_tell may return
 * GIMG_ERR_UNSUPPORTED / (size_t)-1.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../meta/exif_internal.h"
#include "../codec_internal.h"
#include "jpeg_internal.h"

/** Append diagnostic on load error (codec "jpeg", offset, marker). */
static void jpeg_load_diag(GIMG_Diagnostics * d, size_t offset, uint8_t marker,
    GIMG_Result r, const char * action) {
  if (!d) {
    return;
  }
  (void)gimg_diagnostics_append(
      d, "jpeg", offset, (uint32_t)marker, GIMG_DIAG_ERROR, action);
  (void)r;
}

/** Return true if marker has no length/payload (SOI, EOI, RST0..RST7). */
static bool jpeg_marker_has_no_length(uint8_t marker) {
  if (marker == GIMG_JPEG_MARKER_SOI || marker == GIMG_JPEG_MARKER_EOI) {
    return true;
  }
  if (marker >= 0xD0 && marker <= 0xD7) { // RST0..RST7
    return true;
  }
  return false;
}

/** Get max segment payload from options or internal default. */
static size_t jpeg_max_segment_payload(const GIMG_Limits * limits) {
  if (limits && limits->max_chunk_size != 0) {
    return limits->max_chunk_size;
  }
  return GIMG_JPEG_DEFAULT_MAX_SEGMENT_PAYLOAD;
}

/** Return true if first COM payload looks like 7-bit ASCII text; set *out_ptr
 * and *out_len to the text (up to first null). */
static bool jpeg_first_com_looks_like_text(const unsigned char * com_combined,
    size_t com_size, const unsigned char ** out_ptr, size_t * out_len) {
  if (!com_combined || com_size < 2 || !out_ptr || !out_len) {
    return false;
  }
  size_t plen = (size_t)((com_combined[0] << 8) | com_combined[1]);
  if (plen > com_size - 2) {
    return false;
  }
  const unsigned char * payload = com_combined + 2;
  size_t len = plen;
  for (size_t i = 0; i < plen; i++) {
    if (payload[i] == 0) {
      len = i;
      break;
    }
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = payload[i];
    if (c != 0x09 && c != 0x0A && c != 0x0D && (c < 0x20 || c > 0x7E)) {
      return false;
    }
  }
  *out_ptr = payload;
  *out_len = len;
  return true;
}

GIMG_Result gimg_jpeg_verify_soi(GIMG_Stream * stream) {
  unsigned char buf[GIMG_JPEG_SIGNATURE_LEN];
  GIMG_Result r = gimg_stream_read_exact(stream, buf, GIMG_JPEG_SIGNATURE_LEN);
  if (r != GIMG_OK) {
    return GIMG_ERR_FORMAT;
  }
  if (buf[0] != 0xFF || buf[1] != GIMG_JPEG_MARKER_SOI) {
    return GIMG_ERR_FORMAT;
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_read_marker(GIMG_Stream * stream, uint8_t * out_marker) {
  for (;;) {
    unsigned char b = 0;
    size_t n = 0;
    GIMG_Result r = gimg_stream_read(stream, &b, 1, &n);
    if (r != GIMG_OK || n == 0) {
      return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
    }
    if (b != 0xFF) {
      continue; // Skip until 0xFF.
    }
    r = gimg_stream_read(stream, &b, 1, &n);
    if (r != GIMG_OK || n == 0) {
      return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
    }
    if (b == 0x00) {
      continue; // Byte stuffing: 0xFF 0x00 is data.
    }
    *out_marker = b;
    return GIMG_OK;
  }
}

GIMG_Result gimg_jpeg_read_segment_length(
    GIMG_Stream * stream, uint16_t * out_length) {
  unsigned char buf[2];
  GIMG_Result r = gimg_stream_read_exact(stream, buf, 2);
  if (r != GIMG_OK) {
    return GIMG_ERR_FORMAT;
  }
  *out_length = (uint16_t)((buf[0] << 8) | buf[1]);
  return GIMG_OK;
}

/** Parse SOF0 (baseline) or SOF2 (progressive) payload. Length already read. */
static GIMG_Result jpeg_parse_sof(
    const unsigned char * payload, size_t len, gimg_jpeg_sof_t * sof) {
  if (len < 8) {
    return GIMG_ERR_FORMAT;
  }
  uint8_t precision = payload[0];
  uint16_t height = (uint16_t)((payload[1] << 8) | payload[2]);
  uint16_t width = (uint16_t)((payload[3] << 8) | payload[4]);
  uint8_t num_components = payload[5];
  if (precision != 8 || num_components == 0 ||
      num_components > GIMG_JPEG_MAX_COMPONENTS) {
    return GIMG_ERR_FORMAT;
  }
  if (height == 0 || width == 0) {
    return GIMG_ERR_FORMAT;
  }
  if (height > GIMG_JPEG_MAX_DIMENSION || width > GIMG_JPEG_MAX_DIMENSION) {
    return GIMG_ERR_LIMIT;
  }
  size_t need = 6 + (size_t)num_components * 3;
  if (len < need) {
    return GIMG_ERR_FORMAT;
  }
  memset(sof, 0, sizeof(*sof));
  sof->precision = precision;
  sof->height = height;
  sof->width = width;
  sof->num_components = num_components;
  for (uint8_t i = 0; i < num_components; i++) {
    sof->comp_id[i] = payload[6 + i * 3];
    sof->h_samp[i] = (payload[7 + i * 3] >> 4) & 0x0Fu;
    sof->v_samp[i] = payload[7 + i * 3] & 0x0Fu;
    sof->quant_tbl_id[i] = payload[8 + i * 3];
    if (sof->h_samp[i] == 0 || sof->v_samp[i] == 0) {
      return GIMG_ERR_FORMAT;
    }
  }
  return GIMG_OK;
}

/** Append to the current scan's data (state->scans[num_scans - 1]). */
static GIMG_Result jpeg_append_scan_data(
    gimg_jpeg_doc_state_t * state, const unsigned char * data, size_t len) {
  if (state->num_scans == 0) {
    return GIMG_ERR_FORMAT;
  }
  gimg_jpeg_scan_t * scan = &state->scans[state->num_scans - 1];
  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);
  size_t new_size = scan->data_size + len;
  unsigned char * new_buf =
      (unsigned char *)gimg_realloc(alloc, scan->data, new_size);
  if (!new_buf && new_size > 0) {
    return GIMG_ERR_OOM;
  }
  scan->data = new_buf;
  if (len > 0 && data) {
    memcpy(scan->data + scan->data_size, data, len);
  }
  scan->data_size = new_size;
  return GIMG_OK;
}

void gimg_jpeg_free_doc_state(GIMG_Codec * codec, void * codec_private) {
  (void)codec;
  gimg_jpeg_doc_state_t * state = (gimg_jpeg_doc_state_t *)codec_private;
  if (!state) {
    return;
  }
  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);
  for (int i = 0; i < 4; i++) {
    gimg_free(alloc, state->huff_dc[i]);
    gimg_free(alloc, state->huff_ac[i]);
  }
  for (unsigned i = 0; i < state->num_scans; i++) {
    gimg_free(alloc, state->scans[i].data);
  }
  gimg_free(alloc, state->app0_jfif);
  gimg_free(alloc, state->app0_jfxx);
  gimg_free(alloc, state->app1_exif);
  gimg_free(alloc, state->app1_xmp);
  gimg_free(alloc, state->app2_icc);
  gimg_free(alloc, state->com_combined);
  gimg_free(alloc, state);
}

GIMG_Result gimg_jpeg_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc) {
  if (!codec || !stream || !out_doc) {
    return GIMG_ERR_INTERNAL;
  }
  *out_doc = NULL;

  GIMG_Result r = gimg_jpeg_verify_soi(stream);
  if (r != GIMG_OK) {
    if (diagnostics) {
      (void)gimg_diagnostics_append(diagnostics, "jpeg", 0u,
          (uint32_t)GIMG_JPEG_MARKER_SOI, GIMG_DIAG_ERROR,
          "missing or invalid SOI");
    }
    return r;
  }

  const GIMG_Allocator * alloc = codec->allocator;
  alloc = gimg_alloc_or_default(alloc);
  const GIMG_Limits * limits = options ? options->limits : NULL;
  size_t max_seg_payload = jpeg_max_segment_payload(limits);

  gimg_jpeg_doc_state_t * state = (gimg_jpeg_doc_state_t *)gimg_malloc(
      alloc, sizeof(gimg_jpeg_doc_state_t));
  if (!state) {
    return GIMG_ERR_OOM;
  }
  memset(state, 0, sizeof(*state));
  state->allocator = alloc;

  bool seen_sof = false;
  bool have_pending_marker = false;
  uint8_t pending_marker = 0;

  for (;;) {
    size_t seg_start = 0;
    uint8_t marker = 0;
    if (have_pending_marker) {
      marker = pending_marker;
      have_pending_marker = false;
      r = GIMG_OK;
    }
    else {
      size_t pos = gimg_stream_tell(stream);
      if (pos != (size_t)-1) {
        seg_start = pos;
      }
      r = gimg_jpeg_read_marker(stream, &marker);
    }
    if (r != GIMG_OK) {
      jpeg_load_diag(diagnostics, seg_start, 0, r, "truncated before marker");
      gimg_jpeg_free_doc_state(codec, state);
      return r;
    }

    if (marker == GIMG_JPEG_MARKER_EOI) {
      break;
    }

    if (jpeg_marker_has_no_length(marker)) {
      if (marker >= 0xD0 && marker <= 0xD7) {
        // RST: may appear in scan data; we already advanced past 0xFF and
        // marker byte.
      }
      continue;
    }

    uint16_t length = 0;
    r = gimg_jpeg_read_segment_length(stream, &length);
    if (r != GIMG_OK) {
      jpeg_load_diag(
          diagnostics, seg_start, marker, r, "truncated segment length");
      gimg_jpeg_free_doc_state(codec, state);
      return r;
    }
    // Payload size = length - 2 (length field is 2 bytes).
    size_t payload_size = (length >= 2) ? (size_t)(length - 2) : 0;
    if (length < 2) {
      jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
          "invalid segment length");
      gimg_jpeg_free_doc_state(codec, state);
      return GIMG_ERR_FORMAT;
    }
    if (payload_size > max_seg_payload) {
      jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_LIMIT,
          "segment exceeds max_chunk_size");
      gimg_jpeg_free_doc_state(codec, state);
      return GIMG_ERR_LIMIT;
    }

    unsigned char * payload_buf = NULL;
    if (payload_size > 0) {
      payload_buf = (unsigned char *)gimg_malloc(alloc, payload_size);
      if (!payload_buf) {
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      r = gimg_stream_read_exact(stream, payload_buf, payload_size);
      if (r != GIMG_OK) {
        gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_IO,
            "truncated segment payload");
        gimg_jpeg_free_doc_state(codec, state);
        return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
      }
    }

    switch (marker) {
    case GIMG_JPEG_MARKER_SOF0: {
      if (seen_sof) {
        gimg_free(alloc, payload_buf);
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      r = jpeg_parse_sof(payload_buf, payload_size, &state->sof);
      gimg_free(alloc, payload_buf);
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r, "invalid SOF0");
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
      state->is_progressive = 0;
      seen_sof = true;
      break;
    }
    case GIMG_JPEG_MARKER_SOF2: {
      if (seen_sof) {
        gimg_free(alloc, payload_buf);
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      r = jpeg_parse_sof(payload_buf, payload_size, &state->sof);
      gimg_free(alloc, payload_buf);
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r, "invalid SOF2");
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
      state->is_progressive = 1;
      seen_sof = true;
      break;
    }
    case GIMG_JPEG_MARKER_DQT: {
      // DQT: one or more tables. Each table: 1 byte (Pq<<4|Tq), then 64 or 128
      // bytes.
      const unsigned char * p = payload_buf;
      size_t remain = payload_size;
      while (remain >= 2) {
        uint8_t pq_tq = p[0];
        uint8_t tq = pq_tq & 0x0Fu;
        int is_16bit = (pq_tq >> 4) != 0;
        size_t entry_bytes = is_16bit ? 128u : 64u;
        if (tq >= GIMG_JPEG_MAX_QUANT_TABLES || remain < 1 + entry_bytes) {
          break;
        }
        p++;
        remain--;
        if (remain < entry_bytes) {
          break;
        }
        state->quant_tbl_present[tq] = 1;
        if (is_16bit) {
          for (size_t i = 0; i < GIMG_JPEG_DQT_ENTRIES; i++) {
            state->quant_tbl[tq][i] =
                (uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
          }
        }
        else {
          for (size_t i = 0; i < GIMG_JPEG_DQT_ENTRIES; i++) {
            state->quant_tbl[tq][i] = (uint16_t)p[i];
          }
        }
        p += entry_bytes;
        remain -= entry_bytes;
      }
      gimg_free(alloc, payload_buf);
      break;
    }
    case GIMG_JPEG_MARKER_DRI: {
      // DRI: length 2 + 2-byte payload (restart interval in MCUs,
      // big-endian).
      if (payload_size != 2 || !payload_buf) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "DRI payload must be 2 bytes");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      state->restart_interval =
          (uint16_t)((payload_buf[0] << 8) | payload_buf[1]);
      gimg_free(alloc, payload_buf);
      break;
    }
    case GIMG_JPEG_MARKER_DHT: {
      // DHT: one or more tables. Each: 1 byte (Tc<<4|Th), then 16 bytes counts,
      // then symbols.
      const unsigned char * p = payload_buf;
      size_t remain = payload_size;
      while (remain >= 18) {
        uint8_t tc_th = p[0];
        uint8_t th = tc_th & 0x0Fu;
        uint8_t tc = (tc_th >> 4) & 1;
        size_t num_symbols = 0;
        for (int i = 1; i <= 16; i++) {
          num_symbols += p[i];
        }
        if (th >= 4 || remain < 17 + num_symbols) {
          break;
        }
        size_t table_len = 17 + num_symbols;
        unsigned char ** dest = tc ? &state->huff_ac[th] : &state->huff_dc[th];
        size_t * dest_len =
            tc ? &state->huff_ac_len[th] : &state->huff_dc_len[th];
        if (*dest) {
          gimg_free(alloc, *dest);
        }
        *dest = (unsigned char *)gimg_malloc(alloc, table_len);
        if (*dest) {
          memcpy(*dest, p, table_len);
          *dest_len = table_len;
        }
        p += table_len;
        remain -= table_len;
      }
      gimg_free(alloc, payload_buf);
      break;
    }
    case GIMG_JPEG_MARKER_SOS: {
      if (!seen_sof) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(
            diagnostics, seg_start, marker, GIMG_ERR_FORMAT, "SOS before SOF");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      if (!state->is_progressive && state->num_scans > 0) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "multiple SOS in baseline");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      if (state->num_scans >= GIMG_JPEG_MAX_SCANS) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(
            diagnostics, seg_start, marker, GIMG_ERR_LIMIT, "too many scans");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_LIMIT;
      }
      // Parse SOS header per ITU-T T.81 / ISO/IEC 10918-1 Annex B: Ns (1), then
      // Ns x (Cs, Td|Ta), then Ss (1), Se (1), and one byte with Ah (high 4
      // bits) and Al (low 4 bits). So payload length is 6 + 2*Ns - 2 = 4 + 2*Ns
      // bytes.
      const size_t min_sos_payload = 6u; // Ns=1: 1 + 2 + 3 = 6
      if (payload_size < min_sos_payload || !payload_buf) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "SOS payload too short");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      {
        uint8_t ns = payload_buf[0];
        if (ns == 0 || ns > state->sof.num_components ||
            (size_t)(4 + ns * 2) > payload_size) {
          if (payload_buf)
            gimg_free(alloc, payload_buf);
          jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
              "invalid SOS Ns or payload length");
          gimg_jpeg_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        gimg_jpeg_scan_t * scan = &state->scans[state->num_scans];
        memset(scan, 0, sizeof(*scan));
        scan->comp_count = ns;
        for (uint8_t i = 0; i < ns; i++) {
          scan->comp_id[i] = payload_buf[1 + i * 2];
          scan->dc_tbl[i] = (payload_buf[2 + i * 2] >> 4) & 0x0Fu;
          scan->ac_tbl[i] = payload_buf[2 + i * 2] & 0x0Fu;
        }
        scan->ss = payload_buf[1 + ns * 2];
        scan->se = payload_buf[2 + ns * 2];
        {
          uint8_t ah_al =
              payload_buf[3 + ns * 2]; // T.81: Ah high nibble, Al low
          scan->ah = (ah_al >> 4) & 0x0Fu;
          scan->al = ah_al & 0x0Fu;
        }
        state->num_scans++;
      }
      gimg_free(alloc, payload_buf);
      payload_buf = NULL;
      // Read scan data until next marker (0xFF followed by non-0x00).
      for (;;) {
        unsigned char b;
        size_t n = 0;
        r = gimg_stream_read(stream, &b, 1, &n);
        if (r != GIMG_OK || n == 0) {
          break;
        }
        if (b != 0xFF) {
          r = jpeg_append_scan_data(state, &b, 1);
          if (r != GIMG_OK) {
            gimg_jpeg_free_doc_state(codec, state);
            return r;
          }
          continue;
        }
        r = gimg_stream_peek(stream, &b, 1, &n);
        if (r != GIMG_OK || n == 0) {
          r = jpeg_append_scan_data(state, (const unsigned char *)"\xFF", 1);
          if (r != GIMG_OK) {
            gimg_jpeg_free_doc_state(codec, state);
            return r;
          }
          break;
        }
        if (b == 0x00) {
          (void)gimg_stream_read(stream, &b, 1, &n);
          r = jpeg_append_scan_data(
              state, (const unsigned char *)"\xFF\x00", 2);
          if (r != GIMG_OK) {
            gimg_jpeg_free_doc_state(codec, state);
            return r;
          }
          continue;
        }
        if (b >= 0xD0 && b <= 0xD7) {
          // RST0..RST7: part of scan data; consume and continue same scan
          // (do not start a new segment).
          (void)gimg_stream_read(stream, &b, 1, &n);
          continue;
        }
        // Next byte is a real marker; consume it and store for next iteration
        // so the main loop can process it without seeking (non-seekable
        // support).
        (void)gimg_stream_read(stream, &b, 1, &n);
        pending_marker = b;
        have_pending_marker = true;
        break;
      }
      break;
    }
    case GIMG_JPEG_MARKER_APP0: {
      if (payload_size >= 14 && payload_buf &&
          memcmp(payload_buf, "JFIF\0", 5) == 0) {
        if (state->app0_jfif) {
          gimg_free(alloc, state->app0_jfif);
        }
        state->app0_jfif = payload_buf;
        state->app0_jfif_len = payload_size;
        payload_buf = NULL;
      }
      else if (payload_size >= 6 && payload_buf &&
          memcmp(payload_buf, "JFXX\0", 5) == 0) {
        if (state->app0_jfxx) {
          gimg_free(alloc, state->app0_jfxx);
        }
        state->app0_jfxx = payload_buf;
        state->app0_jfxx_len = payload_size;
        payload_buf = NULL;
      }
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      break;
    }
    case GIMG_JPEG_MARKER_APP1: {
      if (payload_size >= 6 && payload_buf &&
          memcmp(payload_buf, "Exif\0\0", 6) == 0) {
        if (state->app1_exif) {
          gimg_free(alloc, state->app1_exif);
        }
        state->app1_exif = payload_buf;
        state->app1_exif_len = payload_size;
        payload_buf = NULL;
      }
      else if (payload_size >= 29 && payload_buf &&
          memcmp(payload_buf, "http://ns.adobe.com/xap/1.0/\0", 29) == 0) {
        if (state->app1_xmp) {
          gimg_free(alloc, state->app1_xmp);
        }
        state->app1_xmp = payload_buf;
        state->app1_xmp_len = payload_size;
        payload_buf = NULL;
      }
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      break;
    }
    case GIMG_JPEG_MARKER_APP2: {
      if (payload_size >= 12 && payload_buf &&
          memcmp(payload_buf, "ICC_PROFILE\0", 12) == 0) {
        if (state->app2_icc) {
          gimg_free(alloc, state->app2_icc);
        }
        state->app2_icc = payload_buf;
        state->app2_icc_len = payload_size;
        payload_buf = NULL;
      }
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      break;
    }
    case GIMG_JPEG_MARKER_COM: {
      // COM: length (2) + comment payload (any bytes). Append to com_combined
      // as (2-byte BE length + payload) for round-trip and multiple COM.
      if (payload_size > 65535u) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        break;
      }
      size_t need = state->com_combined_size + 2 + payload_size;
      unsigned char * new_buf =
          (unsigned char *)gimg_realloc(alloc, state->com_combined, need);
      if (!new_buf && need > 0) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(
            diagnostics, seg_start, marker, GIMG_ERR_OOM, "COM append OOM");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_OOM;
      }
      state->com_combined = new_buf;
      state->com_combined[state->com_combined_size] =
          (unsigned char)(payload_size >> 8);
      state->com_combined[state->com_combined_size + 1] =
          (unsigned char)(payload_size & 0xFFu);
      if (payload_size > 0 && payload_buf) {
        memcpy(state->com_combined + state->com_combined_size + 2, payload_buf,
            payload_size);
        gimg_free(alloc, payload_buf);
      }
      state->com_combined_size = need;
      payload_buf = NULL;
      break;
    }
    default:
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      break;
    }
  }

  if (!seen_sof) {
    jpeg_load_diag(diagnostics, 0u, 0u, GIMG_ERR_FORMAT, "no SOF0/SOF2 found");
    gimg_jpeg_free_doc_state(codec, state);
    return GIMG_ERR_FORMAT;
  }

  // Overflow-safe pixel count and max_decoded_pixels check.
  size_t pixel_count = 0;
  r = gimg_safe_pixel_count(state->sof.width, state->sof.height, &pixel_count);
  if (r != GIMG_OK) {
    gimg_jpeg_free_doc_state(codec, state);
    return GIMG_ERR_LIMIT;
  }
  if (limits && limits->max_decoded_pixels != 0 &&
      pixel_count > limits->max_decoded_pixels) {
    jpeg_load_diag(diagnostics, 0u, (uint32_t)GIMG_JPEG_MARKER_SOF0,
        GIMG_ERR_LIMIT, "increase max_decoded_pixels");
    gimg_jpeg_free_doc_state(codec, state);
    return GIMG_ERR_LIMIT;
  }

  // Build document.
  GIMG_Doc * doc = (GIMG_Doc *)gimg_malloc(alloc, sizeof(GIMG_Doc));
  if (!doc) {
    gimg_jpeg_free_doc_state(codec, state);
    return GIMG_ERR_OOM;
  }
  doc->allocator = alloc;
  doc->item_count = 1;
  doc->items = (GIMG_Item *)gimg_malloc(alloc, sizeof(GIMG_Item));
  if (!doc->items) {
    gimg_free(alloc, doc);
    gimg_jpeg_free_doc_state(codec, state);
    return GIMG_ERR_OOM;
  }
  doc->loaded_by_codec = codec;
  doc->codec_private = state;
  doc->meta_raw = NULL;
  doc->meta_common = NULL;
  doc->items[0].index = 0;
  doc->items[0].doc = doc;
  doc->items[0].frame_delay_num = 0;
  doc->items[0].frame_delay_den = 0;
  doc->items[0].dispose_op = GIMG_DISPOSE_NONE;
  doc->items[0].blend_op = GIMG_BLEND_SOURCE;
  doc->items[0].raster = NULL;

  // Attach APP segments and COM to doc meta_raw for round-trip; populate
  // meta_common.
  GIMG_Meta_Raw * raw = NULL;
  if ((state->app0_jfif && state->app0_jfif_len > 0) ||
      (state->app0_jfxx && state->app0_jfxx_len > 0) ||
      (state->app1_exif && state->app1_exif_len > 0) ||
      (state->app1_xmp && state->app1_xmp_len > 0) ||
      (state->app2_icc && state->app2_icc_len > 0) ||
      (state->com_combined && state->com_combined_size > 0)) {
    r = gimg_doc_ensure_meta_raw(doc, &raw);
    if (r != GIMG_OK) {
      gimg_doc_destroy(doc);
      return r;
    }
    if (state->app0_jfif && state->app0_jfif_len > 0) {
      r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP0,
          state->app0_jfif, state->app0_jfif_len);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
    }
    if (state->app0_jfxx && state->app0_jfxx_len > 0) {
      r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP0_JFXX,
          state->app0_jfxx, state->app0_jfxx_len);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
    }
    if (state->app1_exif && state->app1_exif_len > 0) {
      r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP1_EXIF,
          state->app1_exif, state->app1_exif_len);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
    }
    if (state->app1_xmp && state->app1_xmp_len > 0) {
      r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP1_XMP,
          state->app1_xmp, state->app1_xmp_len);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
    }
    if (state->app2_icc && state->app2_icc_len > 0) {
      r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC,
          state->app2_icc, state->app2_icc_len);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
    }
    if (state->com_combined && state->com_combined_size > 0) {
      r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_COM,
          state->com_combined, state->com_combined_size);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
    }
  }

  // Populate meta_common from EXIF (orientation) and JFIF (DPI).
  GIMG_Meta_Common * meta_common = NULL;
  if (state->app1_exif && state->app1_exif_len > 6) {
    GIMG_Orientation orient = GIMG_ORIENTATION_UNKNOWN;
    if (gimg_exif_parse_orientation(state->app1_exif + 6,
            state->app1_exif_len - 6, &orient) == GIMG_OK &&
        orient != GIMG_ORIENTATION_UNKNOWN) {
      if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
        gimg_meta_common_set_orientation(meta_common, orient);
      }
    }
  }
  if (state->app0_jfif && state->app0_jfif_len >= 14) {
    uint8_t units = state->app0_jfif[7];
    if (units == 1) {
      uint32_t x_dpi = (uint32_t)((state->app0_jfif[8] << 8) |
          (unsigned char)state->app0_jfif[9]);
      uint32_t y_dpi = (uint32_t)((state->app0_jfif[10] << 8) |
          (unsigned char)state->app0_jfif[11]);
      if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
        gimg_meta_common_set_dpi(meta_common, x_dpi, y_dpi);
      }
    }
  }
  // Populate meta_common description from first COM when 7-bit ASCII text.
  if (state->com_combined && state->com_combined_size >= 2) {
    const unsigned char * text_ptr = NULL;
    size_t text_len = 0;
    if (jpeg_first_com_looks_like_text(state->com_combined,
            state->com_combined_size, &text_ptr, &text_len)) {
      if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
        char * buf = (char *)gimg_malloc(alloc, text_len + 1u);
        if (buf) {
          memcpy(buf, text_ptr, text_len);
          buf[text_len] = '\0';
          (void)gimg_meta_common_set_description(meta_common, buf);
          gimg_free(alloc, buf);
        }
      }
    }
  }

  // EXIF embedded thumbnail (IFD1): try Compression=6 (JPEG), then 1 (uncompressed), then 7 (TIFF JPEG).
  if (state->app1_exif && state->app1_exif_len > 6) {
    const void * exif_tiff = state->app1_exif + 6;
    size_t exif_tiff_len = state->app1_exif_len - 6;
    int thumb_added = 0;

    // 1) Compression=6: JPEG thumbnail
    const void * thumb_data = NULL;
    size_t thumb_len = 0;
    if (!thumb_added &&
        gimg_exif_embedded_thumbnail_jpeg(
            exif_tiff, exif_tiff_len, &thumb_data, &thumb_len) == GIMG_OK &&
        thumb_len > 0) {
      GIMG_Stream * thumb_stream = NULL;
      r = gimg_stream_create_memory_with_allocator(
          alloc, thumb_data, thumb_len, &thumb_stream);
      if (r == GIMG_OK && thumb_stream) {
        GIMG_Doc * thumb_doc = NULL;
        r = gimg_doc_load(thumb_stream, options, diagnostics, &thumb_doc);
        gimg_stream_destroy(thumb_stream);
        if (r == GIMG_OK && thumb_doc && gimg_doc_item_count(thumb_doc) >= 1) {
          GIMG_Raster * thumb_raster = NULL;
          r = gimg_item_decode(
              gimg_doc_item(thumb_doc, 0), NULL, &thumb_raster);
          if (r == GIMG_OK && thumb_raster) {
            GIMG_Raster * copy_raster = NULL;
            r = gimg_raster_copy_with_allocator(
                alloc, thumb_raster, &copy_raster);
            gimg_raster_destroy(thumb_raster);
            if (r == GIMG_OK && copy_raster) {
              r = gimg_doc_set_item_count(doc, 2);
              if (r == GIMG_OK) {
                gimg_item_set_raster(gimg_doc_item(doc, 1), copy_raster);
                thumb_added = 1;
              }
              else {
                gimg_raster_destroy(copy_raster);
              }
            }
          }
          gimg_doc_destroy(thumb_doc);
        }
        else if (thumb_doc) {
          gimg_doc_destroy(thumb_doc);
        }
      }
    }

    // 2) Compression=1: Uncompressed (strip data -> raster)
    if (!thumb_added) {
      uint32_t tw = 0;
      uint32_t th = 0;
      uint8_t tbits = 0;
      uint16_t tphoto = 0xFFFF;
      void * strip_data = NULL;
      size_t strip_size = 0;
      if (gimg_exif_embedded_thumbnail_uncompressed(alloc, exif_tiff,
              exif_tiff_len, &tw, &th, &tbits, &tphoto, &strip_data,
              &strip_size) == GIMG_OK &&
          strip_size > 0 && tw > 0 && th > 0) {
        const GIMG_Pixel_Format * fmt = NULL;
        if (tphoto <= 1) {
          fmt = (tbits <= 8) ? &GIMG_PIXEL_GRAY8 : &GIMG_PIXEL_GRAY16;
        }
        else if (tphoto == 2 && tbits == 8) {
          fmt = &GIMG_PIXEL_RGBA8;
        }
        if (fmt) {
          GIMG_Raster * thumb_raster = NULL;
          r = gimg_raster_create_with_allocator(
              alloc, tw, th, fmt, GIMG_RASTER_OWNED, NULL, 0, &thumb_raster);
          if (r == GIMG_OK && thumb_raster) {
            void * pixels = gimg_raster_pixels(thumb_raster);
            size_t stride = gimg_raster_stride_bytes(thumb_raster);
            const unsigned char * src = (const unsigned char *)strip_data;
            if (tphoto <= 1) {
              size_t row_bytes =
                  (tbits <= 8) ? (size_t)tw : (size_t)tw * 2u;
              for (uint32_t y = 0; y < th; y++) {
                memcpy((unsigned char *)pixels + (size_t)y * stride,
                    src + (size_t)y * row_bytes, row_bytes);
              }
            }
            else {
              for (uint32_t y = 0; y < th; y++) {
                for (uint32_t x = 0; x < tw; x++) {
                  size_t src_off = (size_t)(y * tw + x) * 3u;
                  size_t dst_off = (size_t)y * stride + (size_t)x * 4u;
                  ((unsigned char *)pixels)[dst_off + 0] = src[src_off + 0];
                  ((unsigned char *)pixels)[dst_off + 1] = src[src_off + 1];
                  ((unsigned char *)pixels)[dst_off + 2] = src[src_off + 2];
                  ((unsigned char *)pixels)[dst_off + 3] = 255;
                }
              }
            }
            gimg_free(alloc, strip_data);
            strip_data = NULL;
            r = gimg_doc_set_item_count(doc, 2);
            if (r == GIMG_OK) {
              gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);
              thumb_added = 1;
            }
            else {
              gimg_raster_destroy(thumb_raster);
            }
          }
        }
        if (strip_data) {
          gimg_free(alloc, strip_data);
        }
      }
    }

    // 3) Compression=7: TIFF TechNote 2 JPEG (reassembled or single strip)
    if (!thumb_added) {
      void * tiff_jpeg_buf = NULL;
      size_t tiff_jpeg_len = 0;
      if (gimg_exif_embedded_thumbnail_tiff_jpeg(alloc, exif_tiff,
              exif_tiff_len, &tiff_jpeg_buf, &tiff_jpeg_len) == GIMG_OK &&
          tiff_jpeg_len > 0) {
        GIMG_Stream * thumb_stream = NULL;
        r = gimg_stream_create_memory_with_allocator(
            alloc, tiff_jpeg_buf, tiff_jpeg_len, &thumb_stream);
        if (r == GIMG_OK && thumb_stream) {
          GIMG_Doc * thumb_doc = NULL;
          r = gimg_doc_load(thumb_stream, options, diagnostics, &thumb_doc);
          gimg_stream_destroy(thumb_stream);
          gimg_free(alloc, tiff_jpeg_buf);
          tiff_jpeg_buf = NULL;
          if (r == GIMG_OK && thumb_doc &&
              gimg_doc_item_count(thumb_doc) >= 1) {
            GIMG_Raster * thumb_raster = NULL;
            r = gimg_item_decode(
                gimg_doc_item(thumb_doc, 0), NULL, &thumb_raster);
            if (r == GIMG_OK && thumb_raster) {
              GIMG_Raster * copy_raster = NULL;
              r = gimg_raster_copy_with_allocator(
                  alloc, thumb_raster, &copy_raster);
              gimg_raster_destroy(thumb_raster);
              if (r == GIMG_OK && copy_raster) {
                r = gimg_doc_set_item_count(doc, 2);
                if (r == GIMG_OK) {
                  gimg_item_set_raster(
                      gimg_doc_item(doc, 1), copy_raster);
                  thumb_added = 1;
                }
                else {
                  gimg_raster_destroy(copy_raster);
                }
              }
            }
            gimg_doc_destroy(thumb_doc);
          }
          else if (thumb_doc) {
            gimg_doc_destroy(thumb_doc);
          }
        }
        if (tiff_jpeg_buf) {
          gimg_free(alloc, tiff_jpeg_buf);
        }
      }
    }
  }

  // JFIF embedded thumbnail (APP0 bytes 12-15 = X,Y; offset 16 = pixels).
  // Only add second item if EXIF did not already provide a thumbnail.
  if (gimg_doc_item_count(doc) == 1 && state->app0_jfif &&
      state->app0_jfif_len >= 16) {
    uint16_t tx = (uint16_t)((state->app0_jfif[12] << 8) |
        (unsigned char)state->app0_jfif[13]);
    uint16_t ty = (uint16_t)((state->app0_jfif[14] << 8) |
        (unsigned char)state->app0_jfif[15]);
    if (tx > 0 && ty > 0) {
      size_t thumb_pixels = 0;
      if (gimg_safe_pixel_count((uint32_t)tx, (uint32_t)ty,
              &thumb_pixels) == GIMG_OK &&
          thumb_pixels <= GIMG_JPEG_MAX_THUMB_PIXELS) {
        size_t need_rgb = 0;
        size_t need_gray = 0;
        int use_rgb = -1; // 0 = grayscale, 1 = RGB
        if (gimg_safe_mul_size(thumb_pixels, 3u, &need_rgb) &&
            gimg_safe_add_size(16u, need_rgb, &need_rgb) &&
            state->app0_jfif_len >= need_rgb) {
          use_rgb = 1;
        }
        else if (gimg_safe_add_size(16u, thumb_pixels, &need_gray) &&
            state->app0_jfif_len >= need_gray) {
          use_rgb = 0;
        }
        if (use_rgb == 0 || use_rgb == 1) {
          const GIMG_Pixel_Format * fmt =
              use_rgb ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_GRAY8;
          GIMG_Raster * thumb_raster = NULL;
          r = gimg_raster_create_with_allocator(
              alloc, (uint32_t)tx, (uint32_t)ty, fmt, GIMG_RASTER_OWNED, NULL,
              0, &thumb_raster);
          if (r == GIMG_OK && thumb_raster) {
            void * pixels = gimg_raster_pixels(thumb_raster);
            size_t stride = gimg_raster_stride_bytes(thumb_raster);
            const unsigned char * src =
                state->app0_jfif + 16;
            if (use_rgb) {
              for (uint32_t y = 0; y < (uint32_t)ty; y++) {
                for (uint32_t x = 0; x < (uint32_t)tx; x++) {
                  size_t src_off = (size_t)(y * (uint32_t)tx + x) * 3u;
                  size_t dst_off = (size_t)y * stride + (size_t)x * 4u;
                  ((unsigned char *)pixels)[dst_off + 0] = src[src_off + 0];
                  ((unsigned char *)pixels)[dst_off + 1] = src[src_off + 1];
                  ((unsigned char *)pixels)[dst_off + 2] = src[src_off + 2];
                  ((unsigned char *)pixels)[dst_off + 3] = 255;
                }
              }
            }
            else {
              size_t row_bytes = (size_t)tx;
              for (uint32_t y = 0; y < (uint32_t)ty; y++) {
                memcpy((unsigned char *)pixels + (size_t)y * stride,
                    src + (size_t)y * row_bytes, row_bytes);
              }
            }
            r = gimg_doc_set_item_count(doc, 2);
            if (r == GIMG_OK) {
              gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);
            }
            else {
              gimg_raster_destroy(thumb_raster);
            }
          }
        }
      }
    }
  }

  // JFXX (JFIF 1.02 extension): 0x10 = JPEG thumbnail, 0x11 = 1 BPP, 0x13 = 3 BPP.
  // Only add second item if we do not already have one (EXIF or JFIF embedded).
  if (gimg_doc_item_count(doc) == 1 && state->app0_jfxx &&
      state->app0_jfxx_len >= 6) {
    uint8_t ext_code = state->app0_jfxx[5];
    const unsigned char * jfxx_data = state->app0_jfxx + 6;
    size_t jfxx_data_len = state->app0_jfxx_len - 6;

    if (ext_code == 0x10 && jfxx_data_len > 0) {
      // JPEG thumbnail: decode as full JPEG stream.
      GIMG_Stream * thumb_stream = NULL;
      r = gimg_stream_create_memory_with_allocator(
          alloc, jfxx_data, jfxx_data_len, &thumb_stream);
      if (r == GIMG_OK && thumb_stream) {
        GIMG_Doc * thumb_doc = NULL;
        r = gimg_doc_load(thumb_stream, options, diagnostics, &thumb_doc);
        gimg_stream_destroy(thumb_stream);
        if (r == GIMG_OK && thumb_doc && gimg_doc_item_count(thumb_doc) >= 1) {
          GIMG_Raster * thumb_raster = NULL;
          r = gimg_item_decode(
              gimg_doc_item(thumb_doc, 0), NULL, &thumb_raster);
          if (r == GIMG_OK && thumb_raster) {
            GIMG_Raster * copy_raster = NULL;
            r = gimg_raster_copy_with_allocator(
                alloc, thumb_raster, &copy_raster);
            gimg_raster_destroy(thumb_raster);
            if (r == GIMG_OK && copy_raster) {
              r = gimg_doc_set_item_count(doc, 2);
              if (r == GIMG_OK) {
                gimg_item_set_raster(
                    gimg_doc_item(doc, 1), copy_raster);
              }
              else {
                gimg_raster_destroy(copy_raster);
              }
            }
          }
          gimg_doc_destroy(thumb_doc);
        }
        else if (thumb_doc) {
          gimg_doc_destroy(thumb_doc);
        }
      }
    }
    else if ((ext_code == 0x11 || ext_code == 0x13) && state->app0_jfif &&
        state->app0_jfif_len >= 16 && jfxx_data_len > 0) {
      uint16_t tx = (uint16_t)((state->app0_jfif[12] << 8) |
          (unsigned char)state->app0_jfif[13]);
      uint16_t ty = (uint16_t)((state->app0_jfif[14] << 8) |
          (unsigned char)state->app0_jfif[15]);
      if (tx > 0 && ty > 0) {
        size_t thumb_pixels = 0;
        if (gimg_safe_pixel_count((uint32_t)tx, (uint32_t)ty,
                &thumb_pixels) == GIMG_OK &&
            thumb_pixels <= GIMG_JPEG_MAX_THUMB_PIXELS) {
          if (ext_code == 0x11) {
            // 1 BPP: 256*3 palette then tx*ty indices.
            size_t palette_size = 768u;
            size_t indices_size = 0;
            if (gimg_safe_add_size(palette_size, thumb_pixels,
                    &indices_size) &&
                jfxx_data_len >= indices_size) {
              GIMG_Raster * thumb_raster = NULL;
              r = gimg_raster_create_with_allocator(
                  alloc, (uint32_t)tx, (uint32_t)ty, &GIMG_PIXEL_RGBA8,
                  GIMG_RASTER_OWNED, NULL, 0, &thumb_raster);
              if (r == GIMG_OK && thumb_raster) {
                void * pixels = gimg_raster_pixels(thumb_raster);
                size_t stride = gimg_raster_stride_bytes(thumb_raster);
                const unsigned char * pal = jfxx_data;
                const unsigned char * idx = jfxx_data + 768;
                for (uint32_t y = 0; y < (uint32_t)ty; y++) {
                  for (uint32_t x = 0; x < (uint32_t)tx; x++) {
                    unsigned char i = idx[(size_t)y * (uint32_t)tx + x];
                    size_t dst_off =
                        (size_t)y * stride + (size_t)x * 4u;
                    ((unsigned char *)pixels)[dst_off + 0] = pal[(size_t)i * 3];
                    ((unsigned char *)pixels)[dst_off + 1] =
                        pal[(size_t)i * 3 + 1];
                    ((unsigned char *)pixels)[dst_off + 2] =
                        pal[(size_t)i * 3 + 2];
                    ((unsigned char *)pixels)[dst_off + 3] = 255;
                  }
                }
                r = gimg_doc_set_item_count(doc, 2);
                if (r == GIMG_OK) {
                  gimg_item_set_raster(
                      gimg_doc_item(doc, 1), thumb_raster);
                }
                else {
                  gimg_raster_destroy(thumb_raster);
                }
              }
            }
          }
          else {
            // 0x13: 3 BPP RGB.
            size_t need = 0;
            if (gimg_safe_mul_size(thumb_pixels, 3u, &need) &&
                jfxx_data_len >= need) {
              GIMG_Raster * thumb_raster = NULL;
              r = gimg_raster_create_with_allocator(
                  alloc, (uint32_t)tx, (uint32_t)ty, &GIMG_PIXEL_RGBA8,
                  GIMG_RASTER_OWNED, NULL, 0, &thumb_raster);
              if (r == GIMG_OK && thumb_raster) {
                void * pixels = gimg_raster_pixels(thumb_raster);
                size_t stride = gimg_raster_stride_bytes(thumb_raster);
                const unsigned char * src = jfxx_data;
                for (uint32_t y = 0; y < (uint32_t)ty; y++) {
                  for (uint32_t x = 0; x < (uint32_t)tx; x++) {
                    size_t src_off =
                        (size_t)(y * (uint32_t)tx + x) * 3u;
                    size_t dst_off =
                        (size_t)y * stride + (size_t)x * 4u;
                    ((unsigned char *)pixels)[dst_off + 0] = src[src_off + 0];
                    ((unsigned char *)pixels)[dst_off + 1] = src[src_off + 1];
                    ((unsigned char *)pixels)[dst_off + 2] = src[src_off + 2];
                    ((unsigned char *)pixels)[dst_off + 3] = 255;
                  }
                }
                r = gimg_doc_set_item_count(doc, 2);
                if (r == GIMG_OK) {
                  gimg_item_set_raster(
                      gimg_doc_item(doc, 1), thumb_raster);
                }
                else {
                  gimg_raster_destroy(thumb_raster);
                }
              }
            }
          }
        }
      }
    }
  }

  *out_doc = doc;
  return GIMG_OK;
}
