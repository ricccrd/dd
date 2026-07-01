// dd/runtime/include -- aarch64 guest CPU state. The cpu struct is the guest register file +
// engine scratch (shadow stack for §B return prediction, mangle scratch). Offsets are baked into
// emitted code. x18/x28/x30 are STOLEN by the engine (x28=cpu ptr, x30=host link, x18=scratch);
// guest values live in cpu->x[]. See docs/OPTIMIZATIONS.md.

// ---------------- guest CPU state ----------------
// Offsets are baked into the emitted code below; keep them in sync.
struct cpu {
    // 0x000: x0..x30
    uint64_t x[31];
    // 0x0F8 (248)
    uint64_t sp;
    // 0x100 (256) next guest PC (set on block exit)
    uint64_t pc;
    // 0x108 (264) emulated TPIDR_EL0
    uint64_t tls;
    // 0x110 (272) why the block exited
    uint64_t reason;
    // 0x118 (280)
    uint64_t host_sp;
    // 0x120 (288) host x19..x30
    uint64_t host_save[12];
    // 0x180 (384) guest V0..V31 (128-bit each) -- NEON/FP state
    uint64_t v[64];
    // 0x380 (896) host V8..V15 (callee-saved, preserved by run_block)
    uint64_t host_v[16];
    // 0x400 (1024) guest condition flags
    uint64_t nzcv;
    // 0x408 (1032) IBTC: cache-literal addr to fill after an inline-cache miss
    uint64_t ic_site;
    int exited;
    int exit_code;
    // execve set pc/sp directly -> dispatcher must NOT advance pc
    int redirect;
    // CLONE_CHILD_CLEARTID addr: cleared + futex-woken on thread exit
    uint64_t ctid;
    // per-thread blocked-signal mask
    uint64_t sigmask;
    // §B shadow stack: (guest_ret, host_ret) PAIRS, interleaved (per-thread)
    uint64_t sstk[2048];
    // shadow-stack pointer (counts PAIRS; 64-bit so emitted ldr is simple)
    uint64_t ssp;
    // §B mangle + shadow push/ret scratch spill (per-thread; x28=cpu holds the base)
    uint64_t mscratch[8];
    // §B shadow stack: guest SP captured at each `bl` -- disambiguates frames (recursion/unwind)
    uint64_t gsp[1024];
    uint64_t alt_sp, alt_size;
    // sigaltstack (per-thread)
    uint32_t alt_flags;
    // gettid()/tgkill() identity: the guest thread id this cpu runs as. 0 on the initial (init) thread,
    // which reports container_pid() (==1); each spawned thread gets a unique id. Appended after the
    // baked-offset fields so the emitted-code offsets above are unaffected.
    int tid;
    // Thread-DIRECTED pending signals (1<<signo), the per-thread analogue of the process-wide g_pending.
    // A tgkill()/tkill() to THIS thread sets a bit here so the signal is delivered by this thread alone
    // (a process-directed signal in g_pending may be taken by any thread). Drained by maybe_deliver_signal.
    volatile uint64_t tpending;
    // SMC: guest VA of the most recent `ic ivau` (icache invalidate). The emitter spills it here on the
    // R_ICFLUSH exit so smc_icflush() can do PRECISE invalidation -- only drop the translation map + IBTC
    // when that guest page was actually translated, instead of nuking everything on every guest icache
    // flush (V8 issues one per freshly-written line). Appended after the baked-offset fields.
    uint64_t smc_va;
};
#define OFF_SMCVA offsetof(struct cpu, smc_va)
#define OFF_V 384
#define OFF_HOSTV 896
#define OFF_NZCV 1024
#define OFF_ICSITE 1032
#define OFF_SP 248
#define OFF_PC 256
#define OFF_TLS 264
#define OFF_RSN 272
#define OFF_HSP 280
#define OFF_HSAVE 288
// Offset safety (C3): the baked numeric OFF_* above are duplicated into emitted machine code AND the
// run_block/block_return asm. A struct edit that shifts any of them must fail the BUILD, not corrupt a
// guest at runtime -- so assert each baked offset against the real field. (See REFACTOR.md "Offset safety".)
_Static_assert(offsetof(struct cpu, x) == 0, "OFF x[] base drifted");
_Static_assert(offsetof(struct cpu, sp) == OFF_SP, "OFF_SP drifted");
_Static_assert(offsetof(struct cpu, pc) == OFF_PC, "OFF_PC drifted");
_Static_assert(offsetof(struct cpu, tls) == OFF_TLS, "OFF_TLS drifted");
_Static_assert(offsetof(struct cpu, reason) == OFF_RSN, "OFF_RSN drifted");
_Static_assert(offsetof(struct cpu, host_sp) == OFF_HSP, "OFF_HSP drifted");
_Static_assert(offsetof(struct cpu, host_save) == OFF_HSAVE, "OFF_HSAVE drifted");
_Static_assert(offsetof(struct cpu, v) == OFF_V, "OFF_V drifted");
_Static_assert(offsetof(struct cpu, host_v) == OFF_HOSTV, "OFF_HOSTV drifted");
_Static_assert(offsetof(struct cpu, nzcv) == OFF_NZCV, "OFF_NZCV drifted");
_Static_assert(offsetof(struct cpu, ic_site) == OFF_ICSITE, "OFF_ICSITE drifted");
// §B shadow stack base (pairs)
#define OFF_SSTK offsetof(struct cpu, sstk)
#define OFF_SSP offsetof(struct cpu, ssp)
#define OFF_MSCRATCH offsetof(struct cpu, mscratch)
// §B shadow guest-SP array (1 per frame)
#define OFF_GSP offsetof(struct cpu, gsp)
// §B: real x28 reserved = cpu pointer
#define CPUREG 28
// guest_base bias-fold (non-PIE ET_EXEC only): re-target a guest load/store of a LOW image pointer to the
// HIGH mapping (+g_nonpie_bias) at translate time instead of faulting. The bias is materialized INLINE per
// folded access (no stolen host register) -- stealing a 5th GPR was unsafe: a Linux/Go guest uses every
// GPR (Go reserves R27/R28 on ARM64), so any instruction form not flagged by gpr_field_mask would read the
// stolen reg's engine value instead of the guest's. g_nonpie_lo/g_nonpie_bias are defined (and set by
// load_elf) in os/linux/elf.c + container/vfs.c (compiled LATER in the same unity TU); forward-declared
// static here (tentative -> merges with the single later def). Both 0 for PIE/static-PIE -> guestbase_on()
// is 0 -> the fold is inert and codegen stays byte-identical (the test matrix never sees a non-PIE image).
static uint64_t g_nonpie_lo, g_nonpie_bias;
// master enable (default on); cleared at startup by NOGUESTFOLD=1 for an A/B kill-switch.
static int g_guestfold = 1;
static int guestbase_on(void) { return g_guestfold && g_nonpie_lo != 0; }
// A1: x16/x17 engine-private (IBTC scratch); guest x16/x17 mangled like x18 so they never live in
// the host reg -> the per-indirect-branch red-zone stash/restore of x16/x17 disappears.
// x18 volatile, x28=cpu, x30=host link (§B). NOSTEAL1617=1 reverts to the 3-reg stolen set at startup.
static int g_steal1617 = 1;
static int is_stolen(int r) {
    return r == 18 || r == 28 || r == 30 || (g_steal1617 && (r == 16 || r == 17));
}
#define R_BRANCH 0
#define R_SYSCALL 1
