/**
 * @file
 *
 * Exif IFD bounds: offsets read from the file must not wrap a bounds check.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>
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

namespace {

/** Read a JPEG from the shared fixture directory. */
bool read_fixture(const char * name, std::vector<uint8_t> & out) {
  std::string path = std::string(GIMG_TEST_DATA_JPEG) + "/" + name;
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) { return false; }
  const std::streamsize n = f.tellg();
  if (n <= 0) { return false; }
  out.resize((size_t)n);
  f.seekg(0);
  return (bool)f.read((char *)out.data(), n);
}

/** Offset of the SOS marker (0xFF 0xDA), or 0 if there is none. */
size_t find_sos(const std::vector<uint8_t> & jpg) {
  for (size_t i = 2; i + 1 < jpg.size(); i++) {
    if (jpg[i] == 0xFF && jpg[i + 1] == 0xDA) { return i; }
  }
  return 0;
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
std::vector<uint8_t> make_exif_with_tiff_jpeg_thumbnail(
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
  u16(GIMG_EXIF_TAG_COMPRESSION); u16(GIMG_EXIF_TYPE_SHORT); u32(1u);
  u16(7u); e.push_back(0); e.push_back(0);
  entry(GIMG_EXIF_TAG_STRIP_OFFSETS, GIMG_EXIF_TYPE_LONG, 1u, strip_off);
  entry(GIMG_EXIF_TAG_STRIP_BYTE_COUNTS, GIMG_EXIF_TYPE_LONG, 1u,
      (uint32_t)strip.size());
  entry(GIMG_EXIF_TAG_JPEG_TABLES, GIMG_EXIF_TYPE_UNDEFINED,
      (uint32_t)tables.size(), tables_off);
  u32(0);                 // no IFD2
  e.insert(e.end(), tables.begin(), tables.end());
  e.insert(e.end(), strip.begin(), strip.end());
  return e;
}

} // namespace

/**
 * A thumbnail stored as JPEGTables plus a strip is put back together.
 *
 * This is the TIFF/EP form of an embedded thumbnail, and the whole reassembly
 * - stripping the SOI and EOI that bracket the hoisted tables, skipping a
 * leading SOI on the strip, and emitting one SOI in front of the join - had
 * never executed. Neither had the tag parsing that finds JPEGTables at all.
 *
 * The assertion is byte equality with the JPEG the two halves were cut from.
 * A weaker check - that the result is non-empty, or starts with FFD8 - would
 * pass against a reassembly that dropped the tables, duplicated the SOI, or
 * kept the EOI in the middle of the file, which are precisely the mistakes
 * this code is arranged to avoid.
 */
TEST(ExifTiffJpegThumbnail, JpegTablesAndStripAreRejoinedIntoTheOriginal) {
  std::vector<uint8_t> jpg;
  ASSERT_TRUE(read_fixture("baseline_8x8_gray.jpg", jpg));
  const size_t sos = find_sos(jpg);
  ASSERT_GT(sos, 2u) << "fixture has no SOS to split at";

  // Tables: SOI, everything up to the scan header, EOI.
  std::vector<uint8_t> tables(jpg.begin(), jpg.begin() + (long)sos);
  tables.push_back(0xFF);
  tables.push_back(0xD9);

  // Writers differ on whether the strip repeats the SOI. Both are met in the
  // wild and they take different branches, so both are swept: one has to be
  // skipped so the join does not end up with two, the other must not have two
  // bytes taken off the front of its scan header.
  const bool strip_leads_with_soi[] = {false, true};
  for (bool with_soi : strip_leads_with_soi) {
    std::vector<uint8_t> strip;
    if (with_soi) { strip.push_back(0xFF); strip.push_back(0xD8); }
    strip.insert(strip.end(), jpg.begin() + (long)sos, jpg.end());

    const std::vector<uint8_t> e =
        make_exif_with_tiff_jpeg_thumbnail(tables, strip);

    void * out_p = nullptr;
    size_t out_n = 0;
    ASSERT_EQ(gimg_exif_embedded_thumbnail_tiff_jpeg(
                  nullptr, e.data(), e.size(), &out_p, &out_n),
        GIMG_OK)
        << "strip with leading SOI: " << with_soi;
    ASSERT_NE(out_p, nullptr) << "the tables/strip form produced no thumbnail";
    const std::vector<uint8_t> got((uint8_t *)out_p, (uint8_t *)out_p + out_n);
    free(out_p);

    ASSERT_EQ(got.size(), jpg.size())
        << "strip with leading SOI: " << with_soi
        << ": reassembled length differs from the file it was cut from";
    EXPECT_TRUE(got == jpg)
        << "strip with leading SOI: " << with_soi
        << ": the rejoined thumbnail is not the JPEG the halves came from";
  }
}

