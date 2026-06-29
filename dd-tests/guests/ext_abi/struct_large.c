// large struct (>16 bytes) passed by value — goes via memory/hidden pointer in both ABIs.
#include <stdio.h>
typedef struct { long v[8]; } Big;
static Big scale(Big b,long k){ for(int i=0;i<8;i++) b.v[i]=b.v[i]*k+i; return b; }
static long total(Big b){ long s=0; for(int i=0;i<8;i++) s+=b.v[i]; return s; }
int main(void){
  Big b; for(int i=0;i<8;i++) b.v[i]=i+1;
  long acc=0;
  for(int i=0;i<5000;i++){ b=scale(b, (i&3)+1); acc += total(b) & 0xFFFFF; }
  printf("v0=%ld v7=%ld acc=%ld\n", b.v[0], b.v[7], acc);
  return 0;
}
