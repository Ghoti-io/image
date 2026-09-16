/**
 * @file
 *
 * Exif IFD bounds: offsets read from the file must not wrap a bounds check.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <vector>

#include "../../src/meta/exif_internal.h"

namespace {

/**
 * A little-endian TIFF header whose IFD0 pointer is @a ifd0_offset.
 *
 * "II", 42, then the four-byte offset of IFD0, then enough bytes to look like
 * a small IFD. The offset is the only thing that varies.
 */
std::vector<unsigned char> TiffWithIfd0Offset(uint32_t ifd0_offset) {
  std::vector<unsigned char> b = {'I', 'I', 42, 0, 0, 0, 0, 0};
  b[4] = static_cast<unsigned char>(ifd0_offset & 0xFFu);
  b[5] = static_cast<unsigned char>((ifd0_offset >> 8) & 0xFFu);
  b[6] = static_cast<unsigned char>((ifd0_offset >> 16) & 0xFFu);
  b[7] = static_cast<unsigned char>((ifd0_offset >> 24) & 0xFFu);
  b.resize(64, 0);
  return b;
}

} // namespace

TEST(ExifBounds, AnIfdOffsetNearTheTopOfItsRangeIsRejectedNotFollowed) {
  // An IFD offset is a full 32 bits. Checking "offset + 2 > size" in 32-bit
  // arithmetic lets 0xFFFFFFFF + 2 wrap to 1, which is smaller than any real
  // buffer, so the check passed and the parser read from buf + 0xFFFFFFFF.
  // A fuzz corpus input did exactly this and segfaulted in read_u16.
  const uint32_t offsets[] = {
      0xFFFFFFFFu, 0xFFFFFFFEu, 0xFFFFFFFDu, 0x80000000u, 0x7FFFFFFFu, 1000u};
  for (uint32_t off : offsets) {
    std::vector<unsigned char> tiff = TiffWithIfd0Offset(off);
    GIMG_Orientation orientation = GIMG_ORIENTATION_UNKNOWN;
    GIMG_Result r =
        gimg_exif_parse_orientation(tiff.data(), tiff.size(), &orientation);
    EXPECT_EQ(r, GIMG_ERR_CORRUPT)
        << "IFD0 offset 0x" << std::hex << off
        << " lies outside a " << std::dec << tiff.size() << " byte buffer";
  }
}

TEST(ExifBounds, AnEntryCountThatOverrunsTheBufferIsRejected) {
  // The second check has the same shape: offset + 2 + entries * 12 + 4, with
  // up to 65535 entries, overflows 32 bits for a large enough offset.
  std::vector<unsigned char> tiff = TiffWithIfd0Offset(8);
  tiff[8] = 0xFFu; // 65535 entries, far more than the buffer holds
  tiff[9] = 0xFFu;
  GIMG_Orientation orientation = GIMG_ORIENTATION_UNKNOWN;
  EXPECT_EQ(gimg_exif_parse_orientation(tiff.data(), tiff.size(), &orientation),
      GIMG_ERR_CORRUPT);
}

TEST(ExifBounds, AWellFormedOrientationStillReads) {
  // The checks above must not have made every file corrupt: one real entry,
  // tag 0x0112 (orientation), type SHORT, count 1, value 6.
  std::vector<unsigned char> tiff = TiffWithIfd0Offset(8);
  tiff[8] = 1; // one entry
  tiff[9] = 0;
  const unsigned char entry[12] = {
      0x12, 0x01, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0};
  std::memcpy(tiff.data() + 10, entry, sizeof(entry));
  GIMG_Orientation orientation = GIMG_ORIENTATION_UNKNOWN;
  ASSERT_EQ(gimg_exif_parse_orientation(tiff.data(), tiff.size(), &orientation),
      GIMG_OK);
  EXPECT_EQ(orientation, GIMG_ORIENTATION_ROTATE_90_CW);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
