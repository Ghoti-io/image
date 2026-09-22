/**
 * @file
 *
 * Building and inspecting small Exif blobs, for the metadata-policy tests.
 *
 * Shared because the same blob has to go through two writers: the PNG eXIf
 * chunk and the JPEG APP1 segment run separate copies of the policy code, and
 * a helper per file would let the two drift.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GIMG_TESTS_EXIF_TEST_UTILS_H
#define GHOTI_IO_GIMG_TESTS_EXIF_TEST_UTILS_H

#include <cstdint>
#include <cstring>
#include <vector>

namespace exif_test {

/**
 * A real little-endian Exif blob: IFD0 carries Orientation, ResolutionUnit
 * and a GPS IFD pointer (0x8825); the GPS IFD carries GPSLatitudeRef and a
 * GPSLatitude rational triple.
 *
 * This is built here rather than taken from a fixture because the fixture
 * that the metadata-policy tests were using, png_exif.png, holds a six-byte
 * eXIf payload of 00 01 02 03 04 05. That is below GIMG_EXIF_MIN_SIZE, so
 * gimg_exif_strip_gps() rejected it on entry and png_save.c - which uses the
 * stripped blob only `if (... == GIMG_OK)` - wrote the original through. The
 * policy tests passed because nothing happened.
 */
inline std::vector<uint8_t> make_exif_with_gps() {
  std::vector<uint8_t> e;
  auto u16 = [&e](uint16_t v) {
    e.push_back((uint8_t)(v & 0xFF));
    e.push_back((uint8_t)(v >> 8));
  };
  auto u32 = [&e](uint32_t v) {
    for (int i = 0; i < 4; i++) { e.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); }
  };
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t val) {
    u16(tag); u16(type); u32(count); u32(val);
  };
  // TIFF header: little-endian, magic 42, IFD0 at offset 8.
  e.push_back('I'); e.push_back('I');
  u16(42);
  u32(8);
  // IFD0: three entries.  Tags must ascend.
  const uint32_t gps_ifd_off = 8u + 2u + 3u * 12u + 4u; // = 50
  u16(3);
  entry(0x0112u, 3u, 1u, 1u);            // Orientation = 1 (SHORT, inline)
  entry(0x0128u, 3u, 1u, 2u);            // ResolutionUnit = inch
  entry(0x8825u, 4u, 1u, gps_ifd_off);   // GPS IFD pointer (LONG)
  u32(0);                                 // no IFD1
  // GPS IFD: two entries, then the rational payload it points at.
  const uint32_t rational_off = gps_ifd_off + 2u + 2u * 12u + 4u; // = 80
  u16(2);
  entry(0x0001u, 2u, 2u, (uint32_t)('N')); // GPSLatitudeRef "N\0" inline
  entry(0x0002u, 5u, 3u, rational_off);    // GPSLatitude, 3 rationals
  u32(0);
  const uint32_t lat[6] = {51u, 1u, 30u, 1u, 26u, 1u}; // 51 deg 30' 26"
  for (int i = 0; i < 6; i++) { u32(lat[i]); }
  return e;
}

inline uint32_t png_crc(const uint8_t * p, size_t n) {
  static uint32_t table[256];
  static bool built = false;
  if (!built) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) { c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1); }
      table[i] = c;
    }
    built = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) { c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8); }
  return c ^ 0xFFFFFFFFu;
}

/** Replace the eXIf chunk payload of a PNG, fixing length and CRC. */
inline bool replace_exif_chunk(std::vector<uint8_t> & png,
    const std::vector<uint8_t> & exif) {
  size_t i = 8;
  while (i + 12 <= png.size()) {
    uint32_t len = ((uint32_t)png[i] << 24) | ((uint32_t)png[i + 1] << 16) |
        ((uint32_t)png[i + 2] << 8) | (uint32_t)png[i + 3];
    const bool is_exif = std::memcmp(&png[i + 4], "eXIf", 4) == 0;
    if (is_exif) {
      std::vector<uint8_t> out(png.begin(), png.begin() + (long)i);
      const uint32_t n = (uint32_t)exif.size();
      out.push_back((uint8_t)(n >> 24)); out.push_back((uint8_t)(n >> 16));
      out.push_back((uint8_t)(n >> 8));  out.push_back((uint8_t)n);
      const size_t crc_start = out.size();
      out.insert(out.end(), {'e', 'X', 'I', 'f'});
      out.insert(out.end(), exif.begin(), exif.end());
      const uint32_t crc = png_crc(&out[crc_start], 4 + exif.size());
      out.push_back((uint8_t)(crc >> 24)); out.push_back((uint8_t)(crc >> 16));
      out.push_back((uint8_t)(crc >> 8));  out.push_back((uint8_t)crc);
      out.insert(out.end(), png.begin() + (long)(i + 12 + len), png.end());
      png.swap(out);
      return true;
    }
    i += 12 + len;
  }
  return false;
}

/** Does this Exif blob's IFD0 carry a GPS IFD pointer? */
inline bool exif_has_gps_tag(const std::vector<uint8_t> & e) {
  if (e.size() < 14) { return false; }
  const uint32_t ifd0 = (uint32_t)e[4] | ((uint32_t)e[5] << 8) |
      ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
  if (ifd0 + 2u > e.size()) { return false; }
  const uint16_t n = (uint16_t)(e[ifd0] | (e[ifd0 + 1] << 8));
  for (uint16_t k = 0; k < n; k++) {
    const size_t off = ifd0 + 2u + (size_t)k * 12u;
    if (off + 12u > e.size()) { return false; }
    if ((uint16_t)(e[off] | (e[off + 1] << 8)) == 0x8825u) { return true; }
  }
  return false;
}

/** Count IFD0 entries, so "GPS gone" can be told from "everything gone". */
inline int exif_ifd0_entry_count(const std::vector<uint8_t> & e) {
  if (e.size() < 14) { return -1; }
  const uint32_t ifd0 = (uint32_t)e[4] | ((uint32_t)e[5] << 8) |
      ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
  if (ifd0 + 2u > e.size()) { return -1; }
  return (int)(uint16_t)(e[ifd0] | (e[ifd0 + 1] << 8));
}


/** Replace a JPEG's APP1 "Exif\0\0" payload, fixing the segment length. */
inline bool replace_jpeg_exif(std::vector<uint8_t> & jpg,
    const std::vector<uint8_t> & exif) {
  size_t i = 2;
  while (i + 4 < jpg.size()) {
    if (jpg[i] != 0xFF) { i++; continue; }
    const uint8_t m = jpg[i + 1];
    if (m == 0xDA || m == 0xD9) { return false; }
    const size_t len = ((size_t)jpg[i + 2] << 8) | jpg[i + 3];
    if (m == 0xE1 && i + 10 <= jpg.size() &&
        std::memcmp(&jpg[i + 4], "Exif\0\0", 6) == 0) {
      const size_t seg = 2u + 6u + exif.size();
      if (seg > 0xFFFFu) { return false; }
      std::vector<uint8_t> out(jpg.begin(), jpg.begin() + (long)(i + 2));
      out.push_back((uint8_t)(seg >> 8));
      out.push_back((uint8_t)(seg & 0xFF));
      const char * tag = "Exif\0\0";
      out.insert(out.end(), tag, tag + 6);
      out.insert(out.end(), exif.begin(), exif.end());
      out.insert(out.end(), jpg.begin() + (long)(i + 2 + len), jpg.end());
      jpg.swap(out);
      return true;
    }
    i += 2 + len;
  }
  return false;
}

} // namespace exif_test

#endif
