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
