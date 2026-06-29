#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
int main(void){
  long ps=4096; int n=16;
  char *p=mmap(0, ps*n, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  for(int i=0;i<8;i++) p[i*ps]=1;
  unsigned char vec[16]; memset(vec,0,sizeof vec);
  long r=syscall(SYS_mincore, p, ps*n, vec);
  printf("mincore ok=%d first_resident=%d\n", r==0, vec[0]&1); munmap(p,ps*n); return 0; }