/**
 * Without a JPEGTables field the strip is handed back as it stands.
 *
 * The same tag is the switch between two quite different behaviours, so the
 * other side is asserted too: with no tables to splice in there is nothing to
 * reassemble, and the strip is already a whole JPEG. Testing only the join
 * would leave the branch that decides between them untested in the direction
 * that does nothing.
 */
TEST(ExifTiffJpegThumbnail, WithNoJpegTablesTheStripIsReturnedUnchanged) {
  std::vector<uint8_t> jpg;
  ASSERT_TRUE(read_fixture("baseline_8x8_gray.jpg", jpg));

  // One-byte JPEGTables: below the two the reassembly needs, so it is ignored.
  const std::vector<uint8_t> tables(1, 0x00);
  const std::vector<uint8_t> e = make_exif_with_tiff_jpeg_thumbnail(tables, jpg);

  void * out_p = nullptr;
  size_t out_n = 0;
  ASSERT_EQ(gimg_exif_embedded_thumbnail_tiff_jpeg(
                nullptr, e.data(), e.size(), &out_p, &out_n),
      GIMG_OK);
  ASSERT_NE(out_p, nullptr);
  const std::vector<uint8_t> got((uint8_t *)out_p, (uint8_t *)out_p + out_n);
  free(out_p);
  EXPECT_TRUE(got == jpg) << "the strip should come back as it went in";
}

namespace {

/**
 * An Exif blob whose IFD1 holds an UNCOMPRESSED thumbnail, in @p strips.
 *
 * TIFF stores raster data in strips, and a thumbnail may be split across
 * several of them: StripOffsets and StripByteCounts then carry one value per
 * strip, out of line, and the reader concatenates. A single strip is the
 * degenerate case - one value, small enough to sit inside the tag's own value
 * field - and takes a different branch on every read. Both are built here.
 *
 * @p long_offsets picks LONG or SHORT for the two arrays, which is a third
 * branch again: TIFF allows either, and a reader that assumes LONG reads a
 * SHORT array at double stride and concatenates whatever it lands on.
 */
std::vector<uint8_t> make_exif_uncompressed_thumbnail(uint32_t w, uint32_t h,
    uint16_t bits, uint16_t photometric,
    const std::vector<std::vector<uint8_t>> & strips, bool long_offsets) {
  std::vector<uint8_t> e;
  auto u16 = [&e](uint16_t v) {
    e.push_back((uint8_t)(v & 0xFF)); e.push_back((uint8_t)(v >> 8));
  };
  auto u32 = [&e](uint32_t v) {
    for (int i = 0; i < 4; i++) { e.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); }
  };
  auto entry_long = [&](uint16_t tag, uint32_t count, uint32_t val) {
    u16(tag); u16(GIMG_EXIF_TYPE_LONG); u32(count); u32(val);
  };
  auto entry_short = [&](uint16_t tag, uint16_t val) {
    u16(tag); u16(GIMG_EXIF_TYPE_SHORT); u32(1u); u16(val);
    e.push_back(0); e.push_back(0);
  };

  const uint16_t n = (uint16_t)strips.size();
  const uint32_t stride = long_offsets ? 4u : 2u;
  const uint32_t ifd0_off = 8u;
  const uint32_t ifd1_off = ifd0_off + 2u + 4u;
  const uint32_t after_ifd1 = ifd1_off + 2u + 7u * 12u + 4u;
  // With one strip the value fits in the tag; with more it is an array.
  const uint32_t offs_array = after_ifd1;
  const uint32_t counts_array = offs_array + (n > 1 ? n * stride : 0u);
  const uint32_t data_start = counts_array + (n > 1 ? n * stride : 0u);

  std::vector<uint32_t> offsets, counts;
  uint32_t at = data_start;
  for (const std::vector<uint8_t> & s : strips) {
    offsets.push_back(at);
    counts.push_back((uint32_t)s.size());
    at += (uint32_t)s.size();
  }

