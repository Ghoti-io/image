/**
 * @file
 *
 * PNG codec internal constants and structures.
 *
 * Formal specification: ISO/IEC 15948 (PNG); W3C PNG Specification (Third Edition):
 *   https://www.w3.org/TR/PNG/
 * Section anchors for constants below: #3PNGsignature (5.2), #5Chunk-layout (5.3),
 * #5CRC-algorithm (5.5), #11IHDR (11.2.1), #11PLTE (11.2.2), #11IDAT (11.2.3),
 * #11IEND (11.2.4), #11tRNS (11.3.1.1), #11tEXt / #11zTXt / #11iTXt (11.3.3),
 * #11iCCP (11.3.2.3), #11sRGB (11.3.2.5), #11gAMA (11.3.2.2), #11cHRM (11.3.2.1),
 * #eXIf (11.3.4). Interlace (Adam7): W3C DataRep §2.6
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

#ifndef GHOTI_IO_GIMG_PNG_INTERNAL_H
#define GHOTI_IO_GIMG_PNG_INTERNAL_H

#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/stream.h>
#include <stddef.h>
#include <stdint.h>

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
#define GIMG_PNG_IHDR UINT32_C(0x49484452)   // 'IHDR' §11.2.1
#define GIMG_PNG_PLTE UINT32_C(0x504C5445)   // 'PLTE' §11.2.2
#define GIMG_PNG_IDAT UINT32_C(0x49444154)   // 'IDAT' §11.2.3
#define GIMG_PNG_IEND UINT32_C(0x49454E44)   // 'IEND' §11.2.4
#define GIMG_PNG_tRNS UINT32_C(0x74524E53)   // 'tRNS' §11.3.1.1

/** Ancillary chunk type IDs. §11.3 Ancillary chunks. */
#define GIMG_PNG_tEXt UINT32_C(0x74455874)   // 'tEXt' §11.3.3
#define GIMG_PNG_zTXt UINT32_C(0x7A545874)   // 'zTXt' §11.3.3
#define GIMG_PNG_iTXt UINT32_C(0x69545874)   // 'iTXt' §11.3.3
#define GIMG_PNG_iCCP UINT32_C(0x69434350)   // 'iCCP' §11.3.2.3
#define GIMG_PNG_sRGB UINT32_C(0x73524742)   // 'sRGB' §11.3.2.5
#define GIMG_PNG_gAMA UINT32_C(0x67414D41)   // 'gAMA' §11.3.2.2
#define GIMG_PNG_cHRM UINT32_C(0x6348524D)   // 'cHRM' §11.3.2.1
#define GIMG_PNG_eXIf UINT32_C(0x65584966)   // 'eXIf' §11.3.4

/** Single ancillary chunk (payload owned). */
typedef struct {
  gimg_png_chunk_type_t type;
  unsigned char * payload;
  size_t payload_size;
} gimg_png_ancillary_t;

/** IHDR payload length (width, height, bit depth, color type, etc.). §11.2.1 IHDR. */
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
  unsigned char * trns;
  size_t trns_size;
  unsigned char * idat;
  size_t idat_size;
  gimg_png_ancillary_t * ancillary;  ///< Ordered list of ancillary chunks (read order) for round-trip.
  size_t ancillary_count;
  size_t ancillary_capacity;
  const GIMG_Allocator * allocator;
};

/**
 * @brief Validate IHDR and parse into ihdr. Returns GIMG_ERR_FORMAT if invalid.
 */
GIMG_Result gimg_png_parse_ihdr(const unsigned char * payload,
    gimg_png_ihdr_t * ihdr);

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
 * @param stream Stream positioned after chunk header.
 * @param length Payload length from chunk header.
 * @param type Chunk type (for CRC computation).
 * @param payload_buf Buffer for payload (or NULL to skip).
 * @param limits Optional limits; max_chunk_size is enforced.
 * @return GIMG_OK, GIMG_ERR_CORRUPT (bad CRC), GIMG_ERR_LIMIT, GIMG_ERR_IO.
 */
GIMG_Result gimg_png_read_chunk_payload_and_crc(GIMG_Stream * stream,
    uint32_t length, gimg_png_chunk_type_t type, unsigned char * payload_buf,
    const GIMG_Limits * limits);

/**
 * @brief PNG codec load callback (used by png_register).
 */
GIMG_Result gimg_png_load(GIMG_Codec * codec, GIMG_Stream * stream,
    const GIMG_Load_Options * options, GIMG_Diagnostics * diagnostics,
    GIMG_Doc ** out_doc);

/**
 * @brief PNG codec decode callback: decode item to raster (DEFLATE + filters).
 */
GIMG_Result gimg_png_decode(GIMG_Codec * codec, const GIMG_Item * item,
    const GIMG_Decode_Options * options, GIMG_Raster ** out_raster);

/**
 * @brief Write one PNG chunk to stream: length (4 BE), type (4), payload, CRC (4).
 * Stream must support write.
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

#endif // GHOTI_IO_GIMG_PNG_INTERNAL_H
