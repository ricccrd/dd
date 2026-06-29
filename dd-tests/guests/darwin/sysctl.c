// macOS-native sysctl: read hw.ncpu (CPU count) and kern.ostype via sysctlbyname. The BSD system-
// info interface (no Linux equivalent; Linux uses /proc + uname). darwin engine only, golden-checked.
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

int main(void) {
    int ncpu = 0;
    size_t len = sizeof ncpu;
    int r1 = sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0);

    char os[64] = {0};
    len = sizeof os;
    int r2 = sysctlbyname("kern.ostype", os, &len, NULL, 0);

    printf("sysctl ncpu_ok=%d ostype=%s\n", (r1 == 0 && ncpu > 0), (r2 == 0) ? os : "?"); // 1 Darwin
    return 0;
}