  e.push_back('I'); e.push_back('I');
  u16(42);
  u32(ifd0_off);
  u16(0);
  u32(ifd1_off);
  u16(7);  // IFD1: seven entries, tags ascending.
  entry_long(GIMG_EXIF_TAG_IMAGE_WIDTH, 1u, w);
  entry_long(GIMG_EXIF_TAG_IMAGE_LENGTH, 1u, h);
  entry_short(GIMG_EXIF_TAG_BITS_PER_SAMPLE, bits);
  entry_short(GIMG_EXIF_TAG_COMPRESSION, 1u);  // 1 = uncompressed
  entry_short(GIMG_EXIF_TAG_PHOTOMETRIC_INTERPRETATION, photometric);
  const uint16_t arr_type =
      long_offsets ? (uint16_t)GIMG_EXIF_TYPE_LONG : (uint16_t)GIMG_EXIF_TYPE_SHORT;
  if (n > 1) {
    u16(GIMG_EXIF_TAG_STRIP_OFFSETS); u16(arr_type); u32(n); u32(offs_array);
    u16(GIMG_EXIF_TAG_STRIP_BYTE_COUNTS); u16(arr_type); u32(n);
    u32(counts_array);
  }
  else if (long_offsets) {
    entry_long(GIMG_EXIF_TAG_STRIP_OFFSETS, 1u, offsets[0]);
    entry_long(GIMG_EXIF_TAG_STRIP_BYTE_COUNTS, 1u, counts[0]);
  }
  else {
    entry_short(GIMG_EXIF_TAG_STRIP_OFFSETS, (uint16_t)offsets[0]);
    entry_short(GIMG_EXIF_TAG_STRIP_BYTE_COUNTS, (uint16_t)counts[0]);
  }
  u32(0);  // no IFD2

  if (n > 1) {
    for (uint32_t v : offsets) { if (long_offsets) { u32(v); } else { u16((uint16_t)v); } }
    for (uint32_t v : counts) { if (long_offsets) { u32(v); } else { u16((uint16_t)v); } }
  }
  for (const std::vector<uint8_t> & s : strips) {
    e.insert(e.end(), s.begin(), s.end());
  }
  return e;
}

} // namespace

/**
 * An uncompressed thumbnail is read whether it is in one strip or several.
 *
 * TIFF may split raster data across strips, and Exif thumbnails in the wild
 * are written both ways. The multi-strip arms had never run: every read of the
 * offset and count arrays, in both LONG and SHORT widths, and the loop that
 * concatenates. A reader that mishandles any of those still returns *a*
 * thumbnail, of the right length in some cases, so the assertion is on the
 * bytes - each strip carries a distinct fill, and the expected result is their
 * concatenation in order.
 */
TEST(ExifUncompressedThumbnail, StripsAreConcatenatedInOrder) {
  struct Case {
    const char * what;
    int strips;
    bool long_offsets;
  } cases[] = {
      {"one strip, LONG", 1, true},
      {"one strip, SHORT", 1, false},
      {"three strips, LONG", 3, true},
      {"three strips, SHORT", 3, false},
  };

  for (const Case & c : cases) {
    std::vector<std::vector<uint8_t>> strips;
    std::vector<uint8_t> expected;
    for (int i = 0; i < c.strips; i++) {
      // A distinct fill per strip, so an out-of-order or repeated copy shows.
      const std::vector<uint8_t> s(6u, (uint8_t)(0xA0 + i));
      strips.push_back(s);
      expected.insert(expected.end(), s.begin(), s.end());
    }
    const std::vector<uint8_t> e = make_exif_uncompressed_thumbnail(
        6u, (uint32_t)c.strips, 8u, 1u, strips, c.long_offsets);

    uint32_t w = 0, h = 0;
    uint8_t bits = 0;
    uint16_t photometric = 0xFFFF;
    void * out_p = nullptr;
    size_t out_n = 0;
    ASSERT_EQ(gimg_exif_embedded_thumbnail_uncompressed(nullptr, e.data(),
                  e.size(), &w, &h, &bits, &photometric, &out_p, &out_n),
        GIMG_OK)
        << c.what;
    ASSERT_NE(out_p, nullptr) << c.what << ": no thumbnail came back";
    const std::vector<uint8_t> got((uint8_t *)out_p, (uint8_t *)out_p + out_n);
    free(out_p);

    EXPECT_EQ(w, 6u) << c.what;
    EXPECT_EQ(h, (uint32_t)c.strips) << c.what;
    EXPECT_EQ(bits, 8u) << c.what;
    EXPECT_EQ(photometric, 1u) << c.what;
    ASSERT_EQ(got.size(), expected.size()) << c.what;
    EXPECT_TRUE(got == expected)
        << c.what << ": the strips did not come back in order, or at all";
  }
}

