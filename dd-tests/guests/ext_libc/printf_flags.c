// printf flag combinations: + space # 0 - with width/precision. Oracle (raw output).
#include <stdio.h>

int main(void) {
    printf("[%+05d][%+-5d][% 05d]\n", 42, 42, 42);
    printf("[%#010x][%#-10x]\n", 255, 255);
    printf("[%+.3f][% .3f]\n", 1.5, 1.5);
    printf("[%-+8.2f|]\n", 3.14159);
    return 0;
}
