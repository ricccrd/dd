// frontend/x86_64/engine_glue.c -- x86-only engine globals for the shared jit/cache.c lift (PR1).
//
// When the x86 target swapped `#include "../frontend/x86_64/cache.c"` for the shared
// `#include "../jit/cache.c"`, it lost the x86-only diagnostic/trace globals that the old
// per-arch cache.c carried (the shared cache.c only defines the engine-core globals:
// g_cache/g_cp/g_emit_start, g_cpu_key, g_jit_lock/g_cache_lock, g_threaded, g_map+helpers,
// g_ibtc, g_pend/g_npend, g_prof + the aarch64-shaped g_prof_* counters). These globals are
// USED across the x86 frontend (emit.c/dispatch.c/translate.c/elf.c/sigframe.c) and set in
// targets/linux_x86_64.c, so the x86 unity TU must define each exactly once here.
//
// Storage class / type / initializer are copied verbatim from the former frontend/x86_64/cache.c
// so behavior is unchanged. NOTE: g_diag is intentionally non-static (external linkage) because
// frontend/x86_64/elf.c references it via `extern int g_diag;`.
//
// Must be #included AFTER os/linux/container/state.c and BEFORE jit/cache.c + emit.c in the TU.

static int g_trace, g_noibtc, g_itrace; // g_itrace: 1 instruction per block (per-insn register dump)
static uint64_t g_disp_n, g_ibtc_fill;  // PROF: dispatcher round-trips, IBTC fills
// ---- W4-C: rep cmps/scas idiom (R_REPSTR) globals + the NOREPCMP A/B kill-switch ----
static uint64_t g_repstr_n;     // PROF: rep cmps/scas idiom firings
static uint64_t g_repstr_elems; // PROF: elements consumed by the rep cmps/scas idiom
static int g_norepcmp = -1;     // gate: NOREPCMP=1 -> rep cmps/scas use the naive per-element loop
static int norepcmp(void) {     // (the A/B kill-switch; also the bit-exact per-element oracle path)
    if (g_norepcmp < 0) g_norepcmp = getenv("NOREPCMP") ? 1 : 0;
    return g_norepcmp;
}

// ---- opt2: x86-only 2-way set-associative IBTC (gate IBTC1WAY=1) ----
// The x86 engine gets its OWN indirect-branch-target cache here, leaving the SHARED jit/cache.c g_ibtc
// (used by the aarch64 host engine + the W5-C ibtc_ent restructure) byte-for-byte unchanged. A tree-walk
// interpreter (busybox awk evaluate()) returns to a handful of call sites; a direct-mapped (1-way) table
// thrashes when two hot targets alias one slot (~100% conflict misses). 2 ways let an A/B alternation
// coexist so both hit. Each set is two {target,body} entries = 32 bytes; the emitted probe loads the base
// from cpu->ibtc_base (1 insn) and indexes (set<<5). Plain data (no W^X); zeroed at start + on cache flush.
// IBTC1WAY=1 falls back to the OLD shared-g_ibtc 1-way probe/fill (exact prior behavior) for A/B + safety.
#define XIBTC_SETS 8192
#define XIBTC_WAYS 2
static struct {
    uint64_t target;
    void *body;
} g_xibtc[XIBTC_SETS * XIBTC_WAYS];
static int g_ibtc1way = -1; // -1 = uninitialized; cached getenv("IBTC1WAY")
static int ibtc1way(void) {
    if (g_ibtc1way < 0) g_ibtc1way = (getenv("IBTC1WAY") != NULL);
    return g_ibtc1way;
}

// ---- opt8 cold-start timing helper (COLDPROF=1; diagnostic-only, zero-cost when off) ----
static int g_coldprof;
static inline uint64_t coldprof_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

// ---- opt8 persistent translated-code cache (DDJIT_PCACHE=1; default OFF) ----
// Deterministic guest/arena bases let us persist the translated arena + block map and mmap it back on
// the next run of the same binary, skipping retranslation. Emitted blocks bake a handful of HOST pointers
// (block_return, &g_ibtc, &g_fast_count, &g_pending, &g_t2cnt[], &dd_rep_movs/stos, the inline-fastsys
// counters) -- every one lives in THIS PIE binary's image, which dyld slides by ONE vmaddr_slide per run.
// We record each baked slot's arena offset (g_reloc; emitted as a fixed 4-insn movz/movk so it is
// rewritable to any address) and, on load, add (block_return_now - block_return_at_save) -- the single
// image slide -- to each. All x86-only, all gated by g_pcache; default OFF -> emit is byte-identical.
static int g_pcache;          // persistent cache feature on (DDJIT_PCACHE set)
static int g_pcache_loaded;   // cache hit this run (translation skipped)
static uint64_t g_pc_binid;   // identity of (guest binary + interp)
static uint64_t g_pc_entry;   // initial guest rip (cache-validity sanity)
static uint64_t g_force_base; // if nonzero, load_elf() maps the next image MAP_FIXED here (one-shot)
// Documentary kinds (relocation is slide-based + kind-agnostic; these only label save dumps / call sites).
enum {
    PRELOC_BLOCKRET = 1,
    PRELOC_IBTC = 2,
    PRELOC_HOSTGLOBAL = 3 // any other &host-global / &host-func baked into a block (slid by the image slide)
};
static struct {
    uint32_t off; // byte offset into the arena of a fixed 4-insn movz/movk host-pointer load
    uint8_t kind; // PRELOC_* (documentary)
} g_reloc[1 << 16];
static int g_nreloc;

