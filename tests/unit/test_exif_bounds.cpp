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
#include "../exif_test_utils.h"

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


namespace {

// A blob exercising every structure gimg_exif_strip_gps() claims to re-point:
// an out-of-line ASCII payload in IFD0, an Exif sub-IFD, an Interoperability
// IFD below that, an IFD1 thumbnail addressed by the 0x0201/0x0202 pair, and
// the GPS IFD with its own out-of-line rationals.  The blob built by the
// writer tests has none of these, so the compaction path they cover is only
// the trivial one: two inline SHORTs and nothing to move.
constexpr char kMake[] = "Ghoti.io";          // 9 bytes with its NUL
constexpr uint8_t kThumb[] = {0xFF, 0xD8, 0xFF, 0xDB, 0x11, 0x22, 0x33, 0x44,
    0x55, 0x66, 0xFF, 0xD9};

void put16(std::vector<uint8_t> & v, size_t at, uint16_t x) {
  v[at] = (uint8_t)(x & 0xFF);
  v[at + 1] = (uint8_t)(x >> 8);
}
void put32(std::vector<uint8_t> & v, size_t at, uint32_t x) {
  for (int i = 0; i < 4; i++) { v[at + (size_t)i] = (uint8_t)(x >> (8 * i)); }
}
uint16_t get16(const std::vector<uint8_t> & v, size_t at) {
  return (uint16_t)(v[at] | (v[at + 1] << 8));
}
uint32_t get32(const std::vector<uint8_t> & v, size_t at) {
  return (uint32_t)v[at] | ((uint32_t)v[at + 1] << 8) |
      ((uint32_t)v[at + 2] << 16) | ((uint32_t)v[at + 3] << 24);
}

/** Write one 12-byte IFD entry at `at`. */
void put_entry(std::vector<uint8_t> & v, size_t at, uint16_t tag, uint16_t type,
    uint32_t count, uint32_t value) {
  put16(v, at, tag);
  put16(v, at + 2, type);
  put32(v, at + 4, count);
  put32(v, at + 8, value);
}

std::vector<uint8_t> make_rich_exif() {
  const uint32_t ifd0 = 8u;
  const uint32_t exif_ifd = ifd0 + 2u + 4u * 12u + 4u;      // IFD0 has 4
  const uint32_t interop = exif_ifd + 2u + 2u * 12u + 4u;   // Exif has 2
  const uint32_t ifd1 = interop + 2u + 1u * 12u + 4u;       // Interop has 1
  const uint32_t gps = ifd1 + 2u + 2u * 12u + 4u;           // IFD1 has 2
  const uint32_t make_off = gps + 2u + 2u * 12u + 4u;       // GPS has 2
  const uint32_t thumb_off = make_off + (uint32_t)sizeof(kMake);
  const uint32_t rat_off = thumb_off + (uint32_t)sizeof(kThumb);
  const uint32_t total = rat_off + 24u;

  std::vector<uint8_t> v(total, 0);
  v[0] = 'I';
  v[1] = 'I';
  put16(v, 2, 42);
  put32(v, 4, ifd0);

  put16(v, ifd0, 4);
  put_entry(v, ifd0 + 2, 0x010Fu, 2u, (uint32_t)sizeof(kMake), make_off);
  put_entry(v, ifd0 + 14, 0x0112u, 3u, 1u, 1u);
  put_entry(v, ifd0 + 26, 0x8769u, 4u, 1u, exif_ifd);
  put_entry(v, ifd0 + 38, 0x8825u, 4u, 1u, gps);
  put32(v, ifd0 + 50, ifd1);

  put16(v, exif_ifd, 2);
  put_entry(v, exif_ifd + 2, 0x9000u, 7u, 4u, 0x30333230u); // "0230" inline
  put_entry(v, exif_ifd + 14, 0xA005u, 4u, 1u, interop);
  put32(v, exif_ifd + 26, 0);

  put16(v, interop, 1);
  put_entry(v, interop + 2, 0x0001u, 2u, 4u, 0x00383952u); // "R98\0" inline
  put32(v, interop + 14, 0);

  put16(v, ifd1, 2);
  put_entry(v, ifd1 + 2, 0x0201u, 4u, 1u, thumb_off);
  put_entry(v, ifd1 + 14, 0x0202u, 4u, 1u, (uint32_t)sizeof(kThumb));
  put32(v, ifd1 + 26, 0);

  put16(v, gps, 2);
  put_entry(v, gps + 2, 0x0001u, 2u, 2u, (uint32_t)'N');
  put_entry(v, gps + 14, 0x0002u, 5u, 3u, rat_off);
  put32(v, gps + 26, 0);

  std::memcpy(&v[make_off], kMake, sizeof(kMake));
  std::memcpy(&v[thumb_off], kThumb, sizeof(kThumb));
  const uint32_t lat[6] = {51u, 1u, 30u, 1u, 26u, 1u};
  for (int i = 0; i < 6; i++) { put32(v, rat_off + (size_t)i * 4u, lat[i]); }
  return v;
}

/** Entry index of `tag` in the IFD at `ifd`, or -1. */
int find_entry(const std::vector<uint8_t> & v, uint32_t ifd, uint16_t tag) {
  const uint16_t n = get16(v, ifd);
  for (uint16_t i = 0; i < n; i++) {
    if (get16(v, ifd + 2u + (size_t)i * 12u) == tag) { return (int)i; }
  }
  return -1;
}
uint32_t entry_value(const std::vector<uint8_t> & v, uint32_t ifd, uint16_t tag) {
  const int i = find_entry(v, ifd, tag);
  return i < 0 ? 0u : get32(v, ifd + 2u + (size_t)i * 12u + 8u);
}

} // namespace

