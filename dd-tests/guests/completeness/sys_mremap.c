#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void){
  long ps=4096;
  void *p=mmap(0, ps, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if(p==MAP_FAILED){ printf("mremap ok=0 val=-1\n"); return 0; }
  *(int*)p=42;
  void *q=(void*)syscall(SYS_mremap, p, ps, ps*2, 1 /*MREMAP_MAYMOVE*/, 0);
  int ok = q!=MAP_FAILED && q!=(void*)-1;
  printf("mremap ok=%d val=%d\n", ok, ok?*(int*)q:-1);
  if(ok) munmap(q, ps*2); return 0; }
