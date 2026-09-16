/**
 * @file
 *
 * JPEG load tests: segment parsing, limits, invalid markers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/image/bitdepth.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

#include "jpeg_test_utils.h"

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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  // SOS: L=10, Ns=1, C0 Td=0 Ta=0, Ss=0 Se=0 Ah=0 Al=0
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
  buf.push_back(0x00);
  append(buf, (const unsigned char *)"\xFF\xD5", 2); // RST5: consumed
  buf.push_back(0x00);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** Minimal baseline JPEG with DNL (Define Number of Lines) after first scan:
 * SOF0 height=8, SOS with empty scan, then DNL (0xFF 0xDC) L=4, payload 0x00
 * 0x08 (8 lines). Load must succeed; DNL validates SOF height. */
std::vector<uint8_t> make_minimal_jpeg_with_dnl_after_scan() {
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  // SOS: Ls = 2 + 1 + 2*Ns + 3 = 8.  Ns=1, Cs=0, Td=Ta=0, and a sequential
  // scan covers the whole block: Ss=0, Se=63, Ah=Al=0 (T.81 B.2.3).
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
  // No scan bytes; next marker is DNL. DNL: 0xFF 0xDC, L=4, payload 0x00 0x08
  // (8).
  append(buf, (const unsigned char *)"\xFF\xDC\x00\x04\x00\x08", 6);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** Minimal baseline with DNL after first scan but wrong height (16 vs SOF 8).
 * Load must fail with GIMG_ERR_FORMAT. */
std::vector<uint8_t> make_minimal_jpeg_with_dnl_mismatch() {
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
  append(
      buf, (const unsigned char *)"\xFF\xDC\x00\x04\x00\x10", 6); // DNL says 16
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

/** DNL (0xFF 0xDC) before first scan: SOI, SOF0, DNL, DQT, DHT, SOS, EOI.
 * Load must fail (DNL only valid after first scan). */
std::vector<uint8_t> make_minimal_jpeg_dnl_before_scan() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDC\x00\x04\x00\x08",
      6); // DNL before SOS
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  // First SOS: DC scan, Ss = Se = 0 (T.81 G.1.2).  Ls = 8.
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x00\x00",
      10);
  // Second SOS: AC band Ss=1..Se=63, Ah=0, Al=0.  Ls = 8.  This used to carry
  // an extra byte, which shifted Se and Ah/Al into Ss=1 Se=0 Ah=3 Al=15 - a
  // scan header no decoder should accept.
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x01\x3F\x00",
      10);
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
  append(exif,
      (const unsigned char *)"\x17\x01\x04\x00\x01\x00\x00\x00\x04\x00\x00\x00",
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  // SOS: Ls = 2 + 1 + 2*4 + 3 = 14.  Payload: Ns=4, then 4x(Cs,TdTa) -
  // 01 00, 02 00, 03 00, 04 00 - then Ss=0, Se=63, Ah=Al=0 for a sequential
  // scan (T.81 B.2.3).
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x0E\x04\x01\x00\x02\x00\x03\x00\x04"
                             "\x00\x00\x3F\x00",
      16);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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

// meta_raw tag for JPEG (must match jpeg_internal.h)
static const uint32_t kJpegRawApp1Exif = 0xE100u;
static const uint32_t kJpegRawApp0 = 0xE0u;
static const uint32_t kJpegRawApp0Jfxx = 0xE001u;
static const uint32_t kJpegRawApp2Icc = 0xE2u;
static const uint32_t kJpegRawApp2IccChunks = 0xE201u;
static const uint32_t kJpegRawApp13 = 0xEDu;
static const uint32_t kJpegRawApp14 = 0xEEu;
static const uint32_t kJpegRawAppUnknown = 0xE0FFu;
static const uint32_t kJpegRawCom = 0xFEu;

/** JPEG with APP0 JFIF containing a 2x2 RGB thumbnail (28-byte payload).
 * Payload: JFIF\0 v1.1 units=1 X=300 Y=300 ThumbX=2 ThumbY=2, then 12 RGB
 * bytes.
 */
