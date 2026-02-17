/**
 * @file
 *
 * JPEG codec internal constants, limits, and structures.
 *
 * Reference: ISO/IEC 10918-1 (JPEG); ITU-T T.81. Segment format: 0xFF + marker
 * byte + length (big-endian 2 bytes, where present) + payload. SOI 0xFF 0xD8,
 * EOI 0xFF 0xD9.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_JPEG_INTERNAL_H
#define GHOTI_IO_GIMG_JPEG_INTERNAL_H

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** SOI magic: 0xFF 0xD8. Probe uses this to identify JPEG. */
#define GIMG_JPEG_SIGNATURE_LEN 2
extern const unsigned char gimg_jpeg_signature[GIMG_JPEG_SIGNATURE_LEN];

/** Marker bytes (after 0xFF). Per ISO/IEC 10918-1 (ITU-T T.81) Annex B, the
 * only Start-of-Frame (SOF) marker bytes are 0xC0, 0xC1, 0xC2, 0xC3, 0xC5,
 * 0xC6, 0xC7, 0xC9, 0xCA, 0xCB (and 0xCD, 0xCE, 0xCF for SOF13–SOF15). 0xC4 is
 * DHT, 0xC8 is reserved, 0xCC is DAC — not SOF. We support SOF0, SOF1, SOF2
 * only. */
#define GIMG_JPEG_MARKER_SOI 0xD8
#define GIMG_JPEG_MARKER_EOI 0xD9
#define GIMG_JPEG_MARKER_SOF0 0xC0 // Baseline DCT (8-bit only)
#define GIMG_JPEG_MARKER_SOF1 0xC1 // Extended sequential DCT (8- or 12-bit)
#define GIMG_JPEG_MARKER_SOF2 0xC2 // Progressive DCT
#define GIMG_JPEG_MARKER_SOF3 0xC3 // Lossless (not yet supported)
// 0xC4 = DHT (Define Huffman Tables), not SOF
// 0xC5 = SOF5  differential sequential DCT; 0xC6 = SOF6; 0xC7 = SOF7
// differential lossless 0xC8 = reserved; 0xC9 = SOF9 arithmetic sequential;
// 0xCA = SOF10; 0xCB = SOF11 0xCC = DAC; 0xCD = SOF13; 0xCE = SOF14; 0xCF =
// SOF15
#define GIMG_JPEG_MARKER_DHT 0xC4
#define GIMG_JPEG_MARKER_DQT 0xDB
#define GIMG_JPEG_MARKER_SOS 0xDA
#define GIMG_JPEG_MARKER_DRI 0xDD
#define GIMG_JPEG_MARKER_DNL 0xDC ///< Define Number of Lines; after first scan.
#define GIMG_JPEG_MARKER_APP0 0xE0
#define GIMG_JPEG_MARKER_APP1 0xE1
#define GIMG_JPEG_MARKER_APP2 0xE2
#define GIMG_JPEG_MARKER_APP13 0xED
#define GIMG_JPEG_MARKER_APP14 0xEE
#define GIMG_JPEG_MARKER_COM 0xFE

/** Meta_raw tag IDs for round-trip (format_id "jpeg"). */
#define GIMG_JPEG_RAW_APP0 0xE0u
/** APP0 JFXX (JFIF 1.02 extension) segment; written after main APP0 when
 * present. */
#define GIMG_JPEG_RAW_APP0_JFXX 0xE001u
#define GIMG_JPEG_RAW_APP1_EXIF 0xE100u
#define GIMG_JPEG_RAW_APP1_XMP 0xE101u
#define GIMG_JPEG_RAW_APP2_ICC 0xE2u
/** Multi-segment APP2 ICC round-trip: serialized [2B N][2B len1][payload1]...
 * Used when ICC profile was split across multiple APP2 segments. */
