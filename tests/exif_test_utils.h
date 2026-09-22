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
 * A real Exif blob: IFD0 carries Orientation, ResolutionUnit and a GPS IFD
 * pointer (0x8825); the GPS IFD carries GPSLatitudeRef and a GPSLatitude
 * rational triple.
 *
 * @p little_endian picks the TIFF byte order - "II" or "MM". Both are legal
 * (TIFF 6.0 section 2) and both are written by cameras in the field, and the
 * two blobs describe the same image, so anything that reads one must agree
 * with what it reads from the other.
 *
 * This is built here rather than taken from a fixture because the fixture
 * that the metadata-policy tests were using, png_exif.png, holds a six-byte
 * eXIf payload of 00 01 02 03 04 05. That is below GIMG_EXIF_MIN_SIZE, so
 * gimg_exif_strip_gps() rejected it on entry and png_save.c - which uses the
 * stripped blob only `if (... == GIMG_OK)` - wrote the original through. The
 * policy tests passed because nothing happened.
 */
inline std::vector<uint8_t> make_exif_with_gps(bool little_endian = true) {
  std::vector<uint8_t> e;
  auto u16 = [&e, little_endian](uint16_t v) {
    if (little_endian) {
      e.push_back((uint8_t)(v & 0xFF));
      e.push_back((uint8_t)(v >> 8));
    }
    else {
      e.push_back((uint8_t)(v >> 8));
      e.push_back((uint8_t)(v & 0xFF));
    }
  };
  auto u32 = [&e, little_endian](uint32_t v) {
    for (int i = 0; i < 4; i++) {
      const int shift = little_endian ? (8 * i) : (24 - 8 * i);
      e.push_back((uint8_t)((v >> shift) & 0xFF));
    }
  };
  // An entry whose value field holds an OFFSET, or a LONG that fills all four
  // bytes: the value is a 32-bit number and is written as one.
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t val) {
    u16(tag); u16(type); u32(count); u32(val);
  };
  // An entry whose value is small enough to sit INSIDE the value field.
  //
  // TIFF 6.0: a payload of four bytes or fewer is stored in the value field
  // itself, left-justified - so a SHORT occupies the first two bytes and the
  // last two are padding, in BOTH byte orders. Writing it through the 32-bit
  // writer instead puts it in the high half of a big-endian word, where the
  // reader looks at the first two bytes and finds zero. That is a fixture bug
  // that looks exactly like a library bug, and it was one here first.
  auto entry_short = [&](uint16_t tag, uint16_t val) {
    u16(tag); u16(3u); u32(1u); u16(val);
    e.push_back(0); e.push_back(0);
  };
  // TIFF header: the byte-order mark, magic 42, IFD0 at offset 8.
  e.push_back(little_endian ? 'I' : 'M');
  e.push_back(little_endian ? 'I' : 'M');
  u16(42);
  u32(8);
  // IFD0: three entries.  Tags must ascend.
  const uint32_t gps_ifd_off = 8u + 2u + 3u * 12u + 4u; // = 50
  u16(3);
  entry_short(0x0112u, 1u);              // Orientation = 1 (SHORT, inline)
  entry_short(0x0128u, 2u);              // ResolutionUnit = inch
  entry(0x8825u, 4u, 1u, gps_ifd_off);   // GPS IFD pointer (LONG)
  u32(0);                                 // no IFD1
  // GPS IFD: two entries, then the rational payload it points at.
  const uint32_t rational_off = gps_ifd_off + 2u + 2u * 12u + 4u; // = 80
  u16(2);
  // GPSLatitudeRef "N\0": an ASCII payload of 2 bytes is inline, and an inline
  // payload is left-justified in the value field in both byte orders - so it
  // is written as bytes, not as a number that u32 would reorder.
  u16(0x0001u); u16(2u); u32(2u);
  e.push_back('N'); e.push_back(0); e.push_back(0); e.push_back(0);
  entry(0x0002u, 5u, 3u, rational_off);    // GPSLatitude, 3 rationals
  u32(0);
  const uint32_t lat[6] = {51u, 1u, 30u, 1u, 26u, 1u}; // 51 deg 30' 26"
  for (int i = 0; i < 6; i++) { u32(lat[i]); }
  return e;
}

