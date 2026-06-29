#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <immintrin.h>
#include <x86intrin.h>
__attribute__((target("lzcnt"))) static long go(void){
  return _lzcnt_u64(0x0000100000000000UL) + _lzcnt_u32(0x00010000u) + _lzcnt_u32(0x1u); }
int main(void){ printf("lzcnt r=%ld\n", go()); return 0; }
