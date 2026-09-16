/*
 * Generator for the twelve-bit four-component JPEG fixtures, and their oracle.
 *
 * T.81 Table B.2 allows a sample precision of 12 in a DCT-based frame and
 * B.2.2 allows Nf from 1 to 255; the two are independent, so a twelve-bit CMYK
 * or YCCK frame is a legal file.  Nothing in the fixture set had one, and this
 * codec refused them.
 *
 * libjpeg-turbo builds a separate twelve-bit entry set (jpeg12_*), which is
 * what writes and reads these.  Each run produces the .jpg and the .raw beside
 * it; the .raw carries 16-bit samples so that the twelve-bit values survive,
 * left-justified the way GIMG_PIXEL_CMYK16 carries them:
 *
 *   byte 0     mode, 3 = CMYK at 16 bits per sample
 *   bytes 1-4  width, little endian
 *   bytes 5-8  height, little endian
 *   then       w * h * 4 samples, uint16 little endian
 *
 * Build against a libjpeg-turbo checkout:
 *   cc -o mk_cmyk12 mk_cmyk12.c -I<ljt-src> -I<ljt-build> <ljt-build>/libjpeg.a
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JPEG_INTERNAL_OPTIONS
#include <jpeglib.h>

#define W 33u
#define H 17u

/* The twelve-bit counterpart of the pattern in mk_cmyk.c. */
static void fill(J12SAMPLE * buf) {
  for (unsigned y = 0; y < H; y++) {
    for (unsigned x = 0; x < W; x++) {
      J12SAMPLE * p = buf + ((size_t)y * W + x) * 4;
      p[0] = (J12SAMPLE)((x * 107 + y * 53) & 0xFFF);
      p[1] = (J12SAMPLE)((x * 53 + y * 173) & 0xFFF);
      p[2] = (J12SAMPLE)((x * 199 + y * 79) & 0xFFF);
      p[3] = (J12SAMPLE)((x * 31 + y * 29) & 0xFFF);
    }
  }
}

static int write_jpeg(const char * path, int ycck, int h0, int v0, int prog) {
  J12SAMPLE * buf = (J12SAMPLE *)malloc((size_t)W * H * 4 * sizeof(J12SAMPLE));
  if (!buf) return 1;
  fill(buf);
  FILE * f = fopen(path, "wb");
  if (!f) { free(buf); return 1; }
  struct jpeg_compress_struct c;
  struct jpeg_error_mgr e;
  c.err = jpeg_std_error(&e);
  jpeg_create_compress(&c);
  jpeg_stdio_dest(&c, f);
  c.image_width = W;
  c.image_height = H;
  c.input_components = 4;
  c.in_color_space = JCS_CMYK;
  jpeg_set_defaults(&c);
  c.data_precision = 12;
  jpeg_set_colorspace(&c, ycck ? JCS_YCCK : JCS_CMYK);
  c.comp_info[0].h_samp_factor = h0;
  c.comp_info[0].v_samp_factor = v0;
  c.comp_info[1].h_samp_factor = 1;
  c.comp_info[1].v_samp_factor = 1;
  c.comp_info[2].h_samp_factor = 1;
  c.comp_info[2].v_samp_factor = 1;
  c.comp_info[3].h_samp_factor = h0;
  c.comp_info[3].v_samp_factor = v0;
  jpeg_set_quality(&c, 90, TRUE);
  if (prog) jpeg_simple_progression(&c);
  jpeg_start_compress(&c, TRUE);
  while (c.next_scanline < H) {
    J12SAMPROW row = buf + (size_t)c.next_scanline * W * 4;
    jpeg12_write_scanlines(&c, &row, 1);
  }
  jpeg_finish_compress(&c);
  jpeg_destroy_compress(&c);
  fclose(f);
  free(buf);
  return 0;
}

static int write_raw(const char * raw_path, const char * jpg_path) {
  FILE * f = fopen(jpg_path, "rb");
  if (!f) return 1;
  struct jpeg_decompress_struct c;
  struct jpeg_error_mgr e;
  c.err = jpeg_std_error(&e);
  jpeg_create_decompress(&c);
  jpeg_stdio_src(&c, f);
  jpeg_read_header(&c, TRUE);
  jpeg_start_decompress(&c);
  if (c.output_components != 4 || c.data_precision != 12) {
    fprintf(stderr, "%s: not twelve-bit four-component\n", jpg_path);
    return 3;
  }
  unsigned w = c.output_width, h = c.output_height;
  J12SAMPLE * buf =
      (J12SAMPLE *)malloc((size_t)w * h * 4 * sizeof(J12SAMPLE));
  while (c.output_scanline < h) {
    J12SAMPROW row = buf + (size_t)c.output_scanline * w * 4;
    jpeg12_read_scanlines(&c, &row, 1);
  }
  jpeg_finish_decompress(&c);
  jpeg_destroy_decompress(&c);
  fclose(f);
  FILE * o = fopen(raw_path, "wb");
  if (!o) return 1;
  unsigned char hdr[9];
  hdr[0] = 3; /* CMYK, 16-bit samples */
  hdr[1] = (unsigned char)(w & 255); hdr[2] = (unsigned char)((w >> 8) & 255);
  hdr[3] = (unsigned char)((w >> 16) & 255);
  hdr[4] = (unsigned char)((w >> 24) & 255);
  hdr[5] = (unsigned char)(h & 255); hdr[6] = (unsigned char)((h >> 8) & 255);
  hdr[7] = (unsigned char)((h >> 16) & 255);
  hdr[8] = (unsigned char)((h >> 24) & 255);
  fwrite(hdr, 1, 9, o);
  for (size_t i = 0; i < (size_t)w * h * 4; i++) {
    /* Left-justify 12 bits into 16, as gimg_bitdepth_12_to_16 does:
     * v << 4 | v >> 8. */
    unsigned v = (unsigned)buf[i] & 0xFFFu;
    unsigned wide = (v << 4) | (v >> 8);
    unsigned char b[2] = {(unsigned char)(wide & 255),
        (unsigned char)((wide >> 8) & 255)};
    fwrite(b, 1, 2, o);
  }
  fclose(o);
  free(buf);
  printf("%s: %ux%u CMYK12\n", raw_path, w, h);
  return 0;
}

int main(void) {
  struct { const char * base; int ycck, h, v, prog; } cases[] = {
      {"cmyk12_ljt_seq", 0, 1, 1, 0},
      {"cmyk12_ljt_prog", 0, 1, 1, 1},
      {"ycck12_ljt_420", 1, 2, 2, 0},
  };
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char jpg[64], raw[64];
    snprintf(jpg, sizeof(jpg), "%s.jpg", cases[i].base);
    snprintf(raw, sizeof(raw), "%s.raw", cases[i].base);
    if (write_jpeg(jpg, cases[i].ycck, cases[i].h, cases[i].v, cases[i].prog))
      return 1;
    if (write_raw(raw, jpg)) return 1;
  }
  return 0;
}
