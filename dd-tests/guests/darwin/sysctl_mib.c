// macOS-native sysctl via integer MIB (CTL_KERN/KERN_OSTYPE) rather than the by-name string form —
// exercises the raw sysctl(2) array path. darwin engine only, golden-checked.
#include <stdio.h>
#include <sys/sysctl.h>

int main(void) {
    int mib[2] = { CTL_KERN, KERN_OSTYPE };
    char buf[64] = {0}; size_t l = sizeof buf;
    int r = sysctl(mib, 2, buf, &l, NULL, 0);
    printf("sysctl mib ostype=%s ok=%d\n", buf, r == 0); // Darwin 1
    return 0;
}
