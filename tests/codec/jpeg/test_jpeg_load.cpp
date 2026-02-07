/**
 * @file
 *
 * JPEG load tests: segment parsing, limits, invalid markers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "jpeg_test_utils.h"
#include <cstdint>
#include <cstring>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <vector>

namespace {

void append(std::vector<uint8_t> & out, const unsigned char * p, size_t n) {
  out.insert(out.end(), p, p + n);
}

/** Minimal baseline JPEG: SOI, SOF0 (8x8 grayscale), DQT, DHT, SOS (empty
 * scan), EOI. */
std::vector<uint8_t> make_minimal_jpeg() {
  std::vector<uint8_t> buf;
  // SOI
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF0: L=11, P=8, Y=8, X=8, Nf=1, C1=0 H=1 V=1 Tq=0
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  // DQT: L=67, Pq=0 Tq=0, 64 bytes (dummy quant table)
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  // DHT: DC table 0, 16 bytes counts (all 0), 0 symbols. L=19.
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  // SOS: L=10, Ns=1, C0 Td=0 Ta=0, Ss=0 Se=0 Ah=0 Al=0
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  // EOI
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** Minimal baseline JPEG with DRI (restart interval): SOI, DRI (Ri=4), SOF0,
 * DQT, DHT, SOS (empty scan), EOI. */
std::vector<uint8_t> make_minimal_jpeg_with_dri() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // DRI: L=4, payload 2 bytes Ri=4 (big-endian)
  append(buf, (const unsigned char *)"\xFF\xDD\x00\x04\x00\x04", 6);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** Minimal baseline JPEG with RST5 in the middle of scan data: SOI, SOF0, DQT,
 * DHT, SOS, then scan bytes 0x00, 0xFF 0xD5 (RST5 - consumed, not new
 * segment), 0x00, EOI. Load must succeed (RST consumed, scan continues). */
std::vector<uint8_t> make_minimal_jpeg_with_rst_in_scan() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  buf.push_back(0x00);
  append(buf, (const unsigned char *)"\xFF\xD5", 2); // RST5: consumed
  buf.push_back(0x00);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** Minimal progressive JPEG: SOF2 (8x8 grayscale), DQT, DHT, two SOS (DC then
 * AC band). Scan data empty so decode will fail; load and structure are tested.
 */
std::vector<uint8_t> make_minimal_progressive_jpeg() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF2: same layout as SOF0
  append(buf,
      (const unsigned char *)"\xFF\xC2\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  // First SOS: DC only (Ss=0, Se=0)
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  // Second SOS: AC band (Ss=1, Se=63, Ah=0, Al=0)
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x01\x00\x3F\x00\x00",
      12);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** Minimal TIFF/Exif with IFD0 and Orientation tag only (value 6 = 90 CW). */
std::vector<uint8_t> make_minimal_exif_orientation_6() {
  std::vector<uint8_t> exif;
  // TIFF header: II, 42, IFD0 offset 8
  append(exif, (const unsigned char *)"\x49\x49\x2A\x00\x08\x00\x00\x00", 8);
  // IFD: 1 entry, then 12-byte entry, then next IFD (0)
  append(exif, (const unsigned char *)"\x01\x00", 2);
  // Tag 0x0112 (Orientation), type SHORT(3), count 1, value 6
  append(exif,
      (const unsigned char *)"\x12\x01\x03\x00\x01\x00\x00\x00\x06\x00\x00\x00",
      12);
  append(exif, (const unsigned char *)"\x00\x00\x00\x00", 4);
  return exif;
}

/** TIFF/Exif with IFD0 (orientation 6) and IFD1 (JPEG thumbnail). Thumbnail
 * bytes start at offset 68 in the TIFF; length = thumb_jpeg.size(). */
std::vector<uint8_t> make_exif_with_jpeg_thumbnail(
    const std::vector<uint8_t> & thumb_jpeg) {
  std::vector<uint8_t> exif;
  // TIFF header: II, 42, IFD0 at 8
  append(exif, (const unsigned char *)"\x49\x49\x2A\x00\x08\x00\x00\x00", 8);
  // IFD0: 1 entry (orientation), next IFD = 26
  append(exif, (const unsigned char *)"\x01\x00", 2);
  append(exif,
      (const unsigned char *)"\x12\x01\x03\x00\x01\x00\x00\x00\x06\x00\x00\x00",
      12);
  append(exif, (const unsigned char *)"\x1A\x00\x00\x00", 4); // next IFD
  // IFD1 at 26: 3 entries (Compression=6, 0x0201=68, 0x0202=length)
  const uint32_t thumb_off = 68u;
  const uint32_t thumb_len = (uint32_t)thumb_jpeg.size();
  append(exif, (const unsigned char *)"\x03\x00", 2);
  // Tag 0x0103 Compression, SHORT, 1, value 6
  append(exif,
      (const unsigned char *)"\x03\x01\x03\x00\x01\x00\x00\x00\x06\x00\x00\x00",
      12);
  // Tag 0x0201 JPEGInterchangeFormat, LONG, 1, value thumb_off (68)
  append(exif, (const unsigned char *)"\x01\x02\x04\x00\x01\x00\x00\x00", 8);
  exif.push_back((uint8_t)(thumb_off));
  exif.push_back((uint8_t)(thumb_off >> 8));
  exif.push_back((uint8_t)(thumb_off >> 16));
  exif.push_back((uint8_t)(thumb_off >> 24));
  // Tag 0x0202 JPEGInterchangeFormatLength, LONG, 1, value thumb_len
  append(exif, (const unsigned char *)"\x02\x02\x04\x00\x01\x00\x00\x00", 8);
  exif.push_back((uint8_t)(thumb_len));
  exif.push_back((uint8_t)(thumb_len >> 8));
  exif.push_back((uint8_t)(thumb_len >> 16));
  exif.push_back((uint8_t)(thumb_len >> 24));
  append(exif, (const unsigned char *)"\x00\x00\x00\x00", 4); // no next IFD
  // Pad to offset 68 (26 + 2 + 36 + 4 = 68)
  while (exif.size() < thumb_off) {
    exif.push_back(0);
  }
  exif.insert(exif.end(), thumb_jpeg.begin(), thumb_jpeg.end());
  return exif;
}

/** TIFF/Exif with IFD0 (1 entry) and IFD1 Compression=1 (uncompressed)
 * thumbnail: 2x2 grayscale, strip at offset 116, 4 bytes. */
std::vector<uint8_t> make_exif_with_uncompressed_thumbnail(void) {
  std::vector<uint8_t> exif;
  append(exif, (const unsigned char *)"\x49\x49\x2A\x00\x08\x00\x00\x00", 8);
  append(exif, (const unsigned char *)"\x01\x00", 2);
  append(exif,
      (const unsigned char *)"\x12\x01\x03\x00\x01\x00\x00\x00\x06\x00\x00\x00",
      12);
  append(exif, (const unsigned char *)"\x1A\x00\x00\x00", 4);
  // IFD1 at 26: 7 entries
  const uint32_t strip_off = 116u;
  append(exif, (const unsigned char *)"\x07\x00", 2);
  append(exif,
      (const unsigned char *)"\x03\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00",
      12);
  append(exif,
      (const unsigned char *)"\x00\x01\x04\x00\x01\x00\x00\x00\x02\x00\x00\x00",
      12);
  append(exif,
      (const unsigned char *)"\x01\x01\x04\x00\x01\x00\x00\x00\x02\x00\x00\x00",
      12);
  append(exif,
      (const unsigned char *)"\x02\x01\x03\x00\x01\x00\x00\x00\x08\x00\x00\x00",
      12);
  append(exif, (const unsigned char *)"\x11\x01\x04\x00\x01\x00\x00\x00", 8);
  exif.push_back((uint8_t)(strip_off));
  exif.push_back((uint8_t)(strip_off >> 8));
  exif.push_back((uint8_t)(strip_off >> 16));
  exif.push_back((uint8_t)(strip_off >> 24));
  append(exif, (const unsigned char *)"\x17\x01\x04\x00\x01\x00\x00\x00\x04\x00\x00\x00",
      12);
  append(exif,
      (const unsigned char *)"\x06\x01\x03\x00\x01\x00\x00\x00\x01\x00\x00\x00",
      12);
  append(exif, (const unsigned char *)"\x00\x00\x00\x00", 4);
  while (exif.size() < strip_off) {
    exif.push_back(0);
  }
  append(exif, (const unsigned char *)"\x00\x40\x80\xC0", 4);
  return exif;
}

/** TIFF/Exif with IFD1 Compression=7 (TIFF TechNote 2 JPEG). Single strip
 * at offset 68 containing complete JPEG (no JPEGTables). */
std::vector<uint8_t> make_exif_with_tiff_jpeg_thumbnail(
    const std::vector<uint8_t> & strip_jpeg) {
  std::vector<uint8_t> exif;
  append(exif, (const unsigned char *)"\x49\x49\x2A\x00\x08\x00\x00\x00", 8);
  append(exif, (const unsigned char *)"\x01\x00", 2);
  append(exif,
      (const unsigned char *)"\x12\x01\x03\x00\x01\x00\x00\x00\x06\x00\x00\x00",
      12);
  append(exif, (const unsigned char *)"\x1A\x00\x00\x00", 4);
  const uint32_t strip_off = 68u;
  const uint32_t strip_len = (uint32_t)strip_jpeg.size();
  append(exif, (const unsigned char *)"\x03\x00", 2);
  append(exif,
      (const unsigned char *)"\x03\x01\x03\x00\x01\x00\x00\x00\x07\x00\x00\x00",
      12);
  append(exif, (const unsigned char *)"\x11\x01\x04\x00\x01\x00\x00\x00", 8);
  exif.push_back((uint8_t)(strip_off));
  exif.push_back((uint8_t)(strip_off >> 8));
  exif.push_back((uint8_t)(strip_off >> 16));
  exif.push_back((uint8_t)(strip_off >> 24));
  append(exif, (const unsigned char *)"\x17\x01\x04\x00\x01\x00\x00\x00", 8);
  exif.push_back((uint8_t)(strip_len));
  exif.push_back((uint8_t)(strip_len >> 8));
  exif.push_back((uint8_t)(strip_len >> 16));
  exif.push_back((uint8_t)(strip_len >> 24));
  append(exif, (const unsigned char *)"\x00\x00\x00\x00", 4);
  while (exif.size() < strip_off) {
    exif.push_back(0);
  }
  exif.insert(exif.end(), strip_jpeg.begin(), strip_jpeg.end());
  return exif;
}

/** JPEG with APP1 EXIF containing IFD1 uncompressed (Compression=1) thumbnail;
 * main image minimal baseline, second item = 2x2 grayscale thumbnail. */
std::vector<uint8_t> make_jpeg_with_exif_uncompressed_thumbnail(void) {
  std::vector<uint8_t> exif = make_exif_with_uncompressed_thumbnail();
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  size_t app1_payload = 6 + exif.size();
  uint16_t app1_len = (uint16_t)(2 + app1_payload);
  if (app1_len < 2 + app1_payload) {
    return buf;
  }
  append(buf, (const unsigned char *)"\xFF\xE1", 2);
  buf.push_back((uint8_t)(app1_len >> 8));
  buf.push_back((uint8_t)(app1_len & 0xFF));
  append(buf, (const unsigned char *)"Exif\0\0", 6);
  buf.insert(buf.end(), exif.begin(), exif.end());
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** JPEG with SOI, APP1 Exif (orientation 6), then minimal baseline (no decode).
 */
std::vector<uint8_t> make_jpeg_with_app1_exif() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  std::vector<uint8_t> exif = make_minimal_exif_orientation_6();
  uint16_t app1_len = (uint16_t)(6 + exif.size() + 2); // 2 for length field
  append(buf, (const unsigned char *)"\xFF\xE1", 2);
  buf.push_back((uint8_t)(app1_len >> 8));
  buf.push_back((uint8_t)(app1_len & 0xFF));
  append(buf, (const unsigned char *)"Exif\0\0", 6);
  buf.insert(buf.end(), exif.begin(), exif.end());
  // SOF0, DQT, DHT, SOS, EOI from minimal
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** JPEG with APP1 EXIF containing IFD1 JPEG thumbnail; main image + thumbnail
 * exposed as item 0 and item 1. Uses provided thumb_jpeg (must be decodable).
 */
std::vector<uint8_t> make_jpeg_with_exif_thumbnail(
    const std::vector<uint8_t> & thumb_jpeg) {
  std::vector<uint8_t> exif = make_exif_with_jpeg_thumbnail(thumb_jpeg);
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  size_t app1_payload = 6 + exif.size();
  uint16_t app1_len =
      (uint16_t)(2 + app1_payload); // length field = 2 + payload
  if (app1_len < 2 + app1_payload) {
    return buf; // overflow
  }
  append(buf, (const unsigned char *)"\xFF\xE1", 2);
  buf.push_back((uint8_t)(app1_len >> 8));
  buf.push_back((uint8_t)(app1_len & 0xFF));
  append(buf, (const unsigned char *)"Exif\0\0", 6);
  buf.insert(buf.end(), exif.begin(), exif.end());
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** Minimal 4-component (CMYK-style) SOF0: 8x8, Nf=4, Cid 1,2,3,4, H=V=1, Tq=0.
 * DQT one table (all components use Tq=0), DHT one empty, SOS Ns=4. Scan data
 * empty so decode will fail; load and structure are tested. */
std::vector<uint8_t> make_minimal_four_component_jpeg() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF0: L=20 (2 + 18). Payload 18 bytes: P=8, Y=8, X=8, Nf=4, then
  // 4×(C,HV,Tq) = 01 11 00, 02 11 00, 03 11 00, 04 11 00.
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x14\x08\x00\x08\x00\x08\x04"
                             "\x01\x11\x00\x02\x11\x00\x03\x11\x00\x04\x11\x00",
      22);
  // DQT: L=67, Pq=0 Tq=0, 64 bytes
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  // DHT: DC table 0, 16 counts (all 0), 0 symbols. L=19.
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  // SOS: L=15 (2 + 13 payload). Payload: Ns=4, then 4×(Cs,TdTa): 01 00, 02 00,
  // 03 00, 04 00; then Ss=0 Se=0 Ah=0 Al=0. Total 1+8+4=13 bytes.
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0F\x04\x01\x00\x02\x00\x03\x00\x04"
                             "\x00\x00\x00\x00\x00",
      17);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** JPEG with SOI, APP0 JFIF (DPI 300x300), then minimal baseline. */
std::vector<uint8_t> make_jpeg_with_app0_jfif() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // APP0: length 16, payload 14 bytes: "JFIF\0", v1.1, units=1, X=300, Y=300
  // (big-endian), no thumb
  append(buf,
      (const unsigned char *)"\xFF\xE0\x00\x10JFIF\x00\x01\x01\x01\x01\x2C\x01"
                             "\x2C\x00\x00",
      18);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** JPEG with SOI, one COM segment (comment text), then minimal baseline. */
std::vector<uint8_t> make_jpeg_with_com(const char * comment) {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  size_t clen = strlen(comment);
  if (clen > 65533u)
    clen = 65533u;
  uint16_t seg_len = (uint16_t)(2 + clen); // length field includes 2
  append(buf, (const unsigned char *)"\xFF\xFE", 2);
  buf.push_back((uint8_t)(seg_len >> 8));
  buf.push_back((uint8_t)(seg_len & 0xFF));
  append(buf, (const unsigned char *)comment, clen);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** JPEG with SOI, two COM segments, then minimal baseline (order preserved).
 * Use byte arrays for segments containing 0x00 to avoid C string truncation. */
std::vector<uint8_t> make_jpeg_with_two_com() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // COM "First"  (marker 0xFE, len=7, payload "First")
  static const unsigned char com_first[] = {
      0xFF, 0xFE, 0x00, 0x07, 'F', 'i', 'r', 's', 't'};
  append(buf, com_first, sizeof(com_first));
  // COM "Second" (marker 0xFE, len=8, payload "Second")
  static const unsigned char com_second[] = {
      0xFF, 0xFE, 0x00, 0x08, 'S', 'e', 'c', 'o', 'n', 'd'};
  append(buf, com_second, sizeof(com_second));
  // SOF0
  static const unsigned char sof0[] = {0xFF, 0xC0, 0x00, 0x0B, 0x08, 0x00, 0x08,
      0x00, 0x08, 0x01, 0x00, 0x11, 0x00};
  append(buf, sof0, sizeof(sof0));
  static const unsigned char dqt[] = {0xFF, 0xDB, 0x00, 0x43, 0x00};
  append(buf, dqt, sizeof(dqt));
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  static const unsigned char dht[] = {0xFF, 0xC4, 0x00, 0x14, 0x00};
  append(buf, dht, sizeof(dht));
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  static const unsigned char sos[] = {
      0xFF, 0xDA, 0x00, 0x0A, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  append(buf, sos, sizeof(sos));
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

// meta_raw tag for JPEG APP1 EXIF (must match jpeg_internal.h)
static const uint32_t kJpegRawApp1Exif = 0xE100u;
static const uint32_t kJpegRawApp0 = 0xE0u;
static const uint32_t kJpegRawCom = 0xFEu;

} // namespace

TEST(JpegLoad, ProbeAndLoadMinimalJpeg) {
  std::vector<uint8_t> jpeg = make_minimal_jpeg();
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(s, nullptr);

  GIMG_Probe_Result probe = {};
  r = gimg_probe(s, &probe);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_STREQ(probe.format_name, "jpeg");

  r = gimg_stream_seek(s, 0);
  ASSERT_EQ(r, GIMG_OK);

  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "minimal baseline JPEG should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, NoSofReturnsFormat) {
  // SOI then EOI: no SOF.
  unsigned char buf[] = {0xFF, 0xD8, 0xFF, 0xD9};
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf, sizeof(buf), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_ERR_FORMAT);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, SegmentOverLimitReturnsLimit) {
  // SOI, then a segment with huge length to exceed max_chunk_size.
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // APP0 with length 0x0100 (256); if max_chunk_size=8 we reject payload 254.
  append(buf, (const unsigned char *)"\xFF\xE0\x01\x00", 4);
  // Pad 254 bytes so segment is complete
  for (int i = 0; i < 254; i++) {
    buf.push_back(0);
  }
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Limits limits = {};
  limits.max_chunk_size = 8;
  GIMG_Load_Options opts = {};
  opts.limits = &limits;
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, &opts, nullptr, &doc);
  EXPECT_EQ(r, GIMG_ERR_LIMIT);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, InvalidMarkerDiagnostics) {
  // SOI then invalid/corrupt: e.g. segment length too short.
  unsigned char buf[] = {
      0xFF, 0xD8, 0xFF, 0xDB, 0x00, 0x02 // DQT with L=2 (payload 0), invalid
  };
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf, sizeof(buf), &s);
  GIMG_Diagnostics diag = {};
  gimg_diagnostics_init(&diag, nullptr);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, &diag, &doc);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  gimg_diagnostics_clear(&diag);
  gimg_stream_destroy(s);
}

