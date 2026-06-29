// printf float conversions: f e E g G with width/precision/flags. Oracle (raw output).
#include <stdio.h>

int main(void) {
    printf("[%f][%.2f][%10.2f][%-10.2f][%+.2f][%08.2f]\n",
           3.14159, 3.14159, 3.14159, 3.14159, 3.14159, 3.14159);
    printf("[%e][%.3e][%E]\n", 12345.678, 12345.678, 12345.678);
    printf("[%g][%g][%g][%.10g]\n", 0.0001, 100000.0, 1234567.0, 3.14159265358979);
    printf("[%f][%f]\n", -0.0, 0.0);
    printf("[%.0f][%#.0f]\n", 2.5, 2.5);
    return 0;
}
