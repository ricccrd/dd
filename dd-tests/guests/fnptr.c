#include <stdio.h>
static int add(int a,int b){return a+b;} static int mul(int a,int b){return a*b;} static int sub(int a,int b){return a-b;}
int main(void){ int(*op[3])(int,int)={add,mul,sub}; long s=0; for(int i=0;i<40000;i++) s+=op[i%3](i,i+1); printf("fnptr s=%ld\n",s); return 0; }
