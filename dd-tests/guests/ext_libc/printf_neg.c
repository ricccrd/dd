// printf of extreme/limit integer values. Oracle (raw output).
#include <stdio.h>
#include <limits.h>

int main(void) {
    printf("[%d][%d]\n", INT_MIN, INT_MAX);
    printf("[%ld][%ld]\n", LONG_MIN, LONG_MAX);
    printf("[%lld][%lld]\n", LLONG_MIN, LLONG_MAX);
    printf("[%u][%lu]\n", UINT_MAX, ULONG_MAX);
    printf("[%x][%X]\n", -1, -1);
    return 0;
}