#define GIMG_JPEG_RAW_APP2_ICC_CHUNKS 0xE201u
#define GIMG_JPEG_RAW_APP13 0xEDu ///< APP13 IPTC/Photoshop (Photoshop 3.0).
#define GIMG_JPEG_RAW_APP14 0xEEu ///< APP14 Adobe (transform: YCbCr/YCCK).
/** Unknown APP segments (APPn not handled as JFIF/EXIF/XMP/ICC/Adobe). Stored
 * as concatenated (1-byte marker + 2-byte BE payload length + payload) in read
 * order for round-trip. */
#define GIMG_JPEG_RAW_APP_UNKNOWN 0xE0FFu
/** COM (Comment) segment(s). Stored as concatenated (2-byte BE length +
 * payload)* for each COM, to preserve order and support multiple. */
#define GIMG_JPEG_RAW_COM 0xFEu
// RST0..RST7 0xD0..0xD7 have no length/payload.

/**
 * Max image dimension (width or height). Rationale: avoid overflow in MCU and
 * buffer calculations. JPEG spec allows up to 65535; we use a lower cap for
 * safety and to make the limit check effective (uint16_t can hold 65535).
 */
#define GIMG_JPEG_MAX_DIMENSION 32768u

/**
 * Max segment payload size when GIMG_Limits.max_chunk_size is not set.
 * Rationale: bomb protection; reject unreasonably large APP/DQT/DHT segments.
 */
#define GIMG_JPEG_DEFAULT_MAX_SEGMENT_PAYLOAD (64u * 1024u)

/** Max number of components (e.g. 4 for CMYK). */
#define GIMG_JPEG_MAX_COMPONENTS 4u

/** Quantization table size (8x8 = 64 entries). T.81 Annex B. */
#define GIMG_JPEG_DQT_ENTRIES 64u

/** DHT: number of bit-length counts (T.81 B.2.4). Value bytes = sum of these. */
#define GIMG_JPEG_DHT_BIT_COUNTS 16u
/** DHT minimum payload per table: 1 (TcTh) + 16 (bit counts) = 17 bytes. */
#define GIMG_JPEG_DHT_HEADER_LEN 17u
/** 8-bit AC table symbol count (T.81 Annex K Table K.4). */
#define GIMG_JPEG_AC_SYMBOLS_8BIT 162u
/** Extended-precision AC table symbol count (12/16-bit: 162 + 80). */
#define GIMG_JPEG_AC_SYMBOLS_EXTENDED 242u

/** Max number of quantization tables. */
#define GIMG_JPEG_MAX_QUANT_TABLES 4u

/** Max number of Huffman tables (DC + AC per class). */
#define GIMG_JPEG_MAX_HUFF_TABLES 8u

/**
 * Max number of scans (progressive JPEG). Rationale: bomb protection; typical
 * progressive has on the order of 10–20 scans.
 */
#define GIMG_JPEG_MAX_SCANS 128u

/** Max DHT table entries we record (for "first DHT after previous scan" rule). */
#define GIMG_JPEG_MAX_DHT_ENTRIES 128u

/**
 * Max thumbnail pixels (JFIF embedded or JFXX) for bomb protection.
 * Rationale: 256×256 is a common thumbnail cap; avoids overflow in size checks.
 */
#define GIMG_JPEG_MAX_THUMB_PIXELS (256u * 256u)

/** Zigzag order (stream index -> row-major position). DQT is stored in this
 * order. Defined in jpeg_zigzag_internal.h. */
#include "jpeg_zigzag_internal.h"

/** Max APP2 ICC_PROFILE chunks (1-based index in spec; 255 max). */
#define GIMG_JPEG_MAX_ICC_CHUNKS 255u

/**
 * Max assembled ICC profile size (bytes). Rationale: bomb protection; match
 * PNG iCCP limit (4 MiB).
 */
#define GIMG_JPEG_MAX_ICC_PROFILE_SIZE (4u * 1024u * 1024u)

