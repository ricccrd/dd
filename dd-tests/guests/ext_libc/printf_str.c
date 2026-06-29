// printf string/char conversions with width/precision. Oracle (raw output).
#include <stdio.h>

int main(void) {
    printf("[%s][%10s][%-10s][%.3s][%10.3s]\n",
           "hello", "hello", "hello", "hello", "hello");
    printf("[%s]\n", "");
    printf("[%c%c%c]\n", 'x', 'y', 'z');
    printf("[%.0s|]\n", "hidden");
    return 0;
}
