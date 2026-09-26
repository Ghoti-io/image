/*
 * Write one bilevel raster out in every CCITT variant libtiff can encode,
 * so that the TIFF decoder has something to be compared against for a
 * compression the libtiffpic sample set barely covers.
 *
 * The sample set holds two fax files. Both are Group 3, both one-dimensional,
 * and both FillOrder 2 - so three of the four things this codec decides about
 * a CCITT block (two-dimensional coding, Group 4, most-significant-bit fill
 * order, byte-aligned rows) are not in it at all. An axis a corpus cannot
 * vary is an axis its green line says nothing about, and the honest fix is
 * another encoder, not another assertion.
 *
 * This is not part of the library. It links libtiff inside the pinned oracle
 * image and is run by `make oracle-tools`; the files it writes are gitignored
 * for the same reason the sample set is, the pin being the image rather than
 * the bytes.
 *
 * Each variant is read back through libtiff before it is kept. A variant
 * libtiff cannot round-trip is libtiff's business, not a fixture, and one
 * written and never verified would be a reference whose contents nobody
 * checked.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tiffio.h>

#define FAX_WIDTH 2728u
#define FAX_HEIGHT 96u

/**
 * The pattern, chosen so that a decoder that gets any one mode wrong is
 * visibly wrong somewhere.
 *
 * A fax coder's behaviour is decided by run lengths and by how one row
 * relates to the one above it, so the rows here are: solid runs, runs of
 * every terminating length, runs long enough to need two makeup codes,
 * single-pixel alternation (the worst case for the changing-element array),
 * edges that move one, two and three pixels per row (vertical modes),
 * stripes that end above where the row below ends (pass mode) and a
 * deterministic scatter that is none of those.
 *
 * @return 1 for black.
 */
static int fax_pixel(unsigned x, unsigned y) {
  const unsigned w = FAX_WIDTH;
  switch (y % 12u) {
  case 0u:
    return 0; // All white: a single run the length of the line.
  case 1u:
    return 1; // All black: the same, in the other table.
  case 2u:
    return (int)(x & 1u); // Alternating: width changing elements in one row.
  case 3u:
    return x < 2600u; // One run past 2560, so two makeup codes and a
                      // terminating code.
  case 4u:
    return x >= w - 3u; // A short run hard against the right edge.
  case 5u:
    return (int)(((x + (y * 1u)) / 64u) & 1u); // Edges one pixel per row.
  case 6u:
    return (int)(((x + (y * 2u)) / 97u) & 1u); // ...two...
  case 7u:
    return (int)(((x + (y * 3u)) / 131u) & 1u); // ...and three: VR/VL(3).
  case 8u:
    return (int)((x / 64u) & 1u); // Stripes that do not move: V(0) throughout.
  case 9u:
    // Blocks whose edges end before the row below's do, which is what pass
    // mode is for.
    return (int)(((x / 37u) & 1u) && ((y / 12u) & 1u) == 0u);
  case 10u:
    return (int)(((x * 2654435761u) >> 28) < 5u); // Scatter: short runs.
  default:
    return (int)(x >= 1000u && x < 1728u); // A plain block, fax's own width.
  }
}

/** Fill one packed row, most significant bit first, 1 = black. */
static void fax_row(unsigned char * row, unsigned y, size_t row_bytes) {
  memset(row, 0, row_bytes);
  for (unsigned x = 0; x < FAX_WIDTH; x++) {
    if (fax_pixel(x, y)) {
      row[x >> 3] |= (unsigned char)(0x80u >> (x & 7u));
    }
  }
}

typedef struct {
  const char * name;
  uint16_t compression;
  uint32_t group_options;
  uint16_t fill_order;
  uint32_t rows_per_strip;
} fax_variant_t;

