// Mach-O __attribute__((constructor)) ordering: two prioritized constructors run before main, lower
// priority first. Exercises the Mach-O __mod_init_func section the loader walks. darwin engine only.
#include <stdio.h>

static int order = 0;
__attribute__((constructor(200))) static void a(void) { order = order * 10 + 2; }
__attribute__((constructor(300))) static void b(void) { order = order * 10 + 3; }

int main(void) {
    printf("ctor order=%d\n", order); // 23 (200 ran before 300)
    return 0;
}
