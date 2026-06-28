// Extracted from ../vfs.c: guest address-space mapping registry (g_gmap + gmap_add/del/find_len/reset_all)
// Not standalone -- #included by ../vfs.c at the original position (verbatim move, identical
// preprocessed TU). Relies on ../vfs.c's preceding globals/headers; see vfs.c for context.
// Guest address-space registry. Every guest mapping (ELF image, interp, heap, stack, anon/file mmap) is
// tracked so execve() can tear the inherited space down before loading the new image. Without this a
// post-fork exec keeps the PARENT's dense layout, and load_elf must bias a non-PIE ET_EXEC off its fixed
// vaddr (macOS __PAGEZERO reserves the low 4 GB) -> the image's baked absolute refs collide with the
// densely-packed inherited maps -> SIGSEGV. Resetting reproduces the clean fresh-exec layout that works.
#define GMAP_N 8192 // was 1024 -- a heavy guest overflowed it, leaking the untracked mappings at execve teardown
static struct { uint64_t addr, len; } g_gmap[GMAP_N];
static int g_ngmap;
static void gmap_add(uint64_t addr, uint64_t len) {
    if (!addr || addr == (uint64_t)-1 || len == 0 || g_ngmap >= GMAP_N) return;
    g_gmap[g_ngmap].addr = addr;
    g_gmap[g_ngmap].len = len;
    g_ngmap++;
}
static void gmap_del(uint64_t addr) {
    for (int i = 0; i < g_ngmap; i++)
        if (g_gmap[i].addr == addr) { g_gmap[i] = g_gmap[--g_ngmap]; return; }
}
// The tracked extent (incl. any guard tail) of a mapping that starts at addr, or 0 if untracked.
static uint64_t gmap_find_len(uint64_t addr) {
    for (int i = 0; i < g_ngmap; i++)
        if (g_gmap[i].addr == addr) return g_gmap[i].len;
    return 0;
}
static void gmap_reset_all(void) { // munmap every tracked guest mapping; the caller reloads fresh
    for (int i = 0; i < g_ngmap; i++) munmap((void *)g_gmap[i].addr, (size_t)g_gmap[i].len);
    g_ngmap = 0;
}
