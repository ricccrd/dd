// dd/runtime/include -- aarch64 guest CPU state. The cpu struct is the guest register file +
// engine scratch (shadow stack for §B return prediction, mangle scratch). Offsets are baked into
// emitted code. x18/x28/x30 are STOLEN by the engine (x28=cpu ptr, x30=host link, x18=scratch);
// guest values live in cpu->x[]. See docs/OPTIMIZATIONS.md.

// ---------------- guest CPU state ----------------
// Offsets are baked into the emitted code below; keep them in sync.
struct cpu {
    uint64_t x[31];         // 0x000: x0..x30
    uint64_t sp;            // 0x0F8 (248)
    uint64_t pc;            // 0x100 (256) next guest PC (set on block exit)
    uint64_t tls;           // 0x108 (264) emulated TPIDR_EL0
    uint64_t reason;        // 0x110 (272) why the block exited
    uint64_t host_sp;       // 0x118 (280)
    uint64_t host_save[12]; // 0x120 (288) host x19..x30
    uint64_t v[64];         // 0x180 (384) guest V0..V31 (128-bit each) -- NEON/FP state
    uint64_t host_v[16];    // 0x380 (896) host V8..V15 (callee-saved, preserved by run_block)
    uint64_t nzcv;          // 0x400 (1024) guest condition flags
    uint64_t ic_site;       // 0x408 (1032) IBTC: cache-literal addr to fill after an inline-cache miss
    int exited;
    int exit_code;
    int redirect;         // execve set pc/sp directly -> dispatcher must NOT advance pc
    uint64_t ctid;        // CLONE_CHILD_CLEARTID addr: cleared + futex-woken on thread exit
    uint64_t sigmask;     // per-thread blocked-signal mask
    uint64_t sstk[2048];  // §B shadow stack: (guest_ret, host_ret) PAIRS, interleaved (per-thread)
    uint64_t ssp;         // shadow-stack pointer (counts PAIRS; 64-bit so emitted ldr is simple)
    uint64_t mscratch[8]; // §B mangle + shadow push/ret scratch spill (per-thread; x28=cpu holds the base)
    uint64_t gsp[1024];   // §B shadow stack: guest SP captured at each `bl` -- disambiguates frames (recursion/unwind)
    uint64_t alt_sp, alt_size;
    uint32_t alt_flags; // sigaltstack (per-thread)
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
#define OFF_SSTK offsetof(struct cpu, sstk) // §B shadow stack base (pairs)
#define OFF_SSP offsetof(struct cpu, ssp)
#define OFF_MSCRATCH offsetof(struct cpu, mscratch)
#define OFF_GSP offsetof(struct cpu, gsp)                             // §B shadow guest-SP array (1 per frame)
#define CPUREG 28                                                     // §B: real x28 reserved = cpu pointer
static int is_stolen(int r) { return r == 18 || r == 28 || r == 30; } // x18 volatile, x28=cpu, x30=host link (§B)
#define R_BRANCH 0
#define R_SYSCALL 1
