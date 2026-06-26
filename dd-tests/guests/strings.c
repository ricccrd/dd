#include <stdio.h>
#include <string.h>
int main(void){ char b[64]="foo"; strcat(b,"bar"); memmove(b+3,b,4);
  printf("len=%zu s=%s cmp=%d chr=%ld str=%d\n", strlen(b), b, strcmp("a","b")<0, (long)(strchr("hello",'l')-"hello"), (int)(strstr("hello","ll")!=0)); return 0; }
