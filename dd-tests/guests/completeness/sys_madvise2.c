#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void){
  long ps=65536;
  char *p=mmap(0, ps, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  for(long i=0;i<ps;i+=4096) p[i]=1;
  long w=syscall(SYS_madvise, p, ps, 3 /*MADV_WILLNEED*/);
  long s=syscall(SYS_madvise, p, ps, 2 /*MADV_SEQUENTIAL*/);
  long f=syscall(SYS_madvise, p, ps, 8 /*MADV_FREE*/);
  printf("madvise willneed=%d seq=%d free=%d\n", w==0, s==0, f==0); munmap(p,ps); return 0; }