static uint64_t g_tracecap;             // if >0 under trace: stop after this many blocks (runaway guard)
int g_diag;                             // diagnostics (FAULT_ON): print LOADED bases etc. (extern'd by elf.c)
static int g_nochain;                   // WATCH file: disable chaining (exact per-block rip attribution)
static uint64_t g_loadbase;             // main program load base (for file-offset mapping)
static uint8_t *g_w8;
static uint8_t g_w8v;                   // debug byte-watchpoint (armed via magic syscall 500)
static uint64_t g_malloc_n;             // debug: count of __libc_malloc_impl entries
static const char *g_exe_path = "";
static const char *g_self_path = "";    // host path to this jit86 binary (for execve re-exec)

// ---- W3b SSE/string-SIMD idiom upgrade (gate NOSSEOPT=1) ----
// g_pmovmskb_n: # of `pmovmskb` sites lowered to the cascading-shift NEON sequence
// (vs the old per-byte scalar spill loop). Printed under PROF.
static uint64_t g_pmovmskb_n;
static int g_nosseopt = -1; // -1 = uninitialized; cached getenv("NOSSEOPT")
static int nosseopt(void) {
    if (g_nosseopt < 0) g_nosseopt = (getenv("NOSSEOPT") != NULL);
    return g_nosseopt;
}

// ---- opt7 address-gen / memory-fold fast path (gate NOEAOPT=1) ----
// Disabling it reverts emit_ea + the mov [base+disp] load/store fold to the exact baseline
// codegen (movconst-built disp + base+0 load/store). Env read once, then cached.
static int g_noeaopt = -1; // -1 = uninitialized; cached getenv("NOEAOPT")
static int noeaopt(void) {
    if (g_noeaopt < 0) g_noeaopt = (getenv("NOEAOPT") != NULL);
    return g_noeaopt;
}

// ---- guest_base bias-fold (non-PIE ET_EXEC; see docs/design/nonpie-pagezero.md) ----
// A non-PIE x86_64 image maps HIGH (+g_nonpie_bias) but its baked absolute pointers stay LOW (link vaddr);
// a guest load/store through one would hit the unmapped low address and trap (one SIGSEGV per access).
// Instead fold +bias into the effective host address at emit time when the EA is a LOW image address
// (< 4GiB; stack/heap/mmap/libs are all >= the engine's 4GiB __PAGEZERO). g_nonpie_lo/g_nonpie_bias are
// forward-declared here (tentative; merge with the real defs set by load_elf in elf.c / translate.c). 0
// for PIE/static-PIE -> guestfold_on() is 0 -> codegen byte-identical to baseline.
static uint64_t g_nonpie_lo, g_nonpie_bias;
static int g_x86fold = -1; // cached: NOGUESTFOLD=1 disables (A/B kill-switch); default on for non-PIE
static int guestfold_on(void) {
    if (g_x86fold < 0) g_x86fold = (getenv("NOGUESTFOLD") == NULL);
    return g_x86fold && g_nonpie_lo != 0;
}

// ---- W5B adaptive tier-2 (x86 engine) — x86-only glue over the SHARED W4E substrate ----
// The hotness counter table (g_t2cnt/g_t2gpc/g_t2n), the dedup slot allocator (t2_slot), the promotion
// threshold (g_t2thresh, default 1000), the tier-2-build flag (g_tier2_build), the last-body handoff
// (g_last_body) and the promotion counter (g_prof_t2) all live in the shared jit/cache.c (the W4E
// substrate, #included right after this TU). We DON'T redeclare them here — that would be a redefinition
// in the x86 unity build. The x86 engine only adds the two pieces the shared substrate does not carry:
//
//   * its own kill switch NOTIER2X (the shared one is NOTIER2; x86 keeps a distinct env name and gate so
//     the substrate's g_notier2/tier2_env_init -- aarch64-only, never called in the x86 TU -- stay inert),
//   * the flag-save-elision PROF counter g_prof_t2fold (an x86-specific transform with no aarch64 analogue;
//     the shared g_prof_t2 has no field for it).
//
// On the x86->arm64 engine, a tier-1 hot loop carries TWO cross-ISA per-iteration redundancies the
// same-ISA aarch64 engine does not: (1) the conditional back-edge trampoline (`b.cond Ltaken; b body`
// = 2 taken branches/iter) and (2) per-iteration NZCV materialization (`mrs;str cpu->nzcv`) that is dead
// on the back-edge when the loop re-overwrites flags before reading them. Tier-2 folds the back-edge to
// one `b.cond body` AND (for the deferred sub/cmp case proven flag-dead at loop top) hoists the flag save
// onto the loop-exit edge. See frontend/x86_64/translate.c (emit_selfloop_x86 / tier2_promote).
static int g_notier2x = -1;    // NOTIER2X=1 kill switch (pure tier-1 baseline); -1 = uninitialized
static uint64_t g_prof_t2fold; // PROF: of the promoted loops, how many also elided the per-iteration flag save
static int notier2x(void) {
    if (g_notier2x < 0) g_notier2x = (getenv("NOTIER2X") != NULL);
    return g_notier2x;
}
