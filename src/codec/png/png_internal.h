/**
 * @file
 *
 * PNG codec internal constants and structures.
 *
 * Formal specification: ISO/IEC 15948 (PNG); W3C PNG Specification (Third
 * Edition): https://www.w3.org/TR/PNG/ Section anchors for constants below:
 * #3PNGsignature (5.2), #5Chunk-layout (5.3), #5CRC-algorithm (5.5), #11IHDR
 * (11.2.1), #11PLTE (11.2.2), #11IDAT (11.2.3), #11IEND (11.2.4), #11tRNS
 * (11.3.1.1), #11tEXt / #11zTXt / #11iTXt (11.3.3), #11iCCP (11.3.2.3), #11sRGB
 * (11.3.2.5), #11gAMA (11.3.2.2), #11cHRM (11.3.2.1), #eXIf (11.3.4). Interlace
 * (Adam7): W3C DataRep §2.6
 * (https://www.w3.org/TR/PNG-DataRep.html#DR.Interlaced-data-order).
 * See also image/documentation/format-references.md.
 *
 * Internal design: PNG doc state (gimg_png_doc_state_t) holds IHDR, PLTE, tRNS,
 * concatenated IDAT bytes, and an ordered list of ancillary chunks. Ancillary
 * is stored in file read order so that save can emit chunks in spec order
 * without re-sorting. Decode uses this state to DEFLATE-decompress IDAT,
 * apply filters, and produce a raster; color interpretation (sRGB/iCCP/gAMA)
 * is applied from the ancillary list at decode time (see png_decode.c).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_SRC_CODEC_PNG_PNG_INTERNAL_H
/**
 * Largest chunk payload this loader will buffer when neither the caller nor
 * the stream says otherwise.
 *
 * A PNG chunk length is a four-byte field, so a header can claim nearly two
 * gigabytes; the loader reads every payload into memory before it knows
 * whether the bytes exist.  When the stream knows its own length that is the
 * bound to use - a chunk cannot be longer than its file - and this is only the
 * fallback for a stream that does not.  Generous enough for a single IDAT of a
 * large photograph, which is the biggest chunk a real file has.
 */
#define GIMG_PNG_DEFAULT_MAX_CHUNK_PAYLOAD (64u * 1024u * 1024u)

#define GHOTI_IO_GIMG_SRC_CODEC_PNG_PNG_INTERNAL_H

#include <ghoti.io/image/macros.h>

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <ghoti.io/compress/options.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** PNG signature length (bytes). §5.2 PNG signature. */
#define GIMG_PNG_SIGNATURE_LEN 8

/** PNG signature bytes (89 50 4E 47 0D 0A 1A 0A). §5.2. */
extern const unsigned char gimg_png_signature[GIMG_PNG_SIGNATURE_LEN];

/** Chunk header: length (4) + type (4) = 8 bytes. §5.3 Chunk layout. */
#define GIMG_PNG_CHUNK_HEADER_LEN 8

/** CRC length after payload (4 bytes). §5.3, §5.5 CRC algorithm. */
#define GIMG_PNG_CHUNK_CRC_LEN 4

/** Chunk type as 4-byte identifier (e.g. 'IHDR' stored big-endian). §5.3. */
typedef uint32_t gimg_png_chunk_type_t;

/**
 * Critical chunk IDs: 32-bit value equals type after decoding the 4-byte type
 * from the file (first byte = MSB; see png_chunk.c). §4.8.2 Chunk types,
 * §11.2 Critical chunks.
 */
#define GIMG_PNG_IHDR UINT32_C(0x49484452) // 'IHDR' §11.2.1
#define GIMG_PNG_PLTE UINT32_C(0x504C5445) // 'PLTE' §11.2.2
#define GIMG_PNG_IDAT UINT32_C(0x49444154) // 'IDAT' §11.2.3
#define GIMG_PNG_IEND UINT32_C(0x49454E44) // 'IEND' §11.2.4
#define GIMG_PNG_tRNS UINT32_C(0x74524E53) // 'tRNS' §11.3.1.1

