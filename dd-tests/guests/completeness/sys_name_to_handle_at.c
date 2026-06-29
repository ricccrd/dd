#include "compat.h"
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
struct ddc_fh { unsigned handle_bytes; int handle_type; unsigned char f[128]; };
int main(void){
  const char *p="/tmp/ddc_nth"; unlink(p);
  int fd=open(p,O_CREAT|O_WRONLY,0644); close(fd);
  struct ddc_fh fh; fh.handle_bytes=128; int mnt=0;
  long r=syscall(SYS_name_to_handle_at, AT_FDCWD, p, &fh, &mnt, 0);
  printf("name_to_handle_at ok=%d hbytes_pos=%d\n", r==0, fh.handle_bytes>0); unlink(p); return 0; }
