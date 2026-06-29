#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
int main(void){
  const char *p="/tmp/ddc_syn"; unlink(p);
  int fd=open(p,O_CREAT|O_RDWR|O_TRUNC,0644); write(fd,"data",4);
  long fs=syscall(SYS_fsync, fd);
  long fd2=syscall(SYS_fdatasync, fd);
  long sfr=syscall(SYS_sync_file_range, fd, (long)0, (long)4, 0);
  long sf=syscall(SYS_syncfs, fd);
  syscall(SYS_sync);
  printf("syncfam fsync=%d fdatasync=%d sfr=%d syncfs=%d\n", fs==0, fd2==0, sfr==0, sf==0); close(fd); unlink(p); return 0; }