/**
 * Stripping GPS re-points everything else instead of leaving it dangling.
 *
 * The writer-level tests use a blob of two inline SHORTs, where compaction
 * has nothing to move and every offset in the output happens to be right.
 * This one gives the rebuilder each structure it claims to handle and then
 * walks the result, so an offset written to the wrong place is a failure
 * rather than a coincidence that held.
 */
TEST(ExifStripGps, RepointsSubIfdsThumbnailAndPayloads) {
  const std::vector<uint8_t> in = make_rich_exif();
  void * out_p = nullptr;
  size_t out_n = 0;
  ASSERT_EQ(gimg_exif_strip_gps(nullptr, in.data(), in.size(), &out_p, &out_n),
      GIMG_OK);
  ASSERT_NE(out_p, nullptr);
  std::vector<uint8_t> out((uint8_t *)out_p, (uint8_t *)out_p + out_n);
  free(out_p);

  ASSERT_GE(out.size(), 8u);
  EXPECT_EQ(out[0], 'I');
  EXPECT_EQ(out[1], 'I');
  EXPECT_EQ(get16(out, 2), 42u) << "the byte-order mark must survive";

  const uint32_t ifd0 = get32(out, 4);
  ASSERT_LT(ifd0 + 2u, out.size());
  EXPECT_EQ(get16(out, ifd0), 3u) << "GPS removed, the other three kept";
  EXPECT_EQ(find_entry(out, ifd0, 0x8825u), -1) << "GPS pointer still present";

  // The out-of-line ASCII payload must have moved and still read correctly.
  const int make_i = find_entry(out, ifd0, 0x010Fu);
  ASSERT_GE(make_i, 0);
  const uint32_t make_off = entry_value(out, ifd0, 0x010Fu);
  ASSERT_LE((size_t)make_off + sizeof(kMake), out.size())
      << "Make payload points outside the blob";
  EXPECT_EQ(std::memcmp(&out[make_off], kMake, sizeof(kMake)), 0);

  // Exif sub-IFD, and Interoperability below it.
  const uint32_t ex = entry_value(out, ifd0, 0x8769u);
  ASSERT_NE(ex, 0u);
  ASSERT_LT((size_t)ex + 2u, out.size()) << "Exif sub-IFD points outside";
  EXPECT_EQ(get16(out, ex), 2u);
  const uint32_t io = entry_value(out, ex, 0xA005u);
  ASSERT_NE(io, 0u);
  ASSERT_LT((size_t)io + 2u, out.size()) << "Interop IFD points outside";
  EXPECT_EQ(get16(out, io), 1u);
  EXPECT_EQ(entry_value(out, io, 0x0001u), 0x00383952u) << "R98 inline value";

  // IFD1 through IFD0's next pointer, and its thumbnail bytes.
  const uint32_t ifd1 = get32(out, ifd0 + 2u + 3u * 12u);
  ASSERT_NE(ifd1, 0u) << "IFD1 must still be chained";
  ASSERT_LT((size_t)ifd1 + 2u, out.size());
  EXPECT_EQ(get16(out, ifd1), 2u);
  const uint32_t toff = entry_value(out, ifd1, 0x0201u);
  const uint32_t tlen = entry_value(out, ifd1, 0x0202u);
  EXPECT_EQ(tlen, (uint32_t)sizeof(kThumb));
  ASSERT_LE((size_t)toff + tlen, out.size())
      << "thumbnail points outside the blob";
  EXPECT_EQ(std::memcmp(&out[toff], kThumb, sizeof(kThumb)), 0)
      << "thumbnail bytes changed";

  // And the coordinates are gone, not merely unreferenced.
  std::vector<uint8_t> needle(24, 0);
  const uint32_t lat[6] = {51u, 1u, 30u, 1u, 26u, 1u};
  for (int i = 0; i < 6; i++) { put32(needle, (size_t)i * 4u, lat[i]); }
  bool leaked = false;
  for (size_t i = 0; i + needle.size() <= out.size(); i++) {
    if (std::memcmp(&out[i], needle.data(), needle.size()) == 0) {
      leaked = true;
      break;
    }
  }
  EXPECT_FALSE(leaked) << "the latitude rationals are still in the blob";
}