/**
 * One scan (SOS) for baseline (single scan) or progressive (multiple scans).
 * For progressive, each SOS may be preceded by DHT; we snapshot the Huffman
 * tables active at this SOS so decode uses the correct tables per scan.
 */
typedef struct {
  uint8_t comp_count;
  uint8_t comp_id[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t dc_tbl[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t ac_tbl[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t ss, se, ah, al; ///< Spectral selection and successive approximation.
  unsigned char * data;
  size_t data_size;
  /** Snapshot of Huffman tables at this SOS (progressive multi-DHT). NULL = use
   * state's. */
  unsigned char * huff_dc[4];
  size_t huff_dc_len[4];
  unsigned char * huff_ac[4];
  size_t huff_ac_len[4];
  /** AC refinement (17-symbol) tables when Ah!=0; NULL = use huff_ac[]. */
  unsigned char * huff_ac_refine[4];
  size_t huff_ac_refine_len[4];
} gimg_jpeg_scan_t;

/**
 * Parsed SOF0 (baseline) / SOF2 (progressive) fields.
 */
typedef struct {
  uint8_t precision; ///< Sample precision (8 for baseline).
  uint16_t height;
  uint16_t width;
  uint8_t num_components;
  uint8_t comp_id[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t h_samp[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t v_samp[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t quant_tbl_id[GIMG_JPEG_MAX_COMPONENTS];
} gimg_jpeg_sof_t;

/**
 * Codec-private document state for JPEG (baseline or progressive).
 */
typedef struct gimg_jpeg_doc_state {
  const GIMG_Allocator * allocator;
  gimg_jpeg_sof_t sof;
  int is_progressive; ///< SOF2 vs SOF0.

  // Quantization tables: 64 entries each; -1 = not present.
  int quant_tbl_present[GIMG_JPEG_MAX_QUANT_TABLES];
  uint16_t quant_tbl[GIMG_JPEG_MAX_QUANT_TABLES][GIMG_JPEG_DQT_ENTRIES];

  // Huffman tables (simplified: we store raw DHT payloads for decode later).
  unsigned char * huff_dc[4]; ///< DC 0..3
  size_t huff_dc_len[4];
  unsigned char * huff_ac[4]; ///< AC 0..3 (initial/162-symbol)
  size_t huff_ac_len[4];
  unsigned char * huff_ac_refine[4]; ///< AC 0..3 refinement (17-symbol, Ah!=0)
  size_t huff_ac_refine_len[4];

  uint16_t restart_interval; ///< DRI restart interval in MCUs (0 = none).

  // Scans: one for baseline, multiple for progressive.
  unsigned num_scans;
  gimg_jpeg_scan_t scans[GIMG_JPEG_MAX_SCANS];
  /** Pillow/libjpeg compatibility: AC table was updated by a DHT between
   * scans (after first SOS). When snapshotting for the first AC-initial scan,
   * we leave scan->huff_ac NULL so the decoder uses the default AC table. */
  unsigned char ac_from_inter_scan_dht[4];

  /** Record of each DHT table (in parse order) for "last DHT before this scan"
   * (T.81 B.2.4; matches libjpeg-turbo). */
  struct {
    uint8_t tc;
    uint8_t th;
    unsigned char is_ac_refine; /**< 1 for AC 17-symbol (refinement) table. */
    unsigned char * payload;
    size_t len;
  } dht_entries[GIMG_JPEG_MAX_DHT_ENTRIES];
  size_t num_dht_entries;
  /** Index such that dht_entries[j] for j >= this are "after previous scan
   * data". Set once when we first exit the scan-data loop (before any inter-scan
   * DHT), so "first DHT after previous scan" uses the right range. */
  size_t last_scan_data_end_dht_index;
  /** 1 if we have set last_scan_data_end_dht_index for this inter-scan run. */
  unsigned char inter_scan_dht_index_set;

  // APP segments for metadata (round-trip).
  unsigned char * app0_jfif;
  size_t app0_jfif_len;
  /** APP0 JFXX (JFIF 1.02 extension) when present; preserved for round-trip. */
  unsigned char * app0_jfxx;
  size_t app0_jfxx_len;
  unsigned char * app1_exif;
  size_t app1_exif_len;
  unsigned char * app1_xmp;
  size_t app1_xmp_len;
  unsigned char * app2_icc;
  size_t app2_icc_len;
  /** For multi-segment ICC: number of chunks (0 = single segment or none).
   * When > 0, app2_icc points to assembled profile only; chunk payloads stored
   * for round-trip in app2_icc_chunk_* and in meta_raw APP2_ICC_CHUNKS. */
  unsigned app2_icc_num_chunks;
  /** Expected total chunks (multi-segment); 0 until first multi-segment seen.
   */
  unsigned app2_icc_total_chunks;
  /** Chunks received so far (multi-segment). */
  unsigned app2_icc_chunks_received;
  /** Full segment payload (ICC_PROFILE\0 + index + total + data) per chunk. */
  unsigned char * app2_icc_chunk_payload[GIMG_JPEG_MAX_ICC_CHUNKS];
  size_t app2_icc_chunk_len[GIMG_JPEG_MAX_ICC_CHUNKS];
  unsigned char * app13; ///< APP13 IPTC/Photoshop payload when
                         ///< "Photoshop 3.0\0"; else in unknown.
  size_t app13_len;
  unsigned char *
      app14; ///< APP14 Adobe payload when "Adobe\0"; else in unknown.
  size_t app14_len;
  /** APP14 Adobe transform: 0=unknown, 1=YCbCr, 2=YCCK. Used for 4-component
   * decode. */
  uint8_t adobe_transform;
  /** COM segment(s) for round-trip: concatenated (2-byte BE length + payload)
   * per COM, in read order. */
  unsigned char * com_combined;
  size_t com_combined_size;
  /** Unknown APP segments (APP3–APP15 and unhandled APP0/1/2): (marker + 2-byte
   * BE length + payload) per segment, in read order. */
  unsigned char * unknown_app_combined;
  size_t unknown_app_combined_size;
} gimg_jpeg_doc_state_t;

/** Huffman decode table (used by bitstream/block decode). */
typedef struct {
  uint16_t min_code[17];
  uint16_t max_code[17];
  uint16_t base_index[17];
  uint8_t values[256];
  int num_values;
} gimg_jpeg_huff_table_t;

/** Bitstream over scan data (MSB first; 0xFF 0x00 is data). Used by
 * jpeg_bitstream.c and jpeg_block.c. */
typedef struct {
  const unsigned char * data;
  size_t size;
  size_t byte_off;
  int bit_off;
  int pushback;
  unsigned char pushback_buf[16];
  unsigned int pushback_n;
  int pad_at_eob;
  int recover_stuff_zero;
  int stuffed_any;
  int expect_rst;
  int rst_just_skipped;
} gimg_jpeg_bitstream_t;

/**
 * Read and verify SOI (0xFF 0xD8) at current stream position.
 * @return GIMG_OK if SOI present, GIMG_ERR_FORMAT otherwise.
 */
GIMG_Result gimg_jpeg_verify_soi(GIMG_Stream * stream);

/**
 * Read next marker (0xFF + marker_byte). Handles byte stuffing (0xFF 0x00).
 * @param stream Stream at any position (will consume to next 0xFF).
 * @param out_marker Filled with marker byte (0xD8, 0xC0, etc.).
 * @return GIMG_OK, GIMG_ERR_IO, or GIMG_ERR_FORMAT if stream ends before
 * marker.
 */
GIMG_Result gimg_jpeg_read_marker(GIMG_Stream * stream, uint8_t * out_marker);

/**
 * Read segment length (big-endian 2 bytes). Only valid for markers that have a
 * length (not SOI, EOI, RST).
 * @return GIMG_OK with *out_length = value (includes the 2 length bytes).
 */
GIMG_Result gimg_jpeg_read_segment_length(
    GIMG_Stream * stream, uint16_t * out_length);

/**
 * Load JPEG document: parse segments, build doc state, create GIMG_Doc with one
 * item. Enforces GIMG_Limits (max_chunk_size for segment payload). Fills
 * diagnostics on error (codec "jpeg", offset, marker).
 */
GIMG_Result gimg_jpeg_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

/**
 * Free codec-private state (called when document is destroyed).
 */
void gimg_jpeg_free_doc_state(GIMG_Codec * codec, void * codec_private);

/**
 * Decode item to raster (baseline DCT: Huffman, dequant, IDCT, upsample).
 */
GIMG_Result gimg_jpeg_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/**
 * Baseline decode: entropy decode, dequant, IDCT, upsample, color convert.
 * Used by gimg_jpeg_decode when !is_progressive. Internal.
 */
GIMG_Result gimg_jpeg_decode_baseline(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/**
 * Progressive decode: multiple scans (DC then AC spectral/approximation),
 * then dequant, IDCT, upsample, color convert. Internal.
 */
GIMG_Result gimg_jpeg_decode_progressive(const gimg_jpeg_doc_state_t * state,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/**
 * Save document to JPEG stream.
 */
GIMG_Result gimg_jpeg_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report);

/**
 * Encode baseline scan: component buffers (Y or Y/Cb/Cr), produce scan data.
 * h_samp and v_samp may be NULL for 4:4:4 (all 1s). Otherwise h_samp[c],
 * v_samp[c] for each component (1 or 2 for 4:2:0/4:2:2). Caller frees
 * *out_scan_data with document allocator.
 */
GIMG_Result gimg_jpeg_encode_baseline_scan(uint32_t width, uint32_t height,
    int num_components, const unsigned char * comp0,
    const unsigned char * comp1, const unsigned char * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size);

/** Encode baseline (single scan) from coefficient buffer; same block order as
 * gimg_jpeg_progressive_fill_coef_buffer. Used so baseline and progressive
 * use identical coefficients and decode to identical pixels. */
GIMG_Result gimg_jpeg_encode_baseline_scan_from_coef_buffer(
    uint32_t GIMG_MAYBE_UNUSED(width), uint32_t GIMG_MAYBE_UNUSED(height),
    int num_components, const int16_t * coef_buffer, size_t total_blocks,
    const uint8_t * h_samp, const uint8_t * v_samp,
    const GIMG_Allocator * alloc, uint16_t restart_interval,
    unsigned char ** out_scan_data, size_t * out_scan_size);

/** Baseline sequential from coef buffer with extended DHT (12/16-bit). */
GIMG_Result gimg_jpeg_encode_baseline_scan_from_coef_buffer_extended(
    uint32_t width, uint32_t height, int num_components,
    const int16_t * coef_buffer, size_t total_blocks, const uint8_t * h_samp,
    const uint8_t * v_samp, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size);

/** Fill coefficient buffer for progressive encode (DCT, quant, zigzag; MCU
 * order). Caller allocates coef_buffer for *out_total_blocks * 64 int16_t.
 * fdct_method: GIMG_JPEG_FDCT_LOEFFLER (0) or GIMG_JPEG_FDCT_REF (1).
 * quant_method: GIMG_JPEG_QUANT_RECIP (0) or GIMG_JPEG_QUANT_DIV (1). */
GIMG_Result gimg_jpeg_progressive_fill_coef_buffer(uint32_t width,
    uint32_t height, int num_components, const unsigned char * comp0,
    const unsigned char * comp1, const unsigned char * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, unsigned fdct_method, unsigned quant_method,
    int16_t * coef_buffer, size_t * out_total_blocks);

/** Encode one progressive scan from coefficient buffer. Supports Ah>0
 * (refinement). Caller frees *out_scan_data.
 * state_after_scan_out: optional; when non-NULL and scan is AC initial, filled.
 * state_after_previous_scan: optional; when non-NULL and scan is AC refinement, used. */
GIMG_Result gimg_jpeg_encode_progressive_scan(uint32_t width, uint32_t height,
    int num_components, const int16_t * coef_buffer, size_t total_blocks,
    const uint8_t * h_samp, const uint8_t * v_samp, uint8_t Ss, uint8_t Se,
    uint8_t Ah, uint8_t Al, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size, int16_t * state_after_scan_out,
    const int16_t * state_after_previous_scan, int sync_debug_scan_index);

/** Fill scaled default quant tables (quality 1..100). */
void gimg_jpeg_default_quant_scaled(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma);

/** Fill 16-bit quant tables for 12/16-bit DQT (quality 1..100). */
void gimg_jpeg_default_quant_scaled_16bit(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma);

/** Fill 12-bit quant tables (Pq=1; same scaling as 16-bit). */
void gimg_jpeg_default_quant_scaled_12bit(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma);

/** Write standard DHT segments (DC0, AC0, DC1, AC1) to stream. */
GIMG_Result gimg_jpeg_write_standard_dht(
    GIMG_Stream * stream, size_t * out_bytes_written);

/** Write extended DHT for 12/16-bit (DC 0..16, AC 242 symbols). */
GIMG_Result gimg_jpeg_write_standard_dht_extended(
    GIMG_Stream * stream, size_t * out_bytes_written);

/** Baseline encode for 12/16-bit (uint16_t components, extended tables). */
GIMG_Result gimg_jpeg_encode_baseline_scan_16bit(uint32_t width,
    uint32_t height, int num_components, const uint16_t * comp0,
    const uint16_t * comp1, const uint16_t * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, int precision, const GIMG_Allocator * alloc,
    uint16_t restart_interval, unsigned char ** out_scan_data,
    size_t * out_scan_size);

/** Fill coefficient buffer for 12/16-bit progressive encode. */
GIMG_Result gimg_jpeg_progressive_fill_coef_buffer_16bit(uint32_t width,
    uint32_t height, int num_components, const uint16_t * comp0,
    const uint16_t * comp1, const uint16_t * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, int precision, int16_t * coef_buffer,
    size_t * out_total_blocks);

/** Fill coefficient buffer for 12-bit (samples 0..4095, level shift 2048). */
GIMG_Result gimg_jpeg_progressive_fill_coef_buffer_12bit(uint32_t width,
    uint32_t height, int num_components, const uint16_t * comp0,
    const uint16_t * comp1, const uint16_t * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint8_t * h_samp,
    const uint8_t * v_samp, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, int16_t * coef_buffer,
    size_t * out_total_blocks);

/** Progressive scan encode with extended tables (12/16-bit). */
GIMG_Result gimg_jpeg_encode_progressive_scan_16bit(uint32_t width,
    uint32_t height, int num_components, const int16_t * coef_buffer,
    size_t total_blocks, const uint8_t * h_samp, const uint8_t * v_samp,
    uint8_t Ss, uint8_t Se, uint8_t Ah, uint8_t Al,
    const GIMG_Allocator * alloc, uint16_t restart_interval,
    unsigned char ** out_scan_data, size_t * out_scan_size);

/** Write AC refinement DHT (Th=2) for progressive scans with Ah>0. */
GIMG_Result gimg_jpeg_write_ac_refine_dht(
    GIMG_Stream * stream, size_t * out_bytes_written);

/* IDCT module: dezigzag, dequantise, 8x8 inverse DCT (used by jpeg_entropy.c). */
void jpeg_dezigzag(const int16_t * block, int16_t * out);
void jpeg_dequantise(
    const int16_t * block, const uint16_t * quant, int16_t * out);
void jpeg_dequantise_32(
    const int16_t * block, const uint16_t * quant, int32_t * out);
void jpeg_idct_8x8(const int16_t * in, int16_t * out);
void jpeg_idct_8x8_32(const int32_t * in, int32_t * out, int scale);
void jpeg_idct_8x8_islow(const int16_t * in, int16_t * out);

/* Bitstream module: init, read bits, skip RST, build Huffman table, decode
 * symbol, extend (used by jpeg_block.c and jpeg_entropy.c). */
void jpeg_bitstream_init(
    gimg_jpeg_bitstream_t * bs, const unsigned char * data, size_t size);
int jpeg_build_huff_table(
    const unsigned char * dht, size_t dht_len, gimg_jpeg_huff_table_t * tbl);
const unsigned char * jpeg_default_ac_dht_payload(size_t * out_len);
void jpeg_build_pillow_compat_ac_scan1_table(gimg_jpeg_huff_table_t * tbl);
int jpeg_huff_decode(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * tbl, int ac_prefer_eob, int is_ac,
    int first_match_only);
int jpeg_bitstream_read_bit(gimg_jpeg_bitstream_t * bs);
int jpeg_bitstream_read_bits(gimg_jpeg_bitstream_t * bs, int n);
int16_t jpeg_extend(int val, int n);
void jpeg_bitstream_align_skip_rst(gimg_jpeg_bitstream_t * bs);

/* Block module: decode one 8×8 block (baseline + progressive). */
GIMG_Result jpeg_decode_block(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * dc_tbl,
    const gimg_jpeg_huff_table_t * ac_tbl, int16_t * block,
    int16_t * dc_predictor, int is_last_block);
GIMG_Result jpeg_decode_block_progressive_dc(gimg_jpeg_bitstream_t * bs,
    const gimg_jpeg_huff_table_t * dc_tbl, int16_t * block,
    int16_t * dc_predictor, int al, int * out_sym, int * out_diff,
    int trace_all, int is_last_block);
GIMG_Result jpeg_decode_block_progressive_dc_refine(
    gimg_jpeg_bitstream_t * bs, int16_t * block, int16_t * dc_predictor,
    unsigned int al, int * out_bit, int trace_all, int is_last_block);
GIMG_Result jpeg_decode_block_progressive_ac_initial(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se, int al, int do_trace, int trace_block_id,
    unsigned int trace_scan_idx, unsigned int * out_eobrun, int trace_all,
    int is_last_block);
GIMG_Result jpeg_decode_block_progressive_ac_refine(
    gimg_jpeg_bitstream_t * bs, const gimg_jpeg_huff_table_t * ac_tbl,
    int16_t * block, int ss, int se, int al, int do_trace, int trace_block_id,
    int log_sanity, int trace_all, int trace_scan_idx, int is_last_block);

/* Upsample module: chroma upsampling (used by jpeg_entropy.c). */
int jpeg_chroma_sample_fancy_2h2v(const unsigned char * buf,
    size_t stride, uint32_t cw, uint32_t ch, uint32_t x, uint32_t y);

/* Parse module: segment payload → doc state (used by jpeg_load.c). */
GIMG_Result jpeg_parse_sof(const unsigned char * payload, size_t len,
    uint8_t sof_marker, gimg_jpeg_sof_t * sof);
void jpeg_apply_dht_payload(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload_buf, size_t payload_size,
    const GIMG_Allocator * alloc);
void jpeg_record_dht_payload(gimg_jpeg_doc_state_t * state,
    const unsigned char * payload_buf, size_t payload_size,
    const GIMG_Allocator * alloc);
GIMG_Result jpeg_append_scan_data(gimg_jpeg_doc_state_t * state,
    const unsigned char * data, size_t len);
GIMG_Result jpeg_append_unknown_app(gimg_jpeg_doc_state_t * state,
    uint8_t marker, const unsigned char * payload, size_t payload_size,
    const GIMG_Allocator * alloc, GIMG_Diagnostics * diagnostics,
    size_t seg_start);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_JPEG_INTERNAL_H