// Decode tests: limits and corrupt/empty scan.

TEST(JpegLoad, DecodeMinimalJpegReturnsError) {
  std::vector<uint8_t> jpeg = make_minimal_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  GIMG_Decode_Options opts = {};
  GIMG_Result r = gimg_item_decode(item, &opts, &raster);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(raster, nullptr);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, DecodeRespectsMaxDecodedPixels) {
  std::vector<uint8_t> jpeg = make_minimal_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Limits limits = {};
  limits.max_decoded_pixels = 63;
  GIMG_Decode_Options opts = {};
  opts.limits = &limits;
  GIMG_Raster * raster = nullptr;
  GIMG_Result r = gimg_item_decode(item, &opts, &raster);
  EXPECT_EQ(r, GIMG_ERR_LIMIT);
  EXPECT_EQ(raster, nullptr);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, App1ExifPopulatesMetaCommonOrientation) {
  std::vector<uint8_t> jpeg = make_jpeg_with_app1_exif();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Meta_Common * meta = gimg_doc_meta_common(doc);
  ASSERT_NE(meta, nullptr)
      << "APP1 EXIF with orientation should populate meta_common";
  EXPECT_EQ(gimg_meta_common_orientation(meta), GIMG_ORIENTATION_ROTATE_90_CW)
      << "Orientation tag 6 = 90 CW";
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, App1ExifAttachedToMetaRaw) {
  std::vector<uint8_t> jpeg = make_jpeg_with_app1_exif();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr) << "APP1 EXIF should attach doc meta_raw";
  size_t size = 0;
  GIMG_Result r =
      gimg_meta_raw_get(raw, "jpeg", kJpegRawApp1Exif, nullptr, &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_GE(size, 6u + 14u) << "Exif\\0\\0 + minimal TIFF";
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, ExifEmbeddedThumbnailDecodedAsSecondItem) {
  std::vector<uint8_t> thumb_bytes;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", thumb_bytes))
      << "need decodable thumbnail (baseline_8x8_gray.jpg)";
  std::vector<uint8_t> jpeg = make_jpeg_with_exif_thumbnail(thumb_bytes);
  ASSERT_GE(jpeg.size(), 2u) << "build EXIF thumbnail JPEG";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "EXIF IFD1 JPEG thumbnail exposed as second item";
  GIMG_Item * item1 = gimg_doc_item(doc, 1);
  ASSERT_NE(item1, nullptr);
  // Thumbnail is decoded during load and attached as item 1's raster.
  GIMG_Raster * thumb_raster = gimg_item_raster(item1);
  ASSERT_NE(thumb_raster, nullptr) << "thumbnail raster attached";
  EXPECT_EQ(gimg_raster_width(thumb_raster), 8u);
  EXPECT_EQ(gimg_raster_height(thumb_raster), 8u);
  gimg_doc_destroy(doc);
}