namespace {

/** A minimal Exif blob whose IFD0 says nothing but the orientation. */
std::vector<uint8_t> make_exif_with_orientation(uint16_t orientation) {
  std::vector<uint8_t> e;
  auto u16 = [&e](uint16_t v) {
    e.push_back((uint8_t)(v & 0xFF)); e.push_back((uint8_t)(v >> 8));
  };
  auto u32 = [&e](uint32_t v) {
    for (int i = 0; i < 4; i++) { e.push_back((uint8_t)((v >> (8 * i)) & 0xFF)); }
  };
  e.push_back('I'); e.push_back('I');
  u16(42);
  u32(8);
  u16(1);
  u16(GIMG_EXIF_TAG_ORIENTATION); u16(GIMG_EXIF_TYPE_SHORT); u32(1u);
  u16(orientation); e.push_back(0); e.push_back(0);
  u32(0);
  return e;
}

} // namespace

/**
 * Each thumbnail writer carries the orientation over from the base Exif.
 *
 * The three builders exist to put a thumbnail into a blob that a camera's
 * metadata already described, and each documents that it copies the
 * orientation across when it is given a base to copy from. In the suite as it
 * stood, every one of them had been called exactly once and always with no
 * usable base, so the branch that writes an IFD0 entry at all had never run -
 * the built blobs all had an empty IFD0 and nothing noticed, because nothing
 * asked.
 *
 * Each case is a round trip: build, then read back through the matching
 * reader, and require the thumbnail to come out as the bytes that went in.
 * A writer can be wrong in ways that leave the file superficially plausible -
 * an IFD1 offset that points a few bytes off, a count that does not match the
 * entries - and a reader that has been tested against hand-built blobs is a
 * genuinely independent check of it.
 */