/** Nothing to strip: the blob comes back unchanged, byte for byte. */
TEST(ExifStripGps, ABlobWithNoGpsComesBackIdentical) {
  std::vector<uint8_t> in = make_rich_exif();
  // Turn the GPS pointer into an unrelated tag so the rest stays put.
  const uint32_t ifd0 = get32(in, 4);
  put16(in, ifd0 + 2u + 3u * 12u, 0x9286u); // UserComment, harmless
  void * out_p = nullptr;
  size_t out_n = 0;
  ASSERT_EQ(gimg_exif_strip_gps(nullptr, in.data(), in.size(), &out_p, &out_n),
      GIMG_OK);
  ASSERT_NE(out_p, nullptr);
  const bool same = out_n == in.size() && std::memcmp(out_p, in.data(), out_n) == 0;
  free(out_p);
  EXPECT_TRUE(same) << "with no GPS present the contract is an exact copy";
}

namespace {

/** The same picture's metadata, written both ways round. */
struct ByteOrderCase {
  const char * name;
  bool little_endian;
};

const ByteOrderCase kByteOrders[] = {
    {"little-endian (II)", true},
    {"big-endian (MM)", false},
};

} // namespace

/**
 * Exif in either byte order is read, not just "II".
 *
 * TIFF 6.0 section 2 gives the header two legal spellings, "II" for
 * little-endian and "MM" for big-endian, and cameras write both. Every read in
 * exif.c already takes a byte-order flag and has both arms written out, so the
 * support is there - but each entry point checked the magic number *before*
 * working out the byte order, and checked it only in its little-endian
 * spelling (`buf[2] != 42 || buf[3] != 0`). In a big-endian header those two
 * bytes are 00 2A, so every "MM" blob was rejected as corrupt at the door and
 * none of the big-endian arms below had ever run.
 *
 * This is written as a differential rather than as a big-endian assertion on
 * its own: the two blobs describe the same picture, so the answer has to be
 * the same, and comparing them catches a byte-order bug that a single-sided
 * test would have to know the right answer in advance to notice.
 */
TEST(ExifByteOrder, OrientationReadsTheSameEitherWayRound) {
  GIMG_Orientation seen[2];
  for (int i = 0; i < 2; i++) {
    const std::vector<uint8_t> e =
        exif_test::make_exif_with_gps(kByteOrders[i].little_endian);
    seen[i] = GIMG_ORIENTATION_UNKNOWN;
    EXPECT_EQ(gimg_exif_parse_orientation(e.data(), e.size(), &seen[i]),
        GIMG_OK)
        << kByteOrders[i].name << " was refused";
  }
  EXPECT_EQ(seen[0], GIMG_ORIENTATION_NORMAL)
      << "the fixture sets Orientation = 1";
  EXPECT_EQ(seen[1], seen[0])
      << "the same metadata in the other byte order read differently";
}

/**
 * Stripping GPS works on a big-endian blob and keeps it big-endian.
 *
 * The rebuild has to carry the source's byte order through: an output whose
 * header says "MM" but whose fields are little-endian is not readable by
 * anything, and would pass a test that only asked whether the GPS tag was
 * gone. So this walks the result with an inspector that reads the mark out of
 * the blob rather than assuming one.
 */