TEST(JpegLoad, ExifEmbeddedThumbnailUncompressedSecondItem) {
  std::vector<uint8_t> jpeg = make_jpeg_with_exif_uncompressed_thumbnail();
  ASSERT_GE(jpeg.size(), 2u);
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "EXIF IFD1 Compression=1 thumbnail exposed as second item";
  GIMG_Item * item1 = gimg_doc_item(doc, 1);
  ASSERT_NE(item1, nullptr);
  GIMG_Raster * thumb_raster = gimg_item_raster(item1);
  ASSERT_NE(thumb_raster, nullptr);
  EXPECT_EQ(gimg_raster_width(thumb_raster), 2u);
  EXPECT_EQ(gimg_raster_height(thumb_raster), 2u);
  gimg_doc_destroy(doc);
}

TEST(JpegLoad, ExifEmbeddedThumbnailTiffJpegSecondItem) {
  std::vector<uint8_t> strip_bytes;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", strip_bytes))
      << "need decodable strip for Compression=7 thumbnail";
  std::vector<uint8_t> exif = make_exif_with_tiff_jpeg_thumbnail(strip_bytes);
  std::vector<uint8_t> jpeg;
  append(jpeg, (const unsigned char *)"\xFF\xD8", 2);
  size_t app1_payload = 6 + exif.size();
  uint16_t app1_len = (uint16_t)(2 + app1_payload);
  ASSERT_LT(2u + app1_payload, 65536u);
  append(jpeg, (const unsigned char *)"\xFF\xE1", 2);
  jpeg.push_back((uint8_t)(app1_len >> 8));
  jpeg.push_back((uint8_t)(app1_len & 0xFF));
  append(jpeg, (const unsigned char *)"Exif\0\0", 6);
  jpeg.insert(jpeg.end(), exif.begin(), exif.end());
  append(jpeg,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(jpeg, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    jpeg.push_back(1);
  }
  append(jpeg, (const unsigned char *)"\xFF\xC4\x00\x14\x00", 5);
  for (int i = 0; i < 16; i++) {
    jpeg.push_back(0);
  }
  append(jpeg,
      (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00\x00\x00\x00\x00\x00\x00",
      12);
  append(jpeg, (const unsigned char *)"\xFF\xD9", 2);
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "EXIF IFD1 Compression=7 thumbnail exposed as second item";
  GIMG_Item * item1 = gimg_doc_item(doc, 1);
  ASSERT_NE(item1, nullptr);
  GIMG_Raster * thumb_raster = gimg_item_raster(item1);
  ASSERT_NE(thumb_raster, nullptr);
  EXPECT_EQ(gimg_raster_width(thumb_raster), 8u);
  EXPECT_EQ(gimg_raster_height(thumb_raster), 8u);
  gimg_doc_destroy(doc);
}

TEST(JpegLoad, App0JfifPopulatesMetaCommonDpi) {
  std::vector<uint8_t> jpeg = make_jpeg_with_app0_jfif();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Meta_Common * meta = gimg_doc_meta_common(doc);
  ASSERT_NE(meta, nullptr)
      << "APP0 JFIF with units=dpi should populate meta_common";
  uint32_t x = 0, y = 0;
  gimg_meta_common_dpi(meta, &x, &y);
  EXPECT_EQ(x, 300u);
  EXPECT_EQ(y, 300u);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t size = 0;
  GIMG_Result r = gimg_meta_raw_get(raw, "jpeg", kJpegRawApp0, nullptr, &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(size, 14u) << "JFIF payload (no length field)";
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, ComAttachedToMetaRaw) {
  std::vector<uint8_t> jpeg = make_jpeg_with_com("Hello");
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr) << "COM should attach doc meta_raw";
  size_t size = 0;
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawCom, nullptr, &size);
  ASSERT_EQ(r, GIMG_OK);
  // Stored as (2-byte BE length + payload): 00 05 "Hello" = 7 bytes
  EXPECT_EQ(size, 7u);
  std::vector<uint8_t> com_data(size);
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawCom, com_data.data(), &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(com_data[0], 0x00);
  EXPECT_EQ(com_data[1], 0x05);
  EXPECT_EQ(com_data[2], 'H');
  EXPECT_EQ(com_data[3], 'e');
  EXPECT_EQ(com_data[4], 'l');
  EXPECT_EQ(com_data[5], 'l');
  EXPECT_EQ(com_data[6], 'o');
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, MultipleComPreserveOrderAndContent) {
  std::vector<uint8_t> jpeg = make_jpeg_with_two_com();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(s);
  }
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t size = 0;
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawCom, nullptr, &size);
  ASSERT_EQ(r, GIMG_OK);
  // (2+5) + (2+6) = 7 + 8 = 15 bytes
  EXPECT_EQ(size, 15u);
  std::vector<uint8_t> com_data(size);
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawCom, com_data.data(), &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(com_data[0], 0x00);
  EXPECT_EQ(com_data[1], 0x05);
  EXPECT_EQ(memcmp(com_data.data() + 2, "First", 5), 0);
  EXPECT_EQ(com_data[7], 0x00);
  EXPECT_EQ(com_data[8], 0x06);
  EXPECT_EQ(memcmp(com_data.data() + 9, "Second", 6), 0);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, ProgressiveLoadAndDecodeAttempt) {
  std::vector<uint8_t> jpeg = make_minimal_progressive_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "minimal progressive (SOF2 + 2 SOS) should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  GIMG_Decode_Options opts = {};
  r = gimg_item_decode(item, &opts, &raster);
  // Minimal progressive has no real scan data; decode is expected to fail
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(raster, nullptr);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadBaselineFromNonSeekableStream) {
  std::vector<uint8_t> jpeg = make_minimal_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory_no_seek(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(s);
    ASSERT_EQ(r, GIMG_OK)
        << "baseline JPEG load must work from non-seekable stream";
  }
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadProgressiveFromNonSeekableStream) {
  std::vector<uint8_t> jpeg = make_minimal_progressive_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory_no_seek(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(s);
    ASSERT_EQ(r, GIMG_OK)
        << "progressive JPEG load must work from non-seekable stream";
  }
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadBaselineFromChunkedStream) {
  std::vector<uint8_t> jpeg = make_minimal_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_chunked(jpeg.data(), jpeg.size(), 1u, &s),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK)
      << "baseline JPEG load must work with 1-byte-at-a-time (chunked) reads";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadProgressiveFromChunkedStream) {
  std::vector<uint8_t> jpeg = make_minimal_progressive_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_chunked(jpeg.data(), jpeg.size(), 1u, &s),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "progressive JPEG load must work with "
                           "1-byte-at-a-time (chunked) reads";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadBaselineFromChunkedNonSeekableStream) {
  std::vector<uint8_t> jpeg = make_minimal_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_no_seek_chunked(
                jpeg.data(), jpeg.size(), 1u, &s),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    gimg_stream_destroy(s);
    ASSERT_EQ(r, GIMG_OK)
        << "baseline JPEG load must work with chunked non-seekable stream";
  }
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadJpegWithDriSucceeds) {
  // DRI segment (restart interval) before SOF0: load must succeed.
  std::vector<uint8_t> jpeg = make_minimal_jpeg_with_dri();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "JPEG with DRI segment should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadJpegWithRstInScanSucceeds) {
  // RST marker (0xFF 0xD5) in scan data: consumed, not new segment; load
  // must succeed.
  std::vector<uint8_t> jpeg = make_minimal_jpeg_with_rst_in_scan();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "JPEG with RST in scan data should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadFourComponentCmykStructure) {
  // Minimal 4-component SOF0 (CMYK-style): load must succeed.
  std::vector<uint8_t> jpeg = make_minimal_four_component_jpeg();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "4-component SOF0 JPEG should load";
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 1u);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  // Decode will fail (no valid scan data); we only verify load accepts 4 comp.
  GIMG_Raster * raster = nullptr;
  GIMG_Decode_Options opts = {};
  r = gimg_item_decode(item, &opts, &raster);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(raster, nullptr);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