TEST(ExifThumbnailWriters, OrientationAndThumbnailSurviveARoundTrip) {
  std::vector<uint8_t> jpg;
  ASSERT_TRUE(read_fixture("baseline_8x8_gray.jpg", jpg));
  const std::vector<uint8_t> base =
      make_exif_with_orientation(GIMG_ORIENTATION_ROTATE_90_CW);

  // --- Compression = 6: IFD1 points at a whole JPEG. ---
  {
    void * built = nullptr;
    size_t built_n = 0;
    ASSERT_EQ(gimg_exif_build_with_thumbnail_jpeg(nullptr, base.data(),
                  base.size(), jpg.data(), jpg.size(), &built, &built_n),
        GIMG_OK);
    ASSERT_NE(built, nullptr);
    const std::vector<uint8_t> blob(
        (uint8_t *)built, (uint8_t *)built + built_n);

    GIMG_Orientation o = GIMG_ORIENTATION_UNKNOWN;
    EXPECT_EQ(gimg_exif_parse_orientation(blob.data(), blob.size(), &o),
        GIMG_OK);
    EXPECT_EQ(o, GIMG_ORIENTATION_ROTATE_90_CW)
        << "the JPEG thumbnail writer dropped the base orientation";

    const void * thumb = nullptr;
    size_t thumb_n = 0;
    ASSERT_EQ(gimg_exif_embedded_thumbnail_jpeg(
                  blob.data(), blob.size(), &thumb, &thumb_n),
        GIMG_OK);
    ASSERT_EQ(thumb_n, jpg.size());
    EXPECT_EQ(memcmp(thumb, jpg.data(), jpg.size()), 0)
        << "the thumbnail did not survive the round trip";
    free(built);
  }

  // --- Compression = 7: tables hoisted out, entropy data in the strip. ---
  {
    void * built = nullptr;
    size_t built_n = 0;
    ASSERT_EQ(gimg_exif_build_with_thumbnail_tiff_jpeg(nullptr, base.data(),
                  base.size(), jpg.data(), jpg.size(), &built, &built_n),
        GIMG_OK);
    ASSERT_NE(built, nullptr);
    const std::vector<uint8_t> blob(
        (uint8_t *)built, (uint8_t *)built + built_n);
    free(built);

    GIMG_Orientation o = GIMG_ORIENTATION_UNKNOWN;
    EXPECT_EQ(gimg_exif_parse_orientation(blob.data(), blob.size(), &o),
        GIMG_OK);
    EXPECT_EQ(o, GIMG_ORIENTATION_ROTATE_90_CW)
        << "the TechNote-2 writer dropped the base orientation";

    void * thumb = nullptr;
    size_t thumb_n = 0;
    ASSERT_EQ(gimg_exif_embedded_thumbnail_tiff_jpeg(
                  nullptr, blob.data(), blob.size(), &thumb, &thumb_n),
        GIMG_OK);
    ASSERT_NE(thumb, nullptr);
    const std::vector<uint8_t> got((uint8_t *)thumb, (uint8_t *)thumb + thumb_n);
    free(thumb);
    EXPECT_TRUE(got == jpg)
        << "what this writer stored did not read back as the JPEG it was given";
  }

  // --- Compression = 1: raw pixels in a strip. ---
  //
  // One sample per pixel and three take different branches in the writer and
  // in the reader alike, because BitsPerSample is one value for grayscale and
  // three for RGB - and three SHORTs do not fit in a tag's value field, so it
  // goes out of line and the reader has to follow a pointer to find it. The
  // RGB half of that had never run at either end.
  struct PixelCase {
    const char * what;
    uint16_t samples;
    uint16_t photometric;  ///< 1 = BlackIsZero, 2 = RGB.
  } pixel_cases[] = {
      {"grayscale", 1u, 1u},
      {"RGB", 3u, 2u},
  };

  for (const PixelCase & pc : pixel_cases) {
    const uint32_t w_in = 4u, h_in = 2u;
    std::vector<uint8_t> pixels((size_t)w_in * h_in * pc.samples);
    // Every byte distinct, so a reordered or truncated strip shows up.
    for (size_t i = 0; i < pixels.size(); i++) { pixels[i] = (uint8_t)(i * 11 + 3); }

    void * built = nullptr;
    size_t built_n = 0;
    ASSERT_EQ(gimg_exif_build_with_thumbnail_uncompressed(nullptr, base.data(),
                  base.size(), pixels.data(), pixels.size(), w_in, h_in,
                  pc.samples, 8u, &built, &built_n),
        GIMG_OK)
        << pc.what;
    ASSERT_NE(built, nullptr) << pc.what;
    const std::vector<uint8_t> blob(
        (uint8_t *)built, (uint8_t *)built + built_n);
    free(built);

    GIMG_Orientation o = GIMG_ORIENTATION_UNKNOWN;
    EXPECT_EQ(gimg_exif_parse_orientation(blob.data(), blob.size(), &o),
        GIMG_OK)
        << pc.what;
    EXPECT_EQ(o, GIMG_ORIENTATION_ROTATE_90_CW)
        << pc.what << ": the uncompressed writer dropped the base orientation";

    uint32_t w = 0, h = 0;
    uint8_t bits = 0;
    uint16_t photometric = 0xFFFF;
    void * thumb = nullptr;
    size_t thumb_n = 0;
    ASSERT_EQ(gimg_exif_embedded_thumbnail_uncompressed(nullptr, blob.data(),
                  blob.size(), &w, &h, &bits, &photometric, &thumb, &thumb_n),
        GIMG_OK)
        << pc.what;
    ASSERT_NE(thumb, nullptr) << pc.what;
    const std::vector<uint8_t> got((uint8_t *)thumb, (uint8_t *)thumb + thumb_n);
    free(thumb);
    EXPECT_EQ(w, w_in) << pc.what;
    EXPECT_EQ(h, h_in) << pc.what;
    EXPECT_EQ(bits, 8u) << pc.what
                        << ": BitsPerSample did not read back, which for RGB "
                           "means the out-of-line value was not followed";
    EXPECT_EQ(photometric, pc.photometric)
        << pc.what << ": the colour interpretation did not survive";
    EXPECT_TRUE(got == pixels)
        << pc.what << ": the strip did not survive the round trip";
  }
}

/**
 * The uncompressed writer refuses what it cannot represent, and says which.
 *
 * It supports one or three samples at eight bits, which covers the grayscale
 * and RGB thumbnails Exif defines, and it distinguishes "this is a format I do
 * not write" from "you passed me arguments that do not describe anything".
 * Both matter to a caller: the first is a reason to convert, the second is a
 * bug. None of these arms had run, so nothing held the distinction in place.
 */
