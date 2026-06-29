// macOS-native JIT page pattern: allocate a page, write arm64 code (mov w0,#42; ret) into it, make
// it executable, and call it. On real darwin this needs MAP_JIT + W^X toggling; guest JIT runtimes
// (JVM/V8/LuaJIT) all depend on it. KNOWN dd gap (W^X / no exec pages) — see GAPS rwx-mmap. xfail.
#include <stdio.h>
#include <sys/mman.h>
#include <string.h>
#include <pthread.h>

int main(void) {
    size_t sz = 4096;
    void *p = mmap(NULL, sz, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) { printf("jit ret=-1\n"); return 0; }
    // arm64: mov w0, #42 ; ret
    unsigned int code[] = { 0x52800540u, 0xd65f03c0u };
    pthread_jit_write_protect_np(0);
    memcpy(p, code, sizeof code);
    pthread_jit_write_protect_np(1);
    __builtin___clear_cache((char *)p, (char *)p + sizeof code);
    int (*f)(void) = (int (*)(void))p;
    int r = f();
    munmap(p, sz);
    printf("jit ret=%d\n", r); // 42
    return 0;
}
