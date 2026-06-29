#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
int main(void){
  const char *p="/tmp/ddc_fa2"; unlink(p);
  int fd=open(p,O_CREAT|O_WRONLY,0644); close(fd);
  long r=syscall(__NR_faccessat2, AT_FDCWD, p, R_OK|W_OK, 0);
  long n=syscall(__NR_faccessat2, AT_FDCWD, "/tmp/ddc_nope_xyz", F_OK, 0);
  printf("faccessat2 ok=%d enoent=%d\n", r==0, n<0); unlink(p); return 0; }
