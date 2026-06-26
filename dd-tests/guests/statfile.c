#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
int main(void){ int fd=open("/tmp/ddstat",O_CREAT|O_WRONLY|O_TRUNC,0644); write(fd,"abcd",4); close(fd);
  struct stat st; stat("/tmp/ddstat",&st); int acc=access("/tmp/ddstat",R_OK); unlink("/tmp/ddstat");
  printf("stat size=%lld reg=%d acc=%d\n",(long long)st.st_size,(int)(S_ISREG(st.st_mode)!=0),acc); return 0; }
