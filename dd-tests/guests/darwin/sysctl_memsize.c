// macOS-native sysctl: hw.memsize (total RAM, 64-bit) and hw.pagesize. BSD system-info; no Linux
// equivalent (Linux uses sysconf/_SC_PHYS_PAGES + /proc). darwin engine only, golden-checked.
#include <stdio.h>
#include <stdint.h>
#include <sys/sysctl.h>

int main(void) {
    uint64_t mem = 0; size_t l = sizeof mem;
    int r1 = sysctlbyname("hw.memsize", &mem, &l, NULL, 0);
    int ps = 0; l = sizeof ps;
    int r2 = sysctlbyname("hw.pagesize", &ps, &l, NULL, 0);
    printf("sysctl mem_ok=%d page_ok=%d\n",
           (r1 == 0 && mem > 0), (r2 == 0 && (ps == 4096 || ps == 16384))); // 1 1
    return 0;
}
