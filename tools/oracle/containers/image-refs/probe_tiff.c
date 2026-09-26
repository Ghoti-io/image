/* What libtiff says it is, from the header the oracle tool compiles against.
 * See probe_jpeg.c for why this is compiled rather than queried.
 *
 * TIFFLIB_VERSION_STR is a whole sentence - "LIBTIFF, Version 4.7.0\nCopyright
 * ..." - so the digits are cut out of it rather than printed whole, and the
 * line this emits is the shape every other arm emits: a name and a version.
 */
#include <stdio.h>
#include <string.h>
#include <tiffio.h>

int main(void) {
  const char * s = TIFFGetVersion();
  const char * at = strstr(s, "Version ");
  if (!at) {
    fprintf(stderr, "libtiff did not say its version: %s\n", s);
    return 2;
  }
  at += 8;
  size_t n = strcspn(at, " \t\r\n");
  printf("libtiff %.*s\n", (int)n, at);
  return 0;
}