TEST(ExifThumbnailWriters, TheUncompressedWriterRejectsWhatItCannotStore) {
  const std::vector<uint8_t> pixels(8, 0x40);
  void * out_p = nullptr;
  size_t out_n = 0;

  // Two samples per pixel is not a thing Exif stores this way.
  EXPECT_EQ(gimg_exif_build_with_thumbnail_uncompressed(nullptr, nullptr, 0,
                pixels.data(), pixels.size(), 4u, 1u, 2u, 8u, &out_p, &out_n),
      GIMG_ERR_UNSUPPORTED)
      << "two samples per pixel is unsupported, not invalid";

  // Sixteen bits per sample likewise: a real format, not one written here.
  EXPECT_EQ(gimg_exif_build_with_thumbnail_uncompressed(nullptr, nullptr, 0,
                pixels.data(), pixels.size(), 4u, 1u, 1u, 16u, &out_p, &out_n),
      GIMG_ERR_UNSUPPORTED)
      << "16 bits per sample is unsupported, not invalid";

  // A strip that is not width*height*samples describes no image at all.
  EXPECT_EQ(gimg_exif_build_with_thumbnail_uncompressed(nullptr, nullptr, 0,
                pixels.data(), pixels.size(), 4u, 4u, 1u, 8u, &out_p, &out_n),
      GIMG_ERR_INTERNAL)
      << "a strip that does not match the dimensions is the caller's bug";

  // And the null-argument guards, which every one of these shares.
  EXPECT_EQ(gimg_exif_build_with_thumbnail_uncompressed(nullptr, nullptr, 0,
                nullptr, 8u, 4u, 2u, 1u, 8u, &out_p, &out_n),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_exif_build_with_thumbnail_jpeg(
                nullptr, nullptr, 0, nullptr, 8u, &out_p, &out_n),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_exif_build_with_thumbnail_tiff_jpeg(
                nullptr, nullptr, 0, nullptr, 8u, &out_p, &out_n),
      GIMG_ERR_INTERNAL);
  EXPECT_EQ(gimg_exif_embedded_thumbnail_tiff_jpeg(
                nullptr, nullptr, 8u, &out_p, &out_n),
      GIMG_ERR_INTERNAL);
}

/**
 * A thumbnail whose dimensions cannot be read falls back to 160x120.
 *
 * The JPEG writers take the thumbnail's size from its SOF marker, and write
 * a default when there is no marker to read. Exif 2.3 names 160x120 as the
 * usual thumbnail size, so that is what goes in - but a wrong default is
 * recorded in the file as though it were measured, and until now no test had
 * made the writer use it.
 */
TEST(ExifThumbnailWriters, WithNoReadableSofTheThumbnailSizeDefaults) {
  // A JPEG that is not one: enough bytes to store, no SOF to measure.
  const std::vector<uint8_t> not_a_jpeg(32, 0x00);

  void * built = nullptr;
  size_t built_n = 0;
  ASSERT_EQ(gimg_exif_build_with_thumbnail_jpeg(nullptr, nullptr, 0,
                not_a_jpeg.data(), not_a_jpeg.size(), &built, &built_n),
      GIMG_OK);
  ASSERT_NE(built, nullptr);
  const std::vector<uint8_t> blob((uint8_t *)built, (uint8_t *)built + built_n);
  free(built);

  // IFD1 carries the dimensions; find them by walking rather than by offset.
  const uint32_t ifd0 = exif_test::exif_u32(blob, 4);
  const uint16_t n0 = exif_test::exif_u16(blob, ifd0);
  const uint32_t ifd1 = exif_test::exif_u32(blob, ifd0 + 2u + n0 * 12u);
  ASSERT_GT(ifd1, 0u) << "the writer produced no IFD1";
  const uint16_t n1 = exif_test::exif_u16(blob, ifd1);
  uint32_t width = 0, height = 0;
  for (uint16_t i = 0; i < n1; i++) {
    const size_t off = ifd1 + 2u + (size_t)i * 12u;
    const uint16_t tag = exif_test::exif_u16(blob, off);
    if (tag == GIMG_EXIF_TAG_IMAGE_WIDTH) { width = exif_test::exif_u32(blob, off + 8); }
    if (tag == GIMG_EXIF_TAG_IMAGE_LENGTH) { height = exif_test::exif_u32(blob, off + 8); }
  }
  EXPECT_EQ(width, 160u) << "Exif 2.3's default thumbnail width";
  EXPECT_EQ(height, 120u) << "Exif 2.3's default thumbnail height";
}
