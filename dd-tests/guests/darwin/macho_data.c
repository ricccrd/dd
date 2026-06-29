// Mach-O section access: initialized globals (__DATA), zero-init globals (__bss), and a __cstring
// literal — all reached after segment slide. darwin engine only, golden-checked.
#include <stdio.h>

int g_init[4] = {10, 20, 30, 40};
int g_bss[4];
const char *g_str = "DATAOK";

int main(void) {
    long s = 0;
    for (int i = 0; i < 4; i++) s += g_init[i] + g_bss[i];
    printf("data sum=%ld bss=%d str=%s\n", s, g_bss[0], g_str); // 100 0 DATAOK
    return 0;
}
