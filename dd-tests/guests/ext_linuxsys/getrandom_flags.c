// getrandom flags: GRND_NONBLOCK and GRND_RANDOM both fill the buffer; reproducibility verdict only.
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/random.h>

int main(void) {
    unsigned char a[16] = {0}, b[16] = {0};
    ssize_t n1 = getrandom(a, sizeof a, GRND_NONBLOCK);
    ssize_t n2 = getrandom(b, sizeof b, 0);
    int got1 = n1 == 16, got2 = n2 == 16;
    int differ = memcmp(a, b, 16) != 0;
    printf("getrandom_flags nonblock=%d blocking=%d differ=%d\n", got1, got2, differ);
    return 0;
}
