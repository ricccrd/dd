// dd/runtime/frontend/x86_64 -- opt8 persistent translated-code cache (DDJIT_PCACHE=1; default OFF).
//
// Idea: cold start of a short-lived container is dominated by translating the dynamic linker (musl
// ld.so) + the program's startup path -- ~1000 blocks for `busybox echo`. That work is identical on
// every launch of the same binary. We persist the translated arena + block map to a file and mmap it
// back on the next run of the same binary, skipping translation entirely (~40% of internal cold start).
//
// What makes the bytes reusable across *processes*:
//   * The guest image + interp are mapped at FIXED addresses (PC_IMG_BASE / PC_INTERP_BASE), so guest
//     PCs (the block-map keys) and any guest address baked into host code are stable, and arena-internal
//     absolute pointers (g_map host/body, g_pend slots) + PC-relative chaining are valid as-is on reload.
//   * The ONLY host addresses baked into emitted blocks are block_return and &g_ibtc. The ddjit binary
//     itself is PIE, so those move per run; we recorded each baked site in g_reloc (a fixed 4-insn
//     movz/movk slot) and rewrite them on load.
//   * The JIT arena does NOT need fixing (MAP_JIT can't be MAP_FIXED anyway): g_map/g_pend are persisted
//     as arena OFFSETS and rebuilt against the live g_cache.
//
// Invalidation: the cache file is keyed by (engine version, cpu-struct size, MAP_N, IBTC_N, both fixed
// bases, entry PC, and the identity -- dev/ino/size/mtime -- of the guest binary AND its interpreter).
// Any mismatch / truncation / corruption -> graceful MISS: ignore the file and translate fresh, re-save.

#define PC_MAGIC 0x31304350544a4444ull // "DDJPCT01" (LE)
#define PC_VERSION 4                    // BUMP on ANY codegen change (different emitted bytes -> stale)
// Fixed guest VA bases (high, reliably free above the kernel-chosen heap/stack and below the dyld shared
// cache). Probed stable on Apple silicon; PIE images so we choose the base.
#define PC_IMG_BASE 0x0000040000000000ull    // 4 TB
#define PC_INTERP_BASE 0x0000048000000000ull // 4.5 TB

struct pc_hdr {
    uint64_t magic, version;
    uint64_t cpu_sz, map_n, ibtc_n;
    uint64_t img_base, interp_base;
    uint64_t bin_id;     // identity of guest binary + interp
    uint64_t entry_jump; // initial rip (sanity)
    uint64_t arena_used; // bytes of translated code
    uint64_t n_mapent, n_pend, n_reloc;
    uint64_t block_return_at; // block_return's host addr at save time -> the image-slide anchor on load
    uint64_t ibtc_at;         // g_ibtc host addr at save time (diagnostic)
};
struct pc_mapent {
    uint64_t gpc, host_off, body_off;
}; // host/body as arena offsets
struct pc_pend {
    uint64_t slot_off, target;
    uint32_t is_bl;
};

