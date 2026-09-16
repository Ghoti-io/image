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

/**
 * Classify a frame header marker (T.81 Table B.1).
 *
 * The fourteen SOFn codes are laid out so that each property is a property of
 * the code itself: 0xC5-0xC7 and 0xCD-0xCF are the differential frames of
 * Annex J, 0xC9-0xCF use the arithmetic coder of Annex D, and within each group
 * of four the second is progressive and the third lossless.  Reading them off
 * the marker keeps the fourteen cases from having to be spelled out at each
 * place that asks one of these questions.
 */
int jpeg_marker_is_sof(uint8_t m) {
  if (m < 0xC0u || m > 0xCFu) {
    return 0;
  }
  // 0xC4 is DHT, 0xC8 is reserved, 0xCC is DAC: not frame headers.
  return (m != 0xC4u && m != 0xC8u && m != 0xCCu);
}

int jpeg_sof_is_differential(uint8_t m) {
  // T.81 B.3.1: SOF5-SOF7 and SOF13-SOF15.
  return (m >= 0xC5u && m <= 0xC7u) || (m >= 0xCDu && m <= 0xCFu);
}

int jpeg_sof_is_progressive(uint8_t m) {
  // SOF2, SOF6, SOF10, SOF14.
  return m == GIMG_JPEG_MARKER_SOF2 || m == GIMG_JPEG_MARKER_SOF6 ||
      m == GIMG_JPEG_MARKER_SOF10 || m == GIMG_JPEG_MARKER_SOF14;
}

int jpeg_sof_is_lossless(uint8_t m) {
  // SOF3, SOF7, SOF11, SOF15.
  return m == GIMG_JPEG_MARKER_SOF3 || m == GIMG_JPEG_MARKER_SOF7 ||
      m == GIMG_JPEG_MARKER_SOF11 || m == GIMG_JPEG_MARKER_SOF15;
}

int jpeg_sof_is_arithmetic(uint8_t m) {
  // SOF9-SOF11 and SOF13-SOF15.
  return (m >= 0xC9u && m <= 0xCBu) || (m >= 0xCDu && m <= 0xCFu);
}

/**
 * Decide whether a three-component frame carries R, G, B rather than Y, Cb, Cr.
 *
 * T.81 says nothing about colour: a component is a component, and the frame
 * header names them only by identifier.  What a decoder does with three of them
 * is settled by the application conventions layered on top - JFIF, which
 * defines its images to be YCbCr, and Adobe's APP14, whose transform byte says
 * outright which of the two the encoder used.  This is libjpeg's rule
 * (jdapimin.c, default_decompress_parms), and following it is what makes the
 * two libraries agree on files neither standard covers:
 *
 *   - a JFIF APP0 means YCbCr, and outranks everything else;
 *   - otherwise an Adobe APP14 decides, transform 0 being RGB and 1 YCbCr;
 *   - otherwise the component identifiers are the only evidence left, and
 *     'R', 'G', 'B' is the one spelling that means what it says.
 *
 * Everything else falls to YCbCr, which is what the overwhelming majority of
 * three-component JPEGs are.
 */
int jpeg_frame_is_rgb(
    const gimg_jpeg_doc_state_t * state, const gimg_jpeg_sof_t * sof) {
  if (!state || !sof || sof->num_components != 3u) {
    return 0;
  }
  if (state->app0_jfif && state->app0_jfif_len > 0u) {
    return 0;
  }
  if (state->app14 && state->app14_len > 0u) {
    return state->adobe_transform == 0u;
  }
  return sof->comp_id[0] == (uint8_t)'R' && sof->comp_id[1] == (uint8_t)'G' &&
      sof->comp_id[2] == (uint8_t)'B';
}

