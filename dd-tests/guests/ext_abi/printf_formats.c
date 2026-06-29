// broad printf conversion + flag + width/precision matrix (formatting must be byte-identical).
#include <stdio.h>
int main(void){
  printf("[%5d][%-5d][%05d][%+d][% d]\n", 42, 42, 42, 42, 42);
  printf("[%x][%X][%#x][%o][%#o]\n", 255, 255, 255, 64, 64);
  printf("[%8.3f][%-8.3f][%+.2e][%g][%g]\n", 3.14159, 3.14159, 31415.9, 0.0001, 100000.0);
  printf("[%.5s][%10s][%-10s|]\n", "truncated", "right", "left");
  printf("[%ld][%lld][%lu][%zu][%hd][%hhu]\n", -1L, -1LL, 1UL, (size_t)9, (short)-3, (unsigned char)200);
  printf("[%c][%%][%*d][%.*f]\n", 'A', 7, 99, 2, 3.14159);
  return 0;
}
