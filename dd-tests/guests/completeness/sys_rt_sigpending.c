#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
int main(void){
  sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR2);
  sigprocmask(SIG_BLOCK, &set, 0);
  raise(SIGUSR2);
  sigset_t pend; sigemptyset(&pend);
  long r=syscall(SYS_rt_sigpending, &pend, _NSIG/8);
  printf("rt_sigpending ok=%d pending=%d\n", r==0, sigismember(&pend, SIGUSR2)); return 0; }