static uint64_t pcache_id_of(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uint64_t h = 1469598103934665603ull; // FNV-1a over identity fields + path
    uint64_t fields[4] = {(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                          (uint64_t)st.st_mtimespec.tv_sec};
    for (int i = 0; i < 4; i++) {
        h ^= fields[i];
        h *= 1099511628211ull;
    }
    for (const char *p = path; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ull;
    }
    return h;
}
// A per-engine-build tag mixed into every cache id so the cache self-invalidates across dd versions:
// host code emitted by a DIFFERENT engine build is never loaded (loading it would crash). __DATE__/
// __TIME__ change on every (re)build, so a user who updates dd transparently gets a fresh cache --
// they never need to clear ~/.dd/pcache by hand. (Old files just go unreferenced; harmless cruft.)
static uint64_t pcache_engine_id(void) {
    uint64_t h = 1469598103934665603ull;
    for (const char *p = __DATE__ " " __TIME__; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ull; }
    return h;
}
static uint64_t pcache_make_id(const char *prog_host, const char *interp_host) {
    uint64_t a = pcache_id_of(prog_host);
    uint64_t b = interp_host ? pcache_id_of(interp_host) : 0xABCDEFull;
    return (a ^ (b * 1099511628211ull)) ^ pcache_engine_id();
}
static void pcache_file(char *out, size_t n) {
    const char *dir = getenv("DDJIT_PCACHE_DIR");
    if (!dir || !dir[0]) dir = "/tmp/ddjit-pcache";
    mkdir(dir, 0700);
    snprintf(out, n, "%s/%016llx.pcache", dir, (unsigned long long)g_pc_binid);
}

// Rewrite every recorded host-pointer slot for THIS process. Every baked pointer lives in this PIE
// binary's image, which dyld slides as one unit, so a single delta -- (block_return now) minus
// (block_return at save time) -- relocates them ALL. We reconstruct each slot's saved value (and its
// destination register) from the existing movz/movk encoding, add the slide, and re-emit, so we don't
// need to remember which global each slot held.
static void pcache_relocate(uint64_t saved_block_return) {
    uint64_t slide = (uint64_t)block_return - saved_block_return;
    for (int i = 0; i < g_nreloc; i++) {
        uint32_t *p = (uint32_t *)(g_cache + g_reloc[i].off);
        int rd = (int)(p[0] & 0x1f); // movz/movk encode rd in bits[4:0] (same in all 4 words)
        uint64_t old = (uint64_t)((p[0] >> 5) & 0xffff) | ((uint64_t)((p[1] >> 5) & 0xffff) << 16) |
                       ((uint64_t)((p[2] >> 5) & 0xffff) << 32) | ((uint64_t)((p[3] >> 5) & 0xffff) << 48);
        uint64_t v = old + slide;
        p[0] = 0xD2800000u | (0 << 21) | (((uint32_t)(v) & 0xffff) << 5) | rd;       // movz
        p[1] = 0xF2800000u | (1 << 21) | (((uint32_t)(v >> 16) & 0xffff) << 5) | rd; // movk #16
        p[2] = 0xF2800000u | (2 << 21) | (((uint32_t)(v >> 32) & 0xffff) << 5) | rd; // movk #32
        p[3] = 0xF2800000u | (3 << 21) | (((uint32_t)(v >> 48) & 0xffff) << 5) | rd; // movk #48
    }
}

// Returns 1 on a cache hit (arena + maps restored, translation can be skipped). On ANY mismatch,
// truncation, short read, or allocation failure it returns 0 (graceful MISS -> caller translates fresh).
static int pcache_load(uint64_t entry_jump) {
    char path[1024];
    pcache_file(path, sizeof path);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    struct pc_hdr h;
    if (read(fd, &h, sizeof h) != (ssize_t)sizeof h) {
        close(fd);
        return 0;
    }
    if (h.magic != PC_MAGIC || h.version != PC_VERSION || h.cpu_sz != sizeof(struct cpu) || h.map_n != MAP_N ||
        h.ibtc_n != IBTC_N || h.img_base != PC_IMG_BASE || h.interp_base != PC_INTERP_BASE ||
        h.bin_id != g_pc_binid || h.entry_jump != entry_jump || h.arena_used > CACHE_SZ || h.n_mapent > MAP_N ||
        h.n_pend > (1u << 16) || h.n_reloc > (1u << 16)) {
        close(fd);
        return 0;
    }
    // pull the variable-size sections
    struct pc_mapent *me = h.n_mapent ? malloc(h.n_mapent * sizeof *me) : NULL;
    struct pc_pend *pe = h.n_pend ? malloc(h.n_pend * sizeof *pe) : NULL;
    int ok = (h.n_mapent == 0 || me) && (h.n_pend == 0 || pe);
    if (ok && h.n_reloc)
        ok = read(fd, g_reloc, h.n_reloc * sizeof g_reloc[0]) == (ssize_t)(h.n_reloc * sizeof g_reloc[0]);
    if (ok && h.n_mapent) ok = read(fd, me, h.n_mapent * sizeof *me) == (ssize_t)(h.n_mapent * sizeof *me);
    if (ok && h.n_pend) ok = read(fd, pe, h.n_pend * sizeof *pe) == (ssize_t)(h.n_pend * sizeof *pe);
    // Arena bytes: read into a heap buffer, then memcpy into the W^X arena under write mode.
    // (read()'s kernel copyout cannot target a MAP_JIT page gated by the thread's W^X state;
    //  a userspace memcpy can, once pthread_jit_write_protect_np(0) opens the write window.)
    uint8_t *abuf = NULL;
    if (ok && h.arena_used) {
        abuf = malloc(h.arena_used);
        ok = abuf != NULL;
        for (uint64_t got = 0; ok && got < h.arena_used;) {
            ssize_t r = read(fd, abuf + got, h.arena_used - got);
            if (r <= 0) {
                ok = 0;
                break;
            }
            got += (uint64_t)r;
        }
        if (ok) {
            pthread_jit_write_protect_np(0);
            memcpy(g_cache, abuf, h.arena_used);
            pthread_jit_write_protect_np(1);
        }
        free(abuf);
    }
    close(fd);
    if (!ok) {
        free(me);
        free(pe);
        return 0;
    }
    // rebuild the engine state from the offset-relative records
    g_nreloc = (int)h.n_reloc;
    for (uint64_t i = 0; i < h.n_mapent; i++)
        map_put(me[i].gpc, g_cache + me[i].host_off, g_cache + me[i].body_off);
    g_npend = 0;
    for (uint64_t i = 0; i < h.n_pend; i++)
        add_pend2((uint32_t *)(g_cache + pe[i].slot_off), pe[i].target, (int)pe[i].is_bl);
    g_cp = g_cache + h.arena_used;
    free(me);
    free(pe);
    // re-slide every baked PIE host pointer for THIS process + publish the restored code to the i-cache
    pthread_jit_write_protect_np(0);
    pcache_relocate(h.block_return_at);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(g_cache, h.arena_used);
    memset(g_ibtc, 0, sizeof g_ibtc); // runtime cache: repopulates lazily
    g_pcache_loaded = 1;
    return 1;
}

// Persist the current arena + maps (atomic temp+rename). Called at guest exit.
static void pcache_save(void) {
    if (!g_pcache || !g_pc_binid || g_cp == g_cache) return;
    if (g_pcache_loaded && g_prof_xlate == 0) return; // full hit, nothing new to persist
    uint64_t _t0 = g_coldprof ? coldprof_now_ns() : 0;
    // count occupied map slots
    uint64_t nmap = 0;
    for (int i = 0; i < MAP_N; i++)
        if (g_map[i].host) nmap++;
    char path[1024], tmp[1056];
    pcache_file(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.%d.tmp", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    struct pc_hdr h;
    memset(&h, 0, sizeof h);
    h.magic = PC_MAGIC;
    h.version = PC_VERSION;
    h.cpu_sz = sizeof(struct cpu);
    h.map_n = MAP_N;
    h.ibtc_n = IBTC_N;
    h.img_base = PC_IMG_BASE;
    h.interp_base = PC_INTERP_BASE;
    h.bin_id = g_pc_binid;
    h.entry_jump = g_pc_entry;
    h.arena_used = (uint64_t)(g_cp - g_cache);
    h.n_mapent = nmap;
    h.n_pend = (uint64_t)g_npend;
    h.n_reloc = (uint64_t)g_nreloc;
    h.block_return_at = (uint64_t)block_return;
    h.ibtc_at = (uint64_t)g_ibtc;
    // Build the whole image in one heap buffer and write it with a single syscall (per-record write()s
    // were ~1300 syscalls and dominated the one-time save cost).
    size_t total = sizeof h + (size_t)g_nreloc * sizeof g_reloc[0] + (size_t)nmap * sizeof(struct pc_mapent) +
                   (size_t)g_npend * sizeof(struct pc_pend) + h.arena_used;
    uint8_t *buf = malloc(total), *w = buf;
    int ok = buf != NULL;
    if (ok) {
        memcpy(w, &h, sizeof h);
        w += sizeof h;
        memcpy(w, g_reloc, (size_t)g_nreloc * sizeof g_reloc[0]);
        w += (size_t)g_nreloc * sizeof g_reloc[0];
        for (int i = 0; i < MAP_N; i++) {
            if (!g_map[i].host) continue;
            struct pc_mapent e = {g_map[i].gpc, (uint64_t)((uint8_t *)g_map[i].host - g_cache),
                                  (uint64_t)((uint8_t *)g_map[i].body - g_cache)};
            memcpy(w, &e, sizeof e);
            w += sizeof e;
        }
        for (int i = 0; i < g_npend; i++) {
            struct pc_pend e = {(uint64_t)((uint8_t *)g_pend[i].slot - g_cache), g_pend[i].target,
                                (uint32_t)g_pend[i].is_bl};
            memcpy(w, &e, sizeof e);
            w += sizeof e;
        }
        memcpy(w, g_cache, h.arena_used); // read from W^X arena is always permitted
        w += h.arena_used;
        ok = write(fd, buf, total) == (ssize_t)total;
    }
    free(buf);
    close(fd);
    if (ok)
        rename(tmp, path);
    else
        unlink(tmp);
    if (g_coldprof)
        fprintf(stderr, "[pcache] save %s (%llu B arena, %llu blocks) in %.3f ms\n", ok ? "ok" : "FAILED",
                (unsigned long long)h.arena_used, (unsigned long long)nmap, (coldprof_now_ns() - _t0) / 1e6);
}
#define PCACHE_SAVE_HOOK pcache_save()
