#include <stdio.h>
static long fib(int n){ return n<2?n:fib(n-1)+fib(n-2); }
static long ack(int m,int n){ return m==0?n+1:(n==0?ack(m-1,1):ack(m-1,ack(m,n-1))); }
int main(void){ printf("fib=%ld ack=%ld\n", fib(30), ack(2,5)); return 0; }
