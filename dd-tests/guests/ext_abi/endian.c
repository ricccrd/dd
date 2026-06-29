// byte-level access of multibyte values + manual little/big-endian (de)serialization.
#include <stdio.h>
#include <string.h>
static unsigned long rd_le(const unsigned char*p){ unsigned long v=0; for(int i=0;i<8;i++) v|=(unsigned long)p[i]<<(8*i); return v; }
static unsigned long rd_be(const unsigned char*p){ unsigned long v=0; for(int i=0;i<8;i++) v=(v<<8)|p[i]; return v; }
int main(void){
  unsigned long x=0x0123456789ABCDEFUL; unsigned char buf[8]; memcpy(buf,&x,8);
  unsigned long acc=0;
  for(int i=0;i<10000;i++){ unsigned char b[8]; unsigned long v=x+i; memcpy(b,&v,8); acc^=rd_le(b); acc+=rd_be(b); }
  printf("b0=%u b7=%u le=%lx be=%lx acc=%lx\n", buf[0], buf[7], rd_le(buf), rd_be(buf), acc);
  return 0;
}