/**
 * An Exif blob whose IFD1 holds a thumbnail in TIFF/EP "new-style" JPEG form.
 *
 * Exif 2.3 / TIFF TechNote 2 allow a thumbnail to be stored the way a TIFF
 * strip is - Compression = 7, the quantization and Huffman tables hoisted out
 * into a JPEGTables field (0x015B), and the entropy-coded remainder in the
 * strip that StripOffsets and StripByteCounts point at. Rebuilding a decodable
 * JPEG means putting them back together, which is a different code path from
 * the ordinary case where IFD1 simply points at a whole JPEG file.
 *
 * The two halves here are cut from a real JPEG at its SOS marker, so a correct
 * reassembly reproduces that file byte for byte - which is a far stronger
 * check than "the output starts with FFD8".
 */
inline std::vector<uint8_t> make_exif_with_tiff_jpeg_thumbnail(
    const std::vector<uint8_t> & tables, const std::vector<uint8_t> & strip) {
  std::vector<uint8_t> e;
  auto u16 = [&e](uint16_t v) {
    e.push_back((uint8_t)(v & 0xFF)); e.push_back((uint8_t)(v >> 8));
  };
  auto u32 = [&e](uint32_t v) {
    for (int i = 0; i < 4; i++) { e.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); }
  };
  auto entry = [&](uint16_t tag, uint16_t type, uint32_t count, uint32_t val) {
    u16(tag); u16(type); u32(count); u32(val);
  };

  // Layout: header(8) IFD0(2 + 0*12 + 4) IFD1(2 + 4*12 + 4) tables strip
  const uint32_t ifd0_off = 8u;
  const uint32_t ifd1_off = ifd0_off + 2u + 4u;
  const uint32_t tables_off = ifd1_off + 2u + 4u * 12u + 4u;
  const uint32_t strip_off = tables_off + (uint32_t)tables.size();

  e.push_back('I'); e.push_back('I');
  u16(42);
  u32(ifd0_off);
  u16(0);                 // IFD0: no entries...
  u32(ifd1_off);          // ...and IFD1 follows.
  u16(4);                 // IFD1: four entries, tags ascending.
  // Compression = 7 is inline and left-justified, so it is two bytes and two
  // of padding - not a 32-bit 7.
  u16(0x0103u); u16(3u); u32(1u);
  u16(7u); e.push_back(0); e.push_back(0);
  entry(0x0111u, 4u, 1u, strip_off);
  entry(0x0117u, 4u, 1u,
      (uint32_t)strip.size());
  entry(0x015Bu, 7u,
      (uint32_t)tables.size(), tables_off);
  u32(0);                 // no IFD2
  e.insert(e.end(), tables.begin(), tables.end());
  e.insert(e.end(), strip.begin(), strip.end());
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

/** Read this blob's own byte-order mark, so an inspector cannot assume one. */
inline bool exif_is_le(const std::vector<uint8_t> & e) {
  return e.size() >= 2 && e[0] == 'I' && e[1] == 'I';
}

inline uint16_t exif_u16(const std::vector<uint8_t> & e, size_t off) {
  return exif_is_le(e) ? (uint16_t)(e[off] | (e[off + 1] << 8))
                       : (uint16_t)((e[off] << 8) | e[off + 1]);
}

inline uint32_t exif_u32(const std::vector<uint8_t> & e, size_t off) {
  if (exif_is_le(e)) {
    return (uint32_t)e[off] | ((uint32_t)e[off + 1] << 8) |
        ((uint32_t)e[off + 2] << 16) | ((uint32_t)e[off + 3] << 24);
  }
  return ((uint32_t)e[off] << 24) | ((uint32_t)e[off + 1] << 16) |
      ((uint32_t)e[off + 2] << 8) | (uint32_t)e[off + 3];
}

/** Does this Exif blob's IFD0 carry a GPS IFD pointer? */
inline bool exif_has_gps_tag(const std::vector<uint8_t> & e) {
  if (e.size() < 14) { return false; }
  const uint32_t ifd0 = exif_u32(e, 4);
  if (ifd0 + 2u > e.size()) { return false; }
  const uint16_t n = exif_u16(e, ifd0);
  for (uint16_t k = 0; k < n; k++) {
    const size_t off = ifd0 + 2u + (size_t)k * 12u;
    if (off + 12u > e.size()) { return false; }
    if (exif_u16(e, off) == 0x8825u) { return true; }
  }
  return false;
}

/** Count IFD0 entries, so "GPS gone" can be told from "everything gone". */
inline int exif_ifd0_entry_count(const std::vector<uint8_t> & e) {
  if (e.size() < 14) { return -1; }
  const uint32_t ifd0 = exif_u32(e, 4);
  if (ifd0 + 2u > e.size()) { return -1; }
  return (int)exif_u16(e, ifd0);
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
