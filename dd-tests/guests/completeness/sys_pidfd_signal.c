#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <signal.h>
int main(void){
  long fd=syscall(__NR_pidfd_open, getpid(), 0);
  long sg = fd>=0 ? syscall(__NR_pidfd_send_signal, (int)fd, 0 /*sig 0 = existence check*/, (void*)0, 0) : -1;
  printf("pidfd open_ok=%d send_ok=%d\n", fd>=0, sg==0); if(fd>=0) close((int)fd); return 0; }
