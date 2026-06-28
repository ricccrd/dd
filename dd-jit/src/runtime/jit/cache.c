// dd/runtime/jit -- the code cache, the gpc->host block map, and lazy inter-block chaining.
// One 64MB W^X MAP_JIT arena; blocks appended + chained (b/bl backpatch). Host-ISA engine state.

// ---------------- JIT code cache ----------------
#define CACHE_SZ (64u << 20)
// base, bump pointer
static uint8_t *g_cache, *g_cp;
// start of current translation (for icache flush)
static uint8_t *g_emit_start;

// ---- dual-mapped (W^X-toggle-free) code cache ----
// g_cache/g_cp are the RW (writer) alias; the engine EXECUTES through an RX alias of the
// SAME physical pages at g_cache + g_rw2rx (created by vm_remap'ing to a second address,
// the Apple-Silicon dual-map JIT technique). All PC-relative emission/back-patching is a
// difference of two cache addresses, so it is alias-invariant and needs no conversion;
// only the few ABSOLUTE handoffs (run_block target, IBTC/IC body literals, icache flush)
// convert RW<->RX. g_rw2rx == 0 selects the single-MAP_JIT fallback that toggles the whole
// region's W^X per translation/IC-fill (NODUALMAP=1).
static ptrdiff_t g_rw2rx;     // RX_addr - RW_addr (0 in fallback)
static int g_dualmap;         // 1 when the RW/RX dual mapping is active
static uint64_t g_wx_toggles; // # of pthread_jit_write_protect_np() calls actually made (PROF)
#define J_RX(p) ((void *)((uint8_t *)(p) + g_rw2rx)) // RW alias addr -> RX alias addr
#define J_RW(p) ((void *)((uint8_t *)(p) - g_rw2rx)) // RX alias addr -> RW alias addr
// The single W^X gate. Under dual mapping it is a no-op: writes land on the RW alias and
// execution reads the RX alias, so no per-region permission flip (and no peer-thread race).
static inline void jit_wprot(int enable_exec) {
    if (g_dualmap) return;
    g_wx_toggles++;
    pthread_jit_write_protect_np(enable_exec);
}
// Allocate one dual-mapped code cache: a plain anon RW region + an RX alias of the SAME physical
// pages at a second VA (vm_remap). Returns 0 and fills *rw / *delta(=RX-RW) on success.
#include <mach/mach.h>
#include <mach/mach_vm.h>
static int dualmap_alloc(uint8_t **rw_out, ptrdiff_t *delta_out) {
    uint8_t *rw = mmap(NULL, CACHE_SZ, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rw == MAP_FAILED) return -1;
    mach_vm_address_t rx = 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &rx, CACHE_SZ, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                                     (mach_vm_address_t)rw, FALSE, &cur, &max, VM_INHERIT_DEFAULT);
    if (kr == KERN_SUCCESS) kr = mach_vm_protect(mach_task_self(), rx, CACHE_SZ, FALSE, VM_PROT_READ | VM_PROT_EXECUTE);
    if (kr != KERN_SUCCESS) {
        munmap(rw, CACHE_SZ);
        return -1;
    }
    *rw_out = rw;
    *delta_out = (uint8_t *)rx - rw;
    return 0;
}
// PROF-only: accumulated wall time spent in the translate region (the part the W^X toggles bracket).
static uint64_t g_xlate_ns;
static inline uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

// Threads: each guest thread runs run_guest on its OWN struct cpu, stored in a
// pthread TSD slot so emitted block-exit code can recover it from host TLS.
static pthread_key_t g_cpu_key;
// serializes translation
static pthread_mutex_t g_jit_lock = PTHREAD_MUTEX_INITIALIZER;
// guards the FS-metadata cache under threads
static pthread_mutex_t g_cache_lock = PTHREAD_MUTEX_INITIALIZER;
#define CLK                                                                                                            \
    int _th = g_threaded;                                                                                              \
    if (_th) pthread_mutex_lock(&g_cache_lock)
#define CUL                                                                                                            \
    do {                                                                                                               \
        if (_th) pthread_mutex_unlock(&g_cache_lock);                                                                  \
    } while (0)
// >0 once a guest thread is spawned
static int g_threaded;

