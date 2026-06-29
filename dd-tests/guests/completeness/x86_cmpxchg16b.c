#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("cx16"))) static long go(void){
  __int128 v=0; __int128 exp=0, des=((__int128)5<<64)|9;
  int ok=__sync_bool_compare_and_swap(&v, exp, des);
  __int128 v2=__sync_val_compare_and_swap(&v, des, ((__int128)1<<64)|2);
  return (long)(ok + (long)(v2>>64) + (long)(v & 0xff)); }
int main(void){ printf("cmpxchg16b r=%ld\n", go()); return 0; }
