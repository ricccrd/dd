#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
int main(void){ size_t sz=1u<<20; char*p=mmap(0,sz,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  if(p==MAP_FAILED)return 1; memset(p,3,sz); long s=0; for(size_t i=0;i<sz;i+=4096)s+=p[i]; munmap(p,sz);
  printf("mmap sum=%ld\n",s); return 0; }