/** Ancillary chunk type IDs. §11.3 Ancillary chunks. */
#define GIMG_PNG_tEXt UINT32_C(0x74455874) // 'tEXt' §11.3.3
#define GIMG_PNG_zTXt UINT32_C(0x7A545874) // 'zTXt' §11.3.3
#define GIMG_PNG_iTXt UINT32_C(0x69545874) // 'iTXt' §11.3.3
#define GIMG_PNG_iCCP UINT32_C(0x69434350) // 'iCCP' §11.3.2.3
#define GIMG_PNG_sRGB UINT32_C(0x73524742) // 'sRGB' §11.3.2.5
#define GIMG_PNG_gAMA UINT32_C(0x67414D41) // 'gAMA' §11.3.2.2
#define GIMG_PNG_cHRM UINT32_C(0x6348524D) // 'cHRM' §11.3.2.1
#define GIMG_PNG_eXIf UINT32_C(0x65584966) // 'eXIf' §11.3.4

/** PNG Third Edition colour chunks (W3C PNG 3rd ed., 2025). */
#define GIMG_PNG_cICP UINT32_C(0x63494350) // 'cICP' coding-independent points
#define GIMG_PNG_mDCv UINT32_C(0x6D444376) // 'mDCv' mastering display volume
#define GIMG_PNG_cLLi UINT32_C(0x634C4C69) // 'cLLi' content light level

/** cICP payload: colour primaries, transfer function, matrix, range flag. */
#define GIMG_PNG_cICP_LEN 4

/** APNG chunk type IDs (Mozilla APNG spec). */
#define GIMG_PNG_acTL UINT32_C(0x6163544C) // 'acTL' animation control
#define GIMG_PNG_fcTL UINT32_C(0x6663544C) // 'fcTL' frame control
#define GIMG_PNG_fdAT UINT32_C(0x66644154) // 'fdAT' frame data

/** acTL payload length (num_frames + num_plays). */
#define GIMG_PNG_acTL_LEN 8
/** fcTL payload length (sequence, width, height, x_off, y_off, delay_num,
 * delay_den, dispose_op, blend_op). */
#define GIMG_PNG_fcTL_LEN 26
/** fdAT minimum payload (4-byte sequence number + at least 0 bytes frame data).
 */
#define GIMG_PNG_fdAT_SEQ_LEN 4

/** Chunk read stack buffer size (payloads up to this use stack; larger use
 * heap). Rationale: small chunks (IHDR, tEXt, etc.) are common; avoids
 * alloc for typical case. */
#define GIMG_PNG_CHUNK_READ_STACK_BUF 4096

/** Max decoded ICC profile size (iCCP decompression). Bomb protection. */
#define GIMG_PNG_ICC_MAX_DECODED (4u * 1024u * 1024u)

/** Max IDAT/fdAT chunk size when writing (split zlib payload into chunks of
 * this size for decoder compatibility). */
#define GIMG_PNG_IDAT_CHUNK_MAX 32768u

/** Min zlib stream length: 2-byte header + 4-byte Adler-32 (RFC 1950).
 * Payload (DEFLATE) may be empty; decoders skip header/trailer. */
#define GIMG_PNG_ZLIB_MIN_BYTES 6u

/** gAMA chunk stores gamma × this value (PNG §11.3.2.2). */
#define GIMG_PNG_GAMA_SCALE 100000u

/** PLTE max palette entries (PNG §11.2.2). */
#define GIMG_PNG_PLTE_MAX_ENTRIES 256u

/** Parsed fcTL fields (APNG frame control). */
typedef struct {
  uint32_t sequence_number;
  uint32_t width;
  uint32_t height;
  uint32_t x_offset;
  uint32_t y_offset;
  uint16_t delay_num;
  uint16_t delay_den;
  uint8_t dispose_op; ///< 0=NONE, 1=BACKGROUND, 2=PREVIOUS
  uint8_t blend_op;   ///< 0=SOURCE, 1=OVER
} gimg_png_fctl_t;

/** Per-frame data for APNG (fcTL + concatenated IDAT/fdAT bytes). */
typedef struct {
  gimg_png_fctl_t fctl;
  unsigned char * data; ///< Zlib-wrapped DEFLATE (same as IDAT); owned.
  size_t data_size;
} gimg_png_frame_t;

