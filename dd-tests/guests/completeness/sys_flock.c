#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <fcntl.h>
int main(void){
  const char *p="/tmp/ddc_flk"; unlink(p);
  int fd=open(p,O_CREAT|O_RDWR,0644);
  long ex=syscall(SYS_flock, fd, LOCK_EX);
  long sh=syscall(SYS_flock, fd, LOCK_SH);
  long un=syscall(SYS_flock, fd, LOCK_UN);
  printf("flock ex=%d sh=%d un=%d\n", ex==0, sh==0, un==0); close(fd); unlink(p); return 0; }
