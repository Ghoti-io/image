/*
 * Reference pixel dumper: decode a BMP with bmplib (Rupert Weber's, stock and
 * unmodified) and write the raster as packed RGBA8 for tests/data/bmp/
 * bmpsuite_sweep.py to compare against.
 *
 * bmplib is the only decoder reachable from here that reads OS/2 Huffman 1D,
 * OS/2 bitmap arrays and 64-bit BMPs, which is why it is worth building from
 * source when the three installed oracles are a package install away.  It is
 * LGPL/GPL and is therefore used as a separate process, never linked into the
 * library.  See documentation/formats/bmp.md.
 *
 * Build with `make oracle-build oracle-tools`, which expects the source tree and its
 * meson build directory under third_party/bmplib.
 *
 * Usage: dump_bmp_pixels_bmplib [options] <file.bmp>
 *   --array <n>       select image n of an OS/2 bitmap array (default 0)
 *   --conv64 <mode>   srgb (default), linear or none, for 64-bit files
 *   --undefined <m>   leave (default) or alpha, for RLE undefined pixels
 *   --info            print a one-line description to stderr and exit
 *
 * Output on stdout: "BMPO", then width and height as 32-bit little-endian,
 * then width x height x 4 bytes of RGBA8, top-down.
 *
 * Undefined RLE pixels are left alone over a zeroed buffer by default, which
 * is what this library does with them - bmplib's own default would turn them
 * into an alpha channel and make every RLE file disagree for a reason that is
 * a policy difference rather than a decode difference.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bmplib.h>

#define RAW_MAGIC "BMPO"

static void fail(const char *what, BMPHANDLE h)
{
  const char *msg = h ? bmp_errmsg(h) : NULL;
  fprintf(stderr, "%s%s%s\n", what, msg ? ": " : "", msg ? msg : "");
  exit(1);
}

/* Expand one sample to 8 bits by the rule PNG 13.12 states, which is the rule
   the library under test uses.  A decoder that truncates instead differs by at
   most one, which is inside the sweep's colour tolerance either way. */
static unsigned char to_u8(unsigned long v, unsigned long max)
{
  if (max == 255)
    return (unsigned char)v;
  return (unsigned char)((v * 255 + max / 2) / max);
}

static unsigned long sample_at(const unsigned char *p, int bitsperchannel)
{
  uint16_t u16;
  uint32_t u32;
  switch (bitsperchannel) {
    case 8:
      return *p;
    case 16:
      memcpy(&u16, p, sizeof u16);  /* host byte order, per bmplib */
      return u16;
    case 32:
      memcpy(&u32, p, sizeof u32);
      return u32;
    default:
      fprintf(stderr, "unhandled bits per channel %d\n", bitsperchannel);
      exit(1);
  }
}

static void write_u32le(FILE *out, uint32_t v)
{
  unsigned char b[4] = {(unsigned char)(v & 0xff), (unsigned char)((v >> 8) & 0xff),
                        (unsigned char)((v >> 16) & 0xff), (unsigned char)((v >> 24) & 0xff)};
  fwrite(b, 1, sizeof b, out);
}

