#include "memrss.h"
#include <stdlib.h>
#include <string.h>
int main(void){ const size_t SZ=24u*1024*1024;
  for(int w=0;w<2;w++){void*p=malloc(SZ);if(p){memset(p,1,SZ);free(p);}}
  long base=rss_kb();
  for(int i=0;i<160;i++){void*p=malloc(SZ);if(!p){printf("mallocfree bounded=0\n");return 1;}memset(p,(char)i,SZ);free(p);}
  verdict("mallocfree",base,rss_kb(),200*1024); return 0; }