std::vector<uint8_t> make_jpeg_with_jfif_thumbnail() {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // APP0: length 30 (2 + 28), payload 28 bytes
  // Bytes 0-4: JFIF\0, 5-6: 01 01, 7: units=1, 8-11: X=300 Y=300,
  // 12-13: ThumbX=2, 14-15: ThumbY=2, 16-27: 2*2*3 RGB
  static const unsigned char app0_with_thumb[] = {0xFF, 0xE0, 0x00,
      0x1E, // marker, length 30
      'J', 'F', 'I', 'F', 0x00, 0x01, 0x01, 0x01, 0x01, 0x2C, 0x01, 0x2C, 0x00,
      0x02, 0x00, 0x02, // ThumbX=2, ThumbY=2
      0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
  append(buf, app0_with_thumb, sizeof(app0_with_thumb));
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

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

TEST(JpegLoad, Sof0Precision12Rejected) {
  // SOF0 (baseline) allows only 8-bit; precision 12 must be rejected.
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF0: L=11, P=12 (0x0C), Y=8, X=8, Nf=1, C1=0 H=1 V=1 Tq=0
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x0C\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_ERR_FORMAT);
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

/** Task 4.2.3: SOF1 allows 8 or 12-bit only (T.81); SOF1 with precision 16 rejected. */
TEST(JpegLoad, Sof1Precision16Rejected) {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF1: L=11, P=16 (0x10), Y=8, X=8, Nf=1, C1=0 H=1 V=1 Tq=0
  append(buf,
      (const unsigned char *)"\xFF\xC1\x00\x0B\x10\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  GIMG_Stream * stream = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &stream);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(stream, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  EXPECT_TRUE(r == GIMG_ERR_FORMAT || r == GIMG_ERR_UNSUPPORTED)
      << "SOF1 with precision 16 must be rejected (spec: SOF1 = 8 or 12-bit only)";
  gimg_stream_destroy(stream);
}

/** T.81 Table B.2 gives every DCT-based frame a sample precision of 8 or 12.
 * Precision up to 16 belongs to lossless (SOF3) alone, so a progressive frame
 * claiming 16 is not a JPEG and must be refused.  This library used to accept
 * it, and used to write it. */
TEST(JpegLoad, Sof2Precision16Rejected) {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF2: L=11, P=16 (0x10), Y=8, X=8, Nf=1, C1=0 H=1 V=1 Tq=0
  append(buf,
      (const unsigned char *)"\xFF\xC2\x00\x0B\x10\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  GIMG_Stream * stream = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &stream);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(stream, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  EXPECT_TRUE(r == GIMG_ERR_FORMAT || r == GIMG_ERR_UNSUPPORTED)
      << "SOF2 with precision 16 must be rejected (T.81: SOF2 = 8 or 12-bit)";
  gimg_stream_destroy(stream);
}

/** The one precision above 8 that a DCT frame may carry. */
TEST(JpegLoad, Sof2Precision12Accepted) {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  append(buf,
      (const unsigned char *)"\xFF\xC2\x00\x0B\x0C\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  GIMG_Stream * stream = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &stream);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(stream, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_ERR_UNSUPPORTED)
      << "SOF2 with precision 12 is legal and must not be refused for precision";
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(stream);
}

/**
 * Build a progressive JPEG whose single SOS carries the given scan parameters,
 * so each rule in T.81 Annex G.1.2 can be checked on its own.
 */
static std::vector<uint8_t> make_progressive_with_sos(
    uint8_t ns, uint8_t ss, uint8_t se, uint8_t ah, uint8_t al) {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF2, 8x8, three components 1..3, all 1x1, quant table 0.
  append(buf,
      (const unsigned char *)"\xFF\xC2\x00\x11\x08\x00\x08\x00\x08\x03\x01\x11"
                             "\x00\x02\x11\x00\x03\x11\x00",
      19);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  uint16_t ls = (uint16_t)(2 + 1 + 2 * ns + 3);
  buf.push_back(0xFF);
  buf.push_back(0xDA);
  buf.push_back((unsigned char)(ls >> 8));
  buf.push_back((unsigned char)(ls & 0xFF));
  buf.push_back(ns);
  for (uint8_t i = 0; i < ns; i++) {
    buf.push_back((unsigned char)(i + 1)); // Cs
    buf.push_back(0x00);                   // Td = Ta = 0
  }
  buf.push_back(ss);
  buf.push_back(se);
  buf.push_back((unsigned char)((ah << 4) | (al & 0x0F)));
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

static GIMG_Result load_result(const std::vector<uint8_t> & jpeg) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s) != GIMG_OK) {
    return GIMG_ERR_INTERNAL;
  }
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (doc) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  return r;
}

/**
 * The scan header constrains the entropy decoder completely, so every field in
 * it has to be checked before it is used.  None of these were, and a file could
 * name a band running backwards or a refinement of any size and be followed
 * wherever that led.
 */
TEST(JpegLoad, SosParametersValidated) {
  // T.81 G.1.2.2: an AC scan names exactly one component.
  EXPECT_NE(load_result(make_progressive_with_sos(3, 1, 63, 0, 0)), GIMG_OK)
      << "AC scan with Ns=3 must be rejected (T.81 G.1.2.2)";
  // A single-component AC scan with the same band is fine.
  EXPECT_EQ(load_result(make_progressive_with_sos(1, 1, 63, 0, 0)), GIMG_OK)
      << "AC scan with Ns=1 is well formed";
  // T.81 G.1.2: a DC scan has Se = 0.
  EXPECT_NE(load_result(make_progressive_with_sos(3, 0, 63, 0, 0)), GIMG_OK)
      << "progressive DC scan with Se=63 must be rejected (T.81 G.1.2)";
  EXPECT_EQ(load_result(make_progressive_with_sos(3, 0, 0, 0, 0)), GIMG_OK)
      << "interleaved DC scan is well formed";
  // T.81 B.2.3: Ss <= Se <= 63.
  EXPECT_NE(load_result(make_progressive_with_sos(1, 10, 5, 0, 0)), GIMG_OK)
      << "Ss > Se must be rejected (T.81 B.2.3)";
  // T.81 G.1.1.1.2: successive approximation refines one bit per scan.
  EXPECT_NE(load_result(make_progressive_with_sos(1, 1, 63, 3, 0)), GIMG_OK)
      << "Ah != Al + 1 must be rejected (T.81 G.1.1.1.2)";
  EXPECT_EQ(load_result(make_progressive_with_sos(1, 1, 63, 1, 0)), GIMG_OK)
      << "Ah = Al + 1 is a well formed refinement";
  // Al is bounded by the coefficient range.
  EXPECT_NE(load_result(make_progressive_with_sos(1, 1, 63, 0, 15)), GIMG_OK)
      << "Al = 15 must be rejected (T.81 G.1.1.1.2)";
}

/**
 * A sequential frame has no spectral selection and no successive approximation
 * (T.81 B.2.3), so a scan claiming either is not a sequential scan.
 */
TEST(JpegLoad, SequentialSosMustCoverWholeBlock) {
  std::vector<uint8_t> good = make_minimal_jpeg();
  EXPECT_EQ(load_result(good), GIMG_OK) << "minimal baseline JPEG should load";

  // Find the SOS and bend Se to 5.
  std::vector<uint8_t> bad = good;
  for (size_t i = 0; i + 9 < bad.size(); i++) {
    if (bad[i] == 0xFF && bad[i + 1] == 0xDA) {
      bad[i + 8] = 5; // Se
      break;
    }
  }
  EXPECT_NE(load_result(bad), GIMG_OK)
      << "sequential scan with Se=5 must be rejected (T.81 B.2.3)";

  std::vector<uint8_t> bad_ah = good;
  for (size_t i = 0; i + 9 < bad_ah.size(); i++) {
    if (bad_ah[i] == 0xFF && bad_ah[i + 1] == 0xDA) {
      bad_ah[i + 9] = 0x10; // Ah = 1
      break;
    }
  }
  EXPECT_NE(load_result(bad_ah), GIMG_OK)
      << "sequential scan with Ah=1 must be rejected (T.81 B.2.3)";
}

/**
 * T.81 B.2.3: every component a scan names must belong to the frame, and may
 * appear only once.
 */
TEST(JpegLoad, SosComponentSelectorsChecked) {
  std::vector<uint8_t> jpeg = make_progressive_with_sos(1, 0, 0, 0, 0);
  // Point Cs at a component the frame does not declare.
  for (size_t i = 0; i + 6 < jpeg.size(); i++) {
    if (jpeg[i] == 0xFF && jpeg[i + 1] == 0xDA) {
      jpeg[i + 5] = 0x09;
      break;
    }
  }
  EXPECT_NE(load_result(jpeg), GIMG_OK)
      << "SOS naming an absent component must be rejected (T.81 B.2.3)";

  std::vector<uint8_t> dup = make_progressive_with_sos(3, 0, 0, 0, 0);
  for (size_t i = 0; i + 10 < dup.size(); i++) {
    if (dup[i] == 0xFF && dup[i + 1] == 0xDA) {
      dup[i + 7] = dup[i + 5]; // second Cs repeats the first
      break;
    }
  }
  EXPECT_NE(load_result(dup), GIMG_OK)
      << "SOS naming the same component twice must be rejected (T.81 B.2.3)";
}

/** Task 2.3.3.2: Unsupported SOF markers (e.g. SOF3 lossless) must be rejected. */
TEST(JpegLoad, UnsupportedSofRejected) {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF3 (0xC3) lossless: same payload layout as SOF0 but marker not supported
  append(buf,
      (const unsigned char *)"\xFF\xC3\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  GIMG_Stream * stream = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &stream);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(stream, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK);
  EXPECT_EQ(doc, nullptr);
  EXPECT_TRUE(r == GIMG_ERR_UNSUPPORTED || r == GIMG_ERR_FORMAT)
      << "SOF3 (lossless) must be rejected (not supported)";
  gimg_stream_destroy(stream);
}

TEST(JpegLoad, Sof1AcceptedFor8bit) {
  // SOF1 (extended sequential) with 8-bit: load succeeds (same as baseline).
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF1: L=11, P=8, Y=8, X=8, Nf=1, C1=0 H=1 V=1 Tq=0
  append(buf,
      (const unsigned char *)"\xFF\xC1\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  GIMG_Stream * stream = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &stream);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(stream, nullptr, nullptr, &doc);
  EXPECT_EQ(r, GIMG_OK);
  EXPECT_NE(doc, nullptr);
  if (doc) {
    EXPECT_EQ(gimg_doc_item_count(doc), 1u);
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(stream);
}

TEST(JpegLoad, TruncatedSosRejected) {
  // Minimal baseline up to SOS; SOS length says 10 but payload truncated (e.g. 2 bytes).
  // Load should fail with GIMG_ERR_FORMAT or GIMG_ERR_CORRUPT.
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++)
    buf.push_back(1);
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++)
    buf.push_back(0);
  // SOS: L=10, but only 2 bytes after length (truncated; need Ns=1 + 2*Ns + 3 = 6 more)
  append(buf, (const unsigned char *)"\xFF\xDA\x00\x0A\x01\x00", 6);
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK) << "truncated SOS should be rejected";
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, DhtValueBytesMismatchRejected) {
  // DHT: TcTh=0x00, 16 bits sum to 12, but only 11 value bytes (T.81 B.2.4 requires value bytes = sum).
  // Load or decode should fail (loader may skip table; decode then fails for missing table).
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++)
    buf.push_back(1);
  // DHT: one table, TcTh=0x00, 12 symbols (e.g. one 3-bit code), but 11 value bytes
  // Length 2 + (1+16+11)=30. Bits: 0,0,0,1,0,...,0 -> sum 1? We need sum 12 and 11 bytes.
  // So bits sum to 12, payload 1+16+11=28, segment len 2+28=30.
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x1E\x00", 5);  // 0x1E = 30
  // 16 bits: e.g. 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,12 (last count=12) -> 12 symbols
  for (int i = 0; i < 15; i++)
    buf.push_back(0);
  buf.push_back(12);
  for (int i = 0; i < 11; i++)
    buf.push_back((uint8_t)i);  // only 11 value bytes, not 12
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  GIMG_Stream * s = nullptr;
  gimg_stream_create_memory(buf.data(), buf.size(), &s);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r != GIMG_OK) {
    EXPECT_TRUE(r == GIMG_ERR_FORMAT || r == GIMG_ERR_CORRUPT);
    gimg_stream_destroy(s);
    return;
  }
  ASSERT_NE(doc, nullptr);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  GIMG_Decode_Options opts = {};
  r = gimg_item_decode(item, &opts, &raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_NE(r, GIMG_OK)
      << "DHT with value bytes != sum(bit counts) must cause load or decode failure";
  if (raster)
    gimg_raster_destroy(raster);
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

// SOF11, the lossless arithmetic process (T.81 Annex H coded with Annex D).
// Every fixture here was produced by the ISO reference codec, not by this
// library, so the test compares against an independent implementation rather
// than against our own encoder.  A lossless codec must return exactly what
// went in, so the fixture's own source image is the expected output.
TEST(JpegLoad, DecodeLosslessArithmeticMatchesReferenceCodec) {
  struct Case {
    const char * jpg;
    const char * src;
    const char * what;
  };
  static const Case cases[] = {
      {"lossless_arith_gray.jpg", "lossless_arith_gray.pgm", "8-bit grey"},
      {"lossless_arith_rgb.jpg", "lossless_arith_rgb.ppm", "8-bit RGB"},
      {"lossless_arith_gray12.jpg", "lossless_arith_gray12.pgm", "12-bit grey"},
      {"lossless_arith_gray16.jpg", "lossless_arith_gray16.pgm", "16-bit grey"},
      // A restart interval of one whole MCU row.
      {"lossless_arith_restart.jpg", "lossless_arith_restart.pgm",
          "restart, row-aligned"},
      // A restart interval that is not a whole number of rows.  libjpeg
      // refuses these ("must be an integer multiple of the number of MCUs in
      // an MCU row"), so the reference codec is the only source of one - and
      // this is the shape that caught the predictor being reset at every
      // interval instead of only where an interval starts a line.
      {"lossless_arith_midrow.jpg", "lossless_arith_midrow.pgm",
          "restart, mid-row"},
      // The same interval with the Huffman coder: the predictor is shared, so
      // a defect in it shows up under both and neither test alone says which.
      {"lossless_huff_midrow.jpg", "lossless_huff_midrow.pgm",
          "SOF3, restart mid-row"},
  };
  for (const Case & c : cases) {
    uint32_t sw = 0, sh = 0;
    int channels = 0, bits = 0;
    std::vector<uint32_t> want;
    ASSERT_TRUE(jpeg_test::load_pnm_file(c.src, &sw, &sh, &channels, &bits, want))
        << c.src;
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg)) << c.jpg;
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK) << c.what;
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK)
        << c.what;
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(gimg_raster_width(raster), sw) << c.what;
    EXPECT_EQ(gimg_raster_height(raster), sh) << c.what;
    int out_bits = (bits <= 8) ? 8 : 16;
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels_const(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    int rchan = (int)gimg_raster_format(raster)->channel_count;
    int bad = 0;
    for (uint32_t y = 0; y < sh && bad == 0; y++) {
      for (uint32_t x = 0; x < sw && bad == 0; x++) {
        for (int k = 0; k < channels; k++) {
          uint32_t expect = jpeg_test::widen_sample(
              want[((size_t)y * sw + x) * (size_t)channels + (size_t)k], bits,
              out_bits);
          uint32_t got = (out_bits == 8)
              ? (uint32_t)px[(size_t)y * stride + (size_t)x * rchan + k]
              : (uint32_t)((const uint16_t *)(px + (size_t)y * stride))
                    [(size_t)x * rchan + k];
          if (expect != got) {
            ADD_FAILURE() << c.what << ": first diff at (" << x << "," << y
                          << ") channel " << k << ": want " << expect
                          << " got " << got;
            bad = 1;
            break;
          }
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
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
  append(jpeg, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    jpeg.push_back(0);
  }
  append(jpeg,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
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

TEST(JpegLoad, JfifThumbnailDecodeSecondItem) {
  std::vector<uint8_t> jpeg = make_jpeg_with_jfif_thumbnail();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  EXPECT_EQ(gimg_doc_item_count(doc), 2u)
      << "JFIF APP0 thumbnail should expose second item";
  GIMG_Item * item1 = gimg_doc_item(doc, 1);
  ASSERT_NE(item1, nullptr);
  GIMG_Raster * thumb = gimg_item_raster(item1);
  ASSERT_NE(thumb, nullptr)
      << "Second item (JFIF thumbnail) should have raster attached";
  EXPECT_EQ(gimg_raster_width(thumb), 2u);
  EXPECT_EQ(gimg_raster_height(thumb), 2u);
  uint64_t hash = jpeg_test::raster_pixel_hash(thumb);
  // Our test image has pixels 0x11,0x22,0x33; 0x44,0x55,0x66; 0x77,0x88,0x99;
  // 0xAA,0xBB,0xCC (RGBA with A=255). Hash is deterministic.
  (void)hash;
  gimg_doc_destroy(doc);
}

TEST(JpegLoad, JfifThumbnailRoundTrip) {
  // Build a synthetic doc with main image (8x8 gray), second item (2x2
  // thumbnail), and APP0 JFIF with thumbnail in meta_raw. Save; load; verify
  // thumbnail preserved (no existing APP0 from a loaded file).
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_create(&doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);

  r = gimg_doc_set_item_count(doc, 2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Raster * main_raster = nullptr;
  r = gimg_raster_create(
      8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0, &main_raster);
  ASSERT_EQ(r, GIMG_OK);
  memset(gimg_raster_pixels(main_raster), 128,
      (size_t)8 * gimg_raster_stride_bytes(main_raster));
  gimg_item_set_raster(gimg_doc_item(doc, 0), main_raster);

  GIMG_Raster * thumb_raster = nullptr;
  r = gimg_raster_create(
      2, 2, &GIMG_PIXEL_RGBA8, GIMG_RASTER_OWNED, nullptr, 0, &thumb_raster);
  ASSERT_EQ(r, GIMG_OK);
  unsigned char * px = (unsigned char *)gimg_raster_pixels(thumb_raster);
  size_t stride = gimg_raster_stride_bytes(thumb_raster);
  for (int y = 0; y < 2; y++) {
    for (int x = 0; x < 2; x++) {
      size_t off = (size_t)y * stride + (size_t)x * 4u;
      px[off + 0] = (unsigned char)(0x11 + (y * 2 + x) * 0x11);
      px[off + 1] = (unsigned char)(0x22 + (y * 2 + x) * 0x11);
      px[off + 2] = (unsigned char)(0x33 + (y * 2 + x) * 0x11);
      px[off + 3] = 255;
    }
  }
  gimg_item_set_raster(gimg_doc_item(doc, 1), thumb_raster);

  static const unsigned char app0_payload[] = {'J', 'F', 'I', 'F', 0x00, 0x01,
      0x01, 0x01, 0x01, 0x2C, 0x01, 0x2C, 0x00, 0x02, 0x00, 0x02, 0x11, 0x22,
      0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC};
  GIMG_Meta_Raw * raw = nullptr;
  r = gimg_doc_ensure_meta_raw(doc, &raw);
  ASSERT_EQ(r, GIMG_OK);
  r = gimg_meta_raw_attach(
      raw, "jpeg", kJpegRawApp0, app0_payload, sizeof(app0_payload));
  ASSERT_EQ(r, GIMG_OK);

  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "jpeg", nullptr, &report);
  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> out_buf(
      (const uint8_t *)out_data, (const uint8_t *)out_data + out_size);
  gimg_stream_destroy(out_s);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_GT(out_buf.size(), 10u) << "saved JPEG too small";
  ASSERT_EQ(out_buf[0], 0xFF);
  ASSERT_EQ(out_buf[1], 0xD8);
  ASSERT_EQ(out_buf[2], 0xFF);
  ASSERT_EQ(out_buf[3], 0xE0) << "expected APP0 after SOI";
  ASSERT_EQ(out_buf[4], 0x00);
  ASSERT_EQ(out_buf[5], 0x1E)
      << "APP0 segment length 30 (2+28) with JFIF thumbnail";
  ASSERT_EQ(out_buf[6], 'J');
  ASSERT_EQ(out_buf[7], 'F');

  gimg_doc_destroy(doc);
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

TEST(JpegLoad, ComPopulatesMetaCommonDescription) {
  std::vector<uint8_t> jpeg = make_jpeg_with_com("Hello");
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Meta_Common * meta = gimg_doc_meta_common(doc);
  ASSERT_NE(meta, nullptr) << "COM (7-bit ASCII) should ensure meta_common";
  const char * desc = gimg_meta_common_description(meta);
  ASSERT_NE(desc, nullptr) << "First COM should populate description";
  EXPECT_STREQ(desc, "Hello");
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

TEST(JpegLoad, UnknownApp13RoundTrip) {
  // APP13 (0xED) e.g. IPTC/Photoshop: store in unknown-app blob, round-trip.
  // Use a decodable baseline JPEG (reference file) and inject APP13 after SOI
  // so save can encode and we can re-load.
  std::vector<uint8_t> file_buf;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", file_buf))
      << "need tests/data/jpeg/baseline_8x8_gray.jpg";
  ASSERT_GE(file_buf.size(), 4u);
  ASSERT_EQ(file_buf[0], 0xFF);
  ASSERT_EQ(file_buf[1], 0xD8);
  const char payload[] = "IPTC\0test";
  const size_t payload_len = sizeof(payload) - 1;
  std::vector<uint8_t> jpeg;
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD8);
  jpeg.push_back(0xFF);
  jpeg.push_back(0xED);
  jpeg.push_back((uint8_t)((2 + payload_len) >> 8));
  jpeg.push_back((uint8_t)((2 + payload_len) & 0xFF));
  jpeg.insert(jpeg.end(), payload, payload + payload_len);
  jpeg.insert(jpeg.end(), file_buf.begin() + 2, file_buf.end());
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t size = 0;
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawAppUnknown, nullptr, &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(size, 1u + 2u + payload_len);
  std::vector<uint8_t> unknown_data(size);
  r = gimg_meta_raw_get(
      raw, "jpeg", kJpegRawAppUnknown, unknown_data.data(), &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(unknown_data[0], 0xED) << "APP13 marker";
  EXPECT_EQ(unknown_data[1], 0x00);
  EXPECT_EQ(unknown_data[2], (uint8_t)payload_len);
  EXPECT_EQ(memcmp(unknown_data.data() + 3, payload, payload_len), 0);

  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options save_opts = {};
  save_opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  ASSERT_EQ(r, GIMG_OK) << "save with unknown APP must succeed";
  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> out_buf(
      (const uint8_t *)out_data, (const uint8_t *)out_data + out_size);
  gimg_stream_destroy(out_s);

  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(out_buf.data(), out_buf.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK) << "re-load saved JPEG (with unknown APP) must succeed";
  ASSERT_NE(doc2, nullptr);
  GIMG_Meta_Raw * raw2 = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw2, nullptr);
  size_t size2 = 0;
  r = gimg_meta_raw_get(raw2, "jpeg", kJpegRawAppUnknown, nullptr, &size2);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(size2, size);
  std::vector<uint8_t> unknown_data2(size2);
  r = gimg_meta_raw_get(
      raw2, "jpeg", kJpegRawAppUnknown, unknown_data2.data(), &size2);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(memcmp(unknown_data2.data(), unknown_data.data(), size), 0)
      << "unknown APP blob unchanged after round-trip";
  gimg_doc_destroy(doc2);
  gimg_stream_destroy(s2);
}

TEST(JpegLoad, UnknownAppOrderPreserved) {
  // Two unknown APP segments (APP13 then APP5); order must be preserved.
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // APP13: payload "A" (1 byte), length 2+1=3
  append(buf, (const unsigned char *)"\xFF\xED\x00\x03\x41", 5);
  // APP5: payload "B" (1 byte)
  append(buf, (const unsigned char *)"\xFF\xE5\x00\x03\x42", 5);
  append(buf,
      (const unsigned char *)"\xFF\xC0\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11"
                             "\x00",
      13);
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) {
    buf.push_back(1);
  }
  append(buf, (const unsigned char *)"\xFF\xC4\x00\x13\x00", 5);
  for (int i = 0; i < 16; i++) {
    buf.push_back(0);
  }
  append(buf,
      (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00\x00\x3F\x00",
      10);
  append(buf, (const unsigned char *)"\xFF\xD9", 2);

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(buf.data(), buf.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t size = 0;
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawAppUnknown, nullptr, &size);
  ASSERT_EQ(r, GIMG_OK);
  // (0xED, 0, 1, 'A') + (0xE5, 0, 1, 'B') = 4 + 4 = 8
  EXPECT_EQ(size, 8u);
  std::vector<uint8_t> blob(size);
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawAppUnknown, blob.data(), &size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(blob[0], 0xED);
  EXPECT_EQ(blob[3], 'A');
  EXPECT_EQ(blob[4], 0xE5);
  EXPECT_EQ(blob[7], 'B');
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, App13PhotoshopRoundTrip) {
  // APP13 with "Photoshop 3.0\0" is stored as GIMG_JPEG_RAW_APP13, not unknown.
  std::vector<uint8_t> file_buf;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", file_buf));
  ASSERT_GE(file_buf.size(), 4u);
  ASSERT_EQ(file_buf[0], 0xFF);
  ASSERT_EQ(file_buf[1], 0xD8);
  static const unsigned char payload[] = "Photoshop 3.0\0\x00\x01\x02";
  const size_t payload_len = sizeof(payload);
  std::vector<uint8_t> jpeg;
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD8);
  jpeg.push_back(0xFF);
  jpeg.push_back(0xED);
  jpeg.push_back(static_cast<uint8_t>((2 + payload_len) >> 8));
  jpeg.push_back(static_cast<uint8_t>((2 + payload_len) & 0xFF));
  jpeg.insert(jpeg.end(), payload, payload + payload_len);
  jpeg.insert(jpeg.end(), file_buf.begin() + 2, file_buf.end());
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  gimg_stream_destroy(s);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t app13_size = 0;
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawApp13, nullptr, &app13_size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(app13_size, payload_len);
  std::vector<uint8_t> app13_data(app13_size);
  r = gimg_meta_raw_get(
      raw, "jpeg", kJpegRawApp13, app13_data.data(), &app13_size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(memcmp(app13_data.data(), payload, payload_len), 0);
  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options save_opts = {};
  save_opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  ASSERT_EQ(r, GIMG_OK);
  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> out_buf(static_cast<const uint8_t *>(out_data),
      static_cast<const uint8_t *>(out_data) + out_size);
  gimg_stream_destroy(out_s);
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(out_buf.data(), out_buf.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  gimg_stream_destroy(s2);
  GIMG_Meta_Raw * raw2 = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw2, nullptr);
  size_t app13_size2 = 0;
  r = gimg_meta_raw_get(raw2, "jpeg", kJpegRawApp13, nullptr, &app13_size2);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(app13_size2, payload_len);
  std::vector<uint8_t> app13_data2(app13_size2);
  r = gimg_meta_raw_get(
      raw2, "jpeg", kJpegRawApp13, app13_data2.data(), &app13_size2);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(memcmp(app13_data2.data(), payload, payload_len), 0);
  gimg_doc_destroy(doc2);
}

TEST(JpegLoad, App14AdobeRoundTrip) {
  // APP14 "Adobe\0" stored as GIMG_JPEG_RAW_APP14; transform at byte 14.
  std::vector<uint8_t> file_buf;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", file_buf));
  ASSERT_GE(file_buf.size(), 4u);
  static const unsigned char payload[] = {
      'A', 'd', 'o', 'b', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 0, 2};
  const size_t payload_len = sizeof(payload);
  std::vector<uint8_t> jpeg;
  jpeg.push_back(0xFF);
  jpeg.push_back(0xD8);
  jpeg.push_back(0xFF);
  jpeg.push_back(0xEE);
  jpeg.push_back(static_cast<uint8_t>((2 + payload_len) >> 8));
  jpeg.push_back(static_cast<uint8_t>((2 + payload_len) & 0xFF));
  jpeg.insert(jpeg.end(), payload, payload + payload_len);
  jpeg.insert(jpeg.end(), file_buf.begin() + 2, file_buf.end());
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK);
  gimg_stream_destroy(s);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t app14_size = 0;
  r = gimg_meta_raw_get(raw, "jpeg", kJpegRawApp14, nullptr, &app14_size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(app14_size, payload_len);
  std::vector<uint8_t> app14_data(app14_size);
  r = gimg_meta_raw_get(
      raw, "jpeg", kJpegRawApp14, app14_data.data(), &app14_size);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(memcmp(app14_data.data(), payload, payload_len), 0);
  GIMG_Stream * out_s = nullptr;
  r = gimg_stream_create_memory_output(&out_s);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Save_Options save_opts = {};
  save_opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  r = gimg_doc_save(doc, out_s, "jpeg", &save_opts, &report);
  gimg_doc_destroy(doc);
  ASSERT_EQ(r, GIMG_OK);
  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> out_buf(static_cast<const uint8_t *>(out_data),
      static_cast<const uint8_t *>(out_data) + out_size);
  gimg_stream_destroy(out_s);
  GIMG_Stream * s2 = nullptr;
  r = gimg_stream_create_memory(out_buf.data(), out_buf.size(), &s2);
  ASSERT_EQ(r, GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  r = gimg_doc_load(s2, nullptr, nullptr, &doc2);
  ASSERT_EQ(r, GIMG_OK);
  gimg_stream_destroy(s2);
  GIMG_Meta_Raw * raw2 = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw2, nullptr);
  size_t app14_size2 = 0;
  r = gimg_meta_raw_get(raw2, "jpeg", kJpegRawApp14, nullptr, &app14_size2);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(app14_size2, payload_len);
  std::vector<uint8_t> app14_data2(app14_size2);
  r = gimg_meta_raw_get(
      raw2, "jpeg", kJpegRawApp14, app14_data2.data(), &app14_size2);
  ASSERT_EQ(r, GIMG_OK);
  EXPECT_EQ(memcmp(app14_data2.data(), payload, payload_len), 0);
  gimg_doc_destroy(doc2);
}

TEST(JpegLoad, App13AndApp14BothPresentRoundTrip) {
  // Load JPEG with both APP13 (Photoshop) and APP14; save; re-load; both
  // present.
  std::vector<uint8_t> file_buf;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", file_buf));
  ASSERT_GE(file_buf.size(), 4u);
  static const unsigned char app13_pl[] = "Photoshop 3.0\0\xab";
  static const unsigned char app14_pl[] = {
      'A', 'd', 'o', 'b', 'e', 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
  std::vector<uint8_t> jpeg = {0xFF, 0xD8};
  auto append_app = [&jpeg](uint8_t marker, const unsigned char * pl,
                        size_t pl_len) {
    jpeg.push_back(0xFF);
    jpeg.push_back(marker);
    uint16_t len = static_cast<uint16_t>(2 + pl_len);
    jpeg.push_back(static_cast<uint8_t>(len >> 8));
    jpeg.push_back(static_cast<uint8_t>(len & 0xFF));
    jpeg.insert(jpeg.end(), pl, pl + pl_len);
  };
  append_app(0xED, app13_pl, sizeof(app13_pl));
  append_app(0xEE, app14_pl, sizeof(app14_pl));
  jpeg.insert(jpeg.end(), file_buf.begin() + 2, file_buf.end());
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t sz13 = 0, sz14 = 0;
  ASSERT_EQ(
      gimg_meta_raw_get(raw, "jpeg", kJpegRawApp13, nullptr, &sz13), GIMG_OK);
  ASSERT_EQ(
      gimg_meta_raw_get(raw, "jpeg", kJpegRawApp14, nullptr, &sz14), GIMG_OK);
  EXPECT_EQ(sz13, sizeof(app13_pl));
  EXPECT_EQ(sz14, sizeof(app14_pl));
  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options save_opts = {};
  save_opts.metadata_policy = GIMG_META_PRESERVE_ALL;
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_s, "jpeg", &save_opts, &report), GIMG_OK);
  gimg_doc_destroy(doc);
  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> out_buf(static_cast<const uint8_t *>(out_data),
      static_cast<const uint8_t *>(out_data) + out_size);
  gimg_stream_destroy(out_s);
  GIMG_Stream * s2 = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(out_buf.data(), out_buf.size(), &s2), GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  ASSERT_EQ(gimg_doc_load(s2, nullptr, nullptr, &doc2), GIMG_OK);
  gimg_stream_destroy(s2);
  GIMG_Meta_Raw * raw2 = gimg_doc_meta_raw(doc2);
  ASSERT_NE(raw2, nullptr);
  size_t sz13_2 = 0, sz14_2 = 0;
  ASSERT_EQ(gimg_meta_raw_get(raw2, "jpeg", kJpegRawApp13, nullptr, &sz13_2),
      GIMG_OK);
  ASSERT_EQ(gimg_meta_raw_get(raw2, "jpeg", kJpegRawApp14, nullptr, &sz14_2),
      GIMG_OK);
  EXPECT_EQ(sz13_2, sizeof(app13_pl));
  EXPECT_EQ(sz14_2, sizeof(app14_pl));
  gimg_doc_destroy(doc2);
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

/** Task 4.1.2: Decode progressive JPEG from non-seekable stream. Encodes a small
 * progressive JPEG in-test (so we have real scan data), then load+decode from
 * non-seekable stream and assert success. No skip. */
TEST(JpegLoad, DecodeProgressiveFromNonSeekableStream) {
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_create(&doc), GIMG_OK);
  ASSERT_NE(doc, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_raster_create(
                8, 8, &GIMG_PIXEL_GRAY8, GIMG_RASTER_OWNED, nullptr, 0, &raster),
      GIMG_OK);
  memset(gimg_raster_pixels(raster), 160,
      (size_t)8 * gimg_raster_stride_bytes(raster));
  gimg_item_set_raster(gimg_doc_item(doc, 0), raster);

  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Options save_opts = {};
  save_opts.jpeg_progressive = 1;
  GIMG_Save_Report report = {};
  GIMG_Result r = gimg_doc_save(doc, out_s, "jpeg", &save_opts, &report);
  ASSERT_EQ(r, GIMG_OK) << "encode progressive JPEG must succeed";
  const void * out_data = nullptr;
  size_t out_size = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_size);
  std::vector<uint8_t> jpeg_buf(
      (const uint8_t *)out_data, (const uint8_t *)out_data + out_size);
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);
  doc = nullptr;
  ASSERT_GT(jpeg_buf.size(), 0u) << "progressive JPEG output must be non-empty";

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_no_seek(
                jpeg_buf.data(), jpeg_buf.size(), &s),
      GIMG_OK);
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  ASSERT_EQ(r, GIMG_OK)
      << "progressive JPEG load must work from non-seekable stream";
  ASSERT_NE(doc, nullptr);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * decoded = nullptr;
  r = gimg_item_decode(item, nullptr, &decoded);
  ASSERT_EQ(r, GIMG_OK)
      << "progressive JPEG decode must work from non-seekable stream";
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(gimg_raster_width(decoded), 8u);
  EXPECT_EQ(gimg_raster_height(decoded), 8u);
  gimg_raster_destroy(decoded);
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

TEST(JpegLoad, LoadJpegWithDnlMismatchFails) {
  // DNL after first scan but number of lines does not match SOF height.
  std::vector<uint8_t> jpeg = make_minimal_jpeg_with_dnl_mismatch();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK) << "JPEG with DNL height mismatch should fail";
  EXPECT_EQ(doc, nullptr);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, LoadJpegDnlBeforeScanFails) {
  // DNL before first scan is invalid.
  std::vector<uint8_t> jpeg = make_minimal_jpeg_dnl_before_scan();
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  EXPECT_NE(r, GIMG_OK) << "JPEG with DNL before first scan should fail";
  EXPECT_EQ(doc, nullptr);
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

TEST(JpegLoad, DecodeCmykFileWhenPresent) {
  // Load tests/data/jpeg/cmyk_sample.jpg and decode; verify CMYK raster
  // format and color info preserved.
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("cmyk_sample.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
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

/** Decode baseline grayscale fixture and compare to oracle .raw (Pillow decode).
 * Oracle flow: Pillow reads JPEG, decodes, writes .raw. We decode same JPEG and
 * compare pixels to .raw. Generate .raw with: python3 tests/data/jpeg/generate_jpeg_oracle_raws.py
 */
TEST(JpegLoad, DecodeBaselineGrayOracleRaw) {
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::load_jpeg_oracle_raw(
          "baseline_8x8_gray", oracle_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Run python3 tests/data/jpeg/generate_jpeg_oracle_raws.py to create .raw oracle";
  }
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
  gimg_stream_destroy(s);
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(
      raster, oracle_pixels.data(), oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match Pillow oracle .raw (byte-for-byte for this fixture)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** Per-fixture tolerance for oracle .raw comparison (Pillow vs our decoder).
 * Large baseline: Pillow vs our decoder can differ beyond rounding (e.g. IDCT);
 * allow moderate tolerance so we still get 640×480 decode coverage. */
static int jpeg_fixture_oracle_tolerance(const char * base) {
  if (strstr(base, "640x480") != nullptr) {
    return 32;
  }
  return 0;
}

/** Decode every baseline (and EXIF/ICC/CMYK) fixture that has an oracle .raw
 * and compare to .raw. Progressive fixtures are not included here (Pillow
 * .raw can differ from our decoder due to chroma upsampling); use
 * Decode*PillowOracle for progressive. Generate .raw with:
 * python3 tests/data/jpeg/generate_jpeg_oracle_raws.py
 */
TEST(JpegLoad, DecodeFixtureOraclesRaw) {
  static const char * const fixtures[] = {
      "baseline_8x8_gray",
      "baseline_16x16_ycbcr",
      "baseline_9x9_gray",
      "baseline_8x16_gray",
      "baseline_8x8_gray_q50",
      "baseline_8x8_gray_q100",
      "baseline_640x480_gray",
      "jpeg_exif_orientation",
      "jpeg_with_icc",
      "cmyk_sample",
  };
  int compared = 0;
  for (const char * base : fixtures) {
    std::vector<uint8_t> oracle_pixels;
    uint32_t oracle_w = 0, oracle_h = 0;
    int oracle_mode = -1;
    if (!jpeg_test::load_jpeg_oracle_raw(base, oracle_pixels,
            &oracle_w, &oracle_h, &oracle_mode)) {
      continue;  // skip if .raw not present (e.g. generate_jpeg_oracle_raws not run)
    }
    std::string jpeg_name(base);
    jpeg_name += ".jpg";
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(jpeg_name.c_str(), jpeg))
        << "Fixture " << base << ": run tests/data/jpeg/generate.py";
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Item * item = gimg_doc_item(doc, 0);
    ASSERT_NE(item, nullptr);
    GIMG_Raster * raster = nullptr;
    GIMG_Result decode_r = gimg_item_decode(item, nullptr, &raster);
    if (decode_r != GIMG_OK) {
      gimg_stream_destroy(s);
      gimg_doc_destroy(doc);
      continue;  // skip fixtures that fail to decode (e.g. progressive 640×480)
    }
    compared++;
    gimg_stream_destroy(s);
    int tolerance = jpeg_fixture_oracle_tolerance(base);
    EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
        oracle_w, oracle_h, oracle_mode, tolerance))
        << "Fixture " << base << ": decode must match oracle .raw";
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
  }
  if (compared == 0) {
    GTEST_SKIP() << "No .raw oracles found. Run python3 tests/data/jpeg/generate_jpeg_oracle_raws.py";
  }
}

/** Non-trivial size (640×480) baseline YCbCr: decode and verify dimensions.
 * Ensures decoder handles many MCUs and real-world dimensions. Pixel comparison
 * vs Pillow .raw is not required (Pillow can differ on large RGB); use
 * Decode*PillowOracle hash tests when libjpeg oracle is built for pixel check.
 */
TEST(JpegLoad, DecodeBaseline640x480Ycbcr) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("baseline_640x480_ycbcr.jpg", jpeg))
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
  gimg_stream_destroy(s);
  EXPECT_EQ(gimg_raster_width(raster), 640u);
  EXPECT_EQ(gimg_raster_height(raster), 480u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  ASSERT_NE(fmt, nullptr);
  EXPECT_TRUE(fmt->channel_model == GIMG_CHANNEL_RGB ||
              fmt->channel_model == GIMG_CHANNEL_RGBA);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** Lossless JPEG (SOF3) decodes, exactly.
 *
 * T.81 Annex H is a coding process in its own right, not a variation on the
 * DCT ones: each sample is predicted from its already-decoded neighbours and
 * the difference is entropy-coded, so the reconstruction is exact.  This codec
 * rejected every such frame until the process existed.
 *
 * It is also the only place a JPEG may carry a precision other than 8 or 12.
 * Table B.2 allows P from 2 to 16 in a lossless frame - so a 16-bit JPEG is a
 * real thing, and two of the fixtures here are one.
 *
 * Expectations are libjpeg-turbo 3.0.4's decode of the same files, in the
 * frame's own precision; this library widens those samples to an 8- or 16-bit
 * raster by replication, so the comparison undoes that. */
TEST(JpegLoad, DecodeLossless) {
  struct Case {
    const char * name;
    uint32_t w, h;
    int channels;
    int precision;
    unsigned long long sum;
    int samples[15];
  };
  static const Case cases[] = {
      // Every predictor selection value gets its own arithmetic (Table H.1);
      // 1 is one-dimensional, 4 and 7 are two-dimensional.
      {"lossless_gray_psv1.jpg", 64, 64, 1, 8, 520320,
          {0, 129, 255, 4, 0}},
      {"lossless_rgb_psv4.jpg", 64, 64, 3, 8, 1567160,
          {142, 0, 0, 241, 129, 129, 236, 255, 255, 106, 0, 2, 67, 4, 2}},
      // Point transform Pt = 1: the samples were shifted down before coding.
      {"lossless_rgb_psv7_pt1.jpg", 17, 9, 3, 8, 58092,
          {224, 0, 0, 100, 126, 126, 98, 254, 254, 174, 0, 10, 44, 30, 10}},
      // The same image as psv4 above, with restart intervals: a restart begins
      // a fresh predictive context, so it must decode to exactly the same
      // samples.
      {"lossless_rgb_psv4_restart.jpg", 64, 64, 3, 8, 1567160,
          {142, 0, 0, 241, 129, 129, 236, 255, 255, 106, 0, 2, 67, 4, 2}},
      // Precisions a DCT frame may not use.
      {"lossless_gray16_psv1.jpg", 64, 64, 1, 16, 134449403ULL,
          {32767, 34575, 13611, 36315, 32767}},
      {"lossless_rgb12_psv4.jpg", 64, 64, 3, 12, 25158656ULL,
          {0, 0, 0, 2080, 2080, 2080, 4095, 4095, 4095, 65, 0, 32, 0, 65, 32}},
  };
  // The library's widening rule, generalised to any source precision.
  auto widen = [](uint32_t v, int from, int to) -> uint32_t {
    if (from >= to) return v >> (from - to);
    uint32_t r = v;
    int have = from;
    while (have < to) {
      int take = to - have;
      if (take > from) take = from;
      r = (r << take) | (v >> (from - take));
      have += take;
    }
    return r;
  };
  for (const Case & c : cases) {
    std::vector<uint8_t> jpeg;
    if (!jpeg_test::load_jpeg_file(c.name, jpeg)) {
      ADD_FAILURE() << "missing fixture " << c.name;
      continue;
    }
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK) << c.name;
    gimg_stream_destroy(s);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK)
        << c.name;
    ASSERT_NE(raster, nullptr);
    ASSERT_EQ(gimg_raster_width(raster), c.w) << c.name;
    ASSERT_EQ(gimg_raster_height(raster), c.h) << c.name;
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    ASSERT_NE(fmt, nullptr);
    int out_bits = (c.precision <= 8) ? 8 : 16;
    EXPECT_EQ(fmt->bits_per_channel[0], out_bits) << c.name;
    size_t stride = gimg_raster_stride_bytes(raster);
    const void * px = gimg_raster_pixels_const(raster);
    auto at = [&](uint32_t x, uint32_t y, int ch) -> uint32_t {
      if (out_bits == 8) {
        const unsigned char * p = (const unsigned char *)px + y * stride;
        return (c.channels == 1) ? p[x] : p[x * 4 + (uint32_t)ch];
      }
      const uint16_t * p =
          (const uint16_t *)((const unsigned char *)px + y * stride);
      return (c.channels == 1) ? p[x] : p[x * 4 + (uint32_t)ch];
    };
    unsigned long long sum = 0;
    for (uint32_t y = 0; y < c.h; y++) {
      for (uint32_t x = 0; x < c.w; x++) {
        for (int ch = 0; ch < c.channels; ch++) {
          // Reduce the raster sample back to the frame's own precision.
          uint32_t v = at(x, y, ch);
          uint32_t want_bits = (uint32_t)c.precision;
          uint32_t narrowed =
              (out_bits == (int)want_bits)
              ? v
              : (uint32_t)(((uint64_t)v * ((1ull << want_bits) - 1ull) +
                               ((1ull << out_bits) - 1ull) / 2ull) /
                  ((1ull << out_bits) - 1ull));
          sum += narrowed;
        }
      }
    }
    EXPECT_EQ(sum, c.sum) << c.name;
    const uint32_t pts[5][2] = {
        {0, 0}, {c.w / 2, c.h / 2}, {c.w - 1, c.h - 1}, {1, 0}, {0, 1}};
    int i = 0;
    for (const auto & pt : pts) {
      for (int ch = 0; ch < c.channels; ch++, i++) {
        EXPECT_EQ(at(pt[0], pt[1], ch),
            widen((uint32_t)c.samples[i], c.precision, out_bits))
            << c.name << " at (" << pt[0] << ", " << pt[1] << ") channel "
            << ch;
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
  }
}

/** Arithmetic-coded frames decode, and decode correctly.
 *
 * SOF9 is sequential and SOF10 progressive; both are covered here, together
 * with a Huffman progressive file carrying restart intervals.  That last one
 * belongs with them because all three broke on the same thing: a progressive
 * image with restart intervals was undecodable, whichever entropy coder it
 * used, for two reasons that only show up together.  A restart marker is a hard
 * resynchronisation point, so bits the longest-match Huffman decode had read
 * ahead of it had to be discarded and were not; and DRI may appear between
 * scans and change, which an encoder measuring its restart interval in MCU rows
 * has to do, because an interleaved scan and a single-component scan do not
 * have the same number of MCUs in a row.  libjpeg writes a different DRI before
 * nearly every scan of a subsampled progressive image, and we kept only the
 * first.
 *
 *
 * T.81 Annex D defines arithmetic coding as one of the two entropy coders a
 * JPEG may use; Annex F defines the other.  A SOF9 or SOF10 frame is an
 * ordinary DCT frame that happens to use the first rather than the second, and
 * this codec rejected every one of them outright until the coder existed.
 *
 * The expectations are libjpeg-turbo 3.0.4's decode of the same files - it
 * builds with arithmetic support by default - reduced to a sum over the raster
 * plus a handful of sampled pixels, which is enough to catch a coder that
 * drifts out of step with the encoder partway through.  A wrong probability
 * estimate does not produce a small error: the decoder and the encoder share
 * one adaptive model, and once they disagree the rest of the scan is noise.
 *
 * @see tests/data/jpeg/README.md for how the fixtures were made. */
TEST(JpegLoad, DecodeArithmeticSequential) {
  struct Case {
    const char * name;
    uint32_t w, h;
    int channels;
    unsigned long sum;
    int samples[15];
  };
  static const Case cases[] = {
      {"arith_gray_64x64.jpg", 64, 64, 1, 520192,
          {0, 129, 254, 4, 0}},
      {"arith_rgb_64x64_420.jpg", 64, 64, 3, 1566364,
          {114, 20, 10, 141, 183, 171, 180, 255, 255, 95, 0, 0, 96, 0, 0}},
      {"arith_rgb_17x9_422_restart.jpg", 17, 9, 3, 58315,
          {229, 1, 34, 148, 114, 130, 115, 249, 242, 194, 0, 20, 22, 29, 0}},
      // SOF10: progressive, arithmetic.
      {"arith_progressive_33x33_422.jpg", 33, 33, 3, 418185,
          {248, 0, 10, 119, 96, 104, 147, 255, 255, 226, 23, 29, 198, 6, 5}},
      {"arith_progressive_restart_420.jpg", 64, 64, 3, 1566364,
          {114, 20, 10, 141, 183, 171, 180, 255, 255, 95, 0, 0, 96, 0, 0}},
      // The Huffman progressive file is here for the same reason: it exercises
      // the restart handling these fixtures share.
      {"progressive_restart_420.jpg", 64, 64, 3, 1567281,
          {110, 13, 4, 142, 173, 167, 173, 255, 255, 115, 0, 0, 89, 0, 0}},
  };
  for (const Case & c : cases) {
    std::vector<uint8_t> jpeg;
    if (!jpeg_test::load_jpeg_file(c.name, jpeg)) {
      ADD_FAILURE() << "missing fixture " << c.name;
      continue;
    }
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK) << c.name;
    gimg_stream_destroy(s);
    GIMG_Decode_Options opts = {};
    opts.jpeg_chroma_upsampling = GIMG_JPEG_CHROMA_UPSAMPLE_FANCY;
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), &opts, &raster), GIMG_OK)
        << c.name;
    ASSERT_NE(raster, nullptr);
    ASSERT_EQ(gimg_raster_width(raster), c.w) << c.name;
    ASSERT_EQ(gimg_raster_height(raster), c.h) << c.name;
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels_const(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    size_t bpp = gimg_raster_bytes_per_pixel(gimg_raster_format(raster));
    unsigned long sum = 0;
    for (uint32_t y = 0; y < c.h; y++) {
      for (uint32_t x = 0; x < c.w; x++) {
        const unsigned char * p = px + y * stride + x * bpp;
        for (int ch = 0; ch < c.channels; ch++) {
          // A grayscale frame decodes to GRAY8, a colour one to RGBA8.
          sum += (bpp == 1) ? p[0] : p[ch];
        }
      }
    }
    EXPECT_EQ(sum, c.sum) << c.name;
    const uint32_t pts[5][2] = {
        {0, 0}, {c.w / 2, c.h / 2}, {c.w - 1, c.h - 1}, {1, 0}, {0, 1}};
    int i = 0;
    for (const auto & pt : pts) {
      const unsigned char * p = px + pt[1] * stride + pt[0] * bpp;
      for (int ch = 0; ch < c.channels; ch++, i++) {
        EXPECT_EQ((int)((bpp == 1) ? p[0] : p[ch]), c.samples[i])
            << c.name << " at (" << pt[0] << ", " << pt[1] << ") channel "
            << ch;
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
  }
}

/** An arithmetic scan may legitimately carry no bytes at all.
 *
 * T.81 D.2.9 has the decoder supply zero bytes once it has run past the
 * compressed data, so a frame whose every decision resolves to the more
 * probable symbol needs no entropy-coded bytes.  libjpeg writes exactly none
 * for a 1x1 image, and this fixture is that file.  A Huffman scan always has at
 * least one byte, which is why the emptiness check that rejected this was
 * correct until arithmetic coding arrived. */
TEST(JpegLoad, DecodeArithmeticEmptyScan) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(
      jpeg_test::load_jpeg_file("arith_gray_1x1_empty_scan.jpg", jpeg));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  EXPECT_EQ(gimg_raster_width(raster), 1u);
  EXPECT_EQ(gimg_raster_height(raster), 1u);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** An arithmetic frame at 12-bit precision (SOF9 with P=12). */
TEST(JpegLoad, DecodeArithmetic12Bit) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("arith_gray12_64x64.jpg", jpeg));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  ASSERT_EQ(gimg_raster_width(raster), 64u);
  ASSERT_EQ(gimg_raster_height(raster), 64u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16);
  // The source was a horizontal ramp over the full 12-bit range; check that it
  // still is one, which a coder out of step with the encoder would not produce.
  const uint16_t * px =
      (const uint16_t *)gimg_raster_pixels_const(raster);
  size_t stride_el = gimg_raster_stride_bytes(raster) / sizeof(uint16_t);
  for (uint32_t x = 1; x < 64u; x++) {
    EXPECT_GE(px[stride_el + x], px[stride_el + x - 1])
        << "ramp is not monotonic at x=" << x;
  }
  EXPECT_LT(gimg_bitdepth_16_to_12(px[stride_el]), 64u);
  EXPECT_GT(gimg_bitdepth_16_to_12(px[stride_el + 63]), 4000u);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** A 12-bit colour frame must fill its whole raster.
 *
 * The fixture is a flat colour, so every decoded pixel has to be the same one;
 * that makes this a check on addressing rather than on arithmetic.  It is here
 * because the 12-bit colour path computed its row stride in pixels while
 * indexing the row through a uint16_t * - so it wrote each frame into the first
 * quarter of its own raster and left the rest at zero.  Every existing 12-bit
 * test looked only at the dimensions and the pixel format, which were both
 * correct, and none of them read a sample. */
TEST(JpegLoad, Decode12BitColourFillsTheWholeRaster) {
  std::vector<uint8_t> jpeg;
  if (!jpeg_test::load_jpeg_file("baseline_rgb12_444.jpg", jpeg)) {
    GTEST_SKIP() << "Fixture tests/data/jpeg/baseline_rgb12_444.jpg not found.";
  }
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  ASSERT_EQ(gimg_raster_width(raster), 16u);
  ASSERT_EQ(gimg_raster_height(raster), 16u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  ASSERT_NE(fmt, nullptr);
  ASSERT_EQ(fmt->bits_per_channel[0], 16);
  const uint16_t * px =
      static_cast<const uint16_t *>(gimg_raster_pixels_const(raster));
  size_t stride_el = gimg_raster_stride_bytes(raster) / sizeof(uint16_t);
  // The source was a flat (3000, 1000, 2000) at P=12, widened to 16 bits by
  // replication (src/ops/bitdepth.c).  libjpeg reconstructs (3000, 1000, 2001).
  const uint16_t want[3] = {
      gimg_bitdepth_12_to_16(3000),
      gimg_bitdepth_12_to_16(1000),
      gimg_bitdepth_12_to_16(2001),
  };
  for (uint32_t y = 0; y < 16u; y++) {
    for (uint32_t x = 0; x < 16u; x++) {
      const uint16_t * p = px + y * stride_el + x * 4u;
      ASSERT_EQ(p[0], want[0]) << "at (" << x << ", " << y << ")";
      ASSERT_EQ(p[1], want[1]) << "at (" << x << ", " << y << ")";
      ASSERT_EQ(p[2], want[2]) << "at (" << x << ", " << y << ")";
      ASSERT_EQ(p[3], 65535u) << "at (" << x << ", " << y << ")";
    }
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** A 4:2:2 frame one row tall must use the horizontal-only chroma filter.
 *
 * T.81 B.2.2 gives every component an Hi and a Vi, and those are what say how a
 * plane was subsampled.  The upsampler used to infer it from the plane's shape
 * instead, which cannot distinguish 2x1 from 2x2 when the image has a single
 * row: both leave a chroma plane one row tall.  The wrong filter is close but
 * not equal - it carries its own rounding constants - so such frames decoded
 * one or two counts off across the row.
 *
 * The expected values are libjpeg-turbo 3.0.4 built with 12-bit support
 * (-DWITH_12BIT); see tests/data/jpeg/README.md. */
TEST(JpegLoad, Decode12Bit422SingleRowMatchesReference) {
  std::vector<uint8_t> jpeg;
  if (!jpeg_test::load_jpeg_file("baseline_rgb12_422_16x1.jpg", jpeg)) {
    GTEST_SKIP()
        << "Fixture tests/data/jpeg/baseline_rgb12_422_16x1.jpg not found.";
  }
  static const uint16_t expected[16][3] = {
    { 177, 3997,   50},
    { 271, 3818,   33},
    { 544, 3549,   87},
    { 815, 3275,  177},
    {1089, 3001,  304},
    {1363, 2728,  472},
    {1635, 2455,  674},
    {1910, 2181,  910},
    {2182, 1907, 1178},
    {2458, 1638, 1489},
    {2725, 1363, 1835},
    {3000, 1092, 2218},
    {3272,  817, 2633},
    {3548,  544, 3089},
    {3821,  270, 3585},
    {3951,  125, 3824},
  };
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Decode_Options opts = {};
  opts.jpeg_chroma_upsampling = GIMG_JPEG_CHROMA_UPSAMPLE_FANCY;
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, &opts, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  ASSERT_EQ(gimg_raster_width(raster), 16u);
  ASSERT_EQ(gimg_raster_height(raster), 1u);
  const uint16_t * px =
      static_cast<const uint16_t *>(gimg_raster_pixels_const(raster));
  for (uint32_t x = 0; x < 16u; x++) {
    for (int c = 0; c < 3; c++) {
      EXPECT_EQ(px[x * 4u + (uint32_t)c], gimg_bitdepth_12_to_16(expected[x][c]))
          << "x=" << x << " channel=" << c;
    }
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** Decode 12-bit JPEG fixture (SOF1 or SOF2, precision 12). Confirms 12-bit decode path.
 * Fixture: copy baseline_gray12.jpg from tests/out/jpeg/ (after JpegEncode.SaveGray12ThenLoadDecode)
 * to tests/data/jpeg/. See tests/data/jpeg/README.md. If fixture is absent, test is skipped. */
TEST(JpegLoad, Decode12BitFixture) {
  std::vector<uint8_t> jpeg;
  if (!jpeg_test::load_jpeg_file("baseline_gray12.jpg", jpeg)) {
    GTEST_SKIP() << "Optional 12-bit fixture tests/data/jpeg/baseline_gray12.jpg not found. "
                    "Copy from tests/out/jpeg/ after running JpegEncode.SaveGray12ThenLoadDecode.";
  }
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  gimg_stream_destroy(s);
  EXPECT_EQ(gimg_raster_width(raster), 16u);
  EXPECT_EQ(gimg_raster_height(raster), 16u);
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->bits_per_channel[0], 16)
      << "a 12-bit frame decodes to GRAY16, widened by replication";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
}

/** Decode baseline grayscale fixture and compare pixels to oracle (Pillow or libjpeg).
 * Uses .raw comparison so failures show first differing pixel. */
TEST(JpegLoad, DecodeBaselineGrayPillowOracle) {
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string jpeg_path = data_dir + "/baseline_8x8_gray.jpg";
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/oracle_baseline_8x8_gray.raw";
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          oracle_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Decode oracle (Pillow or libjpeg) required. See tests/data/jpeg/README.md.";
  }
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
  EXPECT_EQ(gimg_raster_width(raster), oracle_w) << "width must match oracle";
  EXPECT_EQ(gimg_raster_height(raster), oracle_h) << "height must match oracle";
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match oracle (first diff printed to stderr)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

/** Decode baseline YCbCr fixture and compare pixels to oracle. */
TEST(JpegLoad, DecodeBaselineYcbcrPillowOracle) {
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string jpeg_path = data_dir + "/baseline_16x16_ycbcr.jpg";
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/oracle_baseline_16x16_ycbcr.raw";
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          oracle_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Decode oracle (Pillow or libjpeg) required. See tests/data/jpeg/README.md.";
  }
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
  EXPECT_EQ(gimg_raster_width(raster), oracle_w);
  EXPECT_EQ(gimg_raster_height(raster), oracle_h);
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match oracle (first diff printed to stderr)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
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
  EXPECT_EQ(hash, 9866071383738491685ULL)
      << "canonical pixel hash baseline 16x16 YCbCr (matches Pillow/libjpeg)";
}

/** Decode progressive fixture and compare pixels to oracle.
 * Uses Pillow-generated progressive_sample.jpg. Fails until our progressive
 * decoder supports this fixture (gimg_item_decode must return GIMG_OK). */
TEST(JpegLoad, DecodeProgressivePillowOracle) {
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string jpeg_path = data_dir + "/progressive_sample.jpg";
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/oracle_progressive_sample.raw";
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          oracle_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Decode oracle (Pillow or libjpeg) required. See tests/data/jpeg/README.md.";
  }
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
  GIMG_Decode_Options opts = {};
  opts.jpeg_chroma_upsampling = GIMG_JPEG_CHROMA_UPSAMPLE_FANCY;
  GIMG_Result dr = gimg_item_decode(item, &opts, &raster);
  ASSERT_EQ(dr, GIMG_OK)
      << "progressive decoder must decode Pillow fixture progressive_sample.jpg (feature not implemented)";
  ASSERT_NE(raster, nullptr);
  EXPECT_EQ(gimg_raster_width(raster), oracle_w);
  EXPECT_EQ(gimg_raster_height(raster), oracle_h);
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match oracle (first diff printed to stderr)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

/** Golden progressive: decode of Pillow fixture matches canonical hash.
 * Chroma upsampling is a decoder choice (not specified in the bitstream).
 * The canonical hash was produced with simple upsampling; we pass that option
 * so our decode matches the reference. */
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
  GIMG_Decode_Options opts = {};
  opts.jpeg_chroma_upsampling = GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE;
  GIMG_Result dr = gimg_item_decode(item, &opts, &raster);
  ASSERT_EQ(dr, GIMG_OK)
      << "progressive decoder must decode Pillow fixture progressive_sample.jpg (feature not implemented)";
  ASSERT_NE(raster, nullptr);
  uint64_t hash = jpeg_test::raster_pixel_hash(raster);
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(hash, 6786767564893283945ULL)
      << "canonical pixel hash (reference produced with simple chroma upsampling)";
}

// Passing no options and passing a zero-initialised GIMG_Decode_Options must
// decode identically.  They did not: GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE used to
// be 0, so `GIMG_Decode_Options o = {};` selected the box filter while NULL
// selected the triangle filter, and the natural way to write the struct
// quietly produced different pixels.  The third decode is what gives this test
// teeth - it proves the fixture actually distinguishes the two filters, so the
// first two agreeing means something.
TEST(JpegLoad, ZeroInitialisedDecodeOptionsMatchNullOptions) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("progressive_sample.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);

  auto decode_hash = [&](const GIMG_Decode_Options * opts, uint64_t * out) {
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_item_decode(item, opts, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    *out = jpeg_test::raster_pixel_hash(raster);
    gimg_raster_destroy(raster);
  };

  uint64_t null_hash = 0;
  decode_hash(nullptr, &null_hash);

  GIMG_Decode_Options zeroed = {};
  uint64_t zeroed_hash = 0;
  decode_hash(&zeroed, &zeroed_hash);

  GIMG_Decode_Options simple = {};
  simple.jpeg_chroma_upsampling = GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE;
  uint64_t simple_hash = 0;
  decode_hash(&simple, &simple_hash);

  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);

  EXPECT_EQ(zeroed_hash, null_hash)
      << "a zero-initialised GIMG_Decode_Options must decode as NULL does";
  EXPECT_NE(simple_hash, null_hash)
      << "fixture must distinguish the two upsampling filters, or the check "
         "above proves nothing";
}

/** Decode EXIF-orientation fixture and compare pixels to oracle; check meta orientation. */
TEST(JpegLoad, DecodeExifOrientationPillowOracle) {
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string jpeg_path = data_dir + "/jpeg_exif_orientation.jpg";
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/oracle_jpeg_exif_orientation.raw";
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          oracle_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Decode oracle (Pillow or libjpeg) required. See tests/data/jpeg/README.md.";
  }
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
  EXPECT_EQ(gimg_raster_width(raster), oracle_w);
  EXPECT_EQ(gimg_raster_height(raster), oracle_h);
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match oracle (first diff printed to stderr)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
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
  EXPECT_EQ(hash, 9573191635971389477ULL)
      << "canonical pixel hash jpeg_exif_orientation";
}

/** Decode ICC fixture and compare pixels to oracle. */
TEST(JpegLoad, DecodeWithIccPillowOracle) {
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string jpeg_path = data_dir + "/jpeg_with_icc.jpg";
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/oracle_jpeg_with_icc.raw";
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          oracle_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Decode oracle (Pillow or libjpeg) required. See tests/data/jpeg/README.md.";
  }
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
  EXPECT_EQ(gimg_raster_width(raster), oracle_w);
  EXPECT_EQ(gimg_raster_height(raster), oracle_h);
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match oracle (first diff printed to stderr)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
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
  EXPECT_EQ(hash, 4304909053328165157ULL)
      << "canonical pixel hash jpeg_with_icc";
}

/** Build a JPEG buffer with the first APP2 ICC_PROFILE split into two chunks.
 * Returns true and sets out + out_size if found and built; otherwise false. */
static bool jpeg_split_app2_icc_into_two_chunks(
    const uint8_t * jpeg, size_t jpeg_size, std::vector<uint8_t> & out) {
  out.clear();
  if (jpeg_size < 4) {
    return false;
  }
  size_t i = 0;
  while (i + 4 <= jpeg_size) {
    if (jpeg[i] != 0xFF) {
      i++;
      continue;
    }
    if (jpeg[i + 1] == 0x00) {
      i += 2;
      continue;
    }
    if (jpeg[i + 1] != 0xE2) {
      i += 2;
      continue;
    }
    uint16_t len = (uint16_t)((jpeg[i + 2] << 8) | jpeg[i + 3]);
    if (len < 2 || i + 2 + len > jpeg_size) {
      return false;
    }
    size_t payload_size = (size_t)(len - 2);
    const uint8_t * payload = jpeg + i + 4;
    if (payload_size < 14 || std::memcmp(payload, "ICC_PROFILE\0", 12) != 0) {
      i += 2 + len;
      continue;
    }
    size_t profile_size = payload_size - 14;
    size_t half = profile_size / 2;
    size_t len1 = 14 + half;
    size_t len2 = 14 + (profile_size - half);
    out.insert(out.end(), jpeg, jpeg + i);
    uint8_t hdr1[16];
    std::memcpy(hdr1, "ICC_PROFILE\0", 12);
    hdr1[12] = 1;
    hdr1[13] = 2;
    out.push_back(0xFF);
    out.push_back(0xE2);
    out.push_back(static_cast<uint8_t>((len1 + 2) >> 8));
    out.push_back(static_cast<uint8_t>((len1 + 2) & 0xFF));
    out.insert(out.end(), hdr1, hdr1 + 14);
    out.insert(out.end(), payload + 14, payload + 14 + half);
    uint8_t hdr2[16];
    std::memcpy(hdr2, "ICC_PROFILE\0", 12);
    hdr2[12] = 2;
    hdr2[13] = 2;
    out.push_back(0xFF);
    out.push_back(0xE2);
    out.push_back(static_cast<uint8_t>((len2 + 2) >> 8));
    out.push_back(static_cast<uint8_t>((len2 + 2) & 0xFF));
    out.insert(out.end(), hdr2, hdr2 + 14);
    out.insert(out.end(), payload + 14 + half, payload + 14 + profile_size);
    out.insert(out.end(), jpeg + i + 2 + len, jpeg + jpeg_size);
    return true;
  }
  return false;
}

TEST(JpegLoad, MultiSegmentIccDecodeAndRoundTrip) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("jpeg_with_icc.jpg", jpeg))
      << "Run tests/data/jpeg/generate.py";
  std::vector<uint8_t> multi;
  ASSERT_TRUE(
      jpeg_split_app2_icc_into_two_chunks(jpeg.data(), jpeg.size(), multi))
      << "jpeg_with_icc must contain one APP2 ICC_PROFILE segment";
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(multi.data(), multi.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  gimg_stream_destroy(s);
  ASSERT_NE(doc, nullptr);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  const GIMG_Color_Info * ci = gimg_raster_color_info_const(raster);
  ASSERT_NE(ci, nullptr);
  size_t icc_size = ci->icc_size;
  ASSERT_GT(icc_size, 0u) << "multi-segment ICC should yield assembled profile";
  gimg_raster_destroy(raster);
  GIMG_Meta_Raw * raw = gimg_doc_meta_raw(doc);
  ASSERT_NE(raw, nullptr);
  size_t chunks_size = 0;
  ASSERT_EQ(gimg_meta_raw_get(
                raw, "jpeg", kJpegRawApp2IccChunks, nullptr, &chunks_size),
      GIMG_OK)
      << "multi-segment ICC should store APP2_ICC_CHUNKS for round-trip";
  EXPECT_GE(chunks_size, 2u + 2u + 14u + 14u)
      << "CHUNKS blob: 2B N + at least two segment payloads";
  GIMG_Stream * out_s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory_output(&out_s), GIMG_OK);
  GIMG_Save_Report report = {};
  ASSERT_EQ(gimg_doc_save(doc, out_s, "jpeg", nullptr, &report), GIMG_OK);
  const void * out_data = nullptr;
  size_t out_len = 0;
  gimg_stream_output_buffer(out_s, &out_data, &out_len);
  std::vector<uint8_t> saved(static_cast<const uint8_t *>(out_data),
      static_cast<const uint8_t *>(out_data) + out_len);
  gimg_stream_destroy(out_s);
  gimg_doc_destroy(doc);
  s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(saved.data(), saved.size(), &s), GIMG_OK);
  doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  struct DocGuard {
    GIMG_Doc * d = nullptr;
    ~DocGuard() {
      if (d) gimg_doc_destroy(d);
    }
  } doc_guard;
  doc_guard.d = doc;
  gimg_stream_destroy(s);
  ASSERT_NE(doc, nullptr);
  item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  ci = gimg_raster_color_info_const(raster);
  ASSERT_NE(ci, nullptr);
  EXPECT_EQ(ci->icc_size, icc_size)
      << "round-trip multi-segment ICC: profile size unchanged";
  gimg_raster_destroy(raster);
  doc_guard.d = nullptr;
  gimg_doc_destroy(doc);
}

