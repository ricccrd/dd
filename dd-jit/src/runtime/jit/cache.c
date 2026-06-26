// dd/runtime/jit -- the code cache, the gpc->host block map, and lazy inter-block chaining.
// One 64MB W^X MAP_JIT arena; blocks appended + chained (b/bl backpatch). Host-ISA engine state.

// ---------------- JIT code cache ----------------
#define CACHE_SZ (64u << 20)
static uint8_t *g_cache, *g_cp; // base, bump pointer
static uint8_t *g_emit_start;   // start of current translation (for icache flush)

// Threads: each guest thread runs run_guest on its OWN struct cpu, stored in a
// pthread TSD slot so emitted block-exit code can recover it from host TLS.
static pthread_key_t g_cpu_key;
static pthread_mutex_t g_jit_lock = PTHREAD_MUTEX_INITIALIZER;   // serializes translation
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER; // guards the FS-metadata cache under threads
#define CLK                                                                                                            \
    int _th = g_threaded;                                                                                              \
    if (_th) pthread_mutex_lock(&g_cache_lock)
#define CUL                                                                                                            \
    do {                                                                                                               \
        if (_th) pthread_mutex_unlock(&g_cache_lock);                                                                  \
    } while (0)
static int g_threaded; // >0 once a guest thread is spawned

static struct {
    uint64_t gpc;
    void *host;
    void *body;
} g_map[MAP_N];
static int map_idx(uint64_t gpc) {
    uint32_t h = (uint32_t)((gpc >> 2) * 2654435761u) & (MAP_N - 1);
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
    uint32_t h = (uint32_t)((gpc >> 2) * 2654435761u) & (MAP_N - 1);
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

// IBTC: a shared, direct-mapped hash table {guest target -> host body_ind} probed
// inline by indirect branches. Handles polymorphic dispatch (interpreters) that a
// per-site 1-entry cache can't. Plain data (no W^X); zeroed at start and on flush.
#define IBTC_N 8192
static struct {
    uint64_t target;
    void *body;
} g_ibtc[IBTC_N];
static uint64_t g_prof_cross, g_prof_miss, g_prof_xlate, g_prof_sys, g_lse_n;
static int g_prof; // PROF=1: dispatcher crossings / IBTC misses / translations

// Direct-branch edges whose target wasn't translated yet: remembered so the branch
// can be back-patched into a direct `b target.body` once the target is translated.
static struct {
    uint32_t *slot;
    uint64_t target;
    int is_bl;
} g_pend[1 << 16]; // is_bl: §B host bl, patch as bl
static int g_npend;
static void add_pend2(uint32_t *slot, uint64_t target, int is_bl) {
    if (g_npend < (1 << 16)) {
        g_pend[g_npend].slot = slot;
        g_pend[g_npend].target = target;
        g_pend[g_npend].is_bl = is_bl;
        g_npend++;
    }
}
static void add_pend(uint32_t *slot, uint64_t target) { add_pend2(slot, target, 0); }
static void patch_links_to(uint64_t gpc, void *body) {
    for (int i = 0; i < g_npend;) {
        if (g_pend[i].target == gpc) {
            int64_t d = ((uint8_t *)body - (uint8_t *)g_pend[i].slot) / 4;
            *g_pend[i].slot =
                (g_pend[i].is_bl ? 0x94000000u : 0x14000000u) | ((uint32_t)d & 0x3FFFFFFu); // bl / b target.body
            sys_icache_invalidate(g_pend[i].slot, 4);
            g_pend[i] = g_pend[--g_npend]; // swap-remove
        } else
            i++;
    }
}
