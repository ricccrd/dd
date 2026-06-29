// C runtime teardown: register three atexit handlers and confirm they run in LIFO order on normal
// return from main. Uses raw write(2) (not stdio) so the byte order is deterministic regardless of
// when libc flushes its buffers. Portable -> all engines, golden-checked.
#include <stdlib.h>
#include <unistd.h>

static void a(void) { write(1, "a", 1); }
static void b(void) { write(1, "b", 1); }
static void c(void) { write(1, "c", 1); }

int main(void) {
    write(1, "atexit order=", 13);
    atexit(a);
    atexit(b);
    atexit(c);
    return 0; // LIFO -> "cba" appended on teardown: "atexit order=cba"
}