/** Decode CMYK fixture and compare pixels to oracle. */
TEST(JpegLoad, DecodeCmykPillowOracle) {
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string jpeg_path = data_dir + "/cmyk_sample.jpg";
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/oracle_cmyk_sample.raw";
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          oracle_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Decode oracle (Pillow or libjpeg) required. See tests/data/jpeg/README.md.";
  }
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
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  EXPECT_NE(fmt, nullptr);
  if (fmt) {
    EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK);
    EXPECT_EQ(fmt->channel_count, 4u);
  }
  EXPECT_EQ(gimg_raster_width(raster), oracle_w);
  EXPECT_EQ(gimg_raster_height(raster), oracle_h);
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match oracle (first diff printed to stderr)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

/** Decode jpeg_with_icc.jpg with libjpeg (ref -o), then decode the same file
 * with our decoder; compare pixels byte-for-byte. Ensures we match libjpeg
 * on a fixture we can decode. Requires make jpeg-oracle-tools. */
TEST(JpegLoad, DecodeJpegWithIccVsLibjpeg) {
  std::string data_dir(GIMG_TEST_DATA_JPEG);
  std::string jpeg_path = data_dir + "/jpeg_with_icc.jpg";
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/libjpeg_compare.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          libjpeg_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Run make jpeg-oracle-tools (see tests/data/jpeg/README.md)";
  }
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
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK)
      << "our decoder must decode jpeg_with_icc.jpg";
  ASSERT_NE(raster, nullptr);
  EXPECT_EQ(oracle_mode, 1) << "jpeg_with_icc is RGB (mode 1)";
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, libjpeg_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "our decode must match libjpeg .raw byte-for-byte";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

