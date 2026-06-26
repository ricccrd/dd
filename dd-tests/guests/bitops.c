#include <stdio.h>
int main(void){ unsigned long x=0xF0F0F0F0F0F0F0F0UL;
  printf("popc=%d clz=%d ctz=%d byteswap=%lx\n", __builtin_popcountl(x), __builtin_clzl(x), __builtin_ctzl(x), __builtin_bswap64(x)); return 0; }
