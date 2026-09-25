/* What giflib says it is, from the header the oracle tool compiles against.
 * See probe_jpeg.c for why this is compiled rather than queried.
 */
#include <stdio.h>
#include <gif_lib.h>

int main(void) {
  printf("giflib %d.%d.%d\n", GIFLIB_MAJOR, GIFLIB_MINOR, GIFLIB_RELEASE);
  return 0;
}
