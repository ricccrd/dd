#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
int main(void){
  struct rlimit rl; memset(&rl,0,sizeof rl);
  long r=syscall(SYS_prlimit64, 0, RLIMIT_NOFILE, (void*)0, &rl);
  int sane = (r==0) && rl.rlim_cur<=rl.rlim_max && rl.rlim_max>0;
  printf("prlimit64 ok=%d sane=%d\n", r==0, sane); return 0; }
