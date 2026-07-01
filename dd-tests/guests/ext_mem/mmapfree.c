#include "memrss.h"
#include <sys/mman.h>
#include <string.h>
int main(void){ const size_t SZ=32u*1024*1024;
  for(int w=0;w<2;w++){void*p=mmap(0,SZ,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);if(p!=MAP_FAILED){memset(p,1,SZ);munmap(p,SZ);}}
  long base=rss_kb();
  for(int i=0;i<128;i++){void*p=mmap(0,SZ,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED){printf("mmapfree bounded=0\n");return 1;} memset(p,(char)i,SZ); munmap(p,SZ);}
  verdict("mmapfree",base,rss_kb(),200*1024); return 0; }
