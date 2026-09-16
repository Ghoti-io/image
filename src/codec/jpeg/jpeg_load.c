/**
 * @file
 *
 * JPEG load: verify SOI, parse segments (SOF0/SOF1/SOF2, DQT, DHT, SOS),
 * enforce limits, build doc with one item and codec-private state.
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

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../container/doc_internal.h"
#include "../../core/alloc_internal.h"
#include "../../core/safe_math_internal.h"
#include "../../meta/exif_internal.h"
#include "../codec_internal.h"
#include "jpeg_debug_internal.h"
#include "jpeg_internal.h"

/** If GIMG_JPEG_DEBUG_LOAD is set, log why we're returning FORMAT. */
static void jpeg_load_fmt_debug(const char * action, size_t offset,
    uint8_t marker) {
#if GIMG_JPEG_DEBUG_LOAD
  (void)fprintf(stderr, "JPEG_LOAD_FORMAT: %s (offset=%zu marker=0x%02x)\n",
      action ? action : "?", offset, (unsigned)marker);
#else
  (void)action;
  (void)offset;
  (void)marker;
#endif
}

/** Append diagnostic on load error (codec "jpeg", offset, marker). */
static void jpeg_load_diag(GIMG_Diagnostics * d, size_t offset, uint8_t marker,
    GIMG_Result r, const char * action) {
  jpeg_load_fmt_debug(action, offset, marker);
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

/** Extract IPTC Caption/Abstract (record 2, dataset 80) from APP13
 * Photoshop 3.0 payload. APP13 starts with "Photoshop 3.0\0"; then 8BIM blocks:
 * 4B "8BIM", 2B id BE, 1B name_len, name_len bytes (pad to even), 4B data size
 * BE, data. IPTC is resource id 0x0404. IPTC tag: 0x1C, record, dataset, 2B
 * length BE. On success set *out_ptr and *out_len to caption bytes; return
 * true. */
static bool jpeg_app13_iptc_caption(const unsigned char * app13,
    size_t app13_len, const unsigned char ** out_ptr, size_t * out_len) {
  if (!app13 || app13_len < 14 || !out_ptr || !out_len) {
    return false;
  }
  if (memcmp(app13, "Photoshop 3.0\0", 14) != 0) {
    return false;
  }
  size_t off = 14;
  while (off + 10 <= app13_len && memcmp(app13 + off, "8BIM", 4) == 0) {
    uint16_t id = (uint16_t)((app13[off + 4] << 8) | app13[off + 5]);
    size_t name_len = (size_t)app13[off + 6];
    size_t name_total = 1 + name_len + (name_len & 1u ? 1u : 0u);
    if (off + 6 + name_total + 4 > app13_len) {
      break;
    }
    uint32_t data_size = (uint32_t)((app13[off + 6 + name_total] << 24) |
        (app13[off + 6 + name_total + 1] << 16) |
        (app13[off + 6 + name_total + 2] << 8) |
        app13[off + 6 + name_total + 3]);
    size_t data_off = off + 6 + name_total + 4;
    if (data_off + data_size > app13_len) {
      break;
    }
    if (id == 0x0404 && data_size > 0) {
      const unsigned char * iptc = app13 + data_off;
      size_t iptc_len = data_size;
      size_t i = 0;
      while (i + 5 <= iptc_len) {
        uint16_t tag_len = (uint16_t)((iptc[i + 3] << 8) | iptc[i + 4]);
        if (iptc[i] == 0x1C && iptc[i + 1] == 0x02 && iptc[i + 2] == 0x50 &&
            i + 5 + tag_len <= iptc_len && tag_len > 0) {
          *out_ptr = iptc + i + 5;
          *out_len = (size_t)tag_len;
          return true;
        }
        i += 5 + (size_t)tag_len;
      }
      break;
    }
    off = data_off + data_size;
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

/** Free all codec-private state. Cleanup order (add new fields here to avoid
 * leaks): Huffman tables (dc/ac/refine), DHT entry payloads, scan data and
 * per-scan Huffman, APP/COM buffers, ICC chunks, then state itself. */
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
    gimg_free(alloc, state->huff_ac_refine[i]);
  }
  for (size_t i = 0; i < state->num_dht_entries; i++) {
    gimg_free(alloc, state->dht_entries[i].payload);
  }
  for (unsigned i = 0; i < state->num_scans; i++) {
    gimg_jpeg_scan_t * sc = &state->scans[i];
    gimg_free(alloc, sc->data);
    for (int j = 0; j < 4; j++) {
      gimg_free(alloc, sc->huff_dc[j]);
      gimg_free(alloc, sc->huff_ac[j]);
      gimg_free(alloc, sc->huff_ac_refine[j]);
    }
  }
  gimg_free(alloc, state->app0_jfif);
  gimg_free(alloc, state->app0_jfxx);
  gimg_free(alloc, state->app1_exif);
  gimg_free(alloc, state->app1_xmp);
  gimg_free(alloc, state->app2_icc);
  for (unsigned i = 0; i < GIMG_JPEG_MAX_ICC_CHUNKS; i++) {
    gimg_free(alloc, state->app2_icc_chunk_payload[i]);
  }
  gimg_free(alloc, state->app13);
  gimg_free(alloc, state->app14);
  gimg_free(alloc, state->com_combined);
  gimg_free(alloc, state->unknown_app_combined);
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
    jpeg_load_fmt_debug("verify_soi failed", 0, 0);
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
    {
      size_t pos = gimg_stream_tell(stream);
      if (pos != (size_t)-1) {
        seg_start = pos;
      }
    }
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
    // All payload_buf accesses below are bounded by payload_size (and
    // segment-specific minimums, e.g. SOF/SOS/DHT/DQT length checks).

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
    case GIMG_JPEG_MARKER_SOF0:
    case GIMG_JPEG_MARKER_SOF1: {
      if (seen_sof) {
        jpeg_load_fmt_debug("duplicate SOF0/SOF1", seg_start, marker);
        gimg_free(alloc, payload_buf);
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      r = jpeg_parse_sof(payload_buf, payload_size, marker, &state->sof);
      gimg_free(alloc, payload_buf);
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r,
            marker == GIMG_JPEG_MARKER_SOF0 ? "invalid SOF0" : "invalid SOF1");
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
      state->is_progressive = 0;
      seen_sof = true;
      break;
    }
    case GIMG_JPEG_MARKER_SOF2: {
      if (seen_sof) {
        jpeg_load_fmt_debug("duplicate SOF2", seg_start, marker);
        gimg_free(alloc, payload_buf);
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      r = jpeg_parse_sof(payload_buf, payload_size, marker, &state->sof);
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
    // Unsupported SOF (T.81: SOF3 lossless, SOF5–SOF7 differential, SOF9–SOF15).
    // Reject explicitly so the caller gets a clear error instead of "no SOF".
    case GIMG_JPEG_MARKER_SOF3:
    case 0xC5:
    case 0xC6:
    case 0xC7:
    case 0xC9:
    case 0xCA:
    case 0xCB:
    case 0xCD:
    case 0xCE:
    case 0xCF: {
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_UNSUPPORTED,
          "unsupported SOF marker");
      gimg_jpeg_free_doc_state(codec, state);
      return GIMG_ERR_UNSUPPORTED;
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
    case GIMG_JPEG_MARKER_DNL: {
      // DNL (Define Number of Lines): valid only after the first scan. Payload
      // is 2 bytes (number of lines, big-endian). Validates or sets height.
      if (state->num_scans < 1) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "DNL before first scan");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      if (payload_size != 2 || !payload_buf) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "DNL payload must be 2 bytes");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      uint16_t dnl_lines = (uint16_t)((payload_buf[0] << 8) | payload_buf[1]);
      gimg_free(alloc, payload_buf);
      if (dnl_lines == 0 || dnl_lines > GIMG_JPEG_MAX_DIMENSION) {
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "DNL number of lines out of range");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      if (state->sof.height != 0 && state->sof.height == dnl_lines) {
        // DNL matches SOF height; accept.
        break;
      }
      if (state->sof.height != 0) {
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "DNL number of lines does not match SOF height");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      state->sof.height = dnl_lines;
      break;
    }
    case GIMG_JPEG_MARKER_DHT: {
      jpeg_apply_dht_payload(state, payload_buf, payload_size, alloc);
      jpeg_record_dht_payload(state, payload_buf, payload_size, alloc);
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
        // T.81 B.2.4: a DHT segment defines the Huffman table for its (Tc, Th) and
        // replaces any previous definition of that pair.  The tables a scan uses are
        // therefore simply the ones most recently defined when its SOS is read - so
        // snapshot the current state and nothing more.
        //
        // What stood here instead searched for "the first DHT after the previous
        // scan's data", with four layers of fallback, and carried the note: '"first"
        // matches this fixture; "last" to match libjpeg caused scan 5 to diverge -
        // root cause TBD'.  The root cause was one layer down: the parser sorted AC
        // tables into "initial" and "refinement" by symbol count, so a refinement
        // table that happened not to have 17 or 18 symbols landed in the wrong slot
        // and the search had to be bent to compensate.  T.81 has one kind of AC
        // table; see jpeg_parse.c.
        for (int ti = 0; ti < 4; ti++) {
          if (state->huff_dc[ti] && state->huff_dc_len[ti] > 0) {
            scan->huff_dc[ti] =
                (unsigned char *)gimg_malloc(alloc, state->huff_dc_len[ti]);
            if (scan->huff_dc[ti]) {
              memcpy(scan->huff_dc[ti], state->huff_dc[ti], state->huff_dc_len[ti]);
              scan->huff_dc_len[ti] = state->huff_dc_len[ti];
            }
          }
          if (state->huff_ac[ti] && state->huff_ac_len[ti] > 0) {
            scan->huff_ac[ti] =
                (unsigned char *)gimg_malloc(alloc, state->huff_ac_len[ti]);
            if (scan->huff_ac[ti]) {
              memcpy(scan->huff_ac[ti], state->huff_ac[ti], state->huff_ac_len[ti]);
              scan->huff_ac_len[ti] = state->huff_ac_len[ti];
            }
          }
        }
        state->num_scans++;
        state->inter_scan_dht_index_set = 0;  // Next scan data end will set index.
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
          // T.81 B.2.2: 0x00 after 0xFF is stuffing; include both in scan data
          // so the entropy decoder can skip the 0x00 when it advances past 0xFF.
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
          // RST0..RST7: part of scan data (2-byte marker); append both bytes
          // so entropy decoder can skip them; continue same scan.
          (void)gimg_stream_read(stream, &b, 1, &n);
          r = jpeg_append_scan_data(state, (const unsigned char *)"\xFF", 1);
          if (r != GIMG_OK) {
            gimg_jpeg_free_doc_state(codec, state);
            return r;
          }
          r = jpeg_append_scan_data(state, &b, 1);
          if (r != GIMG_OK) {
            gimg_jpeg_free_doc_state(codec, state);
            return r;
          }
          continue;
        }
        // Consume the marker byte.
        (void)gimg_stream_read(stream, &b, 1, &n);
        // End of this scan's entropy data only at next SOS or EOI (T.81 B.2.4:
        // marker segments such as DHT may appear between scans; 0xFF before
        // them is the start of the marker, not entropy — do not append it).
        if (b == GIMG_JPEG_MARKER_SOS || b == GIMG_JPEG_MARKER_EOI) {
          // T.81 B.2.4: 0xFF that starts the next marker is not scan entropy.
          // Do not overwrite last_scan_data_end_dht_index here; it was set when
          // we first exited this scan's data (before any inter-scan DHT).
          pending_marker = b;
          have_pending_marker = true;
          break;
        }
        if (jpeg_marker_has_no_length(b)) {
          // SOI or other no-length marker; hand off to main loop.
          pending_marker = b;
          have_pending_marker = true;
          break;
        }
        // Per T.81 B.2.2 / B.2.4 the 0xFF we just read starts the next marker
        // (e.g. DHT); it is not entropy. Do not append it to the current scan —
        // that would add 8 bits and decode an extra coefficient from the next
        // scan, producing wrong non-zeros and breaking later AC refinement.
        // Read and process the segment (e.g. apply DHT) so following scans use
        // updated tables. Set last_scan_data_end_dht_index only the first time
        // we exit this scan's data, so "first DHT after previous scan" is correct.
        if (!state->inter_scan_dht_index_set) {
          state->last_scan_data_end_dht_index = state->num_dht_entries;
          state->inter_scan_dht_index_set = 1;
        }
        {
          unsigned char len_buf[2];
          r = gimg_stream_read_exact(stream, len_buf, 2);
          if (r != GIMG_OK) {
            gimg_jpeg_free_doc_state(codec, state);
            return r;
          }
          uint16_t seg_len = (uint16_t)((len_buf[0] << 8) | len_buf[1]);
          if (seg_len < 2) {
            pending_marker = b;
            have_pending_marker = true;
            break;
          }
          size_t payload_size = (size_t)(seg_len - 2);
          if (b == GIMG_JPEG_MARKER_DHT && payload_size > 0) {
            unsigned char * dht_buf =
                (unsigned char *)gimg_malloc(alloc, payload_size);
            if (!dht_buf) {
              gimg_jpeg_free_doc_state(codec, state);
              return GIMG_ERR_OOM;
            }
            r = gimg_stream_read_exact(stream, dht_buf, payload_size);
            if (r != GIMG_OK) {
              gimg_free(alloc, dht_buf);
              gimg_jpeg_free_doc_state(codec, state);
              return r;
            }
            jpeg_apply_dht_payload(state, dht_buf, payload_size, alloc);
            jpeg_record_dht_payload(state, dht_buf, payload_size, alloc);
            {
              const unsigned char * dp = dht_buf;
              size_t dremain = payload_size;
              while (dremain >= GIMG_JPEG_DHT_HEADER_LEN) {
                uint8_t tc_th = dp[0];
                uint8_t th = tc_th & 0x0Fu;
                uint8_t tc = (tc_th >> 4) & 1;
                size_t num_syms = 0;
                for (int i = 1; i <= (int)GIMG_JPEG_DHT_BIT_COUNTS; i++) {
                  num_syms += dp[i];
                }
                if (th >= 4 || dremain < GIMG_JPEG_DHT_HEADER_LEN + num_syms) {
                  break;
                }
                if (tc && num_syms != 17) {
                  state->ac_from_inter_scan_dht[th] = 1;
                }
                dp += GIMG_JPEG_DHT_HEADER_LEN + num_syms;
                dremain -= GIMG_JPEG_DHT_HEADER_LEN + num_syms;
              }
            }
            gimg_free(alloc, dht_buf);
            // T.81 B.2.4: DHT defines conditioning for the *following*
            // entropy-coded segments. This DHT appeared after the current
            // scan's data, so it applies to the next scan. Do not copy state
            // into the current scan; that would wrongly use this table for
            // the scan we just finished (e.g. first AC-initial would get the
            // next scan's table and desync).
          }
          else {
            for (size_t k = 0; k < payload_size; k++) {
              unsigned char discard;
              size_t nr = 0;
              r = gimg_stream_read(stream, &discard, 1, &nr);
              if (r != GIMG_OK || nr == 0) {
                gimg_jpeg_free_doc_state(codec, state);
                return (r != GIMG_OK) ? r : GIMG_ERR_FORMAT;
              }
            }
          }
        }
        // Break so the main loop reads the next marker (e.g. SOS). Do not
        // continue reading bytes into this scan — the next byte is the
        // start of the next scan's data.
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
        r = jpeg_append_unknown_app(state, GIMG_JPEG_MARKER_APP0, payload_buf,
            payload_size, alloc, diagnostics, seg_start);
        gimg_free(alloc, payload_buf);
        payload_buf = NULL;
        if (r != GIMG_OK) {
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
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
        r = jpeg_append_unknown_app(state, GIMG_JPEG_MARKER_APP1, payload_buf,
            payload_size, alloc, diagnostics, seg_start);
        gimg_free(alloc, payload_buf);
        payload_buf = NULL;
        if (r != GIMG_OK) {
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
      }
      break;
    }
    case GIMG_JPEG_MARKER_APP2: {
      if (payload_size >= 14 && payload_buf &&
          memcmp(payload_buf, "ICC_PROFILE\0", 12) == 0) {
        unsigned chunk_index = (unsigned)payload_buf[12];
        unsigned total_chunks = (unsigned)payload_buf[13];
        if (total_chunks == 0 || total_chunks > GIMG_JPEG_MAX_ICC_CHUNKS ||
            chunk_index < 1 || chunk_index > total_chunks) {
          jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
              "APP2 ICC_PROFILE invalid chunk index/total");
          gimg_free(alloc, payload_buf);
          gimg_jpeg_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        if (total_chunks == 1) {
          // Single-segment: keep full payload for round-trip; decode uses +14.
          if (state->app2_icc) {
            gimg_free(alloc, state->app2_icc);
          }
          state->app2_icc = payload_buf;
          state->app2_icc_len = payload_size;
          state->app2_icc_num_chunks = 0;
          payload_buf = NULL;
        }
        else {
          // Multi-segment: accumulate chunks by index.
          if (state->app2_icc_total_chunks == 0) {
            state->app2_icc_total_chunks = total_chunks;
          }
          else if (state->app2_icc_total_chunks != total_chunks) {
            jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
                "APP2 ICC_PROFILE total chunks mismatch");
            gimg_free(alloc, payload_buf);
            gimg_jpeg_free_doc_state(codec, state);
            return GIMG_ERR_FORMAT;
          }
          if (state->app2_icc_chunk_payload[chunk_index - 1] != NULL) {
            jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
                "APP2 ICC_PROFILE duplicate chunk index");
            gimg_free(alloc, payload_buf);
            gimg_jpeg_free_doc_state(codec, state);
            return GIMG_ERR_FORMAT;
          }
          state->app2_icc_chunk_payload[chunk_index - 1] = payload_buf;
          state->app2_icc_chunk_len[chunk_index - 1] = payload_size;
          payload_buf = NULL;
          state->app2_icc_chunks_received++;
          if (state->app2_icc_chunks_received == total_chunks) {
            // Assemble: total profile size = sum of (chunk_len - 14).
            size_t total_profile = 0;
            for (unsigned i = 0; i < total_chunks; i++) {
              size_t data_len = state->app2_icc_chunk_len[i] -
                  (state->app2_icc_chunk_len[i] >= 14u ? 14u : 0u);
              if (total_profile + data_len < total_profile ||
                  total_profile + data_len > GIMG_JPEG_MAX_ICC_PROFILE_SIZE) {
                jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_LIMIT,
                    "APP2 ICC_PROFILE assembled size over limit");
                gimg_jpeg_free_doc_state(codec, state);
                return GIMG_ERR_LIMIT;
              }
              total_profile += data_len;
            }
            unsigned char * assembled =
                (unsigned char *)gimg_malloc(alloc, total_profile);
            if (!assembled && total_profile > 0) {
              jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_OOM,
                  "APP2 ICC_PROFILE assemble OOM");
              gimg_jpeg_free_doc_state(codec, state);
              return GIMG_ERR_OOM;
            }
            size_t off = 0;
            for (unsigned i = 0; i < total_chunks; i++) {
              size_t data_len = state->app2_icc_chunk_len[i] >= 14u
                  ? state->app2_icc_chunk_len[i] - 14u
                  : 0u;
              if (data_len > 0) {
                memcpy(assembled + off, state->app2_icc_chunk_payload[i] + 14,
                    data_len);
                off += data_len;
              }
            }
            state->app2_icc = assembled;
            state->app2_icc_len = total_profile;
            state->app2_icc_num_chunks = total_chunks;
          }
        }
      }
      if (payload_buf) {
        r = jpeg_append_unknown_app(state, GIMG_JPEG_MARKER_APP2, payload_buf,
            payload_size, alloc, diagnostics, seg_start);
        gimg_free(alloc, payload_buf);
        payload_buf = NULL;
        if (r != GIMG_OK) {
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
      }
      break;
    }
    case GIMG_JPEG_MARKER_APP13: {
      // APP13 (0xED): Photoshop 3.0 / IPTC; store raw when "Photoshop 3.0\0".
      if (payload_size >= 14 && payload_buf &&
          memcmp(payload_buf, "Photoshop 3.0\0", 14) == 0) {
        if (state->app13) {
          gimg_free(alloc, state->app13);
        }
        state->app13 = payload_buf;
        state->app13_len = payload_size;
        payload_buf = NULL;
      }
      if (payload_buf) {
        r = jpeg_append_unknown_app(state, GIMG_JPEG_MARKER_APP13, payload_buf,
            payload_size, alloc, diagnostics, seg_start);
        gimg_free(alloc, payload_buf);
        payload_buf = NULL;
        if (r != GIMG_OK) {
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
      }
      break;
    }
    case GIMG_JPEG_MARKER_APP14: {
      // APP14 (0xEE): Adobe; transform at byte 11 (0=unknown, 1=YCbCr, 2=YCCK).
      if (payload_size >= 12 && payload_buf &&
          memcmp(payload_buf, "Adobe\0", 6) == 0) {
        if (state->app14) {
          gimg_free(alloc, state->app14);
        }
        state->app14 = payload_buf;
        state->app14_len = payload_size;
        state->adobe_transform = payload_buf[11];
        payload_buf = NULL;
      }
      if (payload_buf) {
        r = jpeg_append_unknown_app(state, GIMG_JPEG_MARKER_APP14, payload_buf,
            payload_size, alloc, diagnostics, seg_start);
        gimg_free(alloc, payload_buf);
        payload_buf = NULL;
        if (r != GIMG_OK) {
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
      }
      break;
    }
    case 0xE3:
    case 0xE4:
    case 0xE5:
    case 0xE6:
    case 0xE7:
    case 0xE8:
    case 0xE9:
    case 0xEA:
    case 0xEB:
    case 0xEC:
    case 0xEF: {
      // APP3..APP15 (excluding APP13/APP14 when recognized): store for
      // round-trip.
      r = jpeg_append_unknown_app(state, marker, payload_buf, payload_size,
          alloc, diagnostics, seg_start);
      gimg_free(alloc, payload_buf);
      payload_buf = NULL;
      if (r != GIMG_OK) {
        gimg_jpeg_free_doc_state(codec, state);
        return r;
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
    jpeg_load_diag(diagnostics, 0u, 0u, GIMG_ERR_FORMAT, "no SOF found");
    gimg_jpeg_free_doc_state(codec, state);
    return GIMG_ERR_FORMAT;
  }
  if (state->sof.height == 0) {
    jpeg_load_diag(diagnostics, 0u, 0u, GIMG_ERR_FORMAT,
        "image height not specified (SOF height 0 requires DNL after first "
        "scan)");
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
      (state->app13 && state->app13_len > 0) ||
      (state->app14 && state->app14_len > 0) ||
      (state->com_combined && state->com_combined_size > 0) ||
      (state->unknown_app_combined && state->unknown_app_combined_size > 0)) {
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
      if (state->app2_icc_num_chunks > 0) {
        size_t chunks_blob_size = 2;
        for (unsigned i = 0; i < state->app2_icc_num_chunks; i++) {
          chunks_blob_size += 2 + state->app2_icc_chunk_len[i];
        }
        unsigned char * chunks_blob =
            (unsigned char *)gimg_malloc(alloc, chunks_blob_size);
        if (chunks_blob) {
          chunks_blob[0] = (unsigned char)(state->app2_icc_num_chunks >> 8);
          chunks_blob[1] = (unsigned char)(state->app2_icc_num_chunks & 0xFFu);
          size_t off = 2;
          for (unsigned i = 0; i < state->app2_icc_num_chunks; i++) {
            size_t plen = state->app2_icc_chunk_len[i];
            chunks_blob[off] = (unsigned char)(plen >> 8);
            chunks_blob[off + 1] = (unsigned char)(plen & 0xFFu);
            off += 2;
            if (plen > 0 && state->app2_icc_chunk_payload[i]) {
              memcpy(chunks_blob + off, state->app2_icc_chunk_payload[i], plen);
              off += plen;
            }
          }
          r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP2_ICC_CHUNKS,
              chunks_blob, chunks_blob_size);
          gimg_free(alloc, chunks_blob);
          if (r != GIMG_OK) {
            gimg_doc_destroy(doc);
            return r;
          }
        }
      }
    }
    if (state->app13 && state->app13_len > 0) {
      r = gimg_meta_raw_attach(
          raw, "jpeg", GIMG_JPEG_RAW_APP13, state->app13, state->app13_len);
      if (r != GIMG_OK) {
        gimg_doc_destroy(doc);
        return r;
      }
    }
    if (state->app14 && state->app14_len > 0) {
      r = gimg_meta_raw_attach(
          raw, "jpeg", GIMG_JPEG_RAW_APP14, state->app14, state->app14_len);
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
    if (state->unknown_app_combined && state->unknown_app_combined_size > 0) {
      r = gimg_meta_raw_attach(raw, "jpeg", GIMG_JPEG_RAW_APP_UNKNOWN,
          state->unknown_app_combined, state->unknown_app_combined_size);
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
  // Populate meta_common description from APP13 IPTC Caption (2:80) when not
  // set.
  if (state->app13 && state->app13_len >= 14) {
    if (!meta_common &&
        gimg_doc_ensure_meta_common(doc, &meta_common) != GIMG_OK) {
      meta_common = NULL;
    }
    if (meta_common && !gimg_meta_common_description(meta_common)) {
      const unsigned char * cap_ptr = NULL;
      size_t cap_len = 0;
      if (jpeg_app13_iptc_caption(
              state->app13, state->app13_len, &cap_ptr, &cap_len) &&
          cap_len > 0) {
        char * buf = (char *)gimg_malloc(alloc, cap_len + 1u);
        if (buf) {
          memcpy(buf, cap_ptr, cap_len);
          buf[cap_len] = '\0';
          (void)gimg_meta_common_set_description(meta_common, buf);
          gimg_free(alloc, buf);
        }
      }
    }
  }

  // EXIF embedded thumbnail (IFD1): try Compression=6 (JPEG), then 1
  // (uncompressed), then 7 (TIFF JPEG).
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
              size_t row_bytes = (tbits <= 8) ? (size_t)tw : (size_t)tw * 2u;
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
      if (gimg_safe_pixel_count((uint32_t)tx, (uint32_t)ty, &thumb_pixels) ==
              GIMG_OK &&
          thumb_pixels <= GIMG_JPEG_MAX_THUMB_PIXELS) {
        size_t need_rgb = 0;
        size_t need_gray = 0;
        int use_rgb = -1; // 0 = grayscale, 1 = RGB
        if (gcu_safe_mul_size(thumb_pixels, 3u, &need_rgb) &&
            gcu_safe_add_size(16u, need_rgb, &need_rgb) &&
            state->app0_jfif_len >= need_rgb) {
          use_rgb = 1;
        }
        else if (gcu_safe_add_size(16u, thumb_pixels, &need_gray) &&
            state->app0_jfif_len >= need_gray) {
          use_rgb = 0;
        }
        if (use_rgb == 0 || use_rgb == 1) {
          const GIMG_Pixel_Format * fmt =
              use_rgb ? &GIMG_PIXEL_RGBA8 : &GIMG_PIXEL_GRAY8;
          GIMG_Raster * thumb_raster = NULL;
          r = gimg_raster_create_with_allocator(alloc, (uint32_t)tx,
              (uint32_t)ty, fmt, GIMG_RASTER_OWNED, NULL, 0, &thumb_raster);
          if (r == GIMG_OK && thumb_raster) {
            void * pixels = gimg_raster_pixels(thumb_raster);
            size_t stride = gimg_raster_stride_bytes(thumb_raster);
            const unsigned char * src = state->app0_jfif + 16;
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

  // JFXX (JFIF 1.02 extension): 0x10 = JPEG thumbnail, 0x11 = 1 BPP, 0x13 = 3
  // BPP. Only add second item if we do not already have one (EXIF or JFIF
  // embedded).
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
                gimg_item_set_raster(gimg_doc_item(doc, 1), copy_raster);
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
        if (gimg_safe_pixel_count((uint32_t)tx, (uint32_t)ty, &thumb_pixels) ==
                GIMG_OK &&
            thumb_pixels <= GIMG_JPEG_MAX_THUMB_PIXELS) {
          if (ext_code == 0x11) {
            // 1 BPP: 256*3 palette then tx*ty indices.
            size_t palette_size = 768u;
            size_t indices_size = 0;
            if (gcu_safe_add_size(palette_size, thumb_pixels, &indices_size) &&
                jfxx_data_len >= indices_size) {
              GIMG_Raster * thumb_raster = NULL;
              r = gimg_raster_create_with_allocator(alloc, (uint32_t)tx,
                  (uint32_t)ty, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
                  &thumb_raster);
              if (r == GIMG_OK && thumb_raster) {
                void * pixels = gimg_raster_pixels(thumb_raster);
                size_t stride = gimg_raster_stride_bytes(thumb_raster);
                const unsigned char * pal = jfxx_data;
                const unsigned char * idx = jfxx_data + 768;
                for (uint32_t y = 0; y < (uint32_t)ty; y++) {
                  for (uint32_t x = 0; x < (uint32_t)tx; x++) {
                    unsigned char i = idx[(size_t)y * (uint32_t)tx + x];
                    size_t dst_off = (size_t)y * stride + (size_t)x * 4u;
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
                  gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);
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
            if (gcu_safe_mul_size(thumb_pixels, 3u, &need) &&
                jfxx_data_len >= need) {
              GIMG_Raster * thumb_raster = NULL;
              r = gimg_raster_create_with_allocator(alloc, (uint32_t)tx,
                  (uint32_t)ty, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, NULL, 0,
                  &thumb_raster);
              if (r == GIMG_OK && thumb_raster) {
                void * pixels = gimg_raster_pixels(thumb_raster);
                size_t stride = gimg_raster_stride_bytes(thumb_raster);
                const unsigned char * src = jfxx_data;
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
    }
  }

  *out_doc = doc;
  return GIMG_OK;
}
