#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/uio.h>
int main(void){
  char src[]="vm-readv-payload"; char dst[32]; memset(dst,0,sizeof dst);
  struct iovec lo={dst, sizeof src-1}, re={src, sizeof src-1};
  long n=syscall(SYS_process_vm_readv, getpid(), &lo,1, &re,1, 0);
  printf("process_vm_readv n=%ld data=%s\n", n, n>0?dst:"?"); return 0; }
