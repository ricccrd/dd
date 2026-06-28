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
    uint64_t dbg_ibsrc; // debug: guest PC of the last indirect branch (ret/jmp/call reg)
    uint64_t ic_miss;   // IBTC: set by an indirect-branch miss -> dispatcher fills g_ibtc for cpu->rip
    // x87 FPU: a register stack ST(0..7) emulated at DOUBLE precision (enough for printf %f of
    // doubles; loses the 80-bit long-double tail). st[fptop&7]=ST(0). Grows downward (push=--top).
    double st[8];        // x87 stack slots
    uint64_t fptop;      // top-of-stack index (only low 3 bits used)
    uint64_t fpsw, fpcw; // status word (C0-C3 in bits 8/9/10/14), control word
    uint64_t x87_ea;     // m80 (80-bit long double) operand address -> handled in C via R_X87*
    uint64_t divop;      // 64-bit div/idiv divisor -> 128/64 division done in C (ARM has no 128/64 divide)
};
#define OFF_IBSRC ((int)__builtin_offsetof(struct cpu, dbg_ibsrc))
#define OFF_ICMISS ((int)__builtin_offsetof(struct cpu, ic_miss))
#define OFF_ST ((int)__builtin_offsetof(struct cpu, st))
#define OFF_FPTOP ((int)__builtin_offsetof(struct cpu, fptop))
#define OFF_FPSW ((int)__builtin_offsetof(struct cpu, fpsw))
#define OFF_FPCW ((int)__builtin_offsetof(struct cpu, fpcw))
#define OFF_X87EA ((int)__builtin_offsetof(struct cpu, x87_ea))
#define OFF_DIVOP ((int)__builtin_offsetof(struct cpu, divop))
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
// x86 register encodings (== host reg numbers)
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };
