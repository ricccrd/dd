#include "compat.h"
#include <stdio.h>
#include <string.h>
#ifndef GRND_NONBLOCK
#define GRND_NONBLOCK 1
#endif
int main(void){
  unsigned char b[16]; memset(b,0,sizeof b);
  long n=syscall(SYS_getrandom, b, sizeof b, GRND_NONBLOCK);
  int nz=0; for(int i=0;i<16;i++) if(b[i]) nz=1;
  printf("getrandom n=%ld nonzero=%d\n", n, n==16?nz:0); return 0; }
