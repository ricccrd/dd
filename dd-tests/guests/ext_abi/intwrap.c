// signed/unsigned overflow wrapping at the int boundaries (two's-complement wrap is the contract).
#include <stdio.h>
#include <limits.h>
int main(void){
  unsigned u = UINT_MAX; u += 3;                         // wraps to 2
  int a = INT_MAX; int b = (int)((unsigned)a + 7u);      // defined via unsigned, wraps
  int c = INT_MIN; int d = (int)((unsigned)c - 1u);      // wraps to INT_MAX
  unsigned short s = 0xFFFF; s += 5;                      // 16-bit wrap -> 4
  unsigned char ch = 250; ch += 10;                      // 8-bit wrap -> 4
  long lo = LONG_MAX; unsigned long lw = (unsigned long)lo + 9;
  printf("u=%u b=%d d=%d s=%u ch=%u lw=%lu\n", u, b, d, (unsigned)s, (unsigned)ch, lw);
  return 0;
}