/** Write one variant. @return 0 on success. */
static int write_variant(
    const char * dir, const fax_variant_t * v, const unsigned char * src) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s.tif", dir, v->name);
  const size_t row_bytes = (FAX_WIDTH + 7u) / 8u;

  TIFF * out = TIFFOpen(path, "w");
  if (!out) {
    fprintf(stderr, "cannot write %s\n", path);
    return 1;
  }
  TIFFSetField(out, TIFFTAG_IMAGEWIDTH, (uint32_t)FAX_WIDTH);
  TIFFSetField(out, TIFFTAG_IMAGELENGTH, (uint32_t)FAX_HEIGHT);
  TIFFSetField(out, TIFFTAG_BITSPERSAMPLE, (uint16_t)1);
  TIFFSetField(out, TIFFTAG_SAMPLESPERPIXEL, (uint16_t)1);
  TIFFSetField(out, TIFFTAG_PHOTOMETRIC, (uint16_t)PHOTOMETRIC_MINISWHITE);
  TIFFSetField(out, TIFFTAG_PLANARCONFIG, (uint16_t)PLANARCONFIG_CONTIG);
  TIFFSetField(out, TIFFTAG_COMPRESSION, v->compression);
  TIFFSetField(out, TIFFTAG_FILLORDER, v->fill_order);
  TIFFSetField(out, TIFFTAG_ROWSPERSTRIP, v->rows_per_strip);
  if (v->compression == COMPRESSION_CCITTFAX3) {
    TIFFSetField(out, TIFFTAG_GROUP3OPTIONS, v->group_options);
  }
  else if (v->compression == COMPRESSION_CCITTFAX4) {
    TIFFSetField(out, TIFFTAG_GROUP4OPTIONS, v->group_options);
  }
  for (unsigned y = 0; y < FAX_HEIGHT; y++) {
    // TIFFWriteScanline may modify its buffer, so it never sees the source.
    unsigned char scratch[(FAX_WIDTH + 7u) / 8u];
    memcpy(scratch, src + ((size_t)y * row_bytes), row_bytes);
    if (TIFFWriteScanline(out, scratch, y, 0) < 0) {
      fprintf(stderr, "%s: libtiff refused to encode row %u\n", v->name, y);
      TIFFClose(out);
      remove(path);
      return 1;
    }
  }
  TIFFClose(out);

  // Read it back with the same library. A file that does not come back as
  // what went in is not a reference for anything.
  TIFF * back = TIFFOpen(path, "r");
  if (!back) {
    fprintf(stderr, "%s: cannot reopen\n", v->name);
    return 1;
  }
  int bad = 0;
  for (unsigned y = 0; y < FAX_HEIGHT && !bad; y++) {
    unsigned char got[(FAX_WIDTH + 7u) / 8u];
    memset(got, 0, row_bytes);
    if (TIFFReadScanline(back, got, y, 0) < 0 ||
        memcmp(got, src + ((size_t)y * row_bytes), row_bytes) != 0) {
      fprintf(stderr, "%s: libtiff does not round-trip row %u\n", v->name, y);
      bad = 1;
    }
  }
  TIFFClose(back);
  if (bad) {
    remove(path);
    return 1;
  }
  printf("wrote %s.tif\n", v->name);
  return 0;
}

int main(int argc, char ** argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <output directory>\n", argv[0]);
    return 2;
  }
  const char * dir = argv[1];
  const size_t row_bytes = (FAX_WIDTH + 7u) / 8u;
  unsigned char * src = calloc(FAX_HEIGHT, row_bytes);
  if (!src) {
    return 2;
  }
  for (unsigned y = 0; y < FAX_HEIGHT; y++) {
    fax_row(src + ((size_t)y * row_bytes), y, row_bytes);
  }

  // GROUP3OPT_2DENCODING is bit 0, GROUP3OPT_FILLBITS bit 2. The names are
  // libtiff's; the bits are T4Options as TIFF 6.0 defines it.
  const fax_variant_t variants[] = {
      {"fax_g3_1d_msb", COMPRESSION_CCITTFAX3, 0u, FILLORDER_MSB2LSB,
          FAX_HEIGHT},
      {"fax_g3_1d_lsb", COMPRESSION_CCITTFAX3, 0u, FILLORDER_LSB2MSB,
          FAX_HEIGHT},
      {"fax_g3_2d_msb", COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING,
          FILLORDER_MSB2LSB, FAX_HEIGHT},
      {"fax_g3_2d_lsb", COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING,
          FILLORDER_LSB2MSB, FAX_HEIGHT},
      {"fax_g3_1d_fill", COMPRESSION_CCITTFAX3, GROUP3OPT_FILLBITS,
          FILLORDER_MSB2LSB, FAX_HEIGHT},
      {"fax_g3_2d_fill", COMPRESSION_CCITTFAX3,
          GROUP3OPT_2DENCODING | GROUP3OPT_FILLBITS, FILLORDER_MSB2LSB,
          FAX_HEIGHT},
      {"fax_g4_msb", COMPRESSION_CCITTFAX4, 0u, FILLORDER_MSB2LSB, FAX_HEIGHT},
      {"fax_g4_lsb", COMPRESSION_CCITTFAX4, 0u, FILLORDER_LSB2MSB, FAX_HEIGHT},
      // Several strips: each one begins against an imaginary white line
      // again, which a decoder that carries the reference across blocks gets
      // wrong from the second strip on.
      {"fax_g4_strips", COMPRESSION_CCITTFAX4, 0u, FILLORDER_MSB2LSB, 8u},
      {"fax_g3_2d_strips", COMPRESSION_CCITTFAX3, GROUP3OPT_2DENCODING,
          FILLORDER_MSB2LSB, 8u},
      {"fax_rle_msb", COMPRESSION_CCITTRLE, 0u, FILLORDER_MSB2LSB, FAX_HEIGHT},
      {"fax_rle_lsb", COMPRESSION_CCITTRLE, 0u, FILLORDER_LSB2MSB, FAX_HEIGHT},
      {"fax_rle_strips", COMPRESSION_CCITTRLE, 0u, FILLORDER_MSB2LSB, 8u},
      // The same raster with no compression at all: what every variant above
      // has to decode to, checkable without asking libtiff anything.
      {"fax_reference", COMPRESSION_NONE, 0u, FILLORDER_MSB2LSB, FAX_HEIGHT},
  };

  int failures = 0;
  for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]); i++) {
    failures += write_variant(dir, &variants[i], src);
  }
  free(src);
  if (failures) {
    fprintf(stderr, "%d of %zu variants failed\n", failures,
        sizeof(variants) / sizeof(variants[0]));
    return 1;
  }
  return 0;
}
