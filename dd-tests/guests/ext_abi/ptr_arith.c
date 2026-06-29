// pointer arithmetic, multi-dim array indexing, pointer differences and casts.
#include <stdio.h>
int main(void){
  static int grid[16][16];
  for(int i=0;i<16;i++)for(int j=0;j<16;j++) grid[i][j]=i*16+j;
  int* base=&grid[0][0]; long acc=0;
  for(int i=0;i<256;i++) acc += *(base + ((i*7)%256));
  int* a=&grid[3][4]; int* b=&grid[10][2];
  long diff=b-a;
  char buf[64]; for(int i=0;i<64;i++) buf[i]=(char)(i+33);
  char* p=buf+10; acc += (p[-3] + p[5]) + (long)((unsigned char*)p - (unsigned char*)buf);
  printf("acc=%ld diff=%ld first=%d\n", acc, diff, grid[0][0]);
  return 0;
}
