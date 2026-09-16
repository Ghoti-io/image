/**
 * @file
 *
 * Parse JPEG segment payloads into doc-state structures: SOF0/SOF1/SOF2,
 * DQT, DHT, SOS header, APP/COM. No stream I/O; takes payload pointer and
 * length, fills state. Used by jpeg_load.c in its segment loop.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/image/macros.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../../core/alloc_internal.h"
#include "jpeg_internal.h"

GIMG_Result jpeg_parse_sof(const unsigned char * payload, size_t len,
    uint8_t sof_marker, gimg_jpeg_sof_t * sof) {
  if (len < 8) {
    return GIMG_ERR_FORMAT;
  }
  uint8_t precision = payload[0];
  uint16_t height = (uint16_t)((payload[1] << 8) | payload[2]);
  uint16_t width = (uint16_t)((payload[3] << 8) | payload[4]);
  uint8_t num_components = payload[5];
  if (num_components == 0 || num_components > GIMG_JPEG_MAX_COMPONENTS) {
    return GIMG_ERR_FORMAT;
  }
  if (sof_marker == GIMG_JPEG_MARKER_SOF0 && precision != 8) {
    return GIMG_ERR_FORMAT; // Baseline is 8-bit only per spec.
  }
  if (sof_marker == GIMG_JPEG_MARKER_SOF1 &&
      (precision != 8 && precision != 12)) {
    return GIMG_ERR_UNSUPPORTED; // Extended sequential: 8 or 12-bit.
  }
  if (sof_marker == GIMG_JPEG_MARKER_SOF2 &&
      (precision != 8 && precision != 12)) {
    return GIMG_ERR_UNSUPPORTED; // Progressive: 8 or 12-bit.
  }
  // SOF9 and SOF10 are the arithmetic-coded counterparts of SOF1 and SOF2
  // (T.81 Table B.1).  The frame header is identical; only the entropy coder
  // differs, so the same precision rule applies.
  if ((sof_marker == GIMG_JPEG_MARKER_SOF9 ||
          sof_marker == GIMG_JPEG_MARKER_SOF10) &&
      (precision != 8 && precision != 12)) {
    return GIMG_ERR_UNSUPPORTED;
  }
  // T.81 Table B.2: a lossless frame may use any precision from 2 to 16, and a
  // DCT-based one exactly 8 or 12.  This is the only place 16-bit samples are
  // legal in a JPEG.
  if (sof_marker == GIMG_JPEG_MARKER_SOF3) {
    if (precision < 2 || precision > 16) {
      return GIMG_ERR_UNSUPPORTED;
    }
  }
  else if (precision != 8 && precision != 12) {
    return GIMG_ERR_UNSUPPORTED;
  }
  if (width == 0) {
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
    // T.81 Table B.2: Hi and Vi are 1..4, and Tqi selects one of four tables.
    if (sof->h_samp[i] < 1u || sof->h_samp[i] > 4u || sof->v_samp[i] < 1u ||
        sof->v_samp[i] > 4u) {
      return GIMG_ERR_FORMAT;
    }
    if (sof->quant_tbl_id[i] >= GIMG_JPEG_MAX_QUANT_TABLES) {
      return GIMG_ERR_FORMAT;
    }
    // T.81 B.2.2: component identifiers are distinct, since a scan header
    // selects components by identifier.  Duplicates make that ambiguous.
    for (uint8_t j = 0; j < i; j++) {
      if (sof->comp_id[j] == sof->comp_id[i]) {
        return GIMG_ERR_FORMAT;
      }
    }
  }
  // T.81 A.1.1: the MCU holds Hi x Vi blocks of each component and may contain
  // at most ten blocks, so a frame whose sampling factors exceed that cannot be
  // interleaved and is not a valid multi-component frame.
  if (num_components > 1) {
    unsigned blocks_per_mcu = 0;
    for (uint8_t i = 0; i < num_components; i++) {
      blocks_per_mcu += (unsigned)sof->h_samp[i] * (unsigned)sof->v_samp[i];
    }
    if (blocks_per_mcu > 10u) {
      return GIMG_ERR_FORMAT;
    }
  }
  return GIMG_OK;
}

void jpeg_apply_dht_payload(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload_buf, size_t payload_size,
    const GIMG_Allocator * alloc) {
  const unsigned char * p = payload_buf;
  size_t remain = payload_size;
  alloc = gimg_alloc_or_default(alloc);
  while (remain >= GIMG_JPEG_DHT_HEADER_LEN) {
    uint8_t tc_th = p[0];
    uint8_t th = tc_th & 0x0Fu;
    uint8_t tc = (tc_th >> 4) & 1;
    size_t num_symbols = 0;
    for (int i = 1; i <= (int)GIMG_JPEG_DHT_BIT_COUNTS; i++) {
      num_symbols += p[i];
    }
    // T.81 B.2.4: number of value bytes must equal sum of the 16 bit counts.
    if (th >= 4 || remain < GIMG_JPEG_DHT_HEADER_LEN + num_symbols) {
      break;
    }
    size_t table_len = GIMG_JPEG_DHT_HEADER_LEN + num_symbols;
    if (tc) {
      // T.81 B.2.4: one AC table per Th, replaced by each DHT that names it.
      // There is no separate "refinement" table in the spec.  Sorting tables by
      // symbol count (17 or 18 meaning refinement) misfiles any refinement
      // table of another size - a 15-symbol one is perfectly legal and common -
      // and then a scan that wanted it found the wrong table or none.
      unsigned char ** dest = &state->huff_ac[th];
      size_t * dest_len = &state->huff_ac_len[th];
      if (*dest) {
        gimg_free(alloc, *dest);
      }
      *dest = (unsigned char *)gimg_malloc(alloc, table_len);
      if (*dest) {
        memcpy(*dest, p, table_len);
        *dest_len = table_len;
      }
    }
    else {
      unsigned char ** dest = &state->huff_dc[th];
      size_t * dest_len = &state->huff_dc_len[th];
      if (*dest) {
        gimg_free(alloc, *dest);
      }
      *dest = (unsigned char *)gimg_malloc(alloc, table_len);
      if (*dest) {
        memcpy(*dest, p, table_len);
        *dest_len = table_len;
      }
    }
    p += table_len;
    remain -= table_len;
  }
}

void jpeg_record_dht_payload(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload_buf, size_t payload_size,
    const GIMG_Allocator * alloc) {
  const unsigned char * p = payload_buf;
  size_t remain = payload_size;
  alloc = gimg_alloc_or_default(alloc);
  while (remain >= GIMG_JPEG_DHT_HEADER_LEN &&
      state->num_dht_entries < GIMG_JPEG_MAX_DHT_ENTRIES) {
    uint8_t tc_th = p[0];
    uint8_t th = tc_th & 0x0Fu;
    uint8_t tc = (tc_th >> 4) & 1;
    size_t num_symbols = 0;
    for (int i = 1; i <= (int)GIMG_JPEG_DHT_BIT_COUNTS; i++) {
      num_symbols += p[i];
    }
    if (th >= 4 || remain < GIMG_JPEG_DHT_HEADER_LEN + num_symbols) {
      break;
    }
    size_t table_len = GIMG_JPEG_DHT_HEADER_LEN + num_symbols;
    unsigned char is_ac_refine =
        (tc && (num_symbols == 17 || num_symbols == 18)) ? 1 : 0;
    unsigned char * copy = (unsigned char *)gimg_malloc(alloc, table_len);
    if (!copy) {
      break;
    }
    memcpy(copy, p, table_len);
    state->dht_entries[state->num_dht_entries].tc = (uint8_t)tc;
    state->dht_entries[state->num_dht_entries].th = th;
    state->dht_entries[state->num_dht_entries].is_ac_refine = is_ac_refine;
    state->dht_entries[state->num_dht_entries].payload = copy;
    state->dht_entries[state->num_dht_entries].len = table_len;
    state->num_dht_entries++;
    p += table_len;
    remain -= table_len;
  }
}

GIMG_Result jpeg_append_scan_data(gimg_jpeg_doc_state_t * state,
    const unsigned char * data, size_t len) {
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

GIMG_Result jpeg_append_unknown_app(gimg_jpeg_doc_state_t * state,
    uint8_t marker, const unsigned char * payload, size_t payload_size,
    const GIMG_Allocator * alloc, GIMG_Diagnostics * diagnostics,
    size_t seg_start) {
  if (payload_size > 65535u) {
    return GIMG_OK; // Skip oversized; do not fail load
  }
  alloc = gimg_alloc_or_default(alloc);
  size_t need = state->unknown_app_combined_size + 1u + 2u + payload_size;
  unsigned char * new_buf =
      (unsigned char *)gimg_realloc(alloc, state->unknown_app_combined, need);
  if (!new_buf && need > 0) {
    if (diagnostics) {
      (void)gimg_diagnostics_append(
          diagnostics, "jpeg", (uint32_t)seg_start, (uint32_t)marker,
          GIMG_DIAG_ERROR, "unknown APP append OOM");
    }
    return GIMG_ERR_OOM;
  }
  state->unknown_app_combined = new_buf;
  size_t off = state->unknown_app_combined_size;
  state->unknown_app_combined[off] = marker;
  state->unknown_app_combined[off + 1] = (unsigned char)(payload_size >> 8);
  state->unknown_app_combined[off + 2] = (unsigned char)(payload_size & 0xFFu);
  if (payload_size > 0 && payload) {
    memcpy(state->unknown_app_combined + off + 3, payload, payload_size);
  }
  state->unknown_app_combined_size = need;
  return GIMG_OK;
}