/** Single ancillary chunk (payload owned). */
typedef struct {
  gimg_png_chunk_type_t type;
  unsigned char * payload;
  size_t payload_size;
} gimg_png_ancillary_t;

/** IHDR payload length (width, height, bit depth, color type, etc.). §11.2.1
 * IHDR. */
#define GIMG_PNG_IHDR_LEN 13

/** Parsed IHDR fields. §11.2.1 Image header. */
typedef struct {
  uint32_t width;
  uint32_t height;
  uint8_t bit_depth;
  uint8_t color_type;
  uint8_t compression_method;
  uint8_t filter_method;
  uint8_t interlace_method;
} gimg_png_ihdr_t;

/** PNG document state (owned by doc->codec_private). */
typedef struct gimg_png_doc_state gimg_png_doc_state_t;

struct gimg_png_doc_state {
  gimg_png_ihdr_t ihdr;
  unsigned char * plte;
  size_t plte_size;
  int plte_is_suggested; ///< PLTE seen on a truecolour frame: advisory only
                         ///< (PNG 11.2.2), never used to decode.
  unsigned char * trns;
  size_t trns_size;
  unsigned char * idat; ///< Single-frame PNG: image data. APNG: default image
                        ///< only (if not first frame).
  size_t idat_size;
  gimg_png_ancillary_t * ancillary; ///< Ordered list of ancillary chunks (read
                                    ///< order) for round-trip.
  size_t ancillary_count;
  size_t ancillary_capacity;
  const GIMG_Allocator * allocator;
  // APNG: animation control and per-frame data.
  int is_apng;        ///< 1 if acTL was seen (before first IDAT).
  uint32_t num_plays; ///< 0 = loop forever.
  gimg_png_frame_t *
      frames;         ///< Array of length frame_count; NULL for non-APNG.
  size_t frame_count; ///< 1 for static PNG, acTL num_frames for APNG.
};

/** Adam7 pass parameters (W3C §2.6 Interlaced data order). Shared by decode
 * and save. Defined in png_common.c. */
typedef struct {
  unsigned int x_offset;
  unsigned int y_offset;
  unsigned int x_step;
  unsigned int y_step;
} gimg_png_adam7_pass_t;

extern const gimg_png_adam7_pass_t gimg_png_adam7_passes[7];

/** Pass width/height for Adam7 (empty pass returns 0). W3C §2.6. */
void gimg_png_adam7_pass_dims(uint32_t image_width, uint32_t image_height,
    unsigned int pass_index, uint32_t * out_pass_width,
    uint32_t * out_pass_height);

/** Row bytes for a row of @a width pixels (excluding filter byte). Supports
 * color_type 0, 2, 3, 4, 6. Returns 0 for invalid color_type. */
size_t gimg_png_row_bytes(uint8_t color_type, uint8_t bit_depth,
    uint32_t width);

/** Row bytes from IHDR and width (convenience wrapper). */
size_t gimg_png_row_bytes_from_ihdr(const gimg_png_ihdr_t * ihdr,
    uint32_t width);

/**
 * @brief Read one sample of @a depth bits at pixel index @a x from a packed
 * scanline.
 *
 * PNG 7.2: samples of depth 1, 2 and 4 are packed several to a byte, most
 * significant bits first, and each scanline is padded to a byte boundary.
 * Below 8 bits a pixel index is therefore a bit position and not a byte one.
 * @a depth must be 1, 2 or 4.
 */
uint8_t gimg_png_get_sample_bits(
    const unsigned char * row, uint32_t x, uint8_t depth);

/**
 * @brief Write one sample of @a depth bits at pixel index @a x into a packed
 * scanline, leaving the neighbouring samples in that byte untouched. PNG 7.2.
 * @a depth must be 1, 2 or 4.
 */
void gimg_png_set_sample_bits(
    unsigned char * row, uint32_t x, uint8_t depth, uint8_t value);

/** Expected raw size for interlaced (Adam7) image: sum over passes of
 * (1 + row_bytes) * pass_height. Returns true on success, false on overflow. */
bool gimg_png_adam7_raw_size(uint32_t width, uint32_t height,
    uint8_t color_type, uint8_t bit_depth, size_t * out_size);

