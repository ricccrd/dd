// printf positional arguments (%n$) incl positional width/precision. Oracle (raw output).
#include <stdio.h>

int main(void) {
    printf("%2$s %1$s\n", "world", "hello");
    printf("%1$d %1$d %2$d\n", 7, 9);
    printf("%2$.*1$f\n", 3, 3.14159);
    return 0;
}
