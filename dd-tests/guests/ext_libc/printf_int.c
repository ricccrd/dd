// printf integer conversions: d i u o x X c % with width/precision/flags. Oracle (raw output).
#include <stdio.h>

int main(void) {
    printf("[%d][%5d][%-5d][%05d][%+d][% d]\n", 42, 42, 42, 42, 42, 42);
    printf("[%d][%i][%u][%o][%x][%X]\n", -7, -7, 7u, 8, 255, 255);
    printf("[%#o][%#x][%#X]\n", 8, 255, 255);
    printf("[%c][%5c][%%]\n", 'A', 'A');
    printf("[%.3d][%8.3d][%-8.3d]\n", 5, 5, 5);
    printf("[%u][%x]\n", 4294967295u, 4294967295u);
    return 0;
}
