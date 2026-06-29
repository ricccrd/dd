#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
int main(void){
  unsigned long long t1=__rdtsc(); unsigned aux; unsigned long long t2=__rdtscp(&aux);
  unsigned long long t3=__rdtsc();
  printf("rdtsc inc=%d\n", (t2>=t1)&&(t3>=t2)); return 0; }
