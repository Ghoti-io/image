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

/**
 * True when a marker stands alone, carrying no length field and no payload.
 *
 * T.81 B.1.1.3 Table B.1: SOI, EOI, RST0..RST7 and TEM.  TEM (0xFF01) is the
 * one that gets forgotten - "temporary private use in arithmetic coding" - and
 * reading a two-byte length after it consumes the start of whatever follows,
 * so a file carrying one was refused outright.  libjpeg skips it.
 */
bool gimg_jpeg_marker_has_no_length(uint8_t marker) {
  if (marker == GIMG_JPEG_MARKER_SOI || marker == GIMG_JPEG_MARKER_EOI ||
      marker == GIMG_JPEG_MARKER_TEM) {
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
    // Adobe pads the Pascal name so that the length byte *plus the name* comes
    // to an even number of bytes - "a null name consists of two bytes of 0".
    // So the pad depends on the parity of 1 + name_len, which is the opposite
    // of name_len's own: this used to add a byte when name_len was odd, which
    // is backwards for every length and left the reader one byte out on the
    // empty name that every writer in practice emits.  It then read the
    // four-byte resource size from the wrong offset and walked into the data,
    // so no real Photoshop APP13 was ever parsed at all.
    size_t name_total = 1 + name_len + (((1u + name_len) & 1u) ? 1u : 0u);
    if (off + 6 + name_total + 4 > app13_len) {
      break;
    }
    // Widen before shifting, not after: app13 is unsigned char, which promotes
    // to int, so a top byte of 0xFF made this 255 << 24 - undefined, and the
    // cast on the outside is far too late to help.  Same fault as read_u32 in
    // exif.c, found the same way.
    uint32_t data_size = ((uint32_t)app13[off + 6 + name_total] << 24) |
        ((uint32_t)app13[off + 6 + name_total + 1] << 16) |
        ((uint32_t)app13[off + 6 + name_total + 2] << 8) |
        (uint32_t)app13[off + 6 + name_total + 3];
    size_t data_off = off + 6 + name_total + 4;
    if (data_off + data_size > app13_len) {
      break;
    }
    if (id == 0x0404 && data_size > 0) {
      // 0x0404 is IPTC-NAA; inside it, datasets are 0x1C, record, number.
      const unsigned char * iptc = app13 + data_off;
      size_t iptc_len = data_size;
      size_t i = 0;
      while (i + 5 <= iptc_len) {
        uint16_t tag_len = (uint16_t)((iptc[i + 3] << 8) | iptc[i + 4]);
        // 2:120 (0x78) is Caption/Abstract, which is what IPTC's own mapping
        // sends to dc:description and what this field is documented to hold.
        // This read 0x50 - that is 2:80, By-line, the name of the person who
        // took the photograph.  A caller asking what the image is of was
        // being handed a photographer's name.
        if (iptc[i] == 0x1C && iptc[i + 1] == 0x02 && iptc[i + 2] == 0x78 &&
            i + 5 + tag_len <= iptc_len && tag_len > 0) {
          *out_ptr = iptc + i + 5;
          *out_len = (size_t)tag_len;
          return true;
        }
        i += 5 + (size_t)tag_len;
      }
      break;
    }
    // Resource data is padded to an even length as well, so the next '8BIM'
    // does not begin at data_off + data_size when that is odd.
    off = data_off + data_size + (data_size & 1u);
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
  // Hierarchical sequence: every frame owns its scans the same way (B.3.1).
  for (unsigned i = 0; i < state->num_frames; i++) {
    gimg_jpeg_frame_t * f = state->frames[i];
    if (!f) {
      continue;
    }
    for (unsigned k = 0; k < f->num_scans; k++) {
      gimg_jpeg_scan_t * sc = &f->scans[k];
      gimg_free(alloc, sc->data);
      for (int j = 0; j < 4; j++) {
        gimg_free(alloc, sc->huff_dc[j]);
        gimg_free(alloc, sc->huff_ac[j]);
        gimg_free(alloc, sc->huff_ac_refine[j]);
      }
    }
    gimg_free(alloc, f);
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

/**
 * Apply a DNL segment's payload to the frame height.
 *
 * T.81 B.2.5: DNL carries the number of lines as a 2-byte big-endian value and
 * appears after the first scan.  When the frame header gave a height, DNL must
 * agree with it; when the frame header gave zero (the streaming case), DNL
 * supplies it.
 *
 * Returns GIMG_OK on success, and sets *out_why on failure.
 */
/**
 * Apply a DRI segment (T.81 B.2.4.4).
 *
 * The restart interval is not a property of the frame.  B.2.4.4 lets DRI appear
 * anywhere a marker segment may, including between scans, and an encoder that
 * thinks of its restart interval in MCU rows has to change it from scan to
 * scan, because an interleaved scan and a single-component scan do not have the
 * same number of MCUs in a row.  libjpeg writes a different DRI before nearly
 * every scan of a subsampled progressive image.
 */
/**
 * Install a DQT segment's tables (T.81 B.2.4.1).
 *
 * Its own function rather than a case body, because B.4's abbreviated format
 * for table-specification data installs the very same segments from a stream
 * that has no frame in it at all.
 *
 * @param seen_sof Whether a frame header has already been read, which decides
 *   whether the Pq-against-precision rule can be checked yet.
 */
GIMG_Result gimg_jpeg_apply_dqt(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload, size_t payload_size, bool seen_sof,
    const char ** out_why) {
  *out_why = NULL;
  if (!payload) {
    *out_why = "DQT payload missing";
    return GIMG_ERR_FORMAT;
  }
  // One or more tables. Each: 1 byte (Pq<<4 | Tq), then 64 or 128 bytes.
  const unsigned char * p = payload;
  size_t remain = payload_size;
  while (remain >= 2) {
    uint8_t pq_tq = p[0];
    uint8_t tq = pq_tq & 0x0Fu;
    uint8_t pq = (uint8_t)(pq_tq >> 4);
    int is_16bit = (pq != 0);
    size_t entry_bytes = is_16bit ? 128u : 64u;
    // T.81 B.2.4.1: Pq is 0 (8-bit elements) or 1 (16-bit), Tq selects one of
    // four tables, and the segment must actually contain the elements it
    // declares.  A malformed table used to end the loop silently, leaving
    // whatever tables followed it undefined and the frame to fail later
    // somewhere less informative.
    if (pq > 1u) {
      *out_why = "DQT Pq must be 0 or 1 (T.81 B.2.4.1)";
      return GIMG_ERR_FORMAT;
    }
    // B.2.4.1: "Pq shall be zero for 8-bit sample precision."
    if (pq == 1u && seen_sof && state->sof.precision == 8u) {
      *out_why = "DQT Pq=1 with 8-bit sample precision (T.81 B.2.4.1)";
      return GIMG_ERR_FORMAT;
    }
    if (tq >= GIMG_JPEG_MAX_QUANT_TABLES) {
      *out_why = "DQT Tq above 3 (T.81 B.2.4.1)";
      return GIMG_ERR_FORMAT;
    }
    if (remain < 1 + entry_bytes) {
      *out_why = "DQT segment shorter than the table it declares "
                 "(T.81 B.2.4.1)";
      return GIMG_ERR_FORMAT;
    }
    p++;
    remain--;
    state->quant_tbl_present[tq] = 1;
    if (is_16bit) {
      for (size_t i = 0; i < GIMG_JPEG_DQT_ENTRIES; i++) {
        state->quant_tbl[tq][i] = (uint16_t)((p[i * 2] << 8) | p[i * 2 + 1]);
      }
    }
    else {
      for (size_t i = 0; i < GIMG_JPEG_DQT_ENTRIES; i++) {
        state->quant_tbl[tq][i] = (uint16_t)p[i];
      }
    }
    // T.81 B.2.4.1 Table B.4: a quantization value is 1..255 (Pq=0) or
    // 1..65535 (Pq=1).  Zero is not a permitted value, and dequantization
    // multiplies by it, so a zero element silently discards a coefficient
    // rather than being caught anywhere downstream.
    for (size_t i = 0; i < GIMG_JPEG_DQT_ENTRIES; i++) {
      if (state->quant_tbl[tq][i] == 0u) {
        *out_why = "DQT contains a zero quantization value (T.81 B.2.4.1)";
        return GIMG_ERR_FORMAT;
      }
    }
    p += entry_bytes;
    remain -= entry_bytes;
  }
  return GIMG_OK;
}

GIMG_Result gimg_jpeg_apply_dri(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload, size_t payload_size, const char ** out_why) {
  *out_why = NULL;
  if (payload_size != 2 || !payload) {
    *out_why = "DRI payload must be 2 bytes";
    return GIMG_ERR_FORMAT;
  }
  state->restart_interval = (uint16_t)((payload[0] << 8) | payload[1]);
  return GIMG_OK;
}

/**
 * Apply a DAC segment (T.81 B.2.4.3): conditioning for the arithmetic coder.
 *
 * One byte of table class and destination, then one byte of conditioning.  For
 * a DC table (Tc = 0) that byte is U in the high nibble and L in the low one,
 * and L must not exceed U; for an AC table (Tc = 1) it is Kx, which B.2.4.3
 * bounds to 1..63.  Like DRI, this may appear between scans and change.
 */
GIMG_Result gimg_jpeg_apply_dac(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload, size_t payload_size, const char ** out_why) {
  *out_why = NULL;
  if (!payload) {
    *out_why = "DAC payload missing";
    return GIMG_ERR_FORMAT;
  }
  const unsigned char * p = payload;
  size_t remain = payload_size;
  while (remain >= 2) {
    uint8_t tc = (uint8_t)(p[0] >> 4);
    uint8_t tb = (uint8_t)(p[0] & 0x0Fu);
    uint8_t cs = p[1];
    if (tc > 1 || tb >= GIMG_JPEG_ARITH_TABLES) {
      *out_why = "DAC table class or destination out of range";
      return GIMG_ERR_FORMAT;
    }
    if (tc == 0) {
      uint8_t l = (uint8_t)(cs & 0x0Fu);
      uint8_t u = (uint8_t)(cs >> 4);
      if (l > u) {
        *out_why = "DAC DC conditioning has L greater than U";
        return GIMG_ERR_FORMAT;
      }
      state->arith_cond.dc_l[tb] = l;
      state->arith_cond.dc_u[tb] = u;
    }
    else {
      if (cs < 1u || cs > 63u) {
        *out_why = "DAC AC conditioning Kx out of range";
        return GIMG_ERR_FORMAT;
      }
      state->arith_cond.ac_k[tb] = cs;
    }
    p += 2;
    remain -= 2;
  }
  if (remain != 0) {
    *out_why = "DAC payload is not a whole number of entries";
    return GIMG_ERR_FORMAT;
  }
  return GIMG_OK;
}

static GIMG_Result jpeg_apply_dnl(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload, size_t payload_size, const char ** out_why) {
  *out_why = NULL;
  if (state->num_scans < 1) {
    *out_why = "DNL before first scan";
    return GIMG_ERR_FORMAT;
  }
  if (payload_size != 2 || !payload) {
    *out_why = "DNL payload must be 2 bytes";
    return GIMG_ERR_FORMAT;
  }
  uint16_t dnl_lines = (uint16_t)((payload[0] << 8) | payload[1]);
  if (dnl_lines == 0 || dnl_lines > GIMG_JPEG_MAX_DIMENSION) {
    *out_why = "DNL number of lines out of range";
    return GIMG_ERR_FORMAT;
  }
  if (state->sof.height != 0 && state->sof.height != dnl_lines) {
    *out_why = "DNL number of lines does not match SOF height";
    return GIMG_ERR_FORMAT;
  }
  if (state->sof.height == 0) {
    state->sof.height = dnl_lines;
  }
  return GIMG_OK;
}

/**
 * Start another frame of a hierarchical sequence (T.81 B.3.1, Annex J).
 *
 * The frame takes a copy of the quantization tables and arithmetic
 * conditioning in force at its header, because a hierarchical file normally
 * redefines them between frames and the frame is decoded long after the loader
 * has moved past them.  Huffman tables are not copied: each scan already
 * snapshots the tables in force at its own SOS, which B.2.4 makes the finer
 * and correct granularity.
 *
 * Returns GIMG_OK and sets *out_frame, or an error with *out_why set.
 */
static GIMG_Result jpeg_begin_hierarchical_frame(gimg_jpeg_doc_state_t * state,
    uint8_t marker, const gimg_jpeg_sof_t * sof, unsigned char exp_h,
    unsigned char exp_v, gimg_jpeg_frame_t ** out_frame, const char ** out_why) {
  *out_frame = NULL;
  *out_why = NULL;
  // B.3.1: "The first frame for each component or group of components in a
  // hierarchical process shall be encoded by a non-differential frame."  A
  // sequence that opens with a differential frame has nothing to difference
  // against, so the first frame decides what the reference components are.
  if (state->num_frames == 0 && jpeg_sof_is_differential(marker)) {
    *out_why = "hierarchical sequence starts with a differential frame "
               "(T.81 B.3.1: the first frame shall be non-differential)";
    return GIMG_ERR_FORMAT;
  }
  // B.3.1: "The sample precision (P) shall be constant for all frames and have
  // the identical value as that coded in the DHP marker segment."
  if (sof->precision != state->dhp.precision) {
    *out_why = "frame precision differs from DHP (T.81 B.3.1: P shall be "
               "constant for all frames)";
    return GIMG_ERR_FORMAT;
  }
  // B.3.1: "The number of samples per line (X) for all frames shall not exceed
  // the value coded in the DHP marker segment.  If the number of lines (Y) is
  // non-zero in the DHP marker segment, then the number of lines for all
  // frames shall not exceed the value in the DHP marker segment."
  if (sof->width > state->dhp.width) {
    *out_why = "frame is wider than DHP declares (T.81 B.3.1)";
    return GIMG_ERR_FORMAT;
  }
  if (state->dhp.height != 0u && sof->height > state->dhp.height) {
    *out_why = "frame is taller than DHP declares (T.81 B.3.1)";
    return GIMG_ERR_FORMAT;
  }
  // Annex J: either every non-differential frame is DCT-based or every one is
  // lossless, and a lossless sequence admits only lossless frames.  Mixing the
  // two would mean adding a difference produced by one coding model to a
  // reference produced by the other.
  if (state->num_frames > 0) {
    int first_lossless = state->frames[0]->is_lossless;
    if (jpeg_sof_is_lossless(marker) != first_lossless && first_lossless) {
      *out_why = "DCT frame in a lossless hierarchical sequence (T.81 J: if "
                 "the non-differential frames use lossless processes, all "
                 "differential frames shall use lossless processes)";
      return GIMG_ERR_FORMAT;
    }
  }
  if (state->num_frames >= GIMG_JPEG_MAX_FRAMES) {
    *out_why = "too many frames in the hierarchical sequence";
    return GIMG_ERR_LIMIT;
  }
  const GIMG_Allocator * alloc = gimg_alloc_or_default(state->allocator);
  gimg_jpeg_frame_t * f =
      (gimg_jpeg_frame_t *)gimg_malloc(alloc, sizeof(gimg_jpeg_frame_t));
  if (!f) {
    return GIMG_ERR_OOM;
  }
  memset(f, 0, sizeof(*f));
  f->sof = *sof;
  f->sof_marker = marker;
  f->is_differential = (unsigned char)jpeg_sof_is_differential(marker);
  f->is_progressive = (unsigned char)jpeg_sof_is_progressive(marker);
  f->is_lossless = (unsigned char)jpeg_sof_is_lossless(marker);
  f->is_arithmetic = (unsigned char)jpeg_sof_is_arithmetic(marker);
  f->exp_h = exp_h;
  f->exp_v = exp_v;
  memcpy(f->quant_tbl_present, state->quant_tbl_present,
      sizeof(f->quant_tbl_present));
  memcpy(f->quant_tbl, state->quant_tbl, sizeof(f->quant_tbl));
  f->arith_cond = state->arith_cond;
  state->frames[state->num_frames++] = f;
  *out_frame = f;
  return GIMG_OK;
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
  // T.81 B.2.4.3 defaults, in force unless a DAC segment overrides them.
  jpeg_arith_cond_defaults(&state->arith_cond);
  state->allocator = alloc;

  // T.81 B.4: tables installed from a table-specification stream, for a frame
  // whose own are absent.  They go in before anything is read, so that the
  // stream's own segments override them where it has any - which is what
  // B.2.4.1's "until redefined" means.
  if (options && options->jpeg_tables) {
    const char * why = NULL;
    r = gimg_jpeg_tables_install(options->jpeg_tables, state, alloc, &why);
    if (r != GIMG_OK) {
      jpeg_load_diag(diagnostics, 0, 0, r, why);
      gimg_jpeg_free_doc_state(codec, state);
      return r;
    }
  }

  bool seen_sof = false;
  bool have_pending_marker = false;
  uint8_t pending_marker = 0;
  // Hierarchical sequence bookkeeping (T.81 B.3): the frame whose scans are
  // being read, and any EXP segment waiting to be handed to the next frame.
  gimg_jpeg_frame_t * cur_frame = NULL;
  int pending_exp = 0;
  unsigned char pending_exp_h = 0;
  unsigned char pending_exp_v = 0;

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

    if (gimg_jpeg_marker_has_no_length(marker)) {
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
    // T.81 B.3.2: DHP announces hierarchical mode.  It has the syntax of a
    // frame header but introduces no frame: it declares the size and sampling
    // factors the sequence of frames adds up to, which is what the decoded
    // image measures, and what B.3.1 then bounds every frame header against.
    case GIMG_JPEG_MARKER_DHP: {
      if (state->is_hierarchical || seen_sof) {
        gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            state->is_hierarchical
                ? "a second DHP segment (T.81 B.3.2 allows one)"
                : "DHP after a frame header (T.81 B.3.2: it shall precede "
                  "the first frame)");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      r = jpeg_parse_sof(payload_buf, payload_size, marker, &state->dhp);
      gimg_free(alloc, payload_buf);
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r, "invalid DHP");
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
      state->is_hierarchical = 1;
      // The completed image is what this document is: everything downstream
      // that asks how big the image is - the pixel-count limit below, the
      // item's dimensions, the raster the decoder allocates - means the DHP
      // size, not whatever the first (smallest) frame in the pyramid says.
      state->sof = state->dhp;
      break;
    }
    // T.81 B.3.3: EXP asks for the reference components to be expanded by two
    // before the next frame uses them.  It applies to that one frame, so it is
    // held here and handed to the next frame header rather than kept on the
    // document.
    case GIMG_JPEG_MARKER_EXP: {
      if (!state->is_hierarchical) {
        gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "EXP outside a hierarchical sequence (T.81 B.3.3)");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      // Table B.11: Le is 3, so the payload is the single Eh|Ev byte.
      if (payload_size != 1u || !payload_buf) {
        gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "EXP payload is not one byte (T.81 Table B.11: Le = 3)");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      {
        uint8_t eh = (uint8_t)((payload_buf[0] >> 4) & 0x0Fu);
        uint8_t ev = (uint8_t)(payload_buf[0] & 0x0Fu);
        gimg_free(alloc, payload_buf);
        if (eh > 1u || ev > 1u) {
          jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
              "EXP Eh/Ev must be 0 or 1 (T.81 Table B.11)");
          gimg_jpeg_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        if (pending_exp) {
          jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
              "two EXP segments before one frame (T.81 B.3.1: no more than "
              "one shall precede a given frame)");
          gimg_jpeg_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        pending_exp = 1;
        pending_exp_h = eh;
        pending_exp_v = ev;
      }
      break;
    }
    case GIMG_JPEG_MARKER_SOF0:
    case GIMG_JPEG_MARKER_SOF1:
    case GIMG_JPEG_MARKER_SOF2:
    case GIMG_JPEG_MARKER_SOF3:
    case GIMG_JPEG_MARKER_SOF5:
    case GIMG_JPEG_MARKER_SOF6:
    case GIMG_JPEG_MARKER_SOF7:
    case GIMG_JPEG_MARKER_SOF9:
    case GIMG_JPEG_MARKER_SOF10:
    case GIMG_JPEG_MARKER_SOF11:
    case GIMG_JPEG_MARKER_SOF13:
    case GIMG_JPEG_MARKER_SOF14:
    case GIMG_JPEG_MARKER_SOF15: {
      // Outside a hierarchical sequence a file holds exactly one frame
      // (B.2.1), and a differential frame has nothing to be differential
      // against.
      if (!state->is_hierarchical) {
        if (seen_sof) {
          jpeg_load_fmt_debug("duplicate SOF", seg_start, marker);
          gimg_free(alloc, payload_buf);
          gimg_jpeg_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        if (jpeg_sof_is_differential(marker)) {
          gimg_free(alloc, payload_buf);
          jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
              "differential frame outside a hierarchical sequence (T.81 "
              "B.3.1: it shall follow a DHP segment)");
          gimg_jpeg_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        r = jpeg_parse_sof(payload_buf, payload_size, marker, &state->sof);
        gimg_free(alloc, payload_buf);
        if (r != GIMG_OK) {
          jpeg_load_diag(diagnostics, seg_start, marker, r, "invalid SOF");
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
        state->is_progressive = jpeg_sof_is_progressive(marker);
        state->is_lossless = jpeg_sof_is_lossless(marker);
        state->is_arithmetic = jpeg_sof_is_arithmetic(marker);
        seen_sof = true;
        break;
      }

      // Hierarchical: this SOF starts another frame of the sequence (B.3.1).
      {
        gimg_jpeg_sof_t fsof;
        r = jpeg_parse_sof(payload_buf, payload_size, marker, &fsof);
        gimg_free(alloc, payload_buf);
        if (r != GIMG_OK) {
          jpeg_load_diag(
              diagnostics, seg_start, marker, r, "invalid frame header");
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
        const char * why = NULL;
        r = jpeg_begin_hierarchical_frame(state, marker, &fsof, pending_exp_h,
            pending_exp_v, &cur_frame, &why);
        pending_exp = 0;
        pending_exp_h = 0;
        pending_exp_v = 0;
        if (r != GIMG_OK) {
          jpeg_load_diag(diagnostics, seg_start, marker, r,
              why ? why : "invalid frame in hierarchical sequence");
          gimg_jpeg_free_doc_state(codec, state);
          return r;
        }
        seen_sof = true;
      }
      break;
    }
    case GIMG_JPEG_MARKER_DQT: {
      const char * why = NULL;
      r = gimg_jpeg_apply_dqt(state, payload_buf, payload_size, seen_sof, &why);
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r, why);
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
      break;
    }
    case GIMG_JPEG_MARKER_DAC: {
      const char * why = NULL;
      r = gimg_jpeg_apply_dac(state, payload_buf, payload_size, &why);
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r, why);
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
      break;
    }
    case GIMG_JPEG_MARKER_DRI: {
      const char * why = NULL;
      r = gimg_jpeg_apply_dri(state, payload_buf, payload_size, &why);
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r, why);
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
      break;
    }
    case GIMG_JPEG_MARKER_DNL: {
      const char * why = NULL;
      r = jpeg_apply_dnl(state, payload_buf, payload_size, &why);
      if (payload_buf) {
        gimg_free(alloc, payload_buf);
      }
      if (r != GIMG_OK) {
        jpeg_load_diag(diagnostics, seg_start, marker, r, why);
        gimg_jpeg_free_doc_state(codec, state);
        return r;
      }
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
      // In a hierarchical sequence the scan belongs to the frame whose header
      // we last read, not to the document; everything below is otherwise the
      // same, because B.3.1 makes the frame structure identical to the
      // non-hierarchical one.
      gimg_jpeg_scan_t * sos_scans = cur_frame ? cur_frame->scans : state->scans;
      unsigned * sos_num_scans =
          cur_frame ? &cur_frame->num_scans : &state->num_scans;
      const gimg_jpeg_sof_t * sos_sof = cur_frame ? &cur_frame->sof : &state->sof;
      const int sos_progressive =
          cur_frame ? (int)cur_frame->is_progressive : state->is_progressive;
      // T.81 A.2.3: a sequential or lossless frame may be coded as several
      // non-interleaved scans, one per component, rather than as a single
      // interleaved one, and files in the wild are - libjpeg writes them for
      // any scan script that names one component at a time.  What is not legal
      // is a second scan after one that already carried every component, which
      // is a second copy of the frame.
      if (!sos_progressive && *sos_num_scans > 0 &&
          sos_scans[0].comp_count >= sos_sof->num_components) {
        if (payload_buf)
          gimg_free(alloc, payload_buf);
        jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
            "a second scan after an interleaved one (T.81 A.2.3)");
        gimg_jpeg_free_doc_state(codec, state);
        return GIMG_ERR_FORMAT;
      }
      if (*sos_num_scans >= GIMG_JPEG_MAX_SCANS) {
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
        // T.81 B.2.3 Table B.3: Ns is 1 to 4, whatever Nf is.  A frame of more
        // than four components is legal (B.2.2) and is written as several
        // scans; a scan that claims more than four is not.
        if (ns == 0 || ns > GIMG_JPEG_MAX_SCAN_COMPONENTS ||
            ns > sos_sof->num_components ||
            (size_t)(4 + ns * 2) > payload_size) {
          if (payload_buf)
            gimg_free(alloc, payload_buf);
          jpeg_load_diag(diagnostics, seg_start, marker, GIMG_ERR_FORMAT,
              "invalid SOS Ns or payload length");
          gimg_jpeg_free_doc_state(codec, state);
          return GIMG_ERR_FORMAT;
        }
        gimg_jpeg_scan_t * scan = &sos_scans[*sos_num_scans];
        memset(scan, 0, sizeof(*scan));
        // B.2.4.4: the restart interval in force is the one most recently
        // defined before this scan, which is not necessarily the frame's last.
        scan->restart_interval = state->restart_interval;
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
        // Validate the scan header.  These four fields used to be read and
        // never checked, so a file could name a spectral band running backwards,
        // an AC scan covering several components, or a successive-approximation
        // step of any size, and the entropy decoder would be steered by it into
        // whatever that implied.
        {
          const char * why = NULL;
          // In a hierarchical sequence the scan belongs to the frame whose
          // header we last read, and it is that frame's coding process which
          // decides what these four fields mean (B.3.1).
          int progressive = cur_frame ? (cur_frame->is_progressive ? 1 : 0)
                                      : (state->is_progressive ? 1 : 0);
          int lossless = cur_frame ? (cur_frame->is_lossless ? 1 : 0)
                                   : (state->is_lossless ? 1 : 0);
          int differential = cur_frame ? (cur_frame->is_differential ? 1 : 0) : 0;
          // B.2.3: 0 <= Ss <= 63, Ss <= Se <= 63; Td and Ta select one of four
          // tables.  A lossless scan reads these three fields quite
          // differently (H.1): Ss is the predictor selection value, Se is zero,
          // and Al is the point transform - so the DCT reading of them does not
          // apply and would reject every such scan.
          if (!lossless && (scan->se > 63u || scan->ss > scan->se)) {
            why = "SOS spectral selection out of range (T.81 B.2.3)";
          }
          for (uint8_t i = 0; i < ns && !why; i++) {
            if (scan->dc_tbl[i] > 3u || scan->ac_tbl[i] > 3u) {
              why = "SOS names a Huffman table above 3 (T.81 B.2.3)";
            }
            // B.2.3: every Cs in the scan must be one of the frame's
            // components, and a component may appear only once.
            int found = 0;
            for (uint8_t c = 0; c < sos_sof->num_components; c++) {
              if (sos_sof->comp_id[c] == scan->comp_id[i]) {
                found = 1;
                break;
              }
            }
            if (!found) {
              why = "SOS names a component absent from the frame (T.81 B.2.3)";
            }
            for (uint8_t j = 0; j < i && !why; j++) {
              if (scan->comp_id[j] == scan->comp_id[i]) {
                why = "SOS names the same component twice (T.81 B.2.3)";
              }
            }
          }
          // T.81 A.2.2: an interleaved MCU holds Hi x Vi data units of each
          // of the scan's components and may hold at most ten.  This is a
          // property of the scan, not of the frame: a frame of five or more
          // components exceeds ten between them and is still legal, because
          // A.2.3 requires it to be written as several scans, each within the
          // limit.  It used to be checked against the whole frame at the frame
          // header, which refused every such frame outright.
          if (!why && ns > 1u) {
            unsigned data_units = 0;
            for (uint8_t i = 0; i < ns; i++) {
              for (uint8_t c = 0; c < sos_sof->num_components; c++) {
                if (sos_sof->comp_id[c] == scan->comp_id[i]) {
                  data_units += (unsigned)sos_sof->h_samp[c] *
                      (unsigned)sos_sof->v_samp[c];
                  break;
                }
              }
            }
            if (data_units > 10u) {
              why = "interleaved MCU holds more than ten data units "
                    "(T.81 A.2.2)";
            }
          }
          if (!why && progressive) {
            // G.1.2: a DC scan is Ss = Se = 0; an AC scan has Ss >= 1.
            if (scan->ss == 0u && scan->se != 0u) {
              why = "progressive DC scan must have Se = 0 (T.81 G.1.2)";
            }
            // G.1.2.2: "In a scan with Ss not equal to zero, Ns shall be one."
            else if (scan->ss != 0u && ns != 1u) {
              why = "progressive AC scan must name one component "
                    "(T.81 G.1.2.2)";
            }
            // G.1.1.1.2: successive approximation refines one bit per scan, so
            // a refinement scan has Ah = Al + 1; Ah = 0 is the first pass.
            else if (scan->ah != 0u && scan->ah != (uint8_t)(scan->al + 1u)) {
              why = "progressive refinement must have Ah = Al + 1 "
                    "(T.81 G.1.1.1.2)";
            }
            // G.1.1.1.2: Al is a point transform of the coefficients, bounded
            // by the coefficient range.
            else if (scan->al > 13u) {
              why = "progressive Al out of range (T.81 G.1.1.1.2)";
            }
          }
          else if (lossless && !why) {
            // T.81 H.1: Ss carries the predictor selection value, which is
            // 1..7 in a non-differential frame (0 selects "no prediction" and
            // is only meaningful in the differential frames of Annex J); Se is
            // zero; Ah is zero; and Al is the point transform, which cannot
            // discard every bit of a sample.
            // T.81 Table H.1 and J.1.3.2: selection value 0 is "no
            // prediction", "shall only be used for differential coding in the
            // hierarchical mode of operation", and is required there - "The
            // prediction selection parameter in the scan header shall be set
            // to zero."  So the permitted set is exactly one value or the
            // other, depending on which kind of frame this is.
            if (differential ? (scan->ss != 0u)
                             : (scan->ss < 1u || scan->ss > 7u)) {
              why = differential
                  ? "differential lossless scan must have Ss=0 (T.81 J.1.3.2)"
                  : "lossless predictor selection out of range (T.81 H.1)";
            }
            else if (scan->se != 0u) {
              why = "lossless scan must have Se=0 (T.81 B.2.3)";
            }
            else if (scan->ah != 0u) {
              why = "lossless scan must have Ah=0 (T.81 B.2.3)";
            }
            else if (scan->al >= sos_sof->precision) {
              why = "lossless point transform discards the whole sample "
                    "(T.81 H.1)";
            }
          }
          else if (!why) {
            // B.2.3: in a sequential frame the scan covers the whole block and
            // there is no successive approximation.
            if (scan->ss != 0u || scan->se != 63u || scan->ah != 0u ||
                scan->al != 0u) {
              why = "sequential scan must have Ss=0 Se=63 Ah=0 Al=0 "
                    "(T.81 B.2.3)";
            }
          }
          if (why) {
            gimg_free(alloc, payload_buf);
            jpeg_load_diag(
                diagnostics, seg_start, marker, GIMG_ERR_FORMAT, why);
            gimg_jpeg_free_doc_state(codec, state);
            return GIMG_ERR_FORMAT;
          }
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
        (*sos_num_scans)++;
        // Entropy bytes read below belong to this scan wherever it lives.
        state->cur_scan = scan;
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
        // T.81 B.1.1.2: a marker may be preceded by any number of fill bytes,
        // each 0xFF.  Inside entropy-coded data a 0xFF is always followed by
        // the 0x00 of byte stuffing (B.2.2), so 0xFF 0xFF is padding ahead of
        // the marker that follows and never data - drop it rather than append
        // it, or the entropy decoder gains eight bits that were never coded.
        {
          int fill_ran_out = 0;
          while (b == 0xFF) {
            unsigned char fill = 0;
            size_t fn = 0;
            (void)gimg_stream_read(stream, &fill, 1, &fn);
            r = gimg_stream_peek(stream, &b, 1, &n);
            if (r != GIMG_OK || n == 0) {
              fill_ran_out = 1;
              break;
            }
          }
          if (fill_ran_out) {
            r = jpeg_append_scan_data(state, (const unsigned char *)"\xFF", 1);
            if (r != GIMG_OK) {
              gimg_jpeg_free_doc_state(codec, state);
              return r;
            }
            break;
          }
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
        // A frame header, a DHP or an EXP ends the scan just as firmly as the
        // next SOS does, and none of the three is a table-specification
        // segment that this loop may quietly apply and read past.  Before
        // hierarchical mode was understood they were read and discarded here,
        // which is how the EXP that expands a reference component - and the
        // frame header it belongs to - went missing from a pyramid.
        if (b == GIMG_JPEG_MARKER_SOS || b == GIMG_JPEG_MARKER_EOI ||
            b == GIMG_JPEG_MARKER_DHP || b == GIMG_JPEG_MARKER_EXP ||
            jpeg_marker_is_sof(b)) {
          // T.81 B.2.4: 0xFF that starts the next marker is not scan entropy.
          // Do not overwrite last_scan_data_end_dht_index here; it was set when
          // we first exited this scan's data (before any inter-scan DHT).
          pending_marker = b;
          have_pending_marker = true;
          break;
        }
        if (gimg_jpeg_marker_has_no_length(b)) {
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
          else if (b == GIMG_JPEG_MARKER_DNL) {
            // T.81 B.2.5: DNL follows the first scan, so this is where a legal
            // one appears.  Reading it here rather than discarding it is what
            // makes the height check above reachable at all.
            unsigned char dnl_buf[2];
            if (payload_size != 2) {
              jpeg_load_diag(diagnostics, seg_start, b, GIMG_ERR_FORMAT,
                  "DNL payload must be 2 bytes");
              gimg_jpeg_free_doc_state(codec, state);
              return GIMG_ERR_FORMAT;
            }
            r = gimg_stream_read_exact(stream, dnl_buf, 2);
            if (r != GIMG_OK) {
              gimg_jpeg_free_doc_state(codec, state);
              return r;
            }
            const char * why = NULL;
            r = jpeg_apply_dnl(state, dnl_buf, 2, &why);
            if (r != GIMG_OK) {
              jpeg_load_diag(diagnostics, seg_start, b, r, why);
              gimg_jpeg_free_doc_state(codec, state);
              return r;
            }
          }
          else if (b == GIMG_JPEG_MARKER_DRI || b == GIMG_JPEG_MARKER_DAC) {
            // B.2.4.3 and B.2.4.4: both of these may appear between scans and
            // change what the next scan uses, and both were being discarded
            // here.  libjpeg writes a different DRI before nearly every scan of
            // a subsampled progressive image - an interleaved scan and a
            // single-component scan do not have the same number of MCUs in a
            // row - so keeping the frame's first value made every such file
            // decode against the wrong restart positions.
            if (payload_size > 0) {
              unsigned char * seg =
                  (unsigned char *)gimg_malloc(alloc, payload_size);
              if (!seg) {
                gimg_jpeg_free_doc_state(codec, state);
                return GIMG_ERR_OOM;
              }
              r = gimg_stream_read_exact(stream, seg, payload_size);
              if (r != GIMG_OK) {
                gimg_free(alloc, seg);
                gimg_jpeg_free_doc_state(codec, state);
                return r;
              }
              const char * why = NULL;
              r = (b == GIMG_JPEG_MARKER_DRI)
                  ? gimg_jpeg_apply_dri(state, seg, payload_size, &why)
                  : gimg_jpeg_apply_dac(state, seg, payload_size, &why);
              gimg_free(alloc, seg);
              if (r != GIMG_OK) {
                jpeg_load_diag(diagnostics, seg_start, b, r, why);
                gimg_jpeg_free_doc_state(codec, state);
                return r;
              }
            }
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
        // The chunk count and index are single bytes, so the comparison
        // against GIMG_JPEG_MAX_ICC_CHUNKS - which is 255 - cannot fail as
        // this stands, and a coverage report is right to call that clause
        // unreached.  It stays because it is what ties this check to the size
        // of app2_icc_chunk_payload: widen either the field or the array and
        // the other has to move with it, and the clause is where that is
        // written down.  The other three do fire.
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
  // T.81 B.3.1 allows Y to be zero in DHP, in which case it places no bound on
  // the frames and the completed image is as tall as the tallest of them.  A
  // frame header that is itself zero-height still needs DNL, which the loop
  // above applied, so by here every frame knows its own height.
  if (state->is_hierarchical && state->sof.height == 0u) {
    uint16_t tallest = 0;
    for (unsigned i = 0; i < state->num_frames; i++) {
      if (state->frames[i]->sof.height > tallest) {
        tallest = state->frames[i]->sof.height;
      }
    }
    state->sof.height = tallest;
    state->dhp.height = tallest;
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
  // Zero the whole structure: the fields below are assigned individually,
  // so anything added to GIMG_Doc later would otherwise start as whatever
  // malloc returned.
  memset(doc, 0, sizeof(*doc));
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
    const uint8_t units = state->app0_jfif[7];
    const uint32_t xd = (uint32_t)(((unsigned char)state->app0_jfif[8] << 8) |
        (unsigned char)state->app0_jfif[9]);
    const uint32_t yd = (uint32_t)(((unsigned char)state->app0_jfif[10] << 8) |
        (unsigned char)state->app0_jfif[11]);
    if (units == 1) {
      if (gimg_doc_ensure_meta_common(doc, &meta_common) == GIMG_OK) {
        gimg_meta_common_set_dpi(meta_common, xd, yd);
      }
    }
    else if (units == 0 && xd > 0u && yd > 0u && xd != yd) {
      // JFIF 1.02: units 0 means the density fields are not a resolution at
      // all - they are the pixel's aspect ratio, and the file says nothing
      // about how big anything is.  That is exactly what PNG's pHYs unit 0
      // says and what GIF's Pixel Aspect Ratio byte says, so all three reach a
      // caller through gimg_doc_pixel_aspect_ratio().
      //
      // Equal densities are the JFIF way of writing "square pixels, no size",
      // which every encoder emits as 1:1 whether it knows anything or not.
      // Recording it would turn that boilerplate into a claim the writer never
      // made, and would put a pHYs in every PNG converted from a JPEG.
      gimg_doc_set_pixel_aspect_ratio(doc, xd, yd);
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
  // Populate meta_common description from APP13 IPTC Caption/Abstract
  // (2:120) when not already set.
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
      state->app0_jfif_len >= GIMG_JPEG_JFIF_APP0_FIXED_LEN) {
    // JFIF 1.02: the APP0 payload is "JFIF\0" (5), version (2), units (1),
    // Xdensity (2), Ydensity (2), Xthumbnail (1), Ythumbnail (1) - fourteen
    // bytes - and then 3 * Xthumbnail * Ythumbnail bytes of RGB.  The two
    // thumbnail dimensions are a byte each.  This read them as a pair of
    // 16-bit fields at 12 and 14, so a real file's 2x2 thumbnail came out as
    // 514 wide by whatever its first two pixel bytes happened to say, the
    // length check then failed, and the thumbnail was silently dropped.  The
    // fixture that was meant to cover this had been written to the same
    // misreading, so it passed.
    uint16_t tx = (uint16_t)(unsigned char)state->app0_jfif[12];
    uint16_t ty = (uint16_t)(unsigned char)state->app0_jfif[13];
    if (tx > 0 && ty > 0) {
      size_t thumb_pixels = 0;
      if (gimg_safe_pixel_count((uint32_t)tx, (uint32_t)ty, &thumb_pixels) ==
              GIMG_OK &&
          thumb_pixels <= GIMG_JPEG_MAX_THUMB_PIXELS) {
        size_t need_rgb = 0;
        size_t need_gray = 0;
        int use_rgb = -1; // 0 = grayscale, 1 = RGB
        if (gcu_safe_mul_size(thumb_pixels, 3u, &need_rgb) &&
            gcu_safe_add_size(
                GIMG_JPEG_JFIF_APP0_FIXED_LEN, need_rgb, &need_rgb) &&
            state->app0_jfif_len >= need_rgb) {
          use_rgb = 1;
        }
        // One byte per pixel is not JFIF, which says RGB; it is tolerated
        // because writers produce it for grayscale images and the alternative
        // is discarding a thumbnail that is plainly there.
        else if (gcu_safe_add_size(
                     GIMG_JPEG_JFIF_APP0_FIXED_LEN, thumb_pixels, &need_gray) &&
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
            const unsigned char * src =
                state->app0_jfif + GIMG_JPEG_JFIF_APP0_FIXED_LEN;
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
