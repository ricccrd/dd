#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
int main(void){
  sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR1);
  sigprocmask(SIG_BLOCK, &set, 0);
  raise(SIGUSR1);
  struct timespec to={2,0}; siginfo_t info; memset(&info,0,sizeof info);
  long r=syscall(SYS_rt_sigtimedwait, &set, &info, &to, _NSIG/8);
  printf("rt_sigtimedwait ret=%ld sig=%d\n", r, (int)r==SIGUSR1?SIGUSR1:-1); return 0; }