#ifdef GIMG_TEST_DATA_JPEG
TEST(JpegLoad, DecodeCmykFileWhenPresent) {
  // If tests/data/jpeg/cmyk_sample.jpg exists, load and decode; verify CMYK
  // raster format and color info preserved.
  std::vector<uint8_t> jpeg;
  if (!jpeg_test::load_jpeg_file("cmyk_sample.jpg", jpeg)) {
    GTEST_SKIP() << "cmyk_sample.jpg not in GIMG_TEST_DATA_JPEG";
  }
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK) << "CMYK JPEG file should load";
  ASSERT_NE(doc, nullptr);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  GIMG_Decode_Options opts = {};
  r = gimg_item_decode(item, &opts, &raster);
  ASSERT_EQ(r, GIMG_OK) << "CMYK JPEG should decode";
  ASSERT_NE(raster, nullptr);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK)
      << "Decoded 4-component JPEG should be tagged as CMYK";
  EXPECT_EQ(fmt->channel_count, 4u);
  EXPECT_GT(gimg_raster_width(raster), 0u);
  EXPECT_GT(gimg_raster_height(raster), 0u);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

// ---- Golden decode tests: pixel hash (and metadata) per reference file ----

TEST(JpegLoad, GoldenBaselineGray) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  uint64_t hash = jpeg_test::raster_pixel_hash(raster);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(hash, 17944301196088248357ULL)
      << "canonical pixel hash baseline 8x8 gray";
}

