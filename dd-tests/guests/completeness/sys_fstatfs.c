#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <sys/vfs.h>
#include <fcntl.h>
int main(void){
  int fd=open("/tmp", O_RDONLY|O_DIRECTORY);
  struct statfs s; memset(&s,0,sizeof s);
  long r=syscall(SYS_fstatfs, fd, &s);
  int pow2 = s.f_bsize>0 && (s.f_bsize & (s.f_bsize-1))==0;
  printf("fstatfs ok=%d bsize_pow2=%d blocks_pos=%d\n", r==0, pow2, s.f_blocks>0); close(fd); return 0; }
