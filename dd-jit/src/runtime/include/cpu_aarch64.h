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
};
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