/**
 * @brief Validate IHDR and parse into ihdr. Returns GIMG_ERR_FORMAT if invalid.
 */
GIMG_Result gimg_png_parse_ihdr(
    const unsigned char * payload, gimg_png_ihdr_t * ihdr);

/**
 * @brief Append one ancillary chunk (type + payload copy) to doc state.
 * Payload is copied; caller keeps ownership of original.
 */
GIMG_Result gimg_png_append_ancillary(gimg_png_doc_state_t * state,
    gimg_png_chunk_type_t type, const unsigned char * payload,
    size_t payload_size);

/**
 * @brief Free PNG doc state (for codec free_doc_private callback).
 */
void gimg_png_free_doc_state(GIMG_Codec * codec, void * codec_private);

/**
 * @brief Read and verify PNG signature.
 * @return GIMG_OK if signature matches, GIMG_ERR_FORMAT if invalid,
 * GIMG_ERR_IO if stream read failed.
 */
GIMG_Result gimg_png_verify_signature(GIMG_Stream * stream);

/**
 * @brief Read chunk length (4 bytes big-endian) and type (4 bytes).
 * Does not read payload or CRC. Stream position advances by 8 bytes.
 */
GIMG_Result gimg_png_read_chunk_header(GIMG_Stream * stream,
    uint32_t * out_length, gimg_png_chunk_type_t * out_type);

/**
 * @brief Read chunk payload and CRC, verify CRC using type + payload.
 * Stream must be positioned after the 8-byte chunk header. On success,
 * stream is positioned after the CRC. Payload is written to payload_buf
 * (caller allocates; size must be >= length). If payload_buf is NULL,
 * payload is skipped (stream still advanced and CRC still verified).
 * When payload_buf is NULL and length exceeds the stack buffer, a temporary
 * buffer is allocated using alloc; the chunk reader must use the provided
 * allocator for any heap allocation so that codec and document use a single
 * allocator.
 * @param stream Stream positioned after chunk header.
 * @param length Payload length from chunk header.
 * @param type Chunk type (for CRC computation).
 * @param payload_buf Buffer for payload (or NULL to skip).
 * @param limits Optional limits; max_chunk_size is enforced.
 * @param alloc Allocator for temporary read buffer when payload_buf is NULL
 *        and length > GIMG_PNG_CHUNK_READ_STACK_BUF; use codec allocator.
 * @return GIMG_OK, GIMG_ERR_CORRUPT (bad CRC), GIMG_ERR_LIMIT, GIMG_ERR_IO,
 *         GIMG_ERR_OOM.
 */
GIMG_Result gimg_png_read_chunk_payload_and_crc(GIMG_Stream * stream,
    uint32_t length, gimg_png_chunk_type_t type, unsigned char * payload_buf,
    const GIMG_Limits * limits, const GIMG_Allocator * alloc);

/**
 * @brief Parse acTL payload (8 bytes): num_frames, num_plays (big-endian).
 * @return GIMG_OK or GIMG_ERR_FORMAT if invalid.
 */
GIMG_Result gimg_png_parse_actl(
    const unsigned char * payload, uint32_t * num_frames, uint32_t * num_plays);

/**
 * @brief Parse fcTL payload (26 bytes) into fctl (big-endian where applicable).
 * @return GIMG_OK or GIMG_ERR_FORMAT if invalid.
 */
GIMG_Result gimg_png_parse_fctl(
    const unsigned char * payload, gimg_png_fctl_t * fctl);

/**
 * @brief Append bytes to a frame's data buffer (realloc as needed).
 */
GIMG_Result gimg_png_append_frame_data(gimg_png_doc_state_t * state,
    size_t frame_index, const unsigned char * data, size_t len);

/**
 * @brief Create DEFLATE decode options with limits.max_output_bytes set.
 * Caller must call gcomp_options_destroy() when done.
 * @param max_output_bytes Maximum decompressed size (bomb protection).
 * @param out_opts On success, set to new options; on failure, set to NULL.
 * @return GIMG_OK or GIMG_ERR_OOM / GIMG_ERR_INTERNAL.
 */
