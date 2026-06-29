// 64x64->128 widening across the full input space + carry handling (add-with-carry codegen).
#include <stdio.h>
int main(void){
  unsigned __int128 acc=0; unsigned long carries=0;
  unsigned long a=0x9E3779B97F4A7C15UL, b=0xC2B2AE3D27D4EB4FUL;
  for(int i=0;i<50000;i++){
    unsigned long x=a+i, y=b-i;
    unsigned __int128 p=(unsigned __int128)x*y;
    acc += p;
    unsigned long s; carries += __builtin_add_overflow(x,y,&s);   // carry-out count
  }
  printf("hi=%lx lo=%lx carries=%lu\n",(unsigned long)(acc>>64),(unsigned long)acc, carries);
  return 0;
}