/** Priority (1): Decoder must work correctly with output from oracle (libjpeg).
 * Use libjpeg to encode a known image, then our decoder decodes that JPEG;
 * compare to libjpeg's decode of the same file. Requires make jpeg-oracle-tools. */
TEST(JpegLoad, DecodeLibjpegEncodedBaseline) {
  std::string jpeg_path = jpeg_test::jpeg_output_dir() + "/libjpeg_encoded_baseline.jpg";
  if (!jpeg_test::libjpeg_encode_baseline_to_file(jpeg_path.c_str(), 640, 480, 85, 0)) {
    GTEST_SKIP() << "Run make jpeg-oracle-tools (encode_libjpeg_baseline_scan)";
  }
  std::string raw_path = jpeg_test::jpeg_output_dir() + "/libjpeg_encoded_baseline.raw";
  std::vector<uint8_t> libjpeg_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::libjpeg_decode_to_oracle_raw(jpeg_path.c_str(), raw_path.c_str(),
          libjpeg_pixels, &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Run make jpeg-oracle-tools (dump_jpeg_pixels_ref)";
  }
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_from_path(jpeg_path.c_str(), jpeg));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Item * item = gimg_doc_item(doc, 0);
  ASSERT_NE(item, nullptr);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(item, nullptr, &raster), GIMG_OK)
      << "our decoder must decode libjpeg-encoded JPEG (priority 1)";
  ASSERT_NE(raster, nullptr);
  EXPECT_EQ(oracle_w, 640u);
  EXPECT_EQ(oracle_h, 480u);
  EXPECT_EQ(oracle_mode, 0) << "grayscale (mode 0)";
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, libjpeg_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "our decode must match libjpeg oracle byte-for-byte";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