/** @brief Adler-32 of a buffer (RFC 1950 section 2.2). */
uint32_t gimg_png_adler32(const unsigned char * data, size_t len);

/**
 * @brief Inflate a PNG-embedded zlib stream, checking the wrapper.
 *
 * Validates the RFC 1950 header (PNG 10.3 allows only compression method 8,
 * a window of at most 32768 bytes, and no preset dictionary) and verifies the
 * trailing Adler-32 against the bytes produced. @a zlib_size covers the whole
 * stream, header and trailer included.
 *
 * @return GIMG_ERR_FORMAT for a malformed header, GIMG_ERR_CORRUPT for a
 *   stream that does not inflate or whose Adler-32 disagrees, GIMG_ERR_LIMIT
 *   when the output would exceed @a out_capacity.
 */
GIMG_Result gimg_png_zlib_decode(const unsigned char * zlib_data,
    size_t zlib_size, unsigned char * out, size_t out_capacity,
    size_t * out_len);

GIMG_Result gimg_png_deflate_options_for_decode(size_t max_output_bytes,
    gcomp_options_t ** out_opts);

/**
 * @brief Decode one tEXt/zTXt/iTXt chunk to keyword length and text string.
 * Keyword is payload[0..keyword_len-1] (no null). *out_text is alloc'd,
 * null-terminated; caller frees. Returns GIMG_OK on success.
 */
GIMG_Result gimg_png_text_chunk_decode(gimg_png_chunk_type_t type,
    const unsigned char * payload, size_t payload_size,
    const GIMG_Allocator * alloc, size_t * out_keyword_len, char ** out_text);

/**
 * @brief PNG codec load callback (used by png_register).
 */
GIMG_Result gimg_png_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

/**
 * @brief Convert unfiltered raw PNG samples to output-format pixels.
 * Used by both single-frame and APNG decode paths.
 */
void gimg_png_raw_full_to_pixels(const gimg_png_doc_state_t * state,
    const gimg_png_ihdr_t * ihdr, const GIMG_Pixel_Format * format,
    const unsigned char * raw_full, uint32_t w, uint32_t h, size_t row_bytes,
    void * pixels, size_t stride);

/**
 * @brief Decode one frame's IDAT/fdAT (zlib) to pixels. DEFLATE + unfilter +
 * Adam7 reassembly + raw_to_pixels. Caller frees *out_pixels.
 */
GIMG_Result gimg_png_decode_idat_to_pixels(const gimg_png_doc_state_t * state,
    const gimg_png_ihdr_t * ihdr, const unsigned char * idat_ptr,
    size_t idat_len, uint32_t w, uint32_t h, const GIMG_Pixel_Format * format,
    const GIMG_Allocator * alloc, const GIMG_Limits * limits,
    void ** out_pixels, size_t * out_stride);

/**
 * @brief Decode one APNG frame (fcTL + fdAT) to pixels. Thin wrapper around
 * gimg_png_decode_idat_to_pixels. Caller frees *out_pixels.
 */
GIMG_Result gimg_png_decode_one_apng_frame(
    const gimg_png_doc_state_t * state, const gimg_png_ihdr_t * ihdr,
    size_t frame_index, const GIMG_Pixel_Format * format,
    const GIMG_Allocator * alloc, const GIMG_Limits * limits,
    void ** out_pixels, size_t * out_stride, uint32_t * out_fw,
    uint32_t * out_fh);

/**
 * @brief PNG codec decode callback: decode item to raster (DEFLATE + filters).
 */
GIMG_Result gimg_png_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/**
 * @brief Write one PNG chunk to stream: length (4 BE), type (4), payload, CRC
 * (4). Stream must support write.
 */
GIMG_Result gimg_png_write_chunk(GIMG_Stream * stream,
    gimg_png_chunk_type_t type, const unsigned char * payload,
    size_t payload_size);

/**
 * @brief PNG codec save callback: document to PNG bytes.
 */
GIMG_Result gimg_png_save(GIMG_Codec * codec, const GIMG_Doc * doc,
    GIMG_Stream * stream, const char * format_name,
    const GIMG_Save_Options * options, GIMG_Save_Report * report);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GIMG_SRC_CODEC_PNG_PNG_INTERNAL_H