GIMG_Result jpeg_parse_sof(const unsigned char * payload, size_t len,
    uint8_t sof_marker, gimg_jpeg_sof_t * sof) {
  if (len < 8) {
    return GIMG_ERR_FORMAT;
  }
  uint8_t precision = payload[0];
  uint16_t height = (uint16_t)((payload[1] << 8) | payload[2]);
  uint16_t width = (uint16_t)((payload[3] << 8) | payload[4]);
  uint8_t num_components = payload[5];
  // T.81 B.2.2: Nf is 1 to 255, which is the whole range of the byte it is
  // read from, so only zero is out of range.
  if (num_components == 0) {
    return GIMG_ERR_FORMAT;
  }
  if (sof_marker == GIMG_JPEG_MARKER_SOF0 && precision != 8) {
    return GIMG_ERR_FORMAT; // Baseline is 8-bit only per spec.
  }
  // T.81 Table B.2: a lossless frame may use any precision from 2 to 16, and a
  // DCT-based one exactly 8 or 12.  A lossless frame is the only place 16-bit
  // samples are legal in a JPEG.  The rule follows the coding process, not the
  // entropy coder: SOF11 is SOF3 with the arithmetic coder of Annex D, and
  // SOF7 and SOF15 are their differential counterparts (Annex J), so all four
  // take the wider range.
  //
  // DHP is not a frame header but has the same syntax (B.3.2), and describes
  // the completed image of a sequence that may be lossless, so it is given the
  // wider range too; B.3.1 then requires every frame in the sequence to repeat
  // the same precision, which jpeg_load.c checks against this value.
  if (jpeg_sof_is_lossless(sof_marker) || sof_marker == GIMG_JPEG_MARKER_DHP) {
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
    // T.81 B.3.2: "the quantization table destination selector parameter shall
    // be set to zero in the DHP segment".  DHP describes the completed image,
    // not a frame to be decoded, so it selects no table.
    if (sof_marker == GIMG_JPEG_MARKER_DHP && sof->quant_tbl_id[i] != 0u) {
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
  // T.81 A.2.2's ten-data-unit limit is a property of an interleaved MCU, and
  // an MCU belongs to a scan, so it is checked at the scan header (see
  // jpeg_load.c) and not here.  Checking it against the whole frame refused
  // every frame of more than four components outright - B.2.2 allows Nf up to
  // 255, and such a frame is not invalid, it merely cannot be interleaved:
  // A.2.3 requires it to be written as several scans, each within the limit.
  //
  // A frame of four or fewer components could be interleaved, so the limit
  // still applies to it as a whole when it is: that is the same check, made
  // where the scan says how many components it carries.
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
  // In a hierarchical sequence the scan being read belongs to one of frames[],
  // not to state->scans, so the loader records which one rather than letting
  // this recompute it (T.81 B.3.1).
  gimg_jpeg_scan_t * scan = state->cur_scan;
  if (!scan) {
    return GIMG_ERR_FORMAT;
  }
  const GIMG_Allocator * alloc = state->allocator;
  alloc = gimg_alloc_or_default(alloc);
  size_t new_size = scan->data_size + len;
  if (new_size < scan->data_size) {
    return GIMG_ERR_LIMIT; // Wrapped: the scan is longer than size_t can hold.
  }
  if (new_size > scan->data_cap) {
    // Grow geometrically.  This is called once or twice per entropy byte -
    // B.2.2's stuffing has to be read a byte at a time - so resizing to fit
    // each call made loading a scan quadratic in its length, and every
    // intermediate buffer a separate allocation.  A megabyte of entropy data
    // meant a million reallocations.
    size_t cap = scan->data_cap ? scan->data_cap : 4096u;
    while (cap < new_size) {
      if (cap > (size_t)-1 / 2u) {
        cap = new_size;
        break;
      }
      cap *= 2u;
    }
    unsigned char * new_buf =
        (unsigned char *)gimg_realloc(alloc, scan->data, cap);
    if (!new_buf) {
      return GIMG_ERR_OOM;
    }
    scan->data = new_buf;
    scan->data_cap = cap;
  }
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
