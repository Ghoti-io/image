/*
 * Generator for the abbreviated-format fixtures of T.81 B.4.
 *
 * B.4 describes two streams that only mean anything as a pair: one of
 * table-specification data with no frame, and one carrying a frame whose
 * tables are absent.  libjpeg writes both - jpeg_write_tables() for the first,
 * jpeg_start_compress(..., FALSE) for the second - so the fixtures here are
 * its output, and the expected pixels are its decode of the complete file it
 * writes from the same tables.
 *
 * Writes abbrev_ljt_tables.jpg, abbrev_ljt_image.jpg, abbrev_ljt_full.jpg and
 * abbrev_ljt_full.raw (mode 1, RGB, as tests/codec/jpeg's oracle .raw format).
 *
 * Build against a libjpeg-turbo checkout:
 *   cc -o mk_abbrev mk_abbrev.c -I<ljt-src> -I<ljt-build> <ljt-build>/libjpeg.a
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jpeglib.h>

#define W 33u
#define H 17u

static void fill(unsigned char * buf) {
  for (unsigned y = 0; y < H; y++) {
    for (unsigned x = 0; x < W; x++) {
      unsigned char * p = buf + ((size_t)y * W + x) * 3;
      p[0] = (unsigned char)((x * 5 + y * 3) & 0xFF);
      p[1] = (unsigned char)((x * 2 + y * 7) & 0xFF);
      p[2] = (unsigned char)((x * 9 + y * 11) & 0xFF);
    }
  }
}

static void setup(struct jpeg_compress_struct * c) {
  c->image_width = W;
  c->image_height = H;
  c->input_components = 3;
  c->in_color_space = JCS_RGB;
  jpeg_set_defaults(c);
  jpeg_set_quality(c, 88, TRUE);
}

int main(void) {
  unsigned char * buf = (unsigned char *)malloc((size_t)W * H * 3);
  fill(buf);
  struct jpeg_error_mgr e;

  /* One compress object for both streams: jpeg_write_tables() marks the
   * tables as sent, and jpeg_start_compress(..., FALSE) then leaves them out.
   * Two objects would each think its tables had never been written, and the
   * "abbreviated" image would carry a full set. */
  {
    struct jpeg_compress_struct c;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    setup(&c);

    FILE * ft = fopen("abbrev_ljt_tables.jpg", "wb");
    jpeg_stdio_dest(&c, ft);
    jpeg_write_tables(&c); /* B.4: tables only, and it writes EOI itself */
    fclose(ft);

    FILE * fi = fopen("abbrev_ljt_image.jpg", "wb");
    jpeg_stdio_dest(&c, fi);
    jpeg_start_compress(&c, FALSE); /* FALSE: the tables are already sent */
    while (c.next_scanline < H) {
      JSAMPROW row = buf + (size_t)c.next_scanline * W * 3;
      jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    fclose(fi);
    jpeg_destroy_compress(&c);
  }
  /* The same image complete, to decode for the expected pixels. */
  {
    struct jpeg_compress_struct c;
    c.err = jpeg_std_error(&e);
    jpeg_create_compress(&c);
    FILE * f = fopen("abbrev_ljt_full.jpg", "wb");
    jpeg_stdio_dest(&c, f);
    setup(&c);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < H) {
      JSAMPROW row = buf + (size_t)c.next_scanline * W * 3;
      jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    fclose(f);
  }
  {
    struct jpeg_decompress_struct d;
    d.err = jpeg_std_error(&e);
    jpeg_create_decompress(&d);
    FILE * f = fopen("abbrev_ljt_full.jpg", "rb");
    jpeg_stdio_src(&d, f);
    jpeg_read_header(&d, TRUE);
    jpeg_start_decompress(&d);
    unsigned w = d.output_width, h = d.output_height;
    unsigned char * px =
        (unsigned char *)malloc((size_t)w * h * (size_t)d.output_components);
    while (d.output_scanline < h) {
      JSAMPROW row = px + (size_t)d.output_scanline * w * d.output_components;
      jpeg_read_scanlines(&d, &row, 1);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
    fclose(f);
    FILE * o = fopen("abbrev_ljt_full.raw", "wb");
    unsigned char hdr[9];
    hdr[0] = 1; /* RGB */
    hdr[1] = (unsigned char)(w & 255); hdr[2] = (unsigned char)((w >> 8) & 255);
    hdr[3] = (unsigned char)((w >> 16) & 255);
    hdr[4] = (unsigned char)((w >> 24) & 255);
    hdr[5] = (unsigned char)(h & 255); hdr[6] = (unsigned char)((h >> 8) & 255);
    hdr[7] = (unsigned char)((h >> 16) & 255);
    hdr[8] = (unsigned char)((h >> 24) & 255);
    fwrite(hdr, 1, 9, o);
    fwrite(px, 1, (size_t)w * h * 3, o);
    fclose(o);
    printf("abbrev_ljt_full.raw: %ux%u RGB\n", w, h);
  }
  free(buf);
  return 0;
}