TEST(JpegLoad, GoldenBaselineYcbcr) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_16x16_ycbcr.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  uint64_t hash = jpeg_test::raster_pixel_hash(raster);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(hash, 3293244748321644837ULL)
      << "canonical pixel hash baseline 16x16 YCbCr";
}

TEST(JpegLoad, GoldenProgressive) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("progressive_sample.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  GIMG_Result dr = gimg_item_decode(item, nullptr, &raster);
  if (dr != GIMG_OK) {
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
    GTEST_SKIP() << "progressive decode not supported for this file (Pillow "
                    "progressive format may differ)";
  }
  ASSERT_NE(raster, nullptr);
  uint64_t hash = jpeg_test::raster_pixel_hash(raster);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(hash, 3293244748321644837ULL)
      << "canonical pixel hash progressive (same content as 16x16 gray)";
}

TEST(JpegLoad, GoldenExifOrientation) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("jpeg_exif_orientation.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Meta_Common * meta = gimg_doc_meta_common(doc);
  if (meta) {
    EXPECT_EQ(gimg_meta_common_orientation(meta), GIMG_ORIENTATION_ROTATE_90_CW)
        << "EXIF Orientation 6 = 90 CW (if generate.py was run with piexif)";
  }
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  uint64_t hash = jpeg_test::raster_pixel_hash(raster);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(hash, 9569108661638188517ULL)
      << "canonical pixel hash jpeg_exif_orientation";
}

TEST(JpegLoad, GoldenWithIcc) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("jpeg_with_icc.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  uint64_t hash = jpeg_test::raster_pixel_hash(raster);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(hash, 774238021366803749ULL)
      << "canonical pixel hash jpeg_with_icc";
}

TEST(JpegLoad, GoldenCmyk) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("cmyk_sample.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  uint64_t hash = jpeg_test::raster_pixel_hash(raster);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  EXPECT_NE(fmt, nullptr);
  if (fmt) {
    EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK);
    EXPECT_EQ(fmt->channel_count, 4u);
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(hash, 2706856015390867493ULL)
      << "canonical pixel hash cmyk_sample (8x8 black)";
}
#endif

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