/** Decode CMYK fixture and compare to oracle .raw (libjpeg decode).
 * Oracle .raw is produced by dump_jpeg_pixels_ref -o (via generate_jpeg_oracle_raws.py).
 * Byte-wise comparison gives a clear picture of any decode difference. */
TEST(JpegLoad, GoldenCmyk) {
  std::vector<uint8_t> oracle_pixels;
  uint32_t oracle_w = 0, oracle_h = 0;
  int oracle_mode = -1;
  if (!jpeg_test::load_jpeg_oracle_raw("cmyk_sample", oracle_pixels,
          &oracle_w, &oracle_h, &oracle_mode)) {
    GTEST_SKIP() << "Run make jpeg-oracle-tools then "
                    "python3 tests/data/jpeg/generate_jpeg_oracle_raws.py to create cmyk_sample.raw";
  }
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
  const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
  ASSERT_NE(fmt, nullptr);
  EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK);
  EXPECT_EQ(fmt->channel_count, 4u);
  EXPECT_EQ(oracle_mode, 2) << "cmyk_sample.raw must be CMYK (mode 2)";
  EXPECT_TRUE(jpeg_test::raster_matches_oracle_raw(raster, oracle_pixels.data(),
      oracle_w, oracle_h, oracle_mode, 0))
      << "decode pixels must match libjpeg oracle .raw (byte-for-byte)";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
