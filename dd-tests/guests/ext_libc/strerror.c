// strerror text for common errno values. Oracle vs native Linux (glibc message strings).
#include <stdio.h>
#include <string.h>
#include <errno.h>

int main(void) {
    printf("[%s]\n", strerror(ENOENT));
    printf("[%s]\n", strerror(EINVAL));
    printf("[%s]\n", strerror(EACCES));
    printf("[%s]\n", strerror(0));
    return 0;
}
