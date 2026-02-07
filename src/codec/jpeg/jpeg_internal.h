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

/** Marker bytes (after 0xFF). */
#define GIMG_JPEG_MARKER_SOI 0xD8
#define GIMG_JPEG_MARKER_EOI 0xD9
#define GIMG_JPEG_MARKER_SOF0 0xC0 // Baseline DCT
#define GIMG_JPEG_MARKER_SOF2 0xC2 // Progressive DCT
#define GIMG_JPEG_MARKER_DHT 0xC4
#define GIMG_JPEG_MARKER_DQT 0xDB
#define GIMG_JPEG_MARKER_SOS 0xDA
#define GIMG_JPEG_MARKER_DRI 0xDD
#define GIMG_JPEG_MARKER_APP0 0xE0
#define GIMG_JPEG_MARKER_APP1 0xE1
#define GIMG_JPEG_MARKER_APP2 0xE2
#define GIMG_JPEG_MARKER_APP13 0xED
#define GIMG_JPEG_MARKER_APP14 0xEE
#define GIMG_JPEG_MARKER_COM 0xFE

/** Meta_raw tag IDs for round-trip (format_id "jpeg"). */
#define GIMG_JPEG_RAW_APP0 0xE0u
/** APP0 JFXX (JFIF 1.02 extension) segment; written after main APP0 when present. */
#define GIMG_JPEG_RAW_APP0_JFXX 0xE001u
#define GIMG_JPEG_RAW_APP1_EXIF 0xE100u
#define GIMG_JPEG_RAW_APP1_XMP 0xE101u
#define GIMG_JPEG_RAW_APP2_ICC 0xE2u
#define GIMG_JPEG_RAW_APP13 0xEDu   /**< APP13 IPTC/Photoshop (Photoshop 3.0). */
#define GIMG_JPEG_RAW_APP14 0xEEu   /**< APP14 Adobe (transform: YCbCr/YCCK). */
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

/** Quantization table size (8x8 = 64 entries). */
#define GIMG_JPEG_DQT_ENTRIES 64u

/** Max number of quantization tables. */
#define GIMG_JPEG_MAX_QUANT_TABLES 4u

/** Max number of Huffman tables (DC + AC per class). */
#define GIMG_JPEG_MAX_HUFF_TABLES 8u

/**
 * Max number of scans (progressive JPEG). Rationale: bomb protection; typical
 * progressive has on the order of 10–20 scans.
 */
#define GIMG_JPEG_MAX_SCANS 128u

/**
 * Max thumbnail pixels (JFIF embedded or JFXX) for bomb protection.
 * Rationale: 256×256 is a common thumbnail cap; avoids overflow in size checks.
 */
#define GIMG_JPEG_MAX_THUMB_PIXELS (256u * 256u)

/**
 * One scan (SOS) for baseline (single scan) or progressive (multiple scans).
 */
typedef struct {
  uint8_t comp_count;
  uint8_t comp_id[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t dc_tbl[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t ac_tbl[GIMG_JPEG_MAX_COMPONENTS];
  uint8_t ss, se, ah, al; ///< Spectral selection and successive approximation.
  unsigned char * data;
  size_t data_size;
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
  unsigned char * huff_ac[4]; ///< AC 0..3
  size_t huff_ac_len[4];

  uint16_t restart_interval; ///< DRI restart interval in MCUs (0 = none).

  // Scans: one for baseline, multiple for progressive.
  unsigned num_scans;
  gimg_jpeg_scan_t scans[GIMG_JPEG_MAX_SCANS];

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
  unsigned char * app13;   ///< APP13 IPTC/Photoshop payload when "Photoshop 3.0\0"; else in unknown.
  size_t app13_len;
  unsigned char * app14;  ///< APP14 Adobe payload when "Adobe\0"; else in unknown.
  size_t app14_len;
  /** APP14 Adobe transform: 0=unknown, 1=YCbCr, 2=YCCK. Used for 4-component decode. */
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
 * Caller frees *out_scan_data with document allocator.
 */
GIMG_Result gimg_jpeg_encode_baseline_scan(uint32_t width, uint32_t height,
    int num_components, const unsigned char * comp0,
    const unsigned char * comp1, const unsigned char * comp2, size_t stride0,
    size_t stride1, size_t stride2, const uint16_t * quant_luma,
    const uint16_t * quant_chroma, const GIMG_Allocator * alloc,
    unsigned char ** out_scan_data, size_t * out_scan_size);

/** Fill scaled default quant tables (quality 1..100). */
void gimg_jpeg_default_quant_scaled(
    unsigned quality, uint16_t * quant_luma, uint16_t * quant_chroma);

/** Write standard DHT segments (DC0, AC0, DC1, AC1) to stream. */
GIMG_Result gimg_jpeg_write_standard_dht(
    GIMG_Stream * stream, size_t * out_bytes_written);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_JPEG_INTERNAL_H
