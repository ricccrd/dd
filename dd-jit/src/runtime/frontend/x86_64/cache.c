// dd/runtime/frontend/x86_64 -- JIT code cache + block map (the engine, jit86's copy of jit/cache).

// ---------------- JIT code cache (copied from jit.c) ----------------
#define CACHE_SZ (64u << 20)
static uint8_t *g_cache, *g_cp;
static uint8_t *g_emit_start;
static int g_trace, g_prof, g_noibtc, g_itrace; // g_itrace: 1 instruction per block (per-insn register dump)
static uint64_t g_disp_n, g_ibtc_fill;          // PROF: dispatcher round-trips, IBTC fills
static uint64_t g_tracecap;                     // if >0 under trace: stop after this many blocks (runaway guard)
int g_diag;                                     // diagnostics (FAULT_ON): print LOADED bases etc.
static int g_nochain;                           // WATCH file: disable chaining (exact per-block rip attribution)
static pthread_mutex_t g_jit_lock = PTHREAD_MUTEX_INITIALIZER; // serialize cache mutation once threaded
static int g_threaded;          // a guest thread exists -> take g_jit_lock + stop chaining/IBTC fills
static int g_pids_max = 0;      // cgroup pids.max (0 = unlimited)
static _Atomic int g_pids_cur = 1; // live task count (cgroup pids.current)
static uint64_t g_loadbase;                     // main program load base (for file-offset mapping)
static uint8_t *g_w8;
static uint8_t g_w8v;       // debug byte-watchpoint (armed via magic syscall 500)
static uint64_t g_malloc_n; // debug: count of __libc_malloc_impl entries
static const char *g_exe_path = "";
static const char *g_self_path = ""; // host path to this jit86 binary (for execve re-exec)
static pthread_key_t g_cpu_key;

#define MAP_N 65536
static struct {
    uint64_t gpc;
    void *host;
    void *body;
} g_map[MAP_N];
static int map_idx(uint64_t gpc) {
    uint32_t h = (uint32_t)((gpc >> 0) * 2654435761u) & (MAP_N - 1);
    for (int i = 0; i < MAP_N; i++) {
        uint32_t j = (h + i) & (MAP_N - 1);
        if (g_map[j].host && g_map[j].gpc == gpc) return j;
        if (!g_map[j].host) return -1;
    }
    return -1;
}
static void *map_host(uint64_t gpc) {
    int i = map_idx(gpc);
    return i < 0 ? NULL : g_map[i].host;
}
static void *map_body(uint64_t gpc) {
    int i = map_idx(gpc);
    return i < 0 ? NULL : g_map[i].body;
}
static void map_put(uint64_t gpc, void *host, void *body) {
    uint32_t h = (uint32_t)((gpc >> 0) * 2654435761u) & (MAP_N - 1);
    for (int i = 0; i < MAP_N; i++) {
        uint32_t j = (h + i) & (MAP_N - 1);
        if (!g_map[j].host) {
            g_map[j].gpc = gpc;
            g_map[j].host = host;
            g_map[j].body = body;
            return;
        }
    }
}
// IBTC: shared direct-mapped {guest target -> host body} cache, probed inline by
// indirect branches (ret/jmp reg/call reg) so a function return needn't round-trip
// the dispatcher. Plain data (no W^X); zeroed at start and on code-cache flush.
// (mirrors jit.c; simpler here since x16/x17/x19-x21 are scratch, not guest regs.)
#define IBTC_N 8192
static struct {
    uint64_t target;
    void *body;
} g_ibtc[IBTC_N];

static struct {
    uint32_t *slot;
    uint64_t target;
} g_pend[1 << 16];
static int g_npend;
static void add_pend(uint32_t *slot, uint64_t target) {
    if (g_npend < (1 << 16)) {
        g_pend[g_npend].slot = slot;
        g_pend[g_npend].target = target;
        g_npend++;
    }
}
static void patch_links_to(uint64_t gpc, void *body) {
    for (int i = 0; i < g_npend;) {
        if (g_pend[i].target == gpc) {
            int64_t d = ((uint8_t *)body - (uint8_t *)g_pend[i].slot) / 4;
            *g_pend[i].slot = 0x14000000u | ((uint32_t)d & 0x3FFFFFFu); // b body
            sys_icache_invalidate(g_pend[i].slot, 4);
            g_pend[i] = g_pend[--g_npend];
        } else
            i++;
    }
}

