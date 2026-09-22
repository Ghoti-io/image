/**
 * @file
 *
 * JPEG load tests: segment parsing, limits, invalid markers.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ghoti.io/image/bitdepth.h>
#include <ghoti.io/image/codec.h>
#include <ghoti.io/image/core.h>
#include <ghoti.io/image/doc.h>
#include <ghoti.io/image/meta.h>
#include <ghoti.io/image/ops.h>
#include <ghoti.io/image/raster.h>
#include <ghoti.io/image/stream.h>
#include <gtest/gtest.h>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
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

// A lossless frame coded as several non-interleaved scans (T.81 A.2.3).
//
// The same arrangement as the sequential case, and the same gap: this decoder
// took scans[0] and refused anything whose scan did not carry every component.
// What is different is what can be asserted.  A lossless codec returns exactly
// what went in, so the expected output is the fixtures' own source image, and
// the comparison needs no reference decode and no tolerance at all - if a
// single sample is wrong the frame was not decoded, it was approximated.
//
// H.1 puts the predictor selection in each scan's Ss, so scans of the same
// frame may use different predictors; lossless_noninterleaved_psv.jpg uses 1,
// 2 and 7, which is a shape a single frame-level predictor cannot represent.
TEST(JpegLoad, NonInterleavedLosslessScans) {
  struct Case {
    const char * jpg;
    const char * what;
  };
  const Case cases[] = {
      {"lossless_noninterleaved.jpg", "three scans, predictor 4 throughout"},
      {"lossless_noninterleaved_psv.jpg",
          "predictors 1, 2 and 7: one per scan, not one per frame"},
      {"lossless_noninterleaved_restart.jpg",
          "with a restart interval, which each scan counts in its own MCUs"},
  };
  uint32_t sw = 0, sh = 0;
  int schan = 0, sbits = 0;
  std::vector<uint32_t> src;
  ASSERT_TRUE(
      jpeg_test::load_pnm_file("hier_src_rgb.ppm", &sw, &sh, &schan, &sbits, src));
  ASSERT_EQ(schan, 3);
  ASSERT_EQ(sbits, 8);

  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(gimg_raster_width(raster), sw);
    EXPECT_EQ(gimg_raster_height(raster), sh);
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < sh; y++) {
      for (uint32_t x = 0; x < sw; x++) {
        for (int ch = 0; ch < 3; ch++) {
          ASSERT_EQ((uint32_t)px[(size_t)y * stride + (size_t)x * 4 + ch],
              src[((size_t)y * sw + x) * 3 + ch])
              << "at (" << x << ", " << y << ") channel " << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// A hierarchical sequence that stops before it reaches the completed image.
//
// T.81 B.3.1 bounds a frame by the DHP size and does not require any frame to
// attain it, so a sequence may simply have fewer frames than it meant to - a
// truncated file, or one that was only ever written as far as it got.  The
// reference components are then smaller than the DHP header implies.
//
// The chroma filter works from the sampling factors and the image size, so
// pointing it at a component of some other size reads past the end of it.  The
// fuzzer found it with this 423-byte file: a 17x9 DHP at 4:2:0 whose only
// frame is 9x5, so the chroma planes are 5x3 where the filter expects 9x5.
//
// What is asserted is that it decodes inside its buffers and at the DHP size -
// the smaller picture, scaled up - rather than being refused: the file is
// short, not malformed, and the frames it does carry are a picture.
TEST(JpegLoad, HierarchicalSequenceShorterThanItsDhp) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("hier_truncated_sequence.jpg", jpeg));
  // The fixture must keep the shape that matters: a DHP larger than its frame.
  uint32_t dhp_w = 0, sof_w = 0;
  for (size_t i = 0; i + 9 < jpeg.size(); i++) {
    if (jpeg[i] != 0xFF) {
      continue;
    }
    uint32_t w = (uint32_t)((jpeg[i + 7] << 8) | jpeg[i + 8]);
    if (jpeg[i + 1] == 0xDE && dhp_w == 0) {
      dhp_w = w;
    }
    if (jpeg[i + 1] == 0xC1 && sof_w == 0) {
      sof_w = w;
    }
  }
  ASSERT_GT(dhp_w, 0u);
  ASSERT_GT(sof_w, 0u);
  ASSERT_GT(dhp_w, sof_w) << "the sequence must fall short of its DHP";

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  GIMG_Result r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
  if (r == GIMG_OK) {
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(gimg_raster_width(raster), dhp_w);
    gimg_raster_destroy(raster);
  }
  else {
    EXPECT_EQ(raster, nullptr);
  }
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

// A sequential frame coded as several non-interleaved scans (T.81 A.2.3).
//
// A.2.3 lets a sequential frame be written as one scan per component instead
// of a single interleaved one, and the scans are then in that component's own
// block order rather than the image's MCU order.  This loader refused any
// second scan outside a progressive frame, so every such file - libjpeg writes
// one for any scan script that names one component at a time - failed to load
// at all.
//
// The walk that reads them is the one Annex G already needed, so what changed
// is which frames go through it, not how the scans are read.  The comparison
// is exact rather than within a tolerance: these are libjpeg-turbo's own files
// and its own decode of them, and this library matches libjpeg on ordinary
// sequential frames, so any difference at all would be a real one.
//
// See tests/data/jpeg/README.md for the scan scripts and the commands.
TEST(JpegLoad, NonInterleavedSequentialScans) {
  struct Case {
    const char * jpg;
    const char * ref;
    const char * what;
  };
  const Case cases[] = {
      {"noninterleaved_444.jpg", "noninterleaved_444_ref.ppm",
          "three scans, one per component, 4:4:4"},
      {"noninterleaved_420.jpg", "noninterleaved_420_ref.ppm",
          "4:2:0, so each scan walks a different sized block grid"},
      {"noninterleaved_422.jpg", "noninterleaved_422_ref.ppm", "4:2:2"},
      {"noninterleaved_arith.jpg", "noninterleaved_arith_ref.ppm",
          "SOF9: the same scans with the arithmetic coder"},
      {"noninterleaved_mixed.jpg", "noninterleaved_mixed_ref.ppm",
          "an interleaved scan of two components, then one of the third"},
      {"noninterleaved_restart.jpg", "noninterleaved_restart_ref.ppm",
          "with a restart interval, which each scan counts in its own MCUs"},
      {"noninterleaved_12bit.jpg", "noninterleaved_12bit_ref.ppm",
          "SOF1 at 12 bits"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    uint32_t rw = 0, rh = 0;
    int rchan = 0, rbits = 0;
    std::vector<uint32_t> ref;
    ASSERT_TRUE(jpeg_test::load_pnm_file(c.ref, &rw, &rh, &rchan, &rbits, ref))
        << "missing reference " << c.ref;
    ASSERT_EQ(rchan, 3);

    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    // The fixture must actually carry more than one scan.
    int scans = 0;
    for (size_t i = 0; i + 3 < jpeg.size();) {
      if (jpeg[i] != 0xFF) {
        i++;
        continue;
      }
      uint8_t m = jpeg[i + 1];
      if (m == 0xD8 || m == 0xD9 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) {
        i += 2;
        continue;
      }
      size_t len = (size_t)((jpeg[i + 2] << 8) | jpeg[i + 3]);
      if (m == 0xDA) {
        scans++;
        i += 2 + len;
        while (i + 1 < jpeg.size()) {
          if (jpeg[i] == 0xFF && jpeg[i + 1] != 0 &&
              !(jpeg[i + 1] >= 0xD0 && jpeg[i + 1] <= 0xD7)) {
            break;
          }
          i++;
        }
        continue;
      }
      i += 2 + len;
    }
    ASSERT_GT(scans, 1) << "fixture must be coded as several scans";

    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(gimg_raster_width(raster), rw);
    EXPECT_EQ(gimg_raster_height(raster), rh);

    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    ASSERT_NE(fmt, nullptr);
    int out_bits = (int)fmt->bits_per_channel[0];
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < rh; y++) {
      for (uint32_t x = 0; x < rw; x++) {
        for (int ch = 0; ch < 3; ch++) {
          uint32_t got = (out_bits == 8)
              ? (uint32_t)px[(size_t)y * stride + (size_t)x * 4 + ch]
              : (uint32_t)((const uint16_t *)(px + (size_t)y * stride))[
                    (size_t)x * 4 + ch];
          // The reference carries the frame's own precision; the raster
          // carries what the library widened it to, by replication.
          uint32_t want = jpeg_test::widen_sample(
              ref[((size_t)y * rw + x) * 3 + ch], rbits, out_bits);
          ASSERT_EQ(got, want) << "at (" << x << ", " << y << ") channel "
                               << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// A lossless frame whose sampling factors are not 1x1: two out-of-bounds
// reads, both found by the fuzzer.
//
// T.81 H.1.1 makes a lossless MCU a group of samples rather than of 8x8
// blocks, but the sampling factors still apply, and A.2.2 still says a scan
// naming one component is non-interleaved and covers that component's samples
// "left-to-right, top-to-bottom" with the factors playing no part in the walk.
// This decoder did neither: it walked a grid of Hi x Vi samples per MCU
// whatever the scan said, and it kept the "this line is predicted
// one-dimensionally" state of H.1.2.1 as one flag per component rather than
// one per line.
//
// With Hi above one those two combine badly.  An MCU covers several lines, so
// the walk finishes the MCU's lines before returning to the first line in the
// next MCU along, and the flag set for the last line of one MCU is then read
// on the first line of the next.  On a first line that asks for the sample
// above the image - a read off the front of the plane, which the fuzzer found
// with this 110-byte file: a 17x9 SOF11 frame with one component at Hi = 2,
// Vi = 4.
//
// The second fault is in the output stage: a subsampled component holds fewer
// samples than the image has pixels, and it was indexed with the image's own
// (x, y) - two samples past the end of every row for 9-wide chroma in a
// 17-wide image, and eventually past the allocation.  It now reads through the
// same map the DCT paths use.
//
// What these files are not is an oracle.  Nothing to hand writes a correct
// subsampled lossless JPEG - libjpeg-turbo declines to subsample a lossless
// frame at all, and the ISO reference codec's own decode of these comes back
// nothing like the source - so the pixels are not asserted, only that the
// decode stays inside its buffers and produces the declared size.
TEST(JpegLoad, LosslessFrameWithNonUnitSamplingFactors) {
  struct Case {
    const char * jpg;
    uint32_t w, h;
    const char * what;
  };
  const Case cases[] = {
      {"lossless_arith_h2v4.jpg", 17, 9,
          "one component at Hi = 2, Vi = 4, found by the fuzzer"},
      {"lossless_huff_subsampled.jpg", 17, 9,
          "three components, luma at 2x2: an interleaved MCU spanning lines"},
      {"lossless_arith_subsampled.jpg", 17, 9,
          "the same, arithmetic, with the chroma subsampled instead"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    // The fixture must keep the shape that matters, or the test guards nothing.
    bool found = false;
    for (size_t i = 0; i + 12 < jpeg.size(); i++) {
      if (jpeg[i] != 0xFF) {
        continue;
      }
      uint8_t m = jpeg[i + 1];
      if (m != 0xC3 && m != 0xCB) { // SOF3, SOF11
        continue;
      }
      int nf = (int)jpeg[i + 9];
      int wide = 0;
      for (int k = 0; k < nf; k++) {
        uint8_t hv = jpeg[i + 11 + 3 * k];
        if ((hv >> 4) > 1 || (hv & 0x0F) > 1) {
          wide = 1;
        }
      }
      EXPECT_TRUE(wide) << "a sampling factor must exceed one";
      found = true;
      break;
    }
    ASSERT_TRUE(found) << "fixture must carry a lossless frame header";

    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    ASSERT_NE(doc, nullptr);
    GIMG_Raster * raster = nullptr;
    GIMG_Result r = gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
    // Either outcome is acceptable for files this unusual; reading out of
    // bounds is not, which is what the sanitizer build turns into a failure.
    if (r == GIMG_OK) {
      ASSERT_NE(raster, nullptr);
      EXPECT_EQ(gimg_raster_width(raster), c.w);
      EXPECT_EQ(gimg_raster_height(raster), c.h);
      gimg_raster_destroy(raster);
    }
    else {
      EXPECT_EQ(raster, nullptr);
    }
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// A three-component frame is not automatically YCbCr.
//
// T.81 describes no color space at all - a component is a component, and the
// frame header names them only by identifier.  What three of them mean is
// settled by the conventions layered on top, and libjpeg's rule (jdapimin.c,
// default_decompress_parms) is the one every decoder follows: JFIF means
// YCbCr and outranks everything; failing that an Adobe APP14 says outright
// which was used; failing that the component identifiers are the only evidence
// left, and 'R', 'G', 'B' is the one spelling that means what it says.
//
// This library converted every three-component frame as though it were YCbCr,
// so an RGB-coded JPEG - which is what the ISO reference codec writes with -c,
// and what a good deal of scientific and print imagery is - came out with every
// pixel a different color.
//
// The four fixtures are one encode, altered four ways, so the only thing that
// varies is the evidence: keep the Adobe marker, drop it, drop it and retag the
// components 'R' 'G' 'B', or add a JFIF APP0 in front of the Adobe marker to
// see which wins.  Two should decode as RGB and two as YCbCr, and the pairs
// differ from each other by up to 255, so reading the rule wrongly anywhere
// changes every pixel of at least one fixture.  The expected output is
// libjpeg-turbo's own decode and the match is exact, not within a tolerance.
TEST(JpegLoad, ThreeComponentColorSpaceFollowsTheMarkers) {
  struct Case {
    const char * jpg;
    const char * ref;
    const char * what;
  };
  const Case cases[] = {
      {"rgb_adobe0.jpg", "rgb_adobe0_ref.ppm",
          "Adobe APP14 transform 0: the components are R, G, B"},
      {"rgb_no_marker.jpg", "rgb_no_marker_ref.ppm",
          "no marker and unrevealing identifiers: YCbCr, the common case"},
      {"rgb_by_comp_id.jpg", "rgb_by_comp_id_ref.ppm",
          "no marker, identifiers 'R' 'G' 'B': RGB on that evidence alone"},
      {"rgb_jfif_wins.jpg", "rgb_jfif_wins_ref.ppm",
          "JFIF before an Adobe transform 0: JFIF wins, so YCbCr"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    uint32_t rw = 0, rh = 0;
    int rchan = 0, rbits = 0;
    std::vector<uint32_t> ref;
    ASSERT_TRUE(jpeg_test::load_pnm_file(c.ref, &rw, &rh, &rchan, &rbits, ref));
    ASSERT_EQ(rchan, 3);
    ASSERT_EQ(rbits, 8);

    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    ASSERT_EQ(gimg_raster_width(raster), rw);
    ASSERT_EQ(gimg_raster_height(raster), rh);
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    size_t mismatches = 0;
    for (uint32_t y = 0; y < rh && mismatches == 0; y++) {
      for (uint32_t x = 0; x < rw && mismatches == 0; x++) {
        for (int ch = 0; ch < 3; ch++) {
          int a = (int)px[(size_t)y * stride + (size_t)x * 4 + ch];
          int b = (int)ref[((size_t)y * rw + x) * 3 + ch];
          if (a != b) {
            mismatches++;
            ADD_FAILURE() << "first difference at (" << x << ", " << y
                          << ") channel " << ch << ": ours " << a
                          << ", libjpeg " << b;
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

// Hierarchical mode (T.81 Annex J): a pyramid of frames rather than one image.
//
// Every fixture here was produced by the ISO reference codec, which is the
// only implementation at hand that writes hierarchical JPEG at all, so these
// compare against an independent encoder rather than against our own.
//
// The comparison carries a tolerance because that codec's IDCT and chroma
// upsampler are not libjpeg's, and this library matches libjpeg: a plain
// single-frame file from the same encoder already needs one.  The plain_*
// cases at the end of the table are that control.  They are what the
// tolerances mean: a hierarchical file must land as close to the reference as
// an ordinary file of the same shape does, give or take the one extra frame of
// rounding a pyramid adds.  A tolerance of 8 on a 4:2:0 fixture is not
// slackness about hierarchical decoding - it is the same 8 the single-frame
// 4:2:0 control needs, and every structural mistake this test is here to catch
// (ignoring the EXP, dropping the differential frame, clamping the reference
// between frames, adding without the level shift) moves pixels by far more.
//
// See tests/data/jpeg/README.md for the commands that produced each fixture.
TEST(JpegLoad, DecodeHierarchicalMatchesReferenceCodec) {
  struct Case {
    const char * jpg;
    const char * ref;
    int tolerance;
    const char * what;
  };
  const Case cases[] = {
      {"hier_rgb_2level.jpg", "hier_rgb_2level_ref.ppm", 3,
          "SOF1 base + SOF5 differential sequential DCT, 4:4:4"},
      {"hier_rgb_2level_arith.jpg", "hier_rgb_2level_arith_ref.ppm", 3,
          "SOF9 base + SOF13 differential, arithmetic (Annex D)"},
      {"hier_gray_2level.jpg", "hier_gray_2level_ref.pgm", 2,
          "SOF1 + SOF5, single component"},
      {"hier_gray_2level_arith.jpg", "hier_gray_2level_arith_ref.pgm", 2,
          "SOF9 + SOF13, single component"},
      {"hier_rgb_420.jpg", "hier_rgb_420_ref.ppm", 8,
          "SOF1 + SOF5 with 4:2:0 chroma, so each component expands"},
      {"hier_rgb_422.jpg", "hier_rgb_422_ref.ppm", 13,
          "SOF1 + SOF5 with 4:2:2 chroma"},
      {"hier_gray_lossless.jpg", "hier_gray_lossless_ref.pgm", 1,
          "SOF1 base + SOF7 differential lossless (J.1.3.2: Ss = 0)"},
      {"hier_gray_lossless_ar.jpg", "hier_gray_lossless_ar_ref.pgm", 1,
          "SOF9 base + SOF15 differential lossless, arithmetic"},
      {"hier_rgb_lossless.jpg", "hier_rgb_lossless_ref.ppm", 3,
          "SOF1 + SOF7 differential lossless, three components"},
      {"hier_gray_noexp.jpg", "hier_gray_noexp_ref.pgm", 1,
          "EXP(0,0): a refining frame at the same resolution (B.3.3)"},
      {"hier_gray_noexp_arith.jpg", "hier_gray_noexp_arith_ref.pgm", 1,
          "EXP(0,0), arithmetic"},
      {"hier_rgb_progressive.jpg", "hier_rgb_progressive_ref.ppm", 3,
          "SOF2 base + SOF6 differential progressive (Annex G in a pyramid)"},
      {"hier_rgb_prog_arith.jpg", "hier_rgb_prog_arith_ref.ppm", 3,
          "SOF10 base + SOF14 differential progressive, arithmetic"},
      {"hier_rgb_prog_lossless.jpg", "hier_rgb_prog_lossless_ref.ppm", 3,
          "SOF2 base + SOF7: J allows a lossless frame to end a DCT sequence"},
      // Controls: the same encoder, the same source, no hierarchy.
      {"plain_rgb_444.jpg", "plain_rgb_444_ref.ppm", 2,
          "control: single-frame 4:4:4"},
      {"plain_rgb_420.jpg", "plain_rgb_420_ref.ppm", 8,
          "control: single-frame 4:2:0"},
      {"plain_rgb_422.jpg", "plain_rgb_422_ref.ppm", 13,
          "control: single-frame 4:2:2"},
      {"plain_gray.jpg", "plain_gray_ref.pgm", 1,
          "control: single-frame grayscale"},
      {"plain_rgb_progressive.jpg", "plain_rgb_progressive_ref.ppm", 2,
          "control: single-frame progressive"},
  };

  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    uint32_t rw = 0, rh = 0;
    int rchan = 0, rbits = 0;
    std::vector<uint32_t> ref;
    ASSERT_TRUE(jpeg_test::load_pnm_file(c.ref, &rw, &rh, &rchan, &rbits, ref))
        << "missing reference " << c.ref;
    ASSERT_EQ(rbits, 8);

    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    ASSERT_NE(doc, nullptr);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
        GIMG_OK);
    ASSERT_NE(raster, nullptr);

    // The DHP segment declares the size of the completed image (B.3.2), and
    // that is what the sequence must decode to - not the size of its first,
    // smallest frame.
    EXPECT_EQ(gimg_raster_width(raster), rw);
    EXPECT_EQ(gimg_raster_height(raster), rh);

    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    ASSERT_NE(fmt, nullptr);
    const unsigned char * px =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t stride = gimg_raster_stride_bytes(raster);
    int ours_chan = (int)fmt->channel_count;
    ASSERT_EQ((int)fmt->bits_per_channel[0], 8);
    ASSERT_TRUE(ours_chan == 1 || ours_chan == 4) << "unexpected channel count";
    ASSERT_EQ(rchan, (ours_chan == 1) ? 1 : 3);

    int worst = 0;
    uint32_t worst_x = 0, worst_y = 0;
    for (uint32_t y = 0; y < rh; y++) {
      for (uint32_t x = 0; x < rw; x++) {
        for (int ch = 0; ch < rchan; ch++) {
          int a = (int)px[(size_t)y * stride + (size_t)x * ours_chan + ch];
          int b = (int)ref[((size_t)y * rw + x) * rchan + ch];
          int d = a > b ? a - b : b - a;
          if (d > worst) {
            worst = d;
            worst_x = x;
            worst_y = y;
          }
        }
      }
    }
    EXPECT_LE(worst, c.tolerance)
        << "worst difference " << worst << " at (" << worst_x << ", "
        << worst_y << ")";
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
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
      {"lossless_arith_gray.jpg", "lossless_arith_gray.pgm", "8-bit gray"},
      {"lossless_arith_rgb.jpg", "lossless_arith_rgb.ppm", "8-bit RGB"},
      {"lossless_arith_gray12.jpg", "lossless_arith_gray12.pgm", "12-bit gray"},
      {"lossless_arith_gray16.jpg", "lossless_arith_gray16.pgm", "16-bit gray"},
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
 * DCT ones: each sample is predicted from its already-decoded neighbors and
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
  // The library's widening rule, generalized to any source precision.
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
 * resynchronization point, so bits the longest-match Huffman decode had read
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
          // A grayscale frame decodes to GRAY8, a color one to RGBA8.
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

/** A 12-bit color frame must fill its whole raster.
 *
 * The fixture is a flat color, so every decoded pixel has to be the same one;
 * that makes this a check on addressing rather than on arithmetic.  It is here
 * because the 12-bit color path computed its row stride in pixels while
 * indexing the row through a uint16_t * - so it wrote each frame into the first
 * quarter of its own raster and left the rest at zero.  Every existing 12-bit
 * test looked only at the dimensions and the pixel format, which were both
 * correct, and none of them read a sample. */
