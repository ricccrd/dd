// dd/runtime/include -- x86-64 guest CPU state. r[16]=rax..r15, rip, nzcv (ARM-flag substrate for
// x86 EFLAGS), fs/gs_base, xmm in v[32], x87 ST(0..7) at double precision. Offsets baked into emitted
// code. Differs entirely from cpu_aarch64.h -- why os/linux can't be literally shared yet.

// ---------------- guest CPU state ----------------
// Offsets are baked into emitted code; keep in sync (see the OFF_* defines).
struct cpu {
    uint64_t r[16];         // 0x000: rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15
    uint64_t rip;           // 0x080 (128) next guest PC (set on block exit)
    uint64_t nzcv;          // 0x088 (136) saved ARM flags (our x86 flag substrate)
    uint64_t fs_base;       // 0x090 (144) x86 TLS base (arch_prctl SET_FS)
    uint64_t gs_base;       // 0x098 (152)
    uint64_t reason;        // 0x0A0 (160) R_BRANCH / R_SYSCALL
    uint64_t host_sp;       // 0x0A8 (168)
    uint64_t host_save[12]; // 0x0B0 (176) host x19..x30
    uint64_t host_v[16];    // 0x110 (272) host v8..v15 (callee-saved)
    uint64_t v[32];         // 0x190 (400) guest xmm0..15 (128-bit each)
    uint64_t mmscratch[2];  // 0x290 (656) 16-byte scratch for pmovmskb etc.
    int exited;
    int exit_code;
    int redirect;  // execve/sigreturn set rip directly -> don't advance
    uint64_t ctid; // CLONE_CHILD_CLEARTID
    uint64_t sigmask;
    uint64_t alt_sp, alt_size, alt_flags; // sigaltstack (C-only; used by os/linux service)
    uint64_t dbg_ibsrc;                   // debug: guest PC of the last indirect branch (ret/jmp/call reg)
    uint64_t ic_miss; // IBTC: set by an indirect-branch miss -> dispatcher fills g_ibtc for cpu->rip
    // x87 FPU: a register stack ST(0..7) emulated at DOUBLE precision (enough for printf %f of
    // doubles; loses the 80-bit long-double tail). st[fptop&7]=ST(0). Grows downward (push=--top).
    double st[8];        // x87 stack slots
    uint64_t fptop;      // top-of-stack index (only low 3 bits used)
    uint64_t fpsw, fpcw; // status word (C0-C3 in bits 8/9/10/14), control word
    uint64_t x87_ea;     // m80 (80-bit long double) operand address -> handled in C via R_X87*
    uint64_t divop;      // 64-bit div/idiv divisor -> 128/64 division done in C (ARM has no 128/64 divide)
    uint64_t ibtc_base;  // opt2: base of the x86 2-way IBTC g_xibtc (set once at run_guest entry) -> 1-insn
                         // load on the indirect hot path (replaces the 3-insn movz/movk &table materialize)
    // AVX/AVX2/AVX-512 register file, emulated in avx.c (R_AVX). v[] above already holds xmm0..15 (= the
    // low 128 bits of zmm0..15), so legacy-SSE codegen that bakes OFF_V stays byte-identical; these hold
    // the extra width. ymm_n = v[2n..2n+1] ++ vhi[2n..2n+1]; zmm0..15 add vz[]; zmm16..31 live in vx[].
    uint64_t vhi[32]; // bits[128:256) of ymm/zmm0..15
    uint64_t vz[64];  // bits[256:512) of zmm0..15
    uint64_t vx[128]; // zmm16..31 (full 512-bit each)
    uint64_t kreg[8]; // AVX-512 opmask registers k0..k7
    // x86 PF (parity) substrate. There is no real PF in the ARM-NZCV flag model, so flag-setting integer
    // ops stash the low byte of their result here (FP compares stash a synthetic byte: 0 -> PF=1 unordered,
    // 1 -> PF=0). The parity consumers (setp/setnp, cmovp/cmovnp) read it and compute even-parity = x86 PF.
    // Added at the END of the struct so the baked OFF_* offsets above are unaffected.
    uint64_t pf;
    // x86 AF (auxiliary carry, EFLAGS bit 4) substrate. Like PF, there is no ARM-NZCV equivalent, so the
    // add/sub/adc/sbb/inc/dec/neg paths stash (a ^ b ^ result) here -- AF is bit 4 of that value (the carry
    // out of bit 3). Logical ops (AND/OR/XOR), where x86 leaves AF undefined, store 0 (matches qemu's
    // CC_OP_LOGIC). The consumers (lahf/pushfq) extract bit 4; popfq/sahf write it back.
    uint64_t af;
    // gettid()/tgkill() identity (shared os/linux/{thread,proc,signal}.c): the guest thread id this cpu
    // runs as. 0 on the init thread (reports container_pid()==1); each spawned thread gets a unique id.
    int tid;
    // Thread-DIRECTED pending signals (1<<signo) -- the per-thread analogue of g_pending. A tkill/tgkill to
    // THIS thread sets a bit here so only this thread delivers it. Drained by maybe_deliver_signal.
    volatile uint64_t tpending;
};
#define OFF_PF ((int)__builtin_offsetof(struct cpu, pf))
#define OFF_AF ((int)__builtin_offsetof(struct cpu, af))
#define OFF_EXITED ((int)__builtin_offsetof(struct cpu, exited)) // int exited; int exit_code (the next word)
#define OFF_IBSRC ((int)__builtin_offsetof(struct cpu, dbg_ibsrc))
#define OFF_ICMISS ((int)__builtin_offsetof(struct cpu, ic_miss))
#define OFF_ST ((int)__builtin_offsetof(struct cpu, st))
#define OFF_FPTOP ((int)__builtin_offsetof(struct cpu, fptop))
#define OFF_FPSW ((int)__builtin_offsetof(struct cpu, fpsw))
#define OFF_FPCW ((int)__builtin_offsetof(struct cpu, fpcw))
#define OFF_X87EA ((int)__builtin_offsetof(struct cpu, x87_ea))
#define OFF_DIVOP ((int)__builtin_offsetof(struct cpu, divop))
#define OFF_IBTC ((int)__builtin_offsetof(struct cpu, ibtc_base)) // opt2: x86 2-way IBTC base (1-insn load)
#define R_OFF(i) ((i) * 8)
#define OFF_RIP 128
#define OFF_NZCV 136
#define OFF_FS 144
#define OFF_GS 152
#define OFF_RSN 160
#define OFF_HSP 168
#define OFF_HSAVE 176
#define OFF_HOSTV 272
#define OFF_V 400
#define OFF_MM 656
// Offset safety (C3): the baked numeric OFF_* above are duplicated into emitted machine code AND the
// run_block/block_return asm. A struct edit that shifts any of them must fail the BUILD, not corrupt a
// guest at runtime -- so assert each baked offset against the real field. (See REFACTOR.md "Offset safety".)
_Static_assert(__builtin_offsetof(struct cpu, r) == 0, "OFF r[] (R_OFF base) drifted");
_Static_assert(__builtin_offsetof(struct cpu, rip) == OFF_RIP, "OFF_RIP drifted");
_Static_assert(__builtin_offsetof(struct cpu, nzcv) == OFF_NZCV, "OFF_NZCV drifted");
_Static_assert(__builtin_offsetof(struct cpu, fs_base) == OFF_FS, "OFF_FS drifted");
_Static_assert(__builtin_offsetof(struct cpu, gs_base) == OFF_GS, "OFF_GS drifted");
_Static_assert(__builtin_offsetof(struct cpu, reason) == OFF_RSN, "OFF_RSN drifted");
_Static_assert(__builtin_offsetof(struct cpu, host_sp) == OFF_HSP, "OFF_HSP drifted");
_Static_assert(__builtin_offsetof(struct cpu, host_save) == OFF_HSAVE, "OFF_HSAVE drifted");
_Static_assert(__builtin_offsetof(struct cpu, host_v) == OFF_HOSTV, "OFF_HOSTV drifted");
_Static_assert(__builtin_offsetof(struct cpu, v) == OFF_V, "OFF_V drifted");
_Static_assert(__builtin_offsetof(struct cpu, mmscratch) == OFF_MM, "OFF_MM drifted");
#define R_BRANCH 0
#define R_SYSCALL 1
#define R_CPUID 2
#define R_X87FLD 3  // fld m80  -> C converts 80-bit extended -> double, pushes
#define R_X87FSTP 4 // fstp m80 -> C converts ST0 double -> 80-bit, pops
#define R_DIV 5     // 64-bit div  -> C: rax,rdx = (rdx:rax) /,% divop  (unsigned 128/64)
#define R_IDIV 6    // 64-bit idiv -> C: signed 128/64
// W5B tier-2: a hot single-block self-loop's in-cache back-edge counter hit threshold -> the dispatcher
// recompiles (promotes) the block (folded back-edge + dead-flag-save elision), swaps it in live, resumes
// (rip already == loop start). See frontend/x86_64/translate.c tier2_promote().
#define R_TIER2 7
// W4-C: rep cmps/scas (A6/A7/AE/AF) idiom -> C helper does the whole (possibly REP/REPE/REPNE)
// compare/scan in one round-trip (descriptor in cpu->divop), writing the exact x86 RCX/RSI/RDI +
// ZF/SF/CF/OF end-state. Gate NOREPCMP=1 -> naive per-element oracle loop. See do_repstr().
#define R_REPSTR 8
// VEX/EVEX-encoded AVX/AVX2/AVX-512 instruction -> exit the block and emulate it in C (do_avx), which
// reads/writes the v[]/vhi[]/vz[]/vx[]/kreg[] register file + guest memory, then advances rip past it.
// Correctness-first (one block exit per AVX insn); the SSE/scalar fast paths are untouched.
#define R_AVX 9
// Legacy (non-VEX) 0F38/0F3A instruction (SSSE3/SSE4.1/SSE4.2/AES/SHA/PCLMUL/CRC32/MOVBE) -> exit the
// block and emulate it in C (do_sse3b), which reads/writes the v[] xmm file + GPRs + memory, then advances
// rip. Correctness-first (one block exit per insn), mirroring the R_AVX VEX path. See avx.c do_sse3b().
#define R_SSE3B 10
// x87 transcendental (D9 F0-FF subset: F2XM1/FYL2X/FPTAN/FPATAN/FYL2XP1/FSINCOS/FSIN/FCOS) -> exit the
// block and compute it in C (x87_func) via host libm on the double-precision ST stack. These have no
// ARM/SSE counterpart. cpu->x87_ea carries the X87_* selector below. See x86_ops.c x87_func().
#define R_X87FUNC 11
enum { X87_F2XM1, X87_FYL2X, X87_FPTAN, X87_FPATAN, X87_FYL2XP1, X87_FSINCOS, X87_FSIN, X87_FCOS };
// x86 register encodings (== host reg numbers)
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };
