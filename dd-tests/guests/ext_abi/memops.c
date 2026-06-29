// memcpy/memmove/memset/memcmp at varied sizes & alignments (overlap, inlined builtins).
#include <stdio.h>
#include <string.h>
int main(void){
  unsigned char buf[512]; for(int i=0;i<512;i++) buf[i]=(unsigned char)(i*131+7);
  unsigned char dst[512];
  long acc=0;
  for(int sz=1;sz<=200;sz++){
    memcpy(dst, buf+(sz&31), sz);
    memmove(buf+5, buf+1, sz);              // overlapping
    memset(dst+sz, (sz&0xFF), 16);
    acc += memcmp(dst, buf, sz) ? 1 : 0;
    acc += dst[sz/2];
  }
  unsigned long sum=0; for(int i=0;i<512;i++) sum+=buf[i];
  printf("acc=%ld sum=%lu\n", acc, sum);
  return 0;
}
