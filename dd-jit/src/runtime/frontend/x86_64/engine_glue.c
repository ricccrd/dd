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