int main(int argc, char **argv)
{
  const char *path = NULL;
  int want_array = 0, info_only = 0;
  BMPCONV64 conv64 = BMP_CONV64_SRGB;
  BMPUNDEFINED undef = BMP_UNDEFINED_LEAVE;

  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--array") && i + 1 < argc)
      want_array = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--conv64") && i + 1 < argc) {
      const char *m = argv[++i];
      if (!strcmp(m, "srgb")) conv64 = BMP_CONV64_SRGB;
      else if (!strcmp(m, "linear")) conv64 = BMP_CONV64_LINEAR;
      else if (!strcmp(m, "none")) conv64 = BMP_CONV64_NONE;
      else { fprintf(stderr, "unknown --conv64 %s\n", m); return 1; }
    }
    else if (!strcmp(argv[i], "--undefined") && i + 1 < argc)
      undef = strcmp(argv[++i], "alpha") ? BMP_UNDEFINED_LEAVE : BMP_UNDEFINED_TO_ALPHA;
    else if (!strcmp(argv[i], "--info"))
      info_only = 1;
    else
      path = argv[i];
  }
  if (!path) {
    fprintf(stderr, "usage: dump_bmp_pixels_bmplib [options] <file.bmp>\n");
    return 1;
  }

  FILE *fp = fopen(path, "rb");
  if (!fp)
    fail("cannot open file", NULL);

  BMPHANDLE h = bmpread_new(fp);
  if (!h) {
    fclose(fp);
    fail("bmpread_new failed", NULL);
  }

  BMPRESULT r = bmpread_load_info(h);

  /* A bitmap array is a container of ordinary bitmaps; step into the one that
     was asked for and carry on as if it had been the file. */
  if (r == BMP_RESULT_ARRAY) {
    int n = bmpread_array_num(h);
    struct BmpArrayInfo ai;
    if (want_array < 0 || want_array >= n) {
      fprintf(stderr, "array index %d out of range, file holds %d\n", want_array, n);
      return 1;
    }
    if (bmpread_array_info(h, &ai, want_array) != BMP_RESULT_OK)
      fail("bmpread_array_info failed", h);
    if (info_only) {
      fprintf(stderr, "array of %d; [%d] type=0x%04x %dx%d ncolors=%d\n",
              n, want_array, (unsigned)ai.type, ai.width, ai.height, ai.ncolors);
      return 0;
    }
    h = ai.handle;
    r = bmpread_load_info(h);
  }

  if (r != BMP_RESULT_OK && r != BMP_RESULT_TRUNCATED && r != BMP_RESULT_INVALID) {
    if (r == BMP_RESULT_PNG || r == BMP_RESULT_JPEG) {
      fprintf(stderr, "embedded %s stream, not a bmplib raster\n",
              r == BMP_RESULT_PNG ? "PNG" : "JPEG");
      return 2;
    }
    fail("bmpread_load_info failed", h);
  }

  bmpread_set_undefined(h, undef);
  if (bmpread_is_64bit(h) && bmpread_set_64bit_conv(h, conv64) != BMP_RESULT_OK)
    fail("bmpread_set_64bit_conv failed", h);

  int w, ht, channels, bits;
  BMPORIENT orient;
  if (bmpread_dimensions(h, &w, &ht, &channels, &bits, &orient) != BMP_RESULT_OK)
    fail("bmpread_dimensions failed", h);

  if (info_only) {
    fprintf(stderr, "%dx%d channels=%d bits=%d 64bit=%d header=%s compression=%s\n",
            w, ht, channels, bits, bmpread_is_64bit(h),
            bmpread_info_header_name(h), bmpread_info_compression_name(h));
    return 0;
  }

  size_t need = bmpread_buffersize(h);
  unsigned char *buf = calloc(1, need ? need : 1);
  if (!buf)
    fail("out of memory", NULL);

  r = bmpread_load_image(h, &buf);
  if (r != BMP_RESULT_OK && r != BMP_RESULT_TRUNCATED && r != BMP_RESULT_INVALID)
    fail("bmpread_load_image failed", h);
  if (r != BMP_RESULT_OK)
    fprintf(stderr, "note: bmplib reports the image %s\n",
            r == BMP_RESULT_TRUNCATED ? "truncated" : "invalid");

  unsigned long max = (bits == 8) ? 255UL : (bits == 16) ? 65535UL : 4294967295UL;
  size_t stride = (size_t)bits / 8 * (size_t)channels;

  fwrite(RAW_MAGIC, 1, 4, stdout);
  write_u32le(stdout, (uint32_t)w);
  write_u32le(stdout, (uint32_t)ht);

  for (size_t i = 0; i < (size_t)w * (size_t)ht; i++) {
    const unsigned char *px = buf + i * stride;
    unsigned char rgba[4] = {0, 0, 0, 255};
    switch (channels) {
      case 1:
        rgba[0] = rgba[1] = rgba[2] = to_u8(sample_at(px, bits), max);
        break;
      case 2:
        rgba[0] = rgba[1] = rgba[2] = to_u8(sample_at(px, bits), max);
        rgba[3] = to_u8(sample_at(px + bits / 8, bits), max);
        break;
      case 3:
      case 4:
        for (int c = 0; c < channels; c++)
          rgba[c] = to_u8(sample_at(px + (size_t)c * (bits / 8), bits), max);
        break;
      default:
        fprintf(stderr, "unhandled channel count %d\n", channels);
        return 1;
    }
    fwrite(rgba, 1, 4, stdout);
  }

  free(buf);
  return 0;
}