TEST(ExifByteOrder, StrippingGpsKeepsTheSourceByteOrder) {
  for (const ByteOrderCase & bo : kByteOrders) {
    const std::vector<uint8_t> e = exif_test::make_exif_with_gps(bo.little_endian);
    ASSERT_TRUE(exif_test::exif_has_gps_tag(e)) << bo.name << ": bad fixture";
    ASSERT_EQ(exif_test::exif_ifd0_entry_count(e), 3) << bo.name;

    void * out_p = nullptr;
    size_t out_n = 0;
    ASSERT_EQ(gimg_exif_strip_gps(nullptr, e.data(), e.size(), &out_p, &out_n),
        GIMG_OK)
        << bo.name << ": the policy could not be applied";
    ASSERT_NE(out_p, nullptr);
    const std::vector<uint8_t> out(
        (uint8_t *)out_p, (uint8_t *)out_p + out_n);
    free(out_p);

    // The byte-order mark survives, and so does the magic, in its spelling.
    EXPECT_EQ(out[0], bo.little_endian ? 'I' : 'M') << bo.name;
    EXPECT_EQ(out[1], bo.little_endian ? 'I' : 'M') << bo.name;
    EXPECT_EQ(out[2], bo.little_endian ? 42 : 0) << bo.name << ": magic";
    EXPECT_EQ(out[3], bo.little_endian ? 0 : 42) << bo.name << ": magic";

    // Read back through the blob's own mark: GPS gone, the rest still there.
    EXPECT_FALSE(exif_test::exif_has_gps_tag(out)) << bo.name;
    EXPECT_EQ(exif_test::exif_ifd0_entry_count(out), 2)
        << bo.name << ": the other two IFD0 entries must survive";

    // And the library still agrees it is a readable blob saying the same thing.
    GIMG_Orientation o = GIMG_ORIENTATION_UNKNOWN;
    EXPECT_EQ(gimg_exif_parse_orientation(out.data(), out.size(), &o), GIMG_OK)
        << bo.name << ": the stripped blob no longer parses";
    EXPECT_EQ(o, GIMG_ORIENTATION_NORMAL) << bo.name;
  }
}

/**
 * A header whose byte-order mark is neither "II" nor "MM" is refused.
 *
 * The blob this builds is the dangerous shape, not merely a malformed one: a
 * big-endian body, the little-endian spelling of the magic number, and a mark
 * of "XY". The old code tested the magic first and in that spelling only, then
 * set the flag with `h[0] == 'I' && h[1] == 'I'` - false for anything that is
 * not "II" - so this file passed the door, was read as big-endian throughout,
 * and came back as perfectly good metadata. Nothing in the file said it was
 * big-endian. The code inferred it from the absence of "II".
 *
 * So the assertion is that it is *refused*, and the case is built to fail
 * against the old behaviour rather than to be refused by a bounds check on the
 * way past: with the mark restored to "MM" the very same bytes parse, which is
 * the control that says the refusal is about the mark and nothing else.
 */
TEST(ExifByteOrder, AHeaderWithNoRecognizedByteOrderMarkIsRefused) {
  std::vector<uint8_t> e = exif_test::make_exif_with_gps(false); // big-endian
  // The little-endian spelling of 42, which is what the old gate demanded.
  e[2] = 42;
  e[3] = 0;
  e[0] = 'X';
  e[1] = 'Y';

  GIMG_Orientation o = GIMG_ORIENTATION_UNKNOWN;
  EXPECT_EQ(gimg_exif_parse_orientation(e.data(), e.size(), &o),
      GIMG_ERR_CORRUPT)
      << "a mark of 'XY' is not a byte order; reading the file as big-endian "
         "because it is not 'II' is a guess, and it produced metadata";

  void * out_p = nullptr;
  size_t out_n = 0;
  EXPECT_EQ(gimg_exif_strip_gps(nullptr, e.data(), e.size(), &out_p, &out_n),
      GIMG_ERR_CORRUPT)
      << "the same header must be refused by every entry point, not just one";
  if (out_p) { free(out_p); }

  // The control: the only thing wrong with those bytes was the mark.
  std::vector<uint8_t> good = exif_test::make_exif_with_gps(false);
  GIMG_Orientation o2 = GIMG_ORIENTATION_UNKNOWN;
  EXPECT_EQ(gimg_exif_parse_orientation(good.data(), good.size(), &o2), GIMG_OK)
      << "same body, real mark: this must parse, or the test above is only "
         "rejecting a blob that was broken for some other reason";
  EXPECT_EQ(o2, GIMG_ORIENTATION_NORMAL);
}
