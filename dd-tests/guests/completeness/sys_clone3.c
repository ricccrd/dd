#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <stdint.h>
struct ddc_clone_args { uint64_t flags, pidfd, child_tid, parent_tid, exit_signal, stack, stack_size, tls; };
int main(void){
  struct ddc_clone_args ca; memset(&ca,0,sizeof ca); ca.exit_signal=SIGCHLD;
  long pid=syscall(__NR_clone3, &ca, sizeof ca);
  if(pid==0){ _exit(7); }
  int ok = pid>0, st=0, child=-1;
  if(ok){ waitpid((pid_t)pid,&st,0); if(WIFEXITED(st)) child=WEXITSTATUS(st); }
  printf("clone3 ok=%d child=%d\n", ok, child); return 0; }
