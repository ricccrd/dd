#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void){
  long q=syscall(SYS_membarrier, 0 /*QUERY*/, 0, 0);
  long g=syscall(SYS_membarrier, 1 /*GLOBAL*/, 0, 0);
  printf("membarrier query_ok=%d global=%d\n", q>=0, g==0); return 0; }