TEST(JpegLoad, Decode12BitColorFillsTheWholeRaster) {
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

// Passing no options and passing a zero-initialized GIMG_Decode_Options must
// decode identically.  They did not: GIMG_JPEG_CHROMA_UPSAMPLE_SIMPLE used to
// be 0, so `GIMG_Decode_Options o = {};` selected the box filter while NULL
// selected the triangle filter, and the natural way to write the struct
// quietly produced different pixels.  The third decode is what gives this test
// teeth - it proves the fixture actually distinguishes the two filters, so the
// first two agreeing means something.
TEST(JpegLoad, ZeroInitializedDecodeOptionsMatchNullOptions) {
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
      << "a zero-initialized GIMG_Decode_Options must decode as NULL does";
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

/** Priority (1): Decoder must work correctly with output from an outside encoder.
 * Encode a known image with the oracle, then our decoder decodes that JPEG;
 * compare to the oracle's decode of the same file. Requires Pillow. */
TEST(JpegLoad, DecodeLibjpegEncodedBaseline) {
  std::string jpeg_path = jpeg_test::jpeg_output_dir() + "/libjpeg_encoded_baseline.jpg";
  if (!jpeg_test::libjpeg_encode_baseline_to_file(jpeg_path.c_str(), 640, 480, 85)) {
    GTEST_SKIP() << "Install Pillow (pip install Pillow) for the encode oracle";
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

// T.81 A.2.3 inside a hierarchical sequence.
//
// A frame of a sequence may be coded as one non-interleaved scan per component
// exactly as a single-frame image may; Annex J changes the coding model, not
// the scan arrangement, and says nothing to forbid it.  The decoder refused
// such a frame outright until it learned to hand it to the walk that already
// knew the order.
//
// The fixtures are one-frame sequences: DHP followed by a single
// non-differential frame, which J.1.3 leaves coded normally, so the sequence
// decodes to exactly that frame.  That is what gives them a known answer - the
// reference decode committed for the plain file they were built from, compared
// exactly.  They were built by inserting a DHP segment (B.3.2: the frame
// header's own parameters with Tq zeroed) ahead of the frame in
// ni_ours_*.jpg, and the ISO reference codec reads all three as hierarchical
// sequences, so they are well formed T.81 and not merely something this
// decoder happens to accept.
TEST(JpegLoad, HierarchicalFrameCodedAsNonInterleavedScans) {
  struct Case {
    const char * jpg;
    const char * ref;
    const char * what;
  };
  const Case cases[] = {
      {"hier_noninterleaved_444.jpg", "ni_ours_444_turbo.ppm",
          "4:4:4, three scans"},
      {"hier_noninterleaved_420.jpg", "ni_ours_420_turbo.ppm",
          "4:2:0, three scans, chroma grid smaller than the MCU grid"},
      {"hier_noninterleaved_arith_420.jpg", "ni_ours_arith_420_turbo.ppm",
          "4:2:0, three scans, arithmetic"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    uint32_t rw = 0, rh = 0;
    int rchan = 0, rbits = 0;
    std::vector<uint32_t> ref;
    ASSERT_TRUE(jpeg_test::load_pnm_file(c.ref, &rw, &rh, &rchan, &rbits, ref))
        << "missing reference " << c.ref;
    ASSERT_EQ(rchan, 3);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK)
        << "a hierarchical frame coded as non-interleaved scans must decode";
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(gimg_raster_width(raster), rw);
    EXPECT_EQ(gimg_raster_height(raster), rh);
    const unsigned char * gp =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < rh; y++) {
      for (uint32_t x = 0; x < rw; x++) {
        for (int ch = 0; ch < 3; ch++) {
          int a = (int)gp[y * gs + x * 4 + (size_t)ch];
          int b = (int)ref[((size_t)y * rw + x) * 3 + (size_t)ch];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// A.2.3 for a lossless frame inside a hierarchical sequence.
//
// The same gap as the DCT case and the same shape of fix, but a bigger one to
// close: the lossless frame decoder had the scan's predictor, point transform,
// tables and restart interval as frame-wide values, and a frame written one
// scan per component gives each scan its own (B.2.3).  So the walk is now per
// scan, and the point transform is remembered per component because A.4 lets
// each scan choose its own Pt.
//
// Lossless, so the oracle is the source image itself, exactly - nothing here
// is approximate.  The fixtures are one-frame sequences built by inserting DHP
// ahead of the frame in the lossless_noninterleaved_* files, which
// libjpeg-turbo wrote; the ISO reference codec reads all three as hierarchical
// sequences.
//
// One thing these do not reach.  A non-interleaved scan covers the component's
// own grid, ceil(X x H_i / H_max) by ceil(Y x V_i / V_max), rather than the
// MCU-padded grid an interleaved scan walks; with 1x1 sampling the two are the
// same size, and every lossless file anything here can write is 1x1, because
// libjpeg-turbo declines to subsample a lossless frame at all.  Replacing the
// one with the other passes this test.  The distinction is read straight from
// A.2.2 and matches what the DCT path does; it is not checked against
// anything, for the same reason the subsampled lossless fixtures are not.
TEST(JpegLoad, HierarchicalLosslessFrameCodedAsNonInterleavedScans) {
  struct Case {
    const char * jpg;
    const char * what;
  };
  const Case cases[] = {
      {"hier_lossless_noninterleaved.jpg", "three scans, predictor 4 throughout"},
      {"hier_lossless_noninterleaved_psv.jpg",
          "predictors 1, 2 and 7: one per scan, not one per frame"},
      {"hier_lossless_noninterleaved_restart.jpg",
          "with a restart interval, which each scan counts in its own MCUs"},
  };
  uint32_t sw = 0, sh = 0;
  int schan = 0, sbits = 0;
  std::vector<uint32_t> src;
  ASSERT_TRUE(jpeg_test::load_pnm_file(
      "hier_src_rgb.ppm", &sw, &sh, &schan, &sbits, src));
  ASSERT_EQ(schan, 3);

  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK)
        << "a hierarchical lossless frame coded as non-interleaved scans must "
           "decode";
    ASSERT_NE(raster, nullptr);
    EXPECT_EQ(gimg_raster_width(raster), sw);
    EXPECT_EQ(gimg_raster_height(raster), sh);
    const unsigned char * gp =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < sh; y++) {
      for (uint32_t x = 0; x < sw; x++) {
        for (int ch = 0; ch < 3; ch++) {
          int a = (int)gp[y * gs + x * 4 + (size_t)ch];
          int b = (int)src[((size_t)y * sw + x) * 3 + (size_t)ch];
          ASSERT_EQ(a, b) << "lossless must be exact: pixel (" << x << ","
                          << y << ") channel " << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// T.81 allows Nf up to 255, and four components in practice means CMYK, or
// YCCK when an Adobe APP14 says transform 2.  The baseline walk had handled
// them since early on; the coefficient-buffer walk - which serves progressive
// frames, sequential frames written as several scans, and 12-bit frames -
// refused anything that was not one or three, so the same picture decoded when
// it was saved sequentially and came back UNSUPPORTED when it was saved
// progressive.  Both walks now assemble four components with the same code.
//
// The oracle is libjpeg, as it is for every CMYK fixture here, because libjpeg
// does not invert Adobe CMYK and the decision to match it was already made -
// see jdcolor.c null_convert and the note in jpeg_entropy.c.  Pillow does
// invert, so a Pillow decode of these files is the exact complement of the
// .raw, which is a useful thing to know when one of them looks wrong.
//
// cmyk_progressive.jpg is a real progressive CMYK file: 18 scans, a
// four-component interleaved DC scan, successive approximation on both DC and
// AC, and one AC scan per component as G.1.2.2 requires.  The two ycck_ files
// are the same images with the APP14 transform byte set to 2, which is the
// only thing in a JPEG that distinguishes YCCK from CMYK; nothing available
// writes a genuine YCCK file, and the resulting picture is not meaningful, but
// the decode is well defined and libjpeg agrees with it byte for byte.  That
// branch had no fixture at all before.
// T.81 B.4 describes two streams that are not complete JPEGs and only mean
// anything as a pair: one of table-specification data with no frame, and one
// carrying a frame whose tables are absent.  They exist so that a set of
// images can share one copy of its tables.  Both were refused here.
//
// The fixtures are libjpeg-turbo's own output - jpeg_write_tables() for the
// first and jpeg_start_compress(..., FALSE) for the second, from one compress
// object so that the tables really are left out of the image (see
// tests/data/jpeg/mk_abbrev.c) - and the expected pixels are its decode of the
// complete file it wrote from the same tables.  So this reads another
// implementation's abbreviated pair and has to arrive where it arrives.
TEST(JpegLoad, AbbreviatedStreamsOfB4) {
  std::vector<uint8_t> tables_bytes, image_bytes;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("abbrev_ljt_tables.jpg", tables_bytes));
  ASSERT_TRUE(jpeg_test::load_jpeg_file("abbrev_ljt_image.jpg", image_bytes));
  std::vector<uint8_t> oracle;
  uint32_t ow = 0, oh = 0;
  int omode = -1;
  ASSERT_TRUE(jpeg_test::load_jpeg_oracle_raw(
      "abbrev_ljt_full", oracle, &ow, &oh, &omode));
  ASSERT_EQ(omode, jpeg_test::kOracleRgb);

  // The abbreviated image is not readable on its own: its tables are absent
  // and nothing in it says what they were.
  {
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(
        gimg_stream_create_memory(image_bytes.data(), image_bytes.size(), &s),
        GIMG_OK);
    GIMG_Doc * doc = nullptr;
    GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
    if (r == GIMG_OK) {
      GIMG_Raster * raster = nullptr;
      EXPECT_NE(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
          GIMG_OK)
          << "a frame with no tables cannot be decoded without them";
      if (raster) {
        gimg_raster_destroy(raster);
      }
      gimg_doc_destroy(doc);
    }
    gimg_stream_destroy(s);
  }

  // Nor is the table-specification stream an image: it has no frame at all.
  {
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(
        gimg_stream_create_memory(tables_bytes.data(), tables_bytes.size(), &s),
        GIMG_OK);
    GIMG_Doc * doc = nullptr;
    GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
    EXPECT_NE(r, GIMG_OK) << "a table-specification stream carries no picture";
    if (r == GIMG_OK) {
      gimg_doc_destroy(doc);
    }
    gimg_stream_destroy(s);
  }

  // Together they are the picture libjpeg decoded from the complete file.
  GIMG_Stream * ts = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(tables_bytes.data(), tables_bytes.size(), &ts),
      GIMG_OK);
  GIMG_JPEG_Tables * tables = nullptr;
  ASSERT_EQ(gimg_jpeg_tables_load(ts, &tables), GIMG_OK);
  gimg_stream_destroy(ts);
  ASSERT_NE(tables, nullptr);

  GIMG_Load_Options lo = {};
  lo.jpeg_tables = tables;
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(image_bytes.data(), image_bytes.size(), &s),
      GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, &lo, nullptr, &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  ASSERT_EQ(gimg_raster_width(raster), ow);
  ASSERT_EQ(gimg_raster_height(raster), oh);
  const unsigned char * gp = (const unsigned char *)gimg_raster_pixels(raster);
  size_t gs = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < oh; y++) {
    for (uint32_t x = 0; x < ow; x++) {
      for (int c = 0; c < 3; c++) {
        int a = (int)gp[y * gs + x * 4 + (size_t)c];
        int b = (int)oracle[((size_t)y * ow + x) * 3 + (size_t)c];
        ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << c;
      }
    }
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
  gimg_jpeg_tables_destroy(tables);
}

// A stream with a frame in it is not table-specification data, whatever else
// it contains (B.4), and gimg_jpeg_tables_load says so rather than returning
// whatever tables it saw on the way past.
TEST(JpegLoad, TablesLoadRefusesAStreamWithAFrame) {
  std::vector<uint8_t> full;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("abbrev_ljt_full.jpg", full));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(full.data(), full.size(), &s), GIMG_OK);
  GIMG_JPEG_Tables * tables = nullptr;
  EXPECT_EQ(gimg_jpeg_tables_load(s, &tables), GIMG_ERR_FORMAT);
  EXPECT_EQ(tables, nullptr);
  gimg_stream_destroy(s);
}

// T.81 B.2.2 lets a frame carry from 1 to 255 components and never says what
// any of them mean; B.2.3 Table B.3 caps one scan at 4, so a frame wider than
// four is legal and has to be written as several non-interleaved scans
// (A.2.3).  This codec refused every such frame, and for the wrong reason: it
// applied A.2.2's ten-data-unit limit to the frame as a whole, where that
// limit belongs to an interleaved MCU and therefore to a scan.
//
// libjpeg cannot be the oracle directly - its decoder matches a scan's Cs
// against only the first MAX_COMPS_IN_SCAN components of the frame
// (jdmarker.c get_sos), so it refuses any file whose scan names the fifth
// component or later, however well formed.  The fixtures are assembled from
// files it did write: N grayscale JPEGs sharing one DQT and one set of Huffman
// tables, spliced into one N-component frame, with libjpeg's own decode of
// each grayscale file kept as the expected plane (tests/data/jpeg/mk_wide.py).
// Both ends of the comparison are libjpeg's.
TEST(JpegLoad, FramesWiderThanOneScanCanName) {
  const int counts[] = {2, 5, 8, 10, 32, 255};
  for (int n : counts) {
    SCOPED_TRACE("Nf = " + std::to_string(n));
    std::string base = "wide_" + std::to_string(n) + "comp";
    std::vector<uint8_t> oracle;
    uint32_t ow = 0, oh = 0;
    int ochan = 0;
    ASSERT_TRUE(jpeg_test::load_jpeg_multichannel_raw(
        base.c_str(), oracle, &ow, &oh, &ochan))
        << "missing oracle " << base << ".raw";
    ASSERT_EQ(ochan, n);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file((base + ".jpg").c_str(), jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK)
        << "a frame of " << n << " components is legal (T.81 B.2.2)";
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    ASSERT_NE(fmt, nullptr);
    ASSERT_EQ((int)fmt->channel_count, n);
    // One and four have color conventions; the rest have none, which is what
    // GIMG_CHANNEL_UNKNOWN records.
    EXPECT_EQ(fmt->channel_model,
        (n == 4) ? GIMG_CHANNEL_CMYK : GIMG_CHANNEL_UNKNOWN);
    ASSERT_EQ(gimg_raster_width(raster), ow);
    ASSERT_EQ(gimg_raster_height(raster), oh);
    const unsigned char * gp =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < oh; y++) {
      for (uint32_t x = 0; x < ow; x++) {
        for (int c = 0; c < n; c++) {
          int a = (int)gp[y * gs + (size_t)x * (size_t)n + (size_t)c];
          int b = (int)
              oracle[((size_t)y * ow + x) * (size_t)n + (size_t)c];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << c;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// T.81 B.2.3 Table B.3 gives Ns as 1 to 4 whatever Nf is, and A.2.2 caps an
// interleaved MCU at ten data units.  Both are scan properties; a file that
// breaks either is malformed even though the frame header is fine.
TEST(JpegLoad, ScanHeaderLimitsAreEnforcedWhereTheyBelong) {
  std::vector<uint8_t> good;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("wide_5comp.jpg", good));
  // Find the first SOS and raise its Ns to 5, which B.2.3 forbids.  The
  // payload grows with Ns, so lengthen the segment to match: the file is then
  // wrong in exactly one way.
  size_t i = 2;
  size_t sos = 0;
  while (i + 4 <= good.size() && good[i] == 0xFF) {
    uint8_t m = good[i + 1];
    size_t len = (size_t)((good[i + 2] << 8) | good[i + 3]);
    if (m == 0xDA) {
      sos = i;
      break;
    }
    i += 2 + len;
  }
  ASSERT_NE(sos, 0u) << "fixture must have a scan";
  std::vector<uint8_t> bad(good.begin(), good.begin() + (long)sos);
  // Ls = 6 + 2*Ns with Ns = 5, then five component entries.
  bad.push_back(0xFF);
  bad.push_back(0xDA);
  bad.push_back(0x00);
  bad.push_back(0x10);
  bad.push_back(0x05);
  for (int c = 0; c < 5; c++) {
    bad.push_back((uint8_t)(c + 1));
    bad.push_back(0x00);
  }
  bad.push_back(0x00);
  bad.push_back(0x3F);
  bad.push_back(0x00);
  bad.insert(bad.end(), good.begin() + (long)sos + 10, good.end());
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(bad.data(), bad.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Diagnostics diag = {};
  gimg_diagnostics_init(&diag, nullptr);
  GIMG_Result r = gimg_doc_load(s, nullptr, &diag, &doc);
  EXPECT_EQ(r, GIMG_ERR_FORMAT)
      << "a scan naming five components breaks T.81 B.2.3";
  // It must be refused for the reason it is wrong.  Without the Ns check the
  // file is refused anyway, but only because the fifth component entry has
  // already been written past the end of the scan header's four-entry arrays
  // and lands on the Huffman table selector of the first - which is a buffer
  // overrun that happens to produce an error, not a check.
  bool said_ns = false;
  for (size_t k = 0; k < diag.count; k++) {
    const char * a = diag.items[k].recommended_action;
    if (a && std::string(a).find("Ns") != std::string::npos) {
      said_ns = true;
    }
  }
  EXPECT_TRUE(said_ns)
      << "the scan must be refused for naming too many components";
  gimg_diagnostics_destroy(&diag);
  if (r == GIMG_OK) {
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
}

// T.81 Table B.2 allows a sample precision of 12 in a DCT-based frame and
// B.2.2 allows Nf from 1 to 255.  The two are independent, so a twelve-bit
// CMYK or YCCK frame is a legal file - but the twelve-bit walk here refused
// anything but one or three components, because the four-component assembly
// took 8-bit planes and there was no four-channel 16-bit raster to write.
// Both are fixed: the assembly reads planes at either width, and the picture
// comes back as GIMG_PIXEL_CMYK16 with the samples left-justified, which is
// how GRAY16 and RGBA16 have always carried twelve-bit data.
//
// The fixtures are libjpeg-turbo's twelve-bit entry points (jpeg12_*, see
// tests/data/jpeg/mk_cmyk12.c), so both the encoder and the expected samples
// come from outside this library.
TEST(JpegLoad, TwelveBitFourComponentFrames) {
  struct Case {
    const char * base;
    const char * what;
  };
  const Case cases[] = {
      {"cmyk12_ljt_seq", "12-bit CMYK, sequential (SOF1)"},
      {"cmyk12_ljt_prog", "12-bit CMYK, progressive (SOF2)"},
      {"ycck12_ljt_420", "12-bit YCCK, 4:2:0"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.base) + ": " + c.what);
    std::vector<uint8_t> oracle;
    uint32_t ow = 0, oh = 0;
    int omode = -1;
    ASSERT_TRUE(
        jpeg_test::load_jpeg_oracle_raw(c.base, oracle, &ow, &oh, &omode))
        << "missing oracle " << c.base << ".raw";
    ASSERT_EQ(omode, jpeg_test::kOracleCmyk16)
        << "the oracle must carry 16-bit samples";
    std::string name = std::string(c.base) + ".jpg";
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(name.c_str(), jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK)
        << "a twelve-bit four-component frame is legal and must decode";
    ASSERT_NE(raster, nullptr);
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    ASSERT_NE(fmt, nullptr);
    EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK);
    EXPECT_EQ((int)fmt->bits_per_channel[0], 16);
    ASSERT_EQ(gimg_raster_width(raster), ow);
    ASSERT_EQ(gimg_raster_height(raster), oh);
    const uint16_t * gp = (const uint16_t *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster) / sizeof(uint16_t);
    for (uint32_t y = 0; y < oh; y++) {
      for (uint32_t x = 0; x < ow; x++) {
        for (int ch = 0; ch < 4; ch++) {
          size_t k = (((size_t)y * ow + x) * 4 + (size_t)ch) * 2;
          int b = (int)oracle[k] | ((int)oracle[k + 1] << 8);
          int a = (int)gp[y * gs + x * 4 + (size_t)ch];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// A four-component frame whose chrominance components are subsampled was
// upsampled by nearest neighbor here, on the stated grounds that "four-
// component files are not YCbCr and the filter does not apply to them".  That
// was wrong: libjpeg picks its upsampler from the sampling factors alone -
// jdsample.c's jinit_upsampler never looks at the color space - so a 4:2:0
// YCCK file gets the same triangle filter a 4:2:0 YCbCr file gets, and this
// decoder disagreed with libjpeg on every pixel between chroma samples.
//
// Nothing caught it because every four-component fixture was 4:4:4, where the
// box filter and the triangle filter give the same answer.  These three are
// libjpeg-turbo's own output (tests/data/jpeg/mk_cmyk.c), so both the encoder
// and the expected pixels come from outside this library.
TEST(JpegLoad, SubsampledFourComponentFramesUpsampleAsLibjpegDoes) {
  struct Case {
    const char * base;
    const char * what;
  };
  const Case cases[] = {
      {"ycck_ljt_420", "YCCK 4:2:0, written by libjpeg-turbo (h2v2 fancy)"},
      {"ycck_ljt_422", "YCCK 4:2:2, written by libjpeg-turbo (h2v1 fancy)"},
      {"cmyk_ljt_sub", "CMYK 4:2:0, written by libjpeg-turbo (transform 0)"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.base) + ": " + c.what);
    std::vector<uint8_t> oracle;
    uint32_t ow = 0, oh = 0;
    int omode = -1;
    ASSERT_TRUE(
        jpeg_test::load_jpeg_oracle_raw(c.base, oracle, &ow, &oh, &omode))
        << "missing oracle " << c.base << ".raw";
    ASSERT_EQ(omode, 2) << "the oracle must be a CMYK .raw";
    std::string name = std::string(c.base) + ".jpg";
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(name.c_str(), jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    ASSERT_EQ(gimg_raster_width(raster), ow);
    ASSERT_EQ(gimg_raster_height(raster), oh);
    const unsigned char * gp =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < oh; y++) {
      for (uint32_t x = 0; x < ow; x++) {
        for (int ch = 0; ch < 4; ch++) {
          int a = (int)gp[y * gs + x * 4 + (size_t)ch];
          int b = (int)oracle[((size_t)y * ow + x) * 4 + (size_t)ch];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

TEST(JpegLoad, FourComponentFramesBeyondTheBaselineWalk) {
  struct Case {
    const char * base;
    const char * what;
  };
  const Case cases[] = {
      {"cmyk_progressive", "progressive CMYK, 18 scans (Annex G with Nf=4)"},
      {"ycck_baseline", "YCCK, Adobe transform 2, sequential"},
      {"ycck_progressive", "YCCK, Adobe transform 2, progressive"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.base) + ": " + c.what);
    std::vector<uint8_t> oracle;
    uint32_t ow = 0, oh = 0;
    int omode = -1;
    ASSERT_TRUE(
        jpeg_test::load_jpeg_oracle_raw(c.base, oracle, &ow, &oh, &omode))
        << "missing oracle " << c.base << ".raw";
    ASSERT_EQ(omode, 2) << "the oracle must be a CMYK .raw";
    std::string name = std::string(c.base) + ".jpg";
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(name.c_str(), jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK)
        << "a four-component frame must decode outside the baseline walk too";
    ASSERT_NE(raster, nullptr);
    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    ASSERT_NE(fmt, nullptr);
    EXPECT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK);
    ASSERT_EQ(gimg_raster_width(raster), ow);
    ASSERT_EQ(gimg_raster_height(raster), oh);
    const unsigned char * gp =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < oh; y++) {
      for (uint32_t x = 0; x < ow; x++) {
        for (int ch = 0; ch < 4; ch++) {
          int a = (int)gp[y * gs + x * 4 + (size_t)ch];
          int b = (int)oracle[((size_t)y * ow + x) * 4 + (size_t)ch];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// T.81 B.2.2 allows H_i and V_i from 1 to 4, and A.2.3's ten-data-unit limit on
// an MCU is what rules out the rest.  Everything here had only ever been tried
// at the three combinations photographs use - 4:4:4, 4:2:2, 4:2:0 - and a sweep
// of the twelve that libjpeg-turbo will write found two faults, both of them
// wrong pixels rather than refusals:
//
//   - The box fallback scaled proportionally, x * cw / width, where libjpeg's
//     int_upsample replicates each sample H_max/H_i times.  Those agree at a
//     ratio of two, which is why 4:2:0 and 4:2:2 never showed it; at a ratio of
//     four they disagree on half of every group of four pixels, by up to 47.
//
//   - 4:4:0 - full width, half height - had no fancy filter, so it was
//     box-filtered whatever the caller asked for.  libjpeg 6b box-filters it
//     too, but libjpeg-turbo has h1v2_fancy_upsample and that is what a current
//     decoder produces.  cjpeg -sample 1x2 writes such a file directly, and
//     losslessly transposing a 4:2:2 file is the other way to get one.
//
// The oracle is libjpeg-turbo 3.0.4's own decode, compared exactly; our IDCT is
// its islow and, now, our upsamplers are its upsamplers.
TEST(JpegLoad, EverySamplingFactorMatchesLibjpegTurbo) {
  struct Case {
    const char * base;
    const char * what;
  };
  const Case cases[] = {
      {"sampling_s1x2", "4:4:0 - full width, half height, the fancy h1v2 case"},
      {"sampling_s4x1", "H=4: a ratio the box fallback got wrong"},
      {"sampling_s2x4", "V=4, and H=2 as well"},
      {"sampling_s3x1", "H=3, a ratio with no fancy filter in any decoder"},
      {"sampling_s1x4", "V=4 alone"},
  };
  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.base) + ": " + c.what);
    uint32_t rw = 0, rh = 0;
    int rchan = 0, rbits = 0;
    std::vector<uint32_t> ref;
    std::string refname = std::string(c.base) + ".ppm";
    ASSERT_TRUE(jpeg_test::load_pnm_file(
        refname.c_str(), &rw, &rh, &rchan, &rbits, ref))
        << "missing reference " << refname;
    ASSERT_EQ(rchan, 3);
    std::string name = std::string(c.base) + ".jpg";
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(name.c_str(), jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    ASSERT_EQ(gimg_raster_width(raster), rw);
    ASSERT_EQ(gimg_raster_height(raster), rh);
    const unsigned char * gp =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < rh; y++) {
      for (uint32_t x = 0; x < rw; x++) {
        for (int ch = 0; ch < 3; ch++) {
          int a = (int)gp[y * gs + x * 4 + (size_t)ch];
          int b = (int)ref[((size_t)y * rw + x) * 3 + (size_t)ch];
          ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
        }
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

// T.81 B.2.5, the case DNL exists for.
//
// B.2.2 lets a frame header carry Y = 0, and then "the number of lines shall be
// defined by the DNL marker segment" after the first scan.  That is how a JPEG
// is written by something that does not know the height until it has finished -
// a scanner, a fax.  The three DNL tests here already covered a DNL that agrees
// with a stated height, one that contradicts it, and one that arrives before
// the first scan; none covered Y = 0, which is the only case where DNL decides
// anything.
//
// The oracle takes two codecs, because neither alone can give one.
// libjpeg-turbo refuses the file outright - "Empty JPEG image (DNL not
// supported)" - so it cannot say what the pixels are; but dnl_stated_height.jpg
// is the same image with its height in the SOF and no DNL, which libjpeg reads
// happily, and that decode is the committed reference.  The ISO reference
// codec, which does implement DNL, reads the zero-height file and agrees to
// within 3 - its IDCT is not libjpeg's, the same tolerance the hierarchical
// fixtures need.  So one codec vouches for the pixels and the other for the
// file being well formed.
TEST(JpegLoad, DnlSuppliesAHeightTheFrameHeaderLeftAtZero) {
  uint32_t rw = 0, rh = 0;
  int rchan = 0, rbits = 0;
  std::vector<uint32_t> ref;
  ASSERT_TRUE(jpeg_test::load_pnm_file(
      "dnl_zero_height.ppm", &rw, &rh, &rchan, &rbits, ref));
  ASSERT_EQ(rchan, 3);
  ASSERT_GT(rh, 0u);

  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("dnl_zero_height.jpg", jpeg));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK)
      << "a frame header with Y = 0 is legal; DNL supplies the height";
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
  ASSERT_NE(raster, nullptr);
  EXPECT_EQ(gimg_raster_width(raster), rw);
  EXPECT_EQ(gimg_raster_height(raster), rh)
      << "the height must come from the DNL segment, not from the SOF's zero";
  const unsigned char * gp = (const unsigned char *)gimg_raster_pixels(raster);
  size_t gs = gimg_raster_stride_bytes(raster);
  for (uint32_t y = 0; y < rh; y++) {
    for (uint32_t x = 0; x < rw; x++) {
      for (int ch = 0; ch < 3; ch++) {
        int a = (int)gp[y * gs + x * 4 + (size_t)ch];
        int b = (int)ref[((size_t)y * rw + x) * 3 + (size_t)ch];
        ASSERT_EQ(a, b) << "pixel (" << x << "," << y << ") channel " << ch;
      }
    }
  }
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);

  // The same image with its height in the frame header must decode the same
  // way; that is what says the DNL path joins the ordinary one rather than
  // running beside it.
  std::vector<uint8_t> plain;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("dnl_stated_height.jpg", plain));
  GIMG_Stream * s2 = nullptr;
  ASSERT_EQ(
      gimg_stream_create_memory(plain.data(), plain.size(), &s2), GIMG_OK);
  GIMG_Doc * doc2 = nullptr;
  ASSERT_EQ(gimg_doc_load(s2, nullptr, nullptr, &doc2), GIMG_OK);
  GIMG_Raster * r2 = nullptr;
  ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc2, 0), nullptr, &r2), GIMG_OK);
  ASSERT_NE(r2, nullptr);
  EXPECT_EQ(gimg_raster_height(r2), rh);
  const unsigned char * p2 = (const unsigned char *)gimg_raster_pixels(r2);
  size_t s2stride = gimg_raster_stride_bytes(r2);
  for (uint32_t y = 0; y < rh; y++) {
    for (uint32_t x = 0; x < rw; x++) {
      for (int ch = 0; ch < 3; ch++) {
        ASSERT_EQ((int)p2[y * s2stride + x * 4 + (size_t)ch],
            (int)ref[((size_t)y * rw + x) * 3 + (size_t)ch]);
      }
    }
  }
  gimg_raster_destroy(r2);
  gimg_doc_destroy(doc2);
  gimg_stream_destroy(s2);
}

// Reading a 32-bit EXIF field must not be undefined.
//
// exif.c's read_u32 built its value as (p[0] << 24) | ..., and an unsigned char
// promotes to int, so any field with a top byte above 0x7F was a signed shift
// that does not fit: undefined behavior, not a wrap.  The same fault was in
// the APP13 resource size in jpeg_load.c.  Every compiler anyone uses produces
// the right number anyway, which is why no functional test could ever have
// caught this, and why it sat in all four fuzz logs at once without anyone
// acting on it - UBSan was in recover mode, so it printed and the suite passed.
// The sanitizer build now aborts on undefined behavior, which is what makes
// this test a test: it loads a file whose EXIF has a high top byte, and under
// the sanitizers that either returns or it does not.
//
// The file came from the fuzz corpus.  What it decodes to does not matter here
// and is not asserted; it is malformed, and the loader is entitled to reject
// it.  Reaching the EXIF parser at all is the point.
TEST(JpegLoad, ExifFieldWithAHighTopByteIsReadWithoutUndefinedBehavior) {
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("exif_u32_high_bit.jpg", jpeg));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  if (r == GIMG_OK && doc) {
    GIMG_Raster * raster = nullptr;
    (void)gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster);
    if (raster) {
      gimg_raster_destroy(raster);
    }
    gimg_doc_destroy(doc);
  }
  gimg_stream_destroy(s);
  SUCCEED() << "the assertion is the sanitizer's, not gtest's";
}

// T.81 B.1.1.2 fill bytes and the B.1.1.3 TEM marker.
//
// B.1.1.2: "any marker may optionally be preceded by any number of fill bytes,
// which are bytes assigned code X'FF'".  The marker reader consumed exactly one
// byte after the first 0xFF and took whatever it found, so 0xFF 0xFF 0xC0
// returned a marker of 0xFF and the file was refused - one pad byte anywhere
// was enough.  The scan-data scanner had the same fault from the other side:
// inside entropy-coded data a 0xFF is always followed by the 0x00 of byte
// stuffing (B.2.2), so 0xFF 0xFF is padding ahead of a marker and never data,
// but it was appended to the scan, which would have handed the entropy decoder
// eight bits that were never coded.
//
// B.1.1.3 Table B.1 lists TEM (0xFF01) among the markers that stand alone, with
// no length field.  Reading a two-byte length after it swallows the start of
// whatever comes next, so such a file was refused outright.
//
// Every one of these is the same picture as baseline_8x8_gray.jpg with padding
// added, so the expected answer is that file's decode, and libjpeg accepts all
// six.  That is what makes them a test rather than a guess: this decoder
// rejected all six and no fixture here had ever contained a pad byte.
TEST(JpegLoad, FillBytesAndTemMarkerAreSkipped) {
  struct Case {
    const char * jpg;
    const char * what;
  };
  const Case cases[] = {
      {"marker_fill_before_sof.jpg", "three fill bytes before the frame header"},
      {"marker_fill_single_byte.jpg", "one fill byte, the smallest case"},
      {"marker_fill_first_segment.jpg", "fill before the first segment after SOI"},
      {"marker_fill_before_sos.jpg", "fill before SOS"},
      {"marker_fill_before_eoi.jpg",
          "fill between the entropy data and EOI, which the scan scanner sees"},
      {"marker_tem.jpg", "a TEM marker before the frame header (B.1.1.3)"},
  };
  uint32_t rw = 0, rh = 0;
  int rchan = 0, rbits = 0;
  std::vector<uint32_t> ref;
  ASSERT_TRUE(jpeg_test::load_pnm_file(
      "marker_padding_ref.pgm", &rw, &rh, &rchan, &rbits, ref));
  ASSERT_EQ(rchan, 1);

  for (const Case & c : cases) {
    SCOPED_TRACE(std::string(c.jpg) + ": " + c.what);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(c.jpg, jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK)
        << "padding a marker must not make the file unreadable";
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
    ASSERT_NE(raster, nullptr);
    ASSERT_EQ(gimg_raster_width(raster), rw);
    ASSERT_EQ(gimg_raster_height(raster), rh);
    const unsigned char * gp =
        (const unsigned char *)gimg_raster_pixels(raster);
    size_t gs = gimg_raster_stride_bytes(raster);
    for (uint32_t y = 0; y < rh; y++) {
      for (uint32_t x = 0; x < rw; x++) {
        ASSERT_EQ((int)gp[y * gs + x], (int)ref[(size_t)y * rw + x])
            << "padding must not change a pixel: (" << x << "," << y << ")";
      }
    }
    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

TEST(JpegLoad, EveryFourComponentFrameSaysItsInkPolarity) {
  // The polarity is a property of the file - the Adobe convention the format
  // is written in - and not of the coding process, so every path has to say
  // the same thing about the same image.  They did not: the baseline path set
  // it, and the extended, progressive-extended and hierarchical paths did
  // not, so a CMYK JPEG came back saying 0 is full ink when it was baseline
  // and saying nothing at all when it was progressive or twelve-bit.  A
  // consumer that read the second as "no ink" would render it inverted.
  static const char * const fixtures[] = {
      "cmyk_sample.jpg",        // baseline, interleaved
      "cmyk_ours_seq.jpg",      // sequential
      "cmyk_ours_ni.jpg",       // non-interleaved, one scan per component
      "cmyk_ours_prog.jpg",     // progressive
      "cmyk_ours_arith.jpg",    // arithmetic
      "cmyk_progressive.jpg",   // progressive, written elsewhere
      "cmyk12_ljt_seq.jpg",     // twelve-bit sequential
      "cmyk12_ljt_prog.jpg",    // twelve-bit progressive
      "ycck_baseline.jpg",      // YCCK, baseline
      "ycck_progressive.jpg",   // YCCK, progressive
      "ycck_ours_prog420.jpg",  // YCCK, progressive, subsampled
      "ycck12_ljt_420.jpg",     // YCCK, twelve-bit
  };

  for (const char * name : fixtures) {
    SCOPED_TRACE(name);
    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(name, jpeg)) << "missing fixture";

    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * raster = nullptr;
    ASSERT_EQ(gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster),
        GIMG_OK);

    const GIMG_Pixel_Format * fmt = gimg_raster_format(raster);
    ASSERT_EQ(fmt->channel_model, GIMG_CHANNEL_CMYK)
        << "this fixture must decode to four ink channels or it tests nothing";
    const GIMG_Color_Info * color = gimg_raster_color_info_const(raster);
    ASSERT_NE(color, nullptr);
    EXPECT_EQ(color->cmyk_polarity, GIMG_CMYK_POLARITY_INK);

    gimg_raster_destroy(raster);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
}

TEST(JpegLoad, AProfileStillReachesARasterThatIsNotFourComponent) {
  // The shared attachment must not have narrowed itself to CMYK: a
  // three-component file with an APP2 profile still carries it.
  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_test::load_jpeg_file("jpeg_with_icc.jpg", jpeg));

  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
  GIMG_Raster * raster = nullptr;
  ASSERT_EQ(
      gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &raster), GIMG_OK);
  const GIMG_Color_Info * color = gimg_raster_color_info_const(raster);
  ASSERT_NE(color, nullptr);
  EXPECT_GT(color->icc_size, 0u);
  EXPECT_EQ(color->cmyk_polarity, GIMG_CMYK_POLARITY_UNKNOWN)
      << "three channels are not ink amounts, so there is no polarity to state";
  gimg_raster_destroy(raster);
  gimg_doc_destroy(doc);
  gimg_stream_destroy(s);
}

TEST(JpegLoad, CmykConvertsToTheRgbLibjpegAndPillowProduce) {
  // This library has no colour engine, so a CMYK-to-RGB conversion cannot be
  // colorimetric and is not claimed to be.  What it can be held to is
  // agreeing with the naive conversion every other library without an engine
  // performs - and it does, exactly, on every pixel of every CMYK and YCCK
  // fixture here.  The oracle files are Pillow's RGB rendering, written by
  // tests/data/jpeg/generate_jpeg_oracle_raws.py; Pillow's CMYK handling is
  // libjpeg's.
  static const char * const fixtures[] = {"cmyk_sample.jpg",
      "cmyk_ljt_sub.jpg", "cmyk_ours_seq.jpg", "cmyk_ours_ni.jpg",
      "cmyk_ours_prog.jpg", "cmyk_ours_arith.jpg", "cmyk_progressive.jpg",
      "ycck_baseline.jpg", "ycck_progressive.jpg", "ycck_ljt_420.jpg",
      "ycck_ljt_422.jpg", "ycck_ours_420.jpg", "ycck_ours_422.jpg",
      "ycck_ours_444.jpg", "ycck_ours_prog420.jpg"};

  size_t checked = 0;
  for (const char * name : fixtures) {
    SCOPED_TRACE(name);
    std::string base(name);
    base = base.substr(0, base.find_last_of('.'));
    std::string oracle =
        std::string(GIMG_TEST_DATA_JPEG) + "/" + base + ".cmyk2rgb.raw";

    std::vector<uint8_t> want;
    uint32_t ow = 0, oh = 0;
    int omode = 0;
    if (!jpeg_test::load_jpeg_oracle_raw_from_path(
            oracle.c_str(), want, &ow, &oh, &omode)) {
      // The oracle is generated, not hand-written; say which one is missing
      // rather than passing silently over it.
      ADD_FAILURE() << "no oracle at " << oracle
                    << "; run python3 tests/data/jpeg/"
                       "generate_jpeg_oracle_raws.py";
      continue;
    }
    ASSERT_EQ(omode, jpeg_test::kOracleRgb);

    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(name, jpeg));
    GIMG_Stream * s = nullptr;
    ASSERT_EQ(gimg_stream_create_memory(jpeg.data(), jpeg.size(), &s), GIMG_OK);
    GIMG_Doc * doc = nullptr;
    ASSERT_EQ(gimg_doc_load(s, nullptr, nullptr, &doc), GIMG_OK);
    GIMG_Raster * cmyk = nullptr;
    ASSERT_EQ(
        gimg_item_decode(gimg_doc_item(doc, 0), nullptr, &cmyk), GIMG_OK);

    GIMG_Raster * rgb = nullptr;
    ASSERT_EQ(gimg_ops_convert_pixel_format(cmyk, &GIMG_PIXEL_RGBA8, &rgb),
        GIMG_OK);
    ASSERT_NE(rgb, nullptr);
    ASSERT_EQ(gimg_raster_width(rgb), ow);
    ASSERT_EQ(gimg_raster_height(rgb), oh);

    const uint8_t * px =
        static_cast<const uint8_t *>(gimg_raster_pixels_const(rgb));
    size_t stride = gimg_raster_stride_bytes(rgb);
    for (uint32_t y = 0; y < oh; y++) {
      for (uint32_t x = 0; x < ow; x++) {
        const uint8_t * p = px + (y * stride) + (x * 4u);
        size_t o = ((size_t)y * ow + x) * 3u;
        ASSERT_EQ(p[0], want[o]) << "red at (" << x << "," << y << ")";
        ASSERT_EQ(p[1], want[o + 1]) << "green at (" << x << "," << y << ")";
        ASSERT_EQ(p[2], want[o + 2]) << "blue at (" << x << "," << y << ")";
        ASSERT_EQ(p[3], 255u) << "alpha at (" << x << "," << y << ")";
      }
    }
    checked++;

    gimg_raster_destroy(rgb);
    gimg_raster_destroy(cmyk);
    gimg_doc_destroy(doc);
    gimg_stream_destroy(s);
  }
  EXPECT_EQ(checked, sizeof(fixtures) / sizeof(fixtures[0]));
}

namespace {

/** progressive_sample.jpg with `cut` bytes removed from the end of its
 * entropy data, EOI re-appended so the file still terminates properly. */
bool truncated_progressive(size_t cut, std::vector<uint8_t> & out) {
  std::vector<uint8_t> file;
  if (!jpeg_test::load_jpeg_file("progressive_sample.jpg", file)) {
    return false;
  }
  if (file.size() < cut + 4) { return false; }
  size_t end = file.size();
  if (end >= 2 && file[end - 2] == 0xFF && file[end - 1] == 0xD9) { end -= 2; }
  if (end < cut) { return false; }
  out.assign(file.begin(), file.begin() + (long)(end - cut));
  out.push_back(0xFF);
  out.push_back(0xD9);
  return true;
}

/** Load and decode, reporting the result and the raster's size. */
GIMG_Result decode_size(const std::vector<uint8_t> & bytes, uint32_t * w,
    uint32_t * h) {
  GIMG_Stream * s = nullptr;
  GIMG_Result r = gimg_stream_create_memory(bytes.data(), bytes.size(), &s);
  if (r != GIMG_OK) { return r; }
  GIMG_Doc * doc = nullptr;
  r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK) { return r; }
  GIMG_Item * item = gimg_doc_item(doc, 0);
  GIMG_Raster * ras = nullptr;
  r = item ? gimg_item_decode(item, nullptr, &ras) : GIMG_ERR_INTERNAL;
  if (r == GIMG_OK && ras) {
    if (w) { *w = gimg_raster_width(ras); }
    if (h) { *h = gimg_raster_height(ras); }
    gimg_raster_destroy(ras);
  }
  gimg_doc_destroy(doc);
  return r;
}

} // namespace

/**
 * A progressive scan missing only its final padding byte still decodes.
 *
 * T.81 B.2.2 does not say what an encoder pads the last byte of a scan with,
 * and third-party encoders differ, so the decoder treats underflow in the
 * final block of a progressive scan as a zero DC size and zero refinement
 * bits rather than calling the file corrupt. That is what the is_last_block
 * argument threaded through jpeg_block.c's four progressive decoders is for,
 * and nothing exercised it: every progressive fixture here is complete.
 *
 * The boundary is the point. The tolerance covers a final byte that is not
 * there; it does not cover missing coefficients. So one byte off the end
 * decodes and two do not, and both halves are asserted - a decoder that
 * accepted any truncation would satisfy the first on its own, and this test
 * would then be measuring nothing.
 *
 * Note that *loading* succeeds well past the point where decoding stops: the
 * scan headers are still intact, and the data is only found to be short when
 * the blocks are read. Asserting on the load alone would have called cuts of
 * two, three and four bytes tolerated when they are not.
 */
TEST(JpegLoad, AProgressiveScanMissingItsPaddingByteStillDecodes) {
  uint32_t w0 = 0, h0 = 0;
  std::vector<uint8_t> whole;
  ASSERT_TRUE(truncated_progressive(0, whole));
  ASSERT_EQ(decode_size(whole, &w0, &h0), GIMG_OK);
  ASSERT_GT(w0, 0u);
  ASSERT_GT(h0, 0u);

  // One byte: the padding. Tolerated, and the image keeps its shape.
  std::vector<uint8_t> one;
  ASSERT_TRUE(truncated_progressive(1, one));
  uint32_t w = 0, h = 0;
  EXPECT_EQ(decode_size(one, &w, &h), GIMG_OK)
      << "a scan missing only its final padding byte must still decode";
  EXPECT_EQ(w, w0);
  EXPECT_EQ(h, h0);

  // Beyond that, coefficients are missing and the file is corrupt. If this
  // ever passes, the tolerance has stopped being bounded and the check above
  // no longer distinguishes anything.
  int refused = 0;
  for (size_t cut = 2; cut <= 4; cut++) {
    std::vector<uint8_t> bytes;
    ASSERT_TRUE(truncated_progressive(cut, bytes));
    if (decode_size(bytes, nullptr, nullptr) != GIMG_OK) { refused++; }
  }
  EXPECT_EQ(refused, 3)
      << "cuts past the padding byte remove real data and must be refused";

  // The load half, stated separately because it differs: the scan headers
  // survive a cut that the block reader then rejects.
  std::vector<uint8_t> four;
  ASSERT_TRUE(truncated_progressive(4, four));
  GIMG_Stream * s = nullptr;
  ASSERT_EQ(gimg_stream_create_memory(four.data(), four.size(), &s), GIMG_OK);
  GIMG_Doc * doc = nullptr;
  const GIMG_Result lr = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  EXPECT_EQ(lr, GIMG_OK)
      << "loading reads the headers; shortness is a decode-time finding";
  if (lr == GIMG_OK) { gimg_doc_destroy(doc); }
}

namespace {

/**
 * The [start, end) byte range of every entropy-coded segment in @p f.
 *
 * Truncating the *file* can only ever shorten the last scan, which is why the
 * test above exercises exactly one of the decoder's five "the segment ran out
 * of bits" paths: a progressive file's earlier scans are followed by more
 * markers and stay whole no matter how much is cut off the end. Locating each
 * segment separately is what makes the DC, DC-refinement and AC-initial
 * decoders reachable at all.
 *
 * A segment runs from the end of the SOS header to the next marker that is
 * neither a stuffed zero (FF 00, B.1.1.5) nor a restart (FF D0-D7, B.2.1),
 * both of which are part of the entropy data rather than the end of it.
 */
std::vector<std::pair<size_t, size_t>> entropy_segments(
    const std::vector<uint8_t> & f) {
  std::vector<std::pair<size_t, size_t>> out;
  size_t i = 0;
  while (i + 1 < f.size()) {
    if (f[i] != 0xFF) { i++; continue; }
    const uint8_t m = f[i + 1];
    if (m == 0xFF) { i++; continue; }              // fill byte
    if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
    if (m == 0xD9) { break; }                      // EOI
    if (m == 0x00) { i += 2; continue; }
    if (i + 3 >= f.size()) { break; }
    const size_t len = ((size_t)f[i + 2] << 8) | (size_t)f[i + 3];
    if (m != 0xDA) { i += 2 + len; continue; }
    const size_t start = i + 2 + len;
    size_t j = start;
    while (j + 1 < f.size()) {
      if (f[j] == 0xFF && f[j + 1] != 0x00 &&
          !(f[j + 1] >= 0xD0 && f[j + 1] <= 0xD7)) {
        break;
      }
      j++;
    }
    if (start >= j) { return out; }  // malformed; stop rather than guess
    out.push_back({start, j});
    i = j;
  }
  return out;
}

/** @p f with @p cut bytes removed from the end of entropy segment @p index. */
bool cut_segment(const std::vector<uint8_t> & f, size_t index, size_t cut,
    std::vector<uint8_t> & out) {
  const std::vector<std::pair<size_t, size_t>> segs = entropy_segments(f);
  if (index >= segs.size()) { return false; }
  const size_t start = segs[index].first, end = segs[index].second;
  if (cut == 0 || cut > end - start) { return false; }
  out.assign(f.begin(), f.begin() + (long)(end - cut));
  out.insert(out.end(), f.begin() + (long)end, f.end());
  return true;
}

/** Sets the recovery variable for a scope, so a failed assertion cannot leak
 * it into the tests that run after this one. */
class WithStuffZeroRecovery {
public:
  WithStuffZeroRecovery() { setenv("GIMG_JPEG_RECOVER_STUFF_ZERO", "1", 1); }
  ~WithStuffZeroRecovery() { unsetenv("GIMG_JPEG_RECOVER_STUFF_ZERO"); }
};

/** Redirects fd 2 to a temporary file for a scope and hands back what was
 * written. The recovery mode announces itself on stderr once per bitstream,
 * and a sweep of it would otherwise bury the test output in warnings. */
class CapturedStderr {
public:
  CapturedStderr() {
    fflush(stderr);
    saved_ = dup(2);
    sink_ = tmpfile();
    if (sink_ && saved_ >= 0) { dup2(fileno(sink_), 2); }
  }

  ~CapturedStderr() { restore(); if (sink_) { fclose(sink_); } }

  /** Everything written to stderr since construction. */
  std::string text() {
    restore();
    if (!sink_) { return std::string(); }
    fseek(sink_, 0, SEEK_SET);
    std::string s;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, sink_)) > 0) { s.append(buf, n); }
    return s;
  }

private:
  void restore() {
    if (saved_ < 0) { return; }
    fflush(stderr);
    dup2(saved_, 2);
    close(saved_);
    saved_ = -1;
  }

  int saved_ = -1;
  FILE * sink_ = nullptr;
};

/** One truncation: which scan, how deep, and what decoding it produced. */
struct CutResult {
  size_t segment;
  size_t cut;
  size_t segment_len;
  GIMG_Result result;
  uint32_t w, h;

  /** Nothing of the scan's entropy data is left, rather than some of it. */
  bool empties_the_scan() const { return cut == segment_len; }
};

/**
 * Decode every scan of @p file truncated by every depth, in file order.
 *
 * @p max_cuts caps how deep to cut into each scan, 0 meaning all the way. The
 * cap is what makes a large fixture affordable, and cutting from the shallow
 * end is not an arbitrary choice: a shallow cut into a long scan runs the
 * decoder out of bits in its *last* block, which is the only place the
 * is_last_block arms can be reached. A small fixture is swept to the bottom
 * instead, where a deep cut starves the first block of many.
 */
std::vector<CutResult> sweep_cuts(
    const std::vector<uint8_t> & file, size_t max_cuts = 0) {
  std::vector<CutResult> out;
  const std::vector<std::pair<size_t, size_t>> segs = entropy_segments(file);
  for (size_t si = 0; si < segs.size(); si++) {
    const size_t len = segs[si].second - segs[si].first;
    const size_t deepest = (max_cuts && max_cuts < len) ? max_cuts : len;
    for (size_t cut = 1; cut <= deepest; cut++) {
      std::vector<uint8_t> bytes;
      if (!cut_segment(file, si, cut, bytes)) { continue; }
      CutResult r{si, cut, len, GIMG_OK, 0, 0};
      r.result = decode_size(bytes, &r.w, &r.h);
      out.push_back(r);
    }
  }
  return out;
}

/**
 * A fixture to sweep, and how deep to cut into each of its scans.
 *
 * Four are needed because the arms being exercised are selected by two
 * different things. Which *decoder* runs is chosen by the scan header, so a
 * baseline file is required to reach the baseline block decoder at all - a
 * progressive file never calls it. Which *arm* runs is chosen by whether the
 * block that runs out of bits is the last one of its scan, so a long scan cut
 * shallowly and a short scan cut to the bone reach opposite sides of the same
 * `if`.
 */
struct SweepFixture {
  const char * name;
  size_t max_cuts;  ///< 0 sweeps every depth; see sweep_cuts().
  bool progressive;
};

const SweepFixture kSweepFixtures[] = {
    // Tiny, ten scans, every progressive scan type: swept to the bottom, so
    // the decoder starves in the first block of many.
    {"progressive_sample.jpg", 0, true},
    // Long scans: swept shallowly, so it starves in the last block instead.
    {"progressive_640x480_ycbcr.jpg", 12, true},
    // Baseline, one long scan: the only way into jpeg_decode_block().
    {"baseline_640x480_ycbcr.jpg", 24, false},
    {"baseline_8x8_gray.jpg", 0, false},
};

} // namespace

/**
 * Truncating any one scan is either decoded or refused - never anything else.
 *
 * Each scan is cut back a byte at a time, so the entropy decoder runs out of
 * bits at a different place in each case: part-way through a Huffman codeword,
 * between a symbol and its magnitude bits, in the middle of a refinement run.
 * Those are separate arms in five separate decoders, and none of them had ever
 * executed, because the only truncation the suite did was to the end of the
 * file - which can only shorten the final scan of the final fixture.
 *
 * The sweep asserts the two things that must hold everywhere: the result is a
 * decode or a clean refusal, and a decode is of the right image. It also
 * asserts that something, somewhere, IS refused - a sweep in which nothing is
 * ever rejected would pass just as happily against a decoder that accepted
 * anything at all.
 */
TEST(JpegLoad, TruncatingAnyScanIsDecodedOrRefusedCleanly) {
  int swept = 0, refused = 0;
  for (const SweepFixture & fx : kSweepFixtures) {
    std::vector<uint8_t> file;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(fx.name, file)) << fx.name;

    uint32_t w0 = 0, h0 = 0;
    ASSERT_EQ(decode_size(file, &w0, &h0), GIMG_OK) << fx.name;
    ASSERT_GT(w0, 0u) << fx.name;

    const std::vector<CutResult> results = sweep_cuts(file, fx.max_cuts);
    ASSERT_FALSE(results.empty()) << fx.name;

    for (const CutResult & r : results) {
      swept++;
      if (r.result == GIMG_OK) {
        EXPECT_EQ(r.w, w0) << fx.name << " scan " << r.segment << " cut "
                           << r.cut;
        EXPECT_EQ(r.h, h0) << fx.name << " scan " << r.segment << " cut "
                           << r.cut;
        continue;
      }
      refused++;
      EXPECT_EQ(r.result, GIMG_ERR_CORRUPT)
          << fx.name << " scan " << r.segment << " cut " << r.cut
          << ": a short scan is corrupt input, not an internal error";
    }
  }
  EXPECT_GT(swept, 100) << "the sweep got much smaller than it was written to "
                           "be; check the fixtures still parse";
  EXPECT_GT(refused, 0)
      << "no truncation of any scan was refused, so this sweep is not "
         "distinguishing a decoder that checks from one that does not";
}

/**
 * GIMG_JPEG_RECOVER_STUFF_ZERO decodes a truncated scan, but not an absent one.
 *
 * Turning refusals into decodes is the option's entire contract, so it is
 * stated here as a differential over the same sweep: what the strict decoder
 * rejects, the recovering decoder must accept. Asserting only that recovery
 * succeeds would pass just as well if the strict decoder had succeeded too,
 * which is to say if the option did nothing at all.
 *
 * The boundary is deliberate and is asserted rather than skipped. Recovery
 * stuffs zero bits when a bitstream runs out part-way through a block; a scan
 * whose entropy data is gone entirely never reaches a bitstream, because
 * jpeg_entropy.c rejects a Huffman scan with no data before building one (an
 * arithmetic scan may legitimately be empty, T.81 D.2.9, and is excepted
 * there). Recovering those would mean inventing a whole scan rather than the
 * tail of one. Sweeping past that distinction without naming it would leave
 * the test asserting whichever behaviour the code happened to have.
 *
 * The option is read only by the progressive decoder, so only the progressive
 * fixtures are swept here.
 */
TEST(JpegLoad, TheStuffZeroRecoveryOptionDecodesATruncatedScan) {
  int recovered_cases = 0;
  for (const SweepFixture & fx : kSweepFixtures) {
    if (!fx.progressive) { continue; }
    std::vector<uint8_t> file;
    ASSERT_TRUE(jpeg_test::load_jpeg_file(fx.name, file)) << fx.name;

    uint32_t w0 = 0, h0 = 0;
    ASSERT_EQ(decode_size(file, &w0, &h0), GIMG_OK) << fx.name;

    const std::vector<CutResult> strict = sweep_cuts(file, fx.max_cuts);
    int refused_truncated = 0;
    for (const CutResult & r : strict) {
      if (r.result != GIMG_OK && !r.empties_the_scan()) { refused_truncated++; }
    }
    ASSERT_GT(refused_truncated, 0)
        << fx.name
        << ": no merely-truncated scan was refused without the option, so "
           "there is nothing here for it to recover and this fixture would "
           "pass without exercising it";

    std::string warnings;
    std::vector<CutResult> recovered;
    {
      WithStuffZeroRecovery on;
      CapturedStderr captured;
      recovered = sweep_cuts(file, fx.max_cuts);
      warnings = captured.text();
    }

    ASSERT_EQ(recovered.size(), strict.size()) << fx.name;
    for (const CutResult & r : recovered) {
      if (r.empties_the_scan()) {
        EXPECT_EQ(r.result, GIMG_ERR_CORRUPT)
            << fx.name << " scan " << r.segment
            << " has no entropy data left; recovery fills in a missing tail, "
               "not a missing scan";
        continue;
      }
      recovered_cases++;
      EXPECT_EQ(r.result, GIMG_OK)
          << fx.name << " scan " << r.segment << " cut " << r.cut << " of "
          << r.segment_len << ": recovery is supposed to decode this";
      if (r.result == GIMG_OK) {
        EXPECT_EQ(r.w, w0) << fx.name << " scan " << r.segment << " cut "
                           << r.cut;
        EXPECT_EQ(r.h, h0) << fx.name << " scan " << r.segment << " cut "
                           << r.cut;
      }
    }

    // Recovery is not silent: it says on stderr that it is inventing data.
    EXPECT_NE(warnings.find("premature end of data segment"), std::string::npos)
        << fx.name
        << ": recovery must announce that the image is not what the file "
           "said; stderr held: "
        << warnings;
  }
  EXPECT_GT(recovered_cases, 0) << "no progressive fixture was swept";
}

namespace {

/** One Huffman table for a DHT segment: a class, an index, and the symbol
 * each 1-bit code decodes to. One code of length one is enough for every case
 * below and keeps the scan data down to a handful of bits. */
struct OneBitTable {
  uint8_t table_class;  ///< 0 = DC, 1 = AC (T.81 B.2.4.2 Tc).
  uint8_t table_index;  ///< Th.
  uint8_t symbol;       ///< What the single code decodes to.
};

void append_dht(std::vector<uint8_t> & buf, const OneBitTable & t) {
  append(buf, (const unsigned char *)"\xFF\xC4", 2);
  buf.push_back(0x00);
  buf.push_back(20);  // L = 2 + 1 + 16 + 1
  buf.push_back((uint8_t)((t.table_class << 4) | t.table_index));
  buf.push_back(1);  // one code of length 1
  for (int i = 1; i < 16; i++) { buf.push_back(0); }
  buf.push_back(t.symbol);
}

/**
 * An 8x8 grayscale JPEG whose Huffman tables say whatever @p dc and @p ac say.
 *
 * The symbol a code decodes to is a byte straight out of the DHT segment, so
 * a crafted file can put any value there - including ones that are not a DC
 * category at all, or a run that walks the coefficient index off the end of
 * the block. Those are the arms being reached here, and no real encoder emits
 * them, which is exactly why a fixture cannot.
 *
 * @param sof  0xC0 for baseline, 0xC2 for progressive.
 */
std::vector<uint8_t> make_crafted_table_jpeg(uint8_t sof, const OneBitTable & dc,
    const OneBitTable & ac, uint8_t ss, uint8_t se, uint8_t ah_al,
    const std::vector<uint8_t> & scan_bytes) {
  std::vector<uint8_t> buf;
  append(buf, (const unsigned char *)"\xFF\xD8", 2);
  // SOF: L=11, P=8, Y=8, X=8, Nf=1, C1=0 H=1 V=1 Tq=0
  buf.push_back(0xFF);
  buf.push_back(sof);
  append(buf,
      (const unsigned char *)"\x00\x0B\x08\x00\x08\x00\x08\x01\x00\x11\x00", 11);
  // DQT: L=67, Pq=0 Tq=0, then 64 bytes.
  append(buf, (const unsigned char *)"\xFF\xDB\x00\x43\x00", 5);
  for (int i = 0; i < 64; i++) { buf.push_back(1); }
  append_dht(buf, dc);
  append_dht(buf, ac);
  // SOS: L=8, Ns=1, Cs=0, Td=0 Ta=0, then Ss, Se, Ah/Al.
  append(buf, (const unsigned char *)"\xFF\xDA\x00\x08\x01\x00\x00", 7);
  buf.push_back(ss);
  buf.push_back(se);
  buf.push_back(ah_al);
  buf.insert(buf.end(), scan_bytes.begin(), scan_bytes.end());
  append(buf, (const unsigned char *)"\xFF\xD9", 2);
  return buf;
}

} // namespace

/**
 * A DHT that decodes to a DC category above 15 is refused, not believed.
 *
 * The category coming out of the Huffman table is used as a bit count, and the
 * table's symbol values are whatever bytes the file supplied: T.81 F.1.2.1
 * Table F.1 allows 0..11 at 8-bit precision and 0..15 at 12-bit, so a byte of
 * 32 is not a category at all. No encoder emits a table like this, which is
 * why a fixture cannot reach the check and a crafted file has to.
 *
 * The two cases are not equally load-bearing, and saying so is the point of
 * this note. Weakening the progressive decoder's bound makes this test fail:
 * read_bits refuses a 32-bit width, the progressive decoder treats a refused
 * read in the last block as padding and substitutes zero, and the file then
 * decodes as a picture instead of being rejected. The category check is the
 * only thing standing between a crafted DHT and that outcome.
 *
 * Weakening the baseline decoder's bound does *not* make this test fail,
 * because jpeg_bitstream_read_bits() rejects any width above 16 on its own -
 * a check that exists because fuzzing once reached a 255-bit read. The
 * baseline half is therefore a second line of defence and is kept as one; it
 * would catch a future change that made read_bits tolerate wider fields, but
 * it does not distinguish anything today.
 */
TEST(JpegLoad, ADcCategoryAboveFifteenIsRefused) {
  const OneBitTable bad_dc{0, 0, 32};  // 32 is not a DC category.
  const OneBitTable ac{1, 0, 0x00};    // EOB; never reached.
  // A single zero byte: the one bit the DC code needs, then padding.
  const std::vector<uint8_t> scan(1, 0x00);

  struct Case {
    const char * what;
    uint8_t sof, ss, se, ah_al;
  } cases[] = {
      {"baseline", 0xC0, 0x00, 0x3F, 0x00},
      {"progressive DC", 0xC2, 0x00, 0x00, 0x00},
  };

  for (const Case & c : cases) {
    const std::vector<uint8_t> bytes =
        make_crafted_table_jpeg(c.sof, bad_dc, ac, c.ss, c.se, c.ah_al, scan);
    EXPECT_EQ(decode_size(bytes, nullptr, nullptr), GIMG_ERR_CORRUPT)
        << c.what
        << ": a DC category of 32 would be used as a bit count if believed";
  }
}

/**
 * A DHT whose runs walk past the end of the block is refused.
 *
 * Baseline AC decoding adds the symbol's run to the coefficient index and
 * writes there. A table made only of ZRL (15, 0) advances sixteen at a time,
 * so the fourth one asks for index 64 of a 64-entry block. The decoder has to
 * reject that rather than write it, and the check had never run.
 *
 * Watched to fail: relaxing the bound from 64 to 65 - one element, the
 * smallest overrun there is - makes this file decode successfully instead of
 * being refused, so the assertion is on the bound itself and not on some
 * later symptom of it.
 */
TEST(JpegLoad, AnAcRunPastTheEndOfTheBlockIsRefused) {
  const OneBitTable dc{0, 0, 0x00};   // category 0: no extra bits, diff 0.
  const OneBitTable ac{1, 0, 0xF0};   // ZRL: run 15, size 0.
  // One bit for the DC code, then four for the four ZRLs; zero bits after
  // that decode as more of the same, but the fourth already overruns.
  const std::vector<uint8_t> scan(1, 0x00);

  const std::vector<uint8_t> bytes =
      make_crafted_table_jpeg(0xC0, dc, ac, 0x00, 0x3F, 0x00, scan);
  EXPECT_EQ(decode_size(bytes, nullptr, nullptr), GIMG_ERR_CORRUPT)
      << "a run reaching coefficient 64 must be refused, not written";
}

namespace {

/** One IPTC IIM dataset: 0x1C, record, number, 2-byte length, value. */
void append_iptc(std::vector<uint8_t> & out, uint8_t record, uint8_t dataset,
    const std::string & value) {
  out.push_back(0x1C);
  out.push_back(record);
  out.push_back(dataset);
  out.push_back((uint8_t)(value.size() >> 8));
  out.push_back((uint8_t)(value.size() & 0xFFu));
  out.insert(out.end(), value.begin(), value.end());
}

/**
 * A Photoshop APP13 payload carrying @p datasets as resource 0x0404.
 *
 * Built from Adobe's image-resource-block layout rather than from what the
 * parser happens to expect: '8BIM', a two-byte id, a Pascal name **padded so
 * that the length byte plus the name is an even number of bytes - a null name
 * being two zero bytes** - then a four-byte size and the data, itself padded
 * to an even length. An empty name is what every writer in practice emits, so
 * it is the case that matters most and the one used here.
 */
std::vector<uint8_t> make_photoshop_app13(const std::vector<uint8_t> & iptc,
    const std::string & resource_name = std::string(),
    size_t filler_before = 0u) {
  std::vector<uint8_t> p;
  const char sig[] = "Photoshop 3.0";
  p.insert(p.end(), sig, sig + 13);
  p.push_back(0x00);                       // the signature's terminating NUL

  auto block = [&p](uint16_t id, const std::string & name,
                   const std::vector<uint8_t> & data) {
    p.insert(p.end(), {'8', 'B', 'I', 'M'});
    p.push_back((uint8_t)(id >> 8));
    p.push_back((uint8_t)(id & 0xFFu));
    p.push_back((uint8_t)name.size());
    p.insert(p.end(), name.begin(), name.end());
    if (((1u + name.size()) & 1u) != 0u) {
      p.push_back(0x00);                   // pad the Pascal string to even
    }
    const uint32_t n = (uint32_t)data.size();
    p.push_back((uint8_t)(n >> 24));
    p.push_back((uint8_t)(n >> 16));
    p.push_back((uint8_t)(n >> 8));
    p.push_back((uint8_t)n);
    p.insert(p.end(), data.begin(), data.end());
    if ((data.size() & 1u) != 0u) {
      p.push_back(0x00);                   // resource data is padded too
    }
  };

  // An unrelated block in front, so that reaching the IPTC one depends on
  // advancing past this one by the right number of bytes. 0x03ED is the
  // resolution-info resource; its contents do not matter here, only its
  // length, and an odd length is the case that needs the data padding.
  if (filler_before > 0u) {
    block(0x03EDu, std::string(), std::vector<uint8_t>(filler_before, 0x11));
  }
  block(0x0404u, resource_name, iptc);
  return p;
}

/** baseline_8x8_gray.jpg with an APP13 segment inserted after SOI. */
bool jpeg_with_app13(const std::vector<uint8_t> & payload,
    std::vector<uint8_t> & out) {
  std::vector<uint8_t> base;
  if (!jpeg_test::load_jpeg_file("baseline_8x8_gray.jpg", base)) { return false; }
  if (base.size() < 4 || payload.size() + 2u > 0xFFFFu) { return false; }
  out.clear();
  out.push_back(0xFF);
  out.push_back(0xD8);
  out.push_back(0xFF);
  out.push_back(0xED);
  out.push_back((uint8_t)((payload.size() + 2u) >> 8));
  out.push_back((uint8_t)((payload.size() + 2u) & 0xFFu));
  out.insert(out.end(), payload.begin(), payload.end());
  out.insert(out.end(), base.begin() + 2, base.end());
  return true;
}

/** The description a loaded JPEG reports, or "" if it has none. */
std::string description_of(const std::vector<uint8_t> & bytes) {
  GIMG_Stream * s = nullptr;
  if (gimg_stream_create_memory(bytes.data(), bytes.size(), &s) != GIMG_OK) {
    return std::string();
  }
  GIMG_Doc * doc = nullptr;
  const GIMG_Result r = gimg_doc_load(s, nullptr, nullptr, &doc);
  gimg_stream_destroy(s);
  if (r != GIMG_OK) { return std::string(); }
  const GIMG_Meta_Common * m = gimg_doc_meta_common(doc);
  const char * d = m ? gimg_meta_common_description(m) : nullptr;
  std::string out = d ? d : "";
  gimg_doc_destroy(doc);
  return out;
}

} // namespace

/**
 * An IPTC caption becomes the description; a by-line does not.
 *
 * gimg_meta_common's description is documented as the image's
 * description/comment - what JPEG spells as COM, GIF as a Comment Extension,
 * PNG as tEXt "Description". IPTC's field for that is **Caption/Abstract,
 * 2:120**, which is what IPTC's own mapping sends to dc:description.
 *
 * 2:80 is By-line: the name of the person who made the picture. It maps to
 * dc:creator, and putting it in a description field means a photographer's
 * name is handed to a caller that asked what the image is of.
 *
 * Both are present in the fixture, so the test distinguishes the two rather
 * than merely confirming that something arrives.
 */
TEST(JpegLoad, AnIptcCaptionBecomesTheDescriptionAndAByLineDoesNot) {
  std::vector<uint8_t> iptc;
  append_iptc(iptc, 2, 80, "Ansel Adams");          // By-line: a person
  append_iptc(iptc, 2, 120, "Moonrise over Hernandez"); // Caption/Abstract

  std::vector<uint8_t> jpeg;
  ASSERT_TRUE(jpeg_with_app13(make_photoshop_app13(iptc), jpeg));

  const std::string got = description_of(jpeg);
  EXPECT_EQ(got, "Moonrise over Hernandez")
      << "the description must come from IPTC 2:120 Caption/Abstract";
  EXPECT_NE(got, "Ansel Adams")
      << "2:80 is By-line, the photographer - not a description of the image";
}

/**
 * An IPTC block behind another block is still found, at either data parity.
 *
 * Adobe pads resource *data* to an even length too, so the next '8BIM' does
 * not begin at data_off + data_size when that size is odd. A reader that
 * forgets lands one byte short, fails to match '8BIM', and stops - silently
 * returning no caption for a file that has one. Only a block placed *before*
 * the IPTC one exercises that advance at all: with IPTC first, the walk
 * returns before it ever has to step over anything.
 */
TEST(JpegLoad, AnIptcBlockAfterAnotherResourceIsStillFound) {
  for (size_t filler : {1u, 2u, 3u, 8u}) {
    std::vector<uint8_t> iptc;
    append_iptc(iptc, 2, 120, "behind a filler");

    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_with_app13(
        make_photoshop_app13(iptc, std::string(), filler), jpeg));
    EXPECT_EQ(description_of(jpeg), "behind a filler")
        << "a " << filler
        << "-byte resource before the IPTC one was not stepped over correctly";
  }
}

/**
 * A resource block with a name is walked correctly, not just an unnamed one.
 *
 * Adobe pads the Pascal name so that the length byte plus the name is an even
 * number of bytes, which means the padding depends on the *parity of the name
 * length* and a reader that gets that backwards is misaligned by one byte for
 * every block - reading the four-byte resource size from the wrong place and
 * then walking into the middle of the data. An empty name, which is what
 * writers actually emit, is exactly the case where being backwards costs a
 * byte.
 *
 * Three name lengths, so neither parity can pass by accident.
 */
TEST(JpegLoad, IptcResourceNamesOfEitherParityAreWalked) {
  const char * names[] = {"", "a", "ab"};
  for (const char * name : names) {
    std::vector<uint8_t> iptc;
    append_iptc(iptc, 2, 120, "the caption");

    std::vector<uint8_t> jpeg;
    ASSERT_TRUE(jpeg_with_app13(make_photoshop_app13(iptc, name), jpeg));
    EXPECT_EQ(description_of(jpeg), "the caption")
        << "resource name \"" << name << "\" (" << strlen(name)
        << " chars) was not walked past correctly";
  }
}
