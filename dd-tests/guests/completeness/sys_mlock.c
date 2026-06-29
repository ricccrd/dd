#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void){
  long ps=4096;
  void *p=mmap(0, ps, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  long l=syscall(SYS_mlock, p, ps);
  *(int*)p=7;
  long u=syscall(SYS_munlock, p, ps);
  printf("mlock lock=%d unlock=%d\n", l==0, u==0); munmap(p,ps); return 0; }
