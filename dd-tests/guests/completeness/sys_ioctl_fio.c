#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
int main(void){
  int fds[2]; if(pipe(fds)){printf("ioctl fionread=-1 nonblock=0\n");return 0;}
  write(fds[1], "hello", 5);
  int avail=0; long r=syscall(SYS_ioctl, fds[0], FIONREAD, &avail);
  int on=1; long nb=syscall(SYS_ioctl, fds[0], FIONBIO, &on);
  printf("ioctl fionread=%d nb_ok=%d\n", r==0?avail:-1, nb==0); return 0; }
