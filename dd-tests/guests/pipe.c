#include <stdio.h>
#include <unistd.h>
int main(void){ int fd[2]; if(pipe(fd))return 1; const char*m="piped-data";
  write(fd[1],m,10); close(fd[1]); char b[32]={0}; int n=read(fd[0],b,31); close(fd[0]);
  printf("pipe n=%d %s\n",n,b); return 0; }
