// union type punning + bitfields — reinterpreting float/int bit patterns (no UB via memcpy where needed).
#include <stdio.h>
#include <string.h>
struct Bits { unsigned a:3; unsigned b:5; unsigned c:8; int d:16; };
int main(void){
  double d=3.141592653589793; unsigned long bits; memcpy(&bits,&d,8);
  float f=1.5f; unsigned fb; memcpy(&fb,&f,4);
  struct Bits bf={5, 17, 200, -1000};
  unsigned long racc=0;
  for(int i=0;i<1000;i++){ float g=(float)i*0.1f; unsigned gb; memcpy(&gb,&g,4); racc^=gb; }
  printf("dbits=%lx fbits=%x bf=%u,%u,%u,%d racc=%lx\n", bits, fb, bf.a, bf.b, bf.c, bf.d, racc);
  return 0;
}