static struct {
    uint64_t gpc;
    void *host;
    void *body;
} g_map[MAP_N];
static int map_idx(uint64_t gpc) {
    // hash shift is per-arch (frontend/<arch>/abi.h G_GPC_HASH_SHIFT): aarch64 PCs are 4-byte aligned
    // (>>2 spreads), x86 PCs are byte-granular (>>0). Pure tuning constant; aarch64 value is 2 (unchanged).
    uint32_t h = (uint32_t)((gpc >> G_GPC_HASH_SHIFT) * 2654435761u) & (MAP_N - 1);
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
    uint32_t h = (uint32_t)((gpc >> G_GPC_HASH_SHIFT) * 2654435761u) & (MAP_N - 1);
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
// 16-byte aligned so each {target,body} entry sits in a single 16-byte granule -> a
// naturally-aligned 128-bit ldp/stp is single-copy atomic under FEAT_LSE2 (all Apple
// Silicon). That atomicity is what lets a lock-free reader observe {target,body} as an
// indivisible pair: it can never see new-target/old-body or old-target/new-body (the
// torn-dispatch hazard). See G_IBTC_FILL (writer) + emit_ibranch (reader).
typedef struct {
    uint64_t target;
    void *body;
} ibtc_ent;
_Alignas(16) static ibtc_ent g_ibtc[IBTC_N];

// ---- W5C: race-free threaded IBTC fill ----
// g_mtibtc: enable threaded shared-hash IBTC fill (NOMTIBTC=1 disables -> revert to the
// locked-dispatcher path where threaded indirect branches always miss to the C dispatcher).
// g_mtfill: PROF count of threaded shared-hash publishes. g_futexq: per-address futex
// wait queues (NOFUTEXQ=1 -> the legacy single global mutex + broadcast in thread.c).
static int g_mtibtc = 1;
static int g_futexq = 1;
static uint64_t g_mtfill;
// Atomic 128-bit RELEASE publish of a {target, body} pair into a 16-byte-aligned IBTC slot.
// Single writer (the dispatcher holds g_jit_lock across every fill); many lock-free readers.
// `dmb ish` orders all prior stores (incl. the body block's translation + its IC IVAU, both
// already DSB-complete before this point) before the pair becomes observable; the `stp` of two
// X regs to a 16-byte-aligned address is single-copy atomic under FEAT_LSE2 (all Apple Silicon),
// so it is mutually atomic with the reader's plain `ldp`. We use explicit asm rather than a
// 16-byte __atomic (which could lower to a lock-based libatomic call that would NOT be atomic
// against the lock-free ldp reader). Layout: target at +0, body at +8 (matches struct ibtc_ent).
static inline void ibtc_publish(ibtc_ent *e, uint64_t target, void *body) {
    __asm__ volatile("dmb ish\n\t"
                     "stp %1, %2, [%0]\n\t"
                     :
                     : "r"(e), "r"(target), "r"(body)
                     : "memory");
}
static uint64_t g_prof_cross, g_prof_miss, g_prof_xlate, g_prof_sys, g_lse_n;
// PROF=1: dispatcher crossings / IBTC misses / translations
static int g_prof;
// A3 §B instrumentation (PROF=1). Runtime: shadow pushes executed, predicted-return FAST hits (host
// ret, RAS), and returns that fell through emit_shadow_ret to the IBTC fallback. Translate-time:
// how many guest `bl` sites the depth-gate steered to §B (shadow push) vs the cheap leaf Stage-B path.
static uint64_t g_prof_shpush, g_prof_shret_hit, g_prof_shret_fb;
static uint64_t g_prof_bl_shadow, g_prof_bl_leaf;

// ============================================================================
// ARM-B1 FEASIBILITY INSTRUMENTATION (IBPROF) -- gated, measurement-only.
// Forces EVERY guest indirect transfer (br/blr/ret) through the C dispatcher
// (reason R_IBLOG) so we can record per-(site) traffic + the executed target
// stream. The normal inline IBTC is bypassed (no fills) while IBPROF is on, so
// the cost numbers it reports are SHAPE-only (counts), never timing.
//   - per-site histogram: which guest indirect branch dominates traffic
//   - last-value (monomorphic) hit rate per site  (= per-site IC effectiveness)
//   - 1st-order Markov hit rate per (site, prev-target)  (= the B1 thread guard
//     hit rate: given we just ran handler A, is the next handler always B?)
// ============================================================================
#ifndef R_IBLOG
#define R_IBLOG 3
#endif
static int g_ibprof;    // IBPROF=1
static int g_vdbetrace; // VDBETRACE=1 (ARM-B1 prototype gate; defined here, used in translate.c)
#define IBSITE_N 8192
static struct ibsite {
    uint64_t site, count, last_tgt, mono; // mono = #times target==previous target at this site
    uint64_t h1, h2, h3;                  // most-recent 3 targets (h1=newest) for the order-k predictors
} g_ibsite[IBSITE_N];
// One open-addressed predictor table per Markov order. Each entry is a (key)->last-observed-next
// "last value" predictor; hit = #times the actual next equalled the prediction. The order-k key
// folds the site with the last k targets, so order-k models "given the last k opcode handlers, is
// the next handler fixed?" -- exactly the guard a depth-k thread of the VDBE loop would carry.
#define IBTRANS_N (1u << 19)
static struct ibtrans {
    uint64_t site, pred, count, hit;
} g_ibtrans1[IBTRANS_N], g_ibtrans2[IBTRANS_N], g_ibtrans3[IBTRANS_N];
static uint64_t g_ib_total;
static inline uint64_t ibmix(uint64_t a, uint64_t b) {
    uint64_t h = a * 0x9E3779B97F4A7C15ull ^ (b + 0x7F4A7C15ull);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    return h;
}
static struct ibsite *ib_find_site(uint64_t site) {
    uint32_t h = (uint32_t)(ibmix(site, 0) & (IBSITE_N - 1));
    for (int i = 0; i < IBSITE_N; i++) {
        uint32_t j = (h + i) & (IBSITE_N - 1);
        if (g_ibsite[j].count == 0 && g_ibsite[j].site == 0) {
            g_ibsite[j].site = site;
            return &g_ibsite[j];
        }
        if (g_ibsite[j].site == site) return &g_ibsite[j];
    }
    return &g_ibsite[0]; // table full: degrade, don't crash
}
// Keyed predictor slot: the fold key lives in a side array `keys`; `tab[j].site` carries the real
// site (for per-site aggregation in the dump). NULL on probe overflow (rare).
static uint64_t g_k1[IBTRANS_N], g_k2[IBTRANS_N], g_k3[IBTRANS_N];
static struct ibtrans *ib_slot(struct ibtrans *tab, uint64_t *keys, uint64_t key, uint64_t site) {
    if (key == 0) key = 1;
    uint32_t h = (uint32_t)(key & (IBTRANS_N - 1));
    for (int i = 0; i < 64; i++) {
        uint32_t j = (h + i) & (IBTRANS_N - 1);
        if (keys[j] == 0) {
            keys[j] = key;
            tab[j].site = site;
            return &tab[j];
        }
        if (keys[j] == key) return &tab[j];
    }
    return NULL;
}
static inline void ib_pred(struct ibtrans *t, uint64_t target) {
    if (!t) return;
    if (t->count == 0)
        t->pred = target; // seed (uncounted)
    else if (t->pred == target)
        t->hit++;
    else
        t->pred = target; // last-value, adapt
    t->count++;
}
static void ib_log(uint64_t site, uint64_t target) {
    g_ib_total++;
    struct ibsite *s = ib_find_site(site);
    int n = (s->count != 0) + (s->h2 != 0 || s->count > 1) + (s->h3 != 0 || s->count > 2);
    (void)n;
    if (s->count >= 1) { // have h1
        if (target == s->h1) s->mono++;
        ib_pred(ib_slot(g_ibtrans1, g_k1, ibmix(site, s->h1), site), target);
    }
    if (s->count >= 2) // have h1,h2
        ib_pred(ib_slot(g_ibtrans2, g_k2, ibmix(ibmix(site, s->h1), s->h2), site), target);
    if (s->count >= 3) // have h1,h2,h3
        ib_pred(ib_slot(g_ibtrans3, g_k3, ibmix(ibmix(ibmix(site, s->h1), s->h2), s->h3), site), target);
    s->h3 = s->h2;
    s->h2 = s->h1;
    s->h1 = target;
    s->last_tgt = target;
    s->count++;
}
// ARM-B1 VDBETRACE prototype counters/state.
static uint64_t g_vt_inline;    // # of speculative-direct-chain (SDC) JT-dispatch sites emitted
static uint64_t g_vt_fills;     // # of SDC (re)specializations performed by the dispatcher
static uint64_t g_vt_force_inl; // # of dispatch blocks force-inlined into a predecessor (path-specialize)
static int g_vt_hitcount;       // VTHITCOUNT=1: emit an inline SDC guard-hit counter (perturbs timing)
static uint64_t g_vt_hit;       // # of SDC guard hits (threaded direct chains taken)
// ARM-B1 SDC fill: the speculative direct-chain at a JT dispatch missed to the dispatcher (genuinely-new
// target). (Re)specialize the site to this target: write the guard literal and back-patch the in-cache
// `b` slot to a DIRECT branch into the target's body (regs-live entry). `rec_rx` is the RX address of the
// 2-quad record { [0]=guard target literal, [1]=RW addr of the `b` slot } (low tag bit already stripped).
static void sdc_fill(uint64_t rec_rx, uint64_t target) {
    uint64_t *rec = (uint64_t *)J_RW((void *)rec_rx);
    void *body = map_body(target); // RW body (post-prologue, regs-live entry)
    if (!body) return;             // not translated (shouldn't happen: dispatcher just translated pc)
    uint32_t *bslot = (uint32_t *)rec[1]; // RW addr of the direct-branch slot
    jit_wprot(0);
    int64_t d = ((uint8_t *)body - (uint8_t *)bslot) / 4; // RW-RW delta == RX-RX delta (alias-invariant)
    *bslot = 0x14000000u | ((uint32_t)d & 0x3FFFFFFu);     // b body
    rec[0] = target;                                       // arm the guard
    jit_wprot(1);
    sys_icache_invalidate(J_RX(bslot), 4); // the patched `b` executes from the RX alias
    g_vt_fills++;
}
static void vt_dump(void) {
    if (!g_vdbetrace) return;
    fprintf(stderr, "[vdbetrace] sdc_sites=%llu sdc_fills=%llu force_inlined_dispatch=%llu\n",
            (unsigned long long)g_vt_inline, (unsigned long long)g_vt_fills, (unsigned long long)g_vt_force_inl);
    if (g_vt_hitcount)
        fprintf(stderr, "[vdbetrace] guard_hits=%llu (threaded direct chains taken; VTHITCOUNT perturbs timing)\n",
                (unsigned long long)g_vt_hit);
}
// ARM-B1: recognize a clang jump-table switch dispatch at a guest `br xN`. The compiler emits
//   ldrh wM,[xB,wI,uxtw #1] ; adr xA,. ; add xN,xA,wM,sxth #2 ; br xN
// (an indexed 16-bit offset table). Bit-exact opcode match on the 3 predecessors + Rd==br.Rn.
static int is_jt_dispatch_br(uint64_t gpc) {
    uint32_t a = *(uint32_t *)(gpc - 12), b = *(uint32_t *)(gpc - 8), c = *(uint32_t *)(gpc - 4),
             br = *(uint32_t *)gpc;
    int brn = (int)((br >> 5) & 31);
    return (a & 0xFFE0FC00u) == 0x78605800u   // ldrh wM,[xB,wI,uxtw #1]
           && (b & 0x9F000000u) == 0x10000000u // adr xA, .
           && (c & 0xFFE0FC00u) == 0x8B20A800u // add xd,xa,wm,sxth #2
           && (int)(c & 31) == brn;            // add Rd feeds the br
}
// True if the block at `tgt` is a JT-dispatch block (reaches a JT-dispatch `br` with no intervening
// call/return/svc/unconditional-b that would end the block first). Used to force-inline the shared
// dispatch into each predecessor so the resulting `br` is PATH-SPECIFIC (the B1 specialization).
static int is_jt_dispatch_block(uint64_t tgt) {
    for (int i = 3; i < 28; i++) {
        uint64_t p = tgt + 4u * (unsigned)i;
        uint32_t in = *(uint32_t *)p;
        if ((in & 0xFFFFFC1Fu) == 0xD61F0000u) return is_jt_dispatch_br(p); // br
        if ((in & 0xFC000000u) == 0x94000000u) return 0;                    // bl
        if ((in & 0xFFFFFC1Fu) == 0xD63F0000u) return 0;                    // blr
        if ((in & 0xFFFFFC1Fu) == 0xD65F0000u) return 0;                    // ret
        if (in == 0xD4000001u) return 0;                                    // svc
        if ((in & 0xFC000000u) == 0x14000000u) return 0; // unconditional b: block ends before any br
    }
    return 0;
}
static int ibsite_cmp(const void *a, const void *b) {
    uint64_t ca = ((const struct ibsite *)a)->count, cb = ((const struct ibsite *)b)->count;
    return ca < cb ? 1 : ca > cb ? -1 : 0;
}
static void ib_dump(void) {
    if (!g_ibprof) return;
    // copy + sort sites by traffic
    static struct ibsite snap[IBSITE_N];
    int n = 0;
    for (int i = 0; i < IBSITE_N; i++)
        if (g_ibsite[i].count) snap[n++] = g_ibsite[i];
    qsort(snap, (size_t)n, sizeof snap[0], ibsite_cmp);
    fprintf(stderr, "[ibprof] total_indirect=%llu distinct_sites=%d\n", (unsigned long long)g_ib_total, n);
    fprintf(stderr, "[ibprof]  rank        site      count    %%traffic  mono%%   o1%%    o2%%    o3%%   dt\n");
    for (int r = 0; r < n && r < 24; r++) {
        // aggregate order-k hit rates for this site (hits / counted-transitions, excluding seeds)
        uint64_t h1 = 0, c1 = 0, h2 = 0, c2 = 0, h3 = 0, c3 = 0;
        int dt = 0;
        for (uint32_t k = 0; k < IBTRANS_N; k++) {
            if (g_ibtrans1[k].count && g_ibtrans1[k].site == snap[r].site) {
                h1 += g_ibtrans1[k].hit;
                c1 += g_ibtrans1[k].count - 1;
                dt++;
            }
            if (g_ibtrans2[k].count && g_ibtrans2[k].site == snap[r].site) {
                h2 += g_ibtrans2[k].hit;
                c2 += g_ibtrans2[k].count - 1;
            }
            if (g_ibtrans3[k].count && g_ibtrans3[k].site == snap[r].site) {
                h3 += g_ibtrans3[k].hit;
                c3 += g_ibtrans3[k].count - 1;
            }
        }
        double pct = 100.0 * (double)snap[r].count / (double)(g_ib_total ? g_ib_total : 1);
        double monop = 100.0 * (double)snap[r].mono / (double)(snap[r].count ? snap[r].count : 1);
        double o1 = c1 ? 100.0 * (double)h1 / (double)c1 : 0.0;
        double o2 = c2 ? 100.0 * (double)h2 / (double)c2 : 0.0;
        double o3 = c3 ? 100.0 * (double)h3 / (double)c3 : 0.0;
        fprintf(stderr, "[ibprof]  %3d  %12llx  %11llu  %7.2f  %5.1f  %5.2f  %5.2f  %5.2f  %4d\n", r,
                (unsigned long long)snap[r].site, (unsigned long long)snap[r].count, pct, monop, o1, o2, o3, dt);
    }
}

// ---------------- W4E adaptive tier-2 ----------------
// W4E tier-2: a hot self-loop's in-cache back-edge counter reached threshold -> the dispatcher
// recompiles (promotes) the block with the optimized codegen, then resumes (pc already = block start).
// (The reason code normally lives next to R_BRANCH/R_SYSCALL in include/cpu_aarch64.h; it is defined here
// because this engine integration is confined to the jit/ + frontend/aarch64/ translate units.)
// W5B: the x86 engine reuses this substrate but its reason-code space already uses 2 for R_CPUID, so it
// pre-defines R_TIER2=7 in include/cpu_x86_64.h. Guard the aarch64 default so the x86 value wins in the
// x86 unity build; aarch64 (whose cpu_aarch64.h does not define it) still gets 2. No aarch64 change.
#ifndef R_TIER2
#define R_TIER2 2
#endif
//
// A same-ISA aarch64->aarch64 transliterator already keeps every guest GPR in its host reg and flags
// native, so tier-1 hot loops are near-native EXCEPT the conditional back-edge: a self-loop `b.cond` is
// laid as `b.cond Ltaken; b body` -- TWO taken host branches per iteration. Tier-2 recompiles a hot
// self-loop folding that into a single `b.cond body` (native-equivalent).
//
// Hotness must be measured IN-CACHE: a chained hot loop never returns to the dispatcher, so a
// dispatcher-side counter is blind to it. Each translated single-block self-loop therefore carries a
// cheap, flag-free, decrementing back-edge counter (initialized to the threshold). When it hits zero the
// back-edge exits R_TIER2; the dispatcher promotes the block (recompile + swap the map entry + repoint
// pending chains/IBTC) and resumes -- the remaining iterations run folded in-cache. The counter is
// removed by the recompile, so the promoted steady state has ZERO tier-2 overhead.
#define T2_MAX 8192
// per self-loop iteration counter (plain RW data -- NOT in the W^X cache, which is RX while executing;
// emitted code stores to it via an adrp+add absolute address)
static uint64_t g_t2cnt[T2_MAX];
static uint64_t g_t2gpc[T2_MAX]; // the loop-start gpc owning each slot (dedup on re-translate)
static int g_t2n;                // slots allocated
static int g_notier2;            // NOTIER2=1 kill switch (pure tier-1 baseline)
static uint64_t g_t2thresh = 1000; // back-edge iterations before promotion (TIER2_THRESHOLD env)
static uint64_t g_prof_t2;       // PROF: blocks promoted to tier-2
static int g_tier2_build;        // set while recompiling a block as tier-2 (fold, no counter, no map_put)
static void *g_last_body;        // body pointer of the most recent translate_block (for the promoter)
// Kill-switch + threshold env, read ONCE (idempotent static guard; the W4E diff read these in the target
// main(), relocated here to keep the integration inside the allowed jit/ + frontend/aarch64/ units).
static int g_t2_envdone;
static void tier2_env_init(void) {
    if (g_t2_envdone) return;
    g_t2_envdone = 1;
    g_notier2 = getenv("NOTIER2") != NULL;
    const char *t = getenv("TIER2_THRESHOLD");
    if (t && atoll(t) > 0) g_t2thresh = (uint64_t)atoll(t);
}
// Find (or allocate) the counter slot for a self-loop whose body starts at gpc. Re-translation of the
// same loop reuses its slot so the count is not reset (and a re-translated promoted loop won't re-arm a
// fresh counter). Returns -1 if the table is full (-> emit plain tier-1, no counter).
static int t2_slot(uint64_t gpc) {
    for (int i = 0; i < g_t2n; i++)
        if (g_t2gpc[i] == gpc) return i;
    if (g_t2n >= T2_MAX) return -1;
    int i = g_t2n++;
    g_t2gpc[i] = gpc;
    g_t2cnt[i] = g_t2thresh;
    return i;
}

// Direct-branch edges whose target wasn't translated yet: remembered so the branch
// can be back-patched into a direct `b target.body` once the target is translated.
static struct {
    uint32_t *slot;
    uint64_t target;
    int is_bl;
// is_bl: §B host bl, patch as bl
} g_pend[1 << 16];
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
                // bl / b target.body
                (g_pend[i].is_bl ? 0x94000000u : 0x14000000u) | ((uint32_t)d & 0x3FFFFFFu);
            sys_icache_invalidate(g_pend[i].slot, 4);
            // swap-remove
            g_pend[i] = g_pend[--g_npend];
        } else
            i++;
    }
}

// fork() COWs the RW and RX aliases independently, so after a guest fork the child's two views of the
// SAME cache silently diverge (writes through RW never reach the COW'd RX -> the child executes stale/
// zero pages). In the child we build a FRESH dual map (private, correctly re-aliased) and drop the
// inherited translations; the child re-translates on demand. No-op without dual mapping -- the MAP_JIT
// RWX fallback's execute permission lives in the page tables and is inherited across fork() correctly.
// Must run in the child after fork(), before its next run_block.
static void jit_after_fork(void) {
    if (!g_dualmap) return;
    uint8_t *old_rw = g_cache, *old_rx = (uint8_t *)J_RX(g_cache);
    uint8_t *rw;
    ptrdiff_t d;
    if (dualmap_alloc(&rw, &d) != 0) return; // alloc failure (extremely rare): leave map as-is
    g_cache = g_cp = rw;
    g_rw2rx = d;
    memset(g_map, 0, sizeof g_map);
    memset(g_ibtc, 0, sizeof g_ibtc);
    g_npend = 0;
    munmap(old_rw, CACHE_SZ);
    munmap(old_rx, CACHE_SZ);
}
