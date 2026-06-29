// printf %a / %A hex-float conversion. Oracle vs native Linux (glibc-specific rendering).
#include <stdio.h>

int main(void) {
    printf("[%a][%A]\n", 1.0, 1.0);
    printf("[%a]\n", 0.5);
    printf("[%a]\n", 3.14159);
    printf("[%.3a]\n", 1.0 / 3.0);
    printf("[%a][%a]\n", 0.0, -2.0);
    return 0;
}
