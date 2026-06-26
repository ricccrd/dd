// dd/runtime/frontend/x86_64 -- the x86-64-Linux-guest JIT (jit86), brought in WHOLE.
//
// jit86 is under active improvement upstream (poc/runtime/jit86/jit86.c) and has a DIFFERENT cpu
// struct + its own (basic) container runtime, so it is not yet decomposed onto the shared jit/ +
// os/linux/ layer. Stage-1 goal: build the x86 binary alongside aarch64. DEDUP (next stage): lift it
// onto the shared engine + container layer via cpu-access accessors + canonical syscall ids, then
// split it the way aarch64 already is. Re-sync with: make sync-jit86.

// jit86.c — an x86-64-guest JIT (x86-64 -> ARM64) for Linux guests on macOS/arm64.
//
// Sibling of runtime/jit/jit.c (which is aarch64->aarch64). See DESIGN.md for the
// full "what breaks / what doesn't" rationale. Short version:
//
//   * The ISA-AGNOSTIC scaffolding (code cache, guest-PC->host-code map, direct-
//     branch chaining, the run_block/block_return trampolines, the Linux->macOS
//     syscall bodies, the ELF loader, rootfs path rewriting) is COPIED+ADAPTED from
//     jit.c. We can't refactor jit.c (it's under active dev), so we duplicate.
//   * The FRONT-END is new: an x86-64 decoder + per-opcode ARM64 codegen, replacing
//     jit.c's "copy the instruction verbatim" core (which only works same-arch).
//
// Register model (the win from x86 having only 16 GPRs, see DESIGN.md §4):
//   guest  rax rcx rdx rbx rsp rbp rsi rdi  r8..r15
//   host    x0  x1  x2  x3  x4  x5  x6  x7  x8..x15   (guest reg# == host reg#)
//   cpu ptr : x28 (PINNED for the whole block)   scratch : x16,x17   forbidden: x18
//   flags   : ARM nzcv saved/restored to cpu->nzcv (exact for cmp/test->jcc, §9)
//
// Status: BOOTSTRAP. Implements enough to run a freestanding write+exit guest and a
// growing slice toward simple busybox. Unknown opcodes print their bytes and exit —
// that is the iterative workflow (run -> see unimpl -> add it -> repeat).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/times.h>
#include <poll.h>
#include <sys/event.h>   // kqueue: backs epoll/timerfd/inotify on macOS
#include <dirent.h>
#include <signal.h>
#include <libkern/OSCacheControl.h>

// ---------------- guest CPU state ----------------
// Offsets are baked into emitted code; keep in sync (see the OFF_* defines).
struct cpu {
    uint64_t r[16];          // 0x000: rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15
    uint64_t rip;            // 0x080 (128) next guest PC (set on block exit)
    uint64_t nzcv;           // 0x088 (136) saved ARM flags (our x86 flag substrate)
    uint64_t fs_base;        // 0x090 (144) x86 TLS base (arch_prctl SET_FS)
    uint64_t gs_base;        // 0x098 (152)
    uint64_t reason;         // 0x0A0 (160) R_BRANCH / R_SYSCALL
    uint64_t host_sp;        // 0x0A8 (168)
    uint64_t host_save[12];  // 0x0B0 (176) host x19..x30
    uint64_t host_v[16];     // 0x110 (272) host v8..v15 (callee-saved)
    uint64_t v[32];          // 0x190 (400) guest xmm0..15 (128-bit each)
    uint64_t mmscratch[2];   // 0x290 (656) 16-byte scratch for pmovmskb etc.
    int exited; int exit_code;
    int redirect;            // execve/sigreturn set rip directly -> don't advance
    uint64_t ctid;           // CLONE_CHILD_CLEARTID
    uint64_t sigmask;
    uint64_t dbg_ibsrc;      // debug: guest PC of the last indirect branch (ret/jmp/call reg)
    uint64_t ic_miss;        // IBTC: set by an indirect-branch miss -> dispatcher fills g_ibtc for cpu->rip
    // x87 FPU: a register stack ST(0..7) emulated at DOUBLE precision (enough for printf %f of
    // doubles; loses the 80-bit long-double tail). st[fptop&7]=ST(0). Grows downward (push=--top).
    double   st[8];          // x87 stack slots
    uint64_t fptop;          // top-of-stack index (only low 3 bits used)
    uint64_t fpsw, fpcw;     // status word (C0-C3 in bits 8/9/10/14), control word
    uint64_t x87_ea;         // m80 (80-bit long double) operand address -> handled in C via R_X87*
    uint64_t divop;          // 64-bit div/idiv divisor -> 128/64 division done in C (ARM has no 128/64 divide)
};
#define OFF_IBSRC ((int)__builtin_offsetof(struct cpu, dbg_ibsrc))
#define OFF_ICMISS ((int)__builtin_offsetof(struct cpu, ic_miss))
#define OFF_ST    ((int)__builtin_offsetof(struct cpu, st))
#define OFF_FPTOP ((int)__builtin_offsetof(struct cpu, fptop))
#define OFF_FPSW  ((int)__builtin_offsetof(struct cpu, fpsw))
#define OFF_FPCW  ((int)__builtin_offsetof(struct cpu, fpcw))
#define OFF_X87EA ((int)__builtin_offsetof(struct cpu, x87_ea))
#define OFF_DIVOP ((int)__builtin_offsetof(struct cpu, divop))
#define R_OFF(i) ((i) * 8)
#define OFF_RIP   128
#define OFF_NZCV  136
#define OFF_FS    144
#define OFF_GS    152
#define OFF_RSN   160
#define OFF_HSP   168
#define OFF_HSAVE 176
#define OFF_HOSTV 272
#define OFF_V     400
#define OFF_MM    656
#define R_BRANCH 0
#define R_SYSCALL 1
#define R_CPUID 2
#define R_X87FLD 3      // fld m80  -> C converts 80-bit extended -> double, pushes
#define R_X87FSTP 4     // fstp m80 -> C converts ST0 double -> 80-bit, pops
#define R_DIV  5        // 64-bit div  -> C: rax,rdx = (rdx:rax) /,% divop  (unsigned 128/64)
#define R_IDIV 6        // 64-bit idiv -> C: signed 128/64
// x86 register encodings (== host reg numbers)
enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI };

// ---------------- JIT code cache (copied from jit.c) ----------------
#define CACHE_SZ (64u << 20)
static uint8_t *g_cache, *g_cp;
static uint8_t *g_emit_start;
static int g_trace, g_prof, g_noibtc, g_itrace;   // g_itrace: 1 instruction per block (per-insn register dump)
static uint64_t g_disp_n, g_ibtc_fill;   // PROF: dispatcher round-trips, IBTC fills
static uint64_t g_tracecap;              // if >0 under trace: stop after this many blocks (runaway guard)
int g_diag;   // diagnostics (FAULT_ON): print LOADED bases etc.
static int g_nochain;            // WATCH file: disable chaining (exact per-block rip attribution)
static uint64_t g_loadbase;      // main program load base (for file-offset mapping)
static uint8_t *g_w8; static uint8_t g_w8v;   // debug byte-watchpoint (armed via magic syscall 500)
static uint64_t g_malloc_n;                   // debug: count of __libc_malloc_impl entries
static const char *g_exe_path = "";
static const char *g_self_path = "";   // host path to this jit86 binary (for execve re-exec)
static pthread_key_t g_cpu_key;

#define MAP_N 65536
static struct { uint64_t gpc; void *host; void *body; } g_map[MAP_N];
static int map_idx(uint64_t gpc) {
    uint32_t h = (uint32_t)((gpc >> 0) * 2654435761u) & (MAP_N - 1);
    for (int i = 0; i < MAP_N; i++) { uint32_t j = (h + i) & (MAP_N - 1);
        if (g_map[j].host && g_map[j].gpc == gpc) return j;
        if (!g_map[j].host) return -1; }
    return -1;
}
static void *map_host(uint64_t gpc) { int i = map_idx(gpc); return i < 0 ? NULL : g_map[i].host; }
static void *map_body(uint64_t gpc) { int i = map_idx(gpc); return i < 0 ? NULL : g_map[i].body; }
static void map_put(uint64_t gpc, void *host, void *body) {
    uint32_t h = (uint32_t)((gpc >> 0) * 2654435761u) & (MAP_N - 1);
    for (int i = 0; i < MAP_N; i++) { uint32_t j = (h + i) & (MAP_N - 1);
        if (!g_map[j].host) { g_map[j].gpc = gpc; g_map[j].host = host; g_map[j].body = body; return; } }
}
// IBTC: shared direct-mapped {guest target -> host body} cache, probed inline by
// indirect branches (ret/jmp reg/call reg) so a function return needn't round-trip
// the dispatcher. Plain data (no W^X); zeroed at start and on code-cache flush.
// (mirrors jit.c; simpler here since x16/x17/x19-x21 are scratch, not guest regs.)
#define IBTC_N 8192
static struct { uint64_t target; void *body; } g_ibtc[IBTC_N];

static struct { uint32_t *slot; uint64_t target; } g_pend[1 << 16];
static int g_npend;
static void add_pend(uint32_t *slot, uint64_t target) {
    if (g_npend < (1 << 16)) { g_pend[g_npend].slot = slot; g_pend[g_npend].target = target; g_npend++; }
}
static void patch_links_to(uint64_t gpc, void *body) {
    for (int i = 0; i < g_npend;) {
        if (g_pend[i].target == gpc) {
            int64_t d = ((uint8_t *)body - (uint8_t *)g_pend[i].slot) / 4;
            *g_pend[i].slot = 0x14000000u | ((uint32_t)d & 0x3FFFFFFu);   // b body
            sys_icache_invalidate(g_pend[i].slot, 4);
            g_pend[i] = g_pend[--g_npend];
        } else i++;
    }
}

// ---------------- ARM64 instruction emitters ----------------
// (the same-ISA-independent half: these emit HOST code, copied from jit.c +
//  a few width-typed loads/stores the x86 front-end needs.)
static void emit32(uint32_t in) { *(uint32_t *)g_cp = in; g_cp += 4; }
static void e_str(int rt, int rn, int off) { emit32(0xF9000000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt); } // str x
static void e_ldr(int rt, int rn, int off) { emit32(0xF9400000u | (((unsigned)off / 8) << 10) | (rn << 5) | rt); } // ldr x
static void e_movz(int rd, uint32_t imm16, int sh) { emit32(0xD2800000u | (sh << 21) | (imm16 << 5) | rd); }
static void e_movk(int rd, uint32_t imm16, int sh) { emit32(0xF2800000u | (sh << 21) | (imm16 << 5) | rd); }
static void e_br(int rn) { emit32(0xD61F0000u | (rn << 5)); }
static void e_movconst(int rd, uint64_t v) {
    e_movz(rd, v & 0xffff, 0);
    if ((v >> 16) & 0xffff) e_movk(rd, (v >> 16) & 0xffff, 1);
    if ((v >> 32) & 0xffff) e_movk(rd, (v >> 32) & 0xffff, 2);
    if ((v >> 48) & 0xffff) e_movk(rd, (v >> 48) & 0xffff, 3);
}
// width-typed load/store at [rn, #0]. w = 1/2/4/8 bytes. (zero-extends on load)
static void e_load(int w, int rt, int rn) {
    uint32_t b = w == 1 ? 0x39400000u : w == 2 ? 0x79400000u : w == 4 ? 0xB9400000u : 0xF9400000u;
    emit32(b | (rn << 5) | rt);
}
static void e_store(int w, int rt, int rn) {
    uint32_t b = w == 1 ? 0x39000000u : w == 2 ? 0x79000000u : w == 4 ? 0xB9000000u : 0xF9000000u;
    emit32(b | (rn << 5) | rt);
}
static void e_ldrs(int w, int rt, int rn) {   // sign-extending load into X
    uint32_t b = w == 1 ? 0x39800000u : w == 2 ? 0x79800000u : 0xB9800000u;  // ldrsb/ldrsh/ldrsw
    emit32(b | (rn << 5) | rt);
}
static void e_mov_rr(int rd, int rm, int sf) {  // mov rd, rm  (orr rd, xzr, rm)
    emit32((sf ? 0xAA0003E0u : 0x2A0003E0u) | (rm << 16) | rd);
}
static void e_addi(int rd, int rn, unsigned imm12, int sf) {  // add rd, rn, #imm
    emit32((sf ? 0x91000000u : 0x11000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static void e_subi(int rd, int rn, unsigned imm12, int sf) {  // sub rd, rn, #imm
    emit32((sf ? 0xD1000000u : 0x51000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static void e_addi_s(int rd, int rn, unsigned imm12, int sf) { // adds rd, rn, #imm (sets flags)
    emit32((sf ? 0xB1000000u : 0x31000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
static void e_subi_s(int rd, int rn, unsigned imm12, int sf) { // subs rd, rn, #imm (sets flags)
    emit32((sf ? 0xF1000000u : 0x71000000u) | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd);
}
// shifted-register 3-operand (LSL #amt) for add/sub/and/orr/eor and their S-forms.
static void e_rrr(uint32_t base, int rd, int rn, int rm, int sf, int lsl) {
    emit32(base | (sf ? 0x80000000u : 0) | (rm << 16) | ((lsl & 0x3F) << 10) | (rn << 5) | rd);
}
#define A_ADD  0x0B000000u
#define A_ADDS 0x2B000000u
#define A_SUB  0x4B000000u
#define A_SUBS 0x6B000000u
#define A_AND  0x0A000000u
#define A_ANDS 0x6A000000u
#define A_ORR  0x2A000000u
#define A_EOR  0x4A000000u
#define A_ORN  0x2A200000u   // orn (for mvn)
#define A_BIC  0x0A200000u   // bic (and-not)
// nzcv scratch is x20 (a free callee-saved host reg, saved/restored by the trampoline),
// NOT x16/x17 -- so x16(value)/x17(EA) stay usable across flag-setting mem-dest ops.
static void e_nzcv_save(void) { emit32(0xD53B4200u | 20); e_str(20, 28, OFF_NZCV); }   // mrs x20,nzcv; str
static void e_nzcv_load(void) { e_ldr(20, 28, OFF_NZCV); emit32(0xD51B4200u | 20); }   // ldr x20; msr nzcv,x20
// Carry convention: cpu->nzcv stores the ARM *borrow* C (= NOT x86 CF), which ARM SUBS/
// SBCS produce naturally and the jcc table assumes. ARM ADDS/ADCS produce C = x86 CF
// (the opposite), so flags coming from an x86 add/adc must have C flipped to match.
static void e_nzcv_save_ci(void) {                          // save flags, inverting C (scratch x22: x21 may hold a result)
    emit32(0xD53B4200u | 20);                               // mrs x20, nzcv
    e_movconst(22, 1u << 29);                               // C is bit 29 of nzcv
    e_rrr(A_EOR, 20, 20, 22, 1, 0);                         // eor x20, x20, #(1<<29)
    e_str(20, 28, OFF_NZCV); emit32(0xD51B4200u | 20);     // also sync live ARM nzcv (msr) so spill persists the corrected value
}
static void e_nzcv_load_ci(void) {                          // load flags into live nzcv, inverting C
    e_ldr(20, 28, OFF_NZCV);
    e_movconst(22, 1u << 29); e_rrr(A_EOR, 20, 20, 22, 1, 0);
    emit32(0xD51B4200u | 20);                               // msr nzcv, x20
}
static void e_nzcv_save_c1(void) {                          // logical ops: x86 CF=0,OF=0; ARM ANDS/TST leave C,V stale
    emit32(0xD53B4200u | 20);                               // mrs x20, nzcv
    e_movconst(22, 1u << 28); e_rrr(A_BIC, 20, 20, 22, 1, 0);   // clear V (bit 28) -> OF=0  (jg/jle test SF==OF)
    e_movconst(22, 1u << 29); e_rrr(A_ORR, 20, 20, 22, 1, 0);   // set C (bit 29) -> stored borrow for CF=0
    e_str(20, 28, OFF_NZCV); emit32(0xD51B4200u | 20);     // sync live ARM nzcv
}
static void e_nzcv_save_setcf(int cfreg) {                  // save N/Z (from ARM nzcv), set stored C = NOT x86CF (cfreg holds 0/1)
    emit32(0xD53B4200u | 20);                               // mrs x20, nzcv  (N,Z valid)
    e_movconst(22, 1u << 29); e_rrr(A_BIC, 20, 20, 22, 1, 0);   // clear C
    e_movconst(23, 1); e_rrr(A_EOR, 23, cfreg, 23, 0, 0);      // x23 = NOT cf (0/1)
    e_rrr(A_ORR, 20, 20, 23, 1, 29);                          // stored C = (NOT cf) << 29
    e_str(20, 28, OFF_NZCV); emit32(0xD51B4200u | 20);     // sync live ARM nzcv
}
static void e_nzcv_save_keepC(void) {                       // inc/dec: take new N/Z/V, KEEP stored C (x86 inc/dec don't touch CF)
    emit32(0xD53B4200u | 20);                               // mrs x20, nzcv (new N,Z,V; C junk) -- scratch x24/x25 (x21 may hold a result)
    e_ldr(24, 28, OFF_NZCV);                                // x24 = old stored flags (has the C to keep)
    e_movconst(25, 1u << 29);
    e_rrr(A_BIC, 20, 20, 25, 1, 0);                         // clear C in new
    e_rrr(A_AND, 24, 24, 25, 1, 0);                         // isolate old C
    e_rrr(A_ORR, 20, 20, 24, 1, 0);                         // new N/Z/V | old C
    e_str(20, 28, OFF_NZCV); emit32(0xD51B4200u | 20);     // sync live ARM nzcv
}
static void e_bcond(int cond, int32_t off19) { emit32(0x54000000u | (((uint32_t)off19 & 0x7FFFF) << 5) | (cond & 0xF)); }
static void e_cset(int rd, int cond, int sf) {                 // cset rd, cond
    emit32((sf ? 0x9A9F07E0u : 0x1A9F07E0u) | (((cond ^ 1) & 0xF) << 12) | rd);
}
static void e_csel(int rd, int rn_t, int rm_f, int cond, int sf) {
    emit32((sf ? 0x9A800000u : 0x1A800000u) | (rm_f << 16) | ((cond & 0xF) << 12) | (rn_t << 5) | rd);
}
static void e_uxt(int rd, int rn, int w) {       // uxtb/uxth (zero-extend reg)
    emit32((w == 1 ? 0x12001C00u : 0x12003C00u) | (rn << 5) | rd);
}
static void e_sxt(int rd, int rn, int w) {       // sxtb/sxth/sxtw into X
    uint32_t b = w == 1 ? 0x93401C00u : w == 2 ? 0x93403C00u : 0x93407C00u;
    emit32(b | (rn << 5) | rd);
}
// shift by immediate (UBFM/SBFM). sh in [0,31] (32-bit) or [0,63] (64-bit).
static void e_lsl_i(int rd, int rn, int sh, int sf) {
    int w = sf ? 64 : 32, immr = (w - sh) & (w - 1), imms = w - 1 - sh;
    emit32((sf ? 0xD3400000u : 0x53000000u) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}
static void e_lsr_i(int rd, int rn, int sh, int sf) {
    int imms = sf ? 63 : 31;
    emit32((sf ? 0xD3400000u : 0x53000000u) | (sh << 16) | (imms << 10) | (rn << 5) | rd);
}
static void e_asr_i(int rd, int rn, int sh, int sf) {   // asr = SBFM rd,rn,#sh,#(w-1)
    int imms = sf ? 63 : 31;
    emit32((sf ? 0x93400000u : 0x13000000u) | (sh << 16) | (imms << 10) | (rn << 5) | rd);
}
static void e_bfi(int rd, int rn, int lsb, int width, int sf) {  // bfi rd,rn,#lsb,#width
    int w = sf ? 64 : 32, immr = (w - lsb) & (w - 1), imms = width - 1;
    emit32((sf ? 0xB3400000u : 0x33000000u) | (immr << 16) | (imms << 10) | (rn << 5) | rd);
}
// variable shift (LSLV/LSRV/ASRV/RORV), count in rm (low bits used)
static void e_shv(uint32_t base32, int rd, int rn, int rm, int sf) {
    emit32(base32 | (sf ? 0x80000000u : 0) | (rm << 16) | (rn << 5) | rd);
}
#define S_LSLV 0x1AC02000u
#define S_LSRV 0x1AC02400u
#define S_ASRV 0x1AC02800u
#define S_RORV 0x1AC02C00u
static void e_mul(int rd, int rn, int rm, int sf) { emit32((sf ? 0x9B007C00u : 0x1B007C00u) | (rm << 16) | (rn << 5) | rd); }
static void e_umulh(int rd, int rn, int rm) { emit32(0x9BC07C00u | (rm << 16) | (rn << 5) | rd); }
static void e_smulh(int rd, int rn, int rm) { emit32(0x9B407C00u | (rm << 16) | (rn << 5) | rd); }
static void e_udiv(int rd, int rn, int rm, int sf) { emit32((sf ? 0x9AC00800u : 0x1AC00800u) | (rm << 16) | (rn << 5) | rd); }
static void e_sdiv(int rd, int rn, int rm, int sf) { emit32((sf ? 0x9AC00C00u : 0x1AC00C00u) | (rm << 16) | (rn << 5) | rd); }
static void e_msub(int rd, int rn, int rm, int ra, int sf) { emit32((sf ? 0x9B008000u : 0x1B008000u) | (rm << 16) | (ra << 10) | (rn << 5) | rd); }
static void e_ror_i(int rd, int rn, int sh, int sf) {   // ror rd,rn,#sh  (EXTR rd,rn,rn,#sh)
    emit32((sf ? 0x93C00000u : 0x13800000u) | (rn << 16) | ((sh & (sf ? 63 : 31)) << 10) | (rn << 5) | rd);
}
static void e_extr(int rd, int rn, int rm, int lsb, int sf) {   // EXTR rd, rn, rm, #lsb  = (rn:rm) >> lsb
    emit32((sf ? 0x93C00000u : 0x13800000u) | (rm << 16) | ((lsb & (sf ? 63 : 31)) << 10) | (rn << 5) | rd);
}
static void e_tst(int rn, int sf) { emit32((sf ? 0xEA00001Fu : 0x6A00001Fu) | (rn << 16) | (rn << 5)); } // ands xzr,rn,rn
static void e_rbit(int rd, int rn, int sf) { emit32((sf ? 0xDAC00000u : 0x5AC00000u) | (rn << 5) | rd); }
static void e_clz(int rd, int rn, int sf) { emit32((sf ? 0xDAC01000u : 0x5AC01000u) | (rn << 5) | rd); }

// ---- NEON / SSE encoders (guest xmm0..15 live in host v0..v15) ----
static void e_str_q(int t, int rn, int off) { emit32(0x3D800000u | (((unsigned)off / 16) << 10) | (rn << 5) | t); }
static void e_ldr_q(int t, int rn, int off) { emit32(0x3DC00000u | (((unsigned)off / 16) << 10) | (rn << 5) | t); }
static void e_stp_q(int t1, int t2, int rn, int off) { emit32(0xAD000000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1); }
static void e_ldp_q(int t1, int t2, int rn, int off) { emit32(0xAD400000u | (((unsigned)(off / 16) & 0x7F) << 15) | (t2 << 10) | (rn << 5) | t1); }
static void e_ldr_d(int t, int rn) { emit32(0xFD400000u | (rn << 5) | t); }   // ldr d,[xn]
static void e_str_d(int t, int rn) { emit32(0xFD000000u | (rn << 5) | t); }   // str d,[xn]
static void e_ldr_s(int t, int rn) { emit32(0xBD400000u | (rn << 5) | t); }   // ldr s,[xn]
static void e_str_s(int t, int rn) { emit32(0xBD000000u | (rn << 5) | t); }   // str s,[xn]
static void e_fmov_to_d(int vd, int xn) { emit32(0x9E670000u | (xn << 5) | vd); }   // fmov d[vd], x[xn] (zeroes hi)
static void e_fmov_to_s(int vd, int wn) { emit32(0x1E270000u | (wn << 5) | vd); }   // fmov s[vd], w[wn]
static void e_fmov_from_d(int xd, int vn) { emit32(0x9E660000u | (vn << 5) | xd); } // fmov x[xd], d[vn]
static void e_fmov_from_s(int wd, int vn) { emit32(0x1E260000u | (vn << 5) | wd); } // fmov w[wd], s[vn]
static void e_vmov(int vd, int vn) { emit32(0x4EA01C00u | (vn << 16) | (vn << 5) | vd); }  // mov vd.16b, vn.16b (orr)
static void e_vmov8(int vd, int vn) { emit32(0x0EA01C00u | (vn << 16) | (vn << 5) | vd); } // mov vd.8b, vn.8b (low 64, zero upper)
static void e_ins_d(int vd, int ld, int vn, int ls) {   // ins vd.d[ld], vn.d[ls]
    emit32(0x6E000400u | ((unsigned)((ld << 4) | 8) << 16) | ((unsigned)(ls << 3) << 11) | (vn << 5) | vd);
}
static void e_ins_s(int vd, int ls_lane, int vn, int sl) {   // ins vd.s[ls_lane], vn.s[sl]
    emit32(0x6E000400u | ((unsigned)((ls_lane << 3) | 4) << 16) | ((unsigned)(sl << 2) << 11) | (vn << 5) | vd);
}
// ---- x87 FPU stack helpers (ST(i) emulated at double precision in cpu->st[]) ----
// ST(0) = cpu->st[fptop & 7]; the stack grows downward (push: --top). Scratch: x16/x17, v16+.
static void e_st_addr(int xa, int i) {   // xa = &cpu->st[(fptop+i)&7]   (clobbers x16)
    e_ldr(16, 28, OFF_FPTOP);
    if (i) emit32(0x11000000u | ((unsigned)(i & 7) << 10) | (16 << 5) | 16);   // add w16,w16,#i
    emit32(0x12000800u | (16 << 5) | 16);                                      // and w16,w16,#7
    emit32(0x91000000u | ((unsigned)OFF_ST << 10) | (28 << 5) | xa);           // add xa,x28,#OFF_ST
    emit32(0x8B000000u | (16 << 16) | (3 << 10) | (xa << 5) | xa);             // add xa,xa,x16,lsl#3
}
static void e_fp_settop(int delta) {     // fptop = (fptop+delta) & 7   (clobbers x16)
    e_ldr(16, 28, OFF_FPTOP);
    if (delta < 0) emit32(0x51000000u | ((unsigned)(-delta) << 10) | (16 << 5) | 16);  // sub w16,w16,#n
    else if (delta) emit32(0x11000000u | ((unsigned)delta << 10) | (16 << 5) | 16);    // add w16,w16,#n
    emit32(0x12000800u | (16 << 5) | 16);                                              // and w16,w16,#7
    e_str(16, 28, OFF_FPTOP);
}
static void e_fp_ld(int vd, int i) { e_st_addr(17, i); e_ldr_d(vd, 17); }   // vd = ST(i)
static void e_fp_st(int vs, int i) { e_st_addr(17, i); e_str_d(vs, 17); }   // ST(i) = vs
static void e_fp_push(int vs) { e_fp_settop(-1); e_st_addr(17, 0); e_str_d(vs, 17); }   // push vs -> ST(0)
// fcom-family compare: FCMP dn,dm then set cpu->fpsw bits C0(8)/C2(10)/C3(14) so a
// following fnstsw ax + sahf reproduces x86 ZF/PF/CF. (clobbers x16/x17/x20)
static void e_fcom_setfpsw(int n, int m) {
    emit32(0x1E602000u | (m << 16) | (n << 5));   // fcmp dn, dm
    e_cset(16, 3, 0);                              // less       (LO: C clear)
    e_cset(20, 6, 0);                              // unordered  (VS)
    e_rrr(A_ORR, 16, 16, 20, 0, 0);                // C0 = less | unordered
    e_cset(17, 0, 0);                              // equal      (EQ)
    e_rrr(A_ORR, 17, 17, 20, 0, 0);                // C3 = equal | unordered
    e_lsl_i(16, 16, 8, 0);
    e_lsl_i(20, 20, 10, 0);  e_rrr(A_ORR, 16, 16, 20, 0, 0);   // | C2<<10
    e_lsl_i(17, 17, 14, 0);  e_rrr(A_ORR, 16, 16, 17, 0, 0);   // | C3<<14
    e_str(16, 28, OFF_FPSW);
}
// SSE shift-by-immediate -> NEON USHR/SSHR/SHL (esize in bits: 16/32/64)
static void e_vshr_imm(int vd, int vn, int esize, int sh, int sgn) {
    if (sh <= 0) { e_vmov(vd, vn); return; } if (sh > esize) sh = esize;
    unsigned immhb = 2 * esize - sh;
    emit32((sgn ? 0x4F000400u : 0x6F000400u) | (immhb << 16) | (vn << 5) | vd);
}
static void e_vshl_imm(int vd, int vn, int esize, int sh) {
    if (sh <= 0) { e_vmov(vd, vn); return; }
    if (sh >= esize) { emit32(0x6E201C00u | (vd << 16) | (vd << 5) | vd); return; }  // >=width -> 0 (eor vd,vd,vd)
    unsigned immhb = esize + sh;
    emit32(0x4F005400u | (immhb << 16) | (vn << 5) | vd);
}
static void e_ext(int vd, int vn, int vm, int idx) { emit32(0x6E000000u | (vm << 16) | ((idx & 0xF) << 11) | (vn << 5) | vd); }
static void e_v3(uint32_t base, int vd, int vn, int vm) { emit32(base | (vm << 16) | (vn << 5) | vd); } // NEON 3-same .16b/.Ns
// LSE atomics (AL ordering). sz: 1/2/4/8 bytes.
static void e_lse(uint32_t base, int sz, int rs, int rt, int rn) {
    uint32_t szb = sz == 8 ? 0xC0000000u : sz == 4 ? 0x80000000u : sz == 2 ? 0x40000000u : 0;
    emit32((base & 0x3FFFFFFFu) | szb | (rs << 16) | (rn << 5) | rt);
}
static void e_cas(int sz, int rs, int rt, int rn) {   // casal Rs(old/cmp), Rt(new), [Rn]
    uint32_t b = sz == 8 ? 0xC8E0FC00u : sz == 4 ? 0x88E0FC00u : sz == 2 ? 0x48E0FC00u : 0x08E0FC00u;
    emit32(b | (rs << 16) | (rn << 5) | rt);
}
#define LSE_LDADD 0xB8E00000u   // ldaddal

static int64_t sext(uint64_t v, int bits) { uint64_t m = 1ull << (bits - 1); return (int64_t)((v ^ m) - m); }
static void block_return(void);

// ---------------- prologue / spill / exits ----------------
// Prologue: entered x0 = &cpu. Pin cpu in x28, restore flags + 16 guest GPRs (x0 last).
static void emit_prologue(void) {
    emit32(0xAA0003FCu);                        // mov x28, x0   (cpu)
    e_nzcv_load();                              // restore flags
    for (int t = 0; t < 16; t += 2) e_ldp_q(t, t + 1, 28, OFF_V + t * 16);  // guest xmm0..15 -> v0..v15
    for (int r = 1; r <= 15; r++) e_ldr(r, 28, R_OFF(r));
    e_ldr(0, 28, 0);                            // rax last
}
// Spill: x28 is live (== cpu), so store the 16 GPRs + flags + xmm. The flag save reads the
// LIVE ARM nzcv -- which is kept == cpu->nzcv by every flag producer (the borrow-convention
// helpers below `msr` their corrected value back into ARM nzcv so this stays consistent).
static void emit_spill(void) {
    for (int r = 0; r <= 15; r++) e_str(r, 28, R_OFF(r));
    for (int t = 0; t < 16; t += 2) e_stp_q(t, t + 1, 28, OFF_V + t * 16);  // v0..v15 -> guest xmm
    e_nzcv_save();
}
static void emit_exit_const(uint64_t rip, uint64_t reason) {
    emit_spill();
    e_movconst(16, rip);    e_str(16, 28, OFF_RIP);
    e_movconst(16, reason); e_str(16, 28, OFF_RSN);
    e_movconst(16, (uint64_t)block_return); e_br(16);   // block_return uses x28 (still cpu)
}
// Direct-branch chaining: if target already translated, single `b body`; else a full
// exit whose first insn is remembered and back-patched to `b body` later. (from jit.c)
static void emit_chain_exit(uint64_t target) {
    if (g_trace || g_nochain) { emit_exit_const(target, R_BRANCH); return; }   // debug: no chaining -> exact rip per block
    void *body = map_body(target);
    uint32_t *slot = (uint32_t *)g_cp;
    if (body) { int64_t d = ((uint8_t *)body - (uint8_t *)slot) / 4;
                emit32(0x14000000u | ((uint32_t)d & 0x3FFFFFFu)); return; }
    add_pend(slot, target);
    emit_exit_const(target, R_BRANCH);
}
// Indirect branch (ret / jmp reg / call reg) with the guest target already in x16.
// Probe the IBTC inline: HIT -> jump straight into the cached body (guest regs stay
// live, no spill/dispatch); MISS -> spill and flag the dispatcher to fill the cache.
// Scratch x16/x17/x19/x20/x21 are not guest registers here, and `sub` (not `subs`)
// keeps nzcv live, so the cached body is entered exactly like a chained block.
static void emit_ibranch(void) {
    if (g_trace || g_nochain || g_noibtc) {                       // debug: always dispatch (exact rip)
        e_str(16, 28, OFF_RIP); emit_spill();
        e_movconst(16, R_BRANCH); e_str(16, 28, OFF_RSN);
        e_movconst(16, (uint64_t)block_return); e_br(16); return;
    }
    emit32(0xD3423800u | (16 << 5) | 17);                         // ubfx x17, x16, #2, #13  ((tgt>>2)&0x1FFF)
    e_movconst(19, (uint64_t)g_ibtc);                             // x19 = &g_ibtc
    emit32(0x8B000000u | (17 << 16) | (4 << 10) | (19 << 5) | 19);// add x19, x19, x17, lsl #4   (slot)
    e_ldr(20, 19, 0);                                             // x20 = slot.target
    emit32(0xCB000000u | (16 << 16) | (20 << 5) | 20);            // sub x20, x20, x16  (NOT subs: keep nzcv)
    uint32_t *p_cbnz = (uint32_t *)g_cp; emit32(0);               // cbnz x20, Lmiss
    e_ldr(21, 19, 8); e_br(21);                                   // HIT: x21 = slot.body -> jump (regs live)
    uint32_t *miss = (uint32_t *)g_cp;
    e_str(16, 28, OFF_RIP); emit_spill();                         // MISS: slow path
    e_movconst(16, R_BRANCH); e_str(16, 28, OFF_RSN);
    e_movconst(16, 1); e_str(16, 28, OFF_ICMISS);                 // dispatcher fills g_ibtc for cpu->rip
    e_movconst(16, (uint64_t)block_return); e_br(16);
    *p_cbnz = 0xB5000000u | (((uint32_t)(((uint8_t *)miss - (uint8_t *)p_cbnz) / 4) & 0x7FFFF) << 5) | 20;
}

// ---------------- x86-64 decoder ----------------
struct insn {
    int len;
    int rexW, rexR, rexX, rexB, has_rex;
    int opsize;          // operand size in bytes: 1/2/4/8
    int p66;             // 0x66 prefix seen (mandatory-prefix for SSE; distinct from opsize)
    int addr32;          // 0x67
    int seg;             // 0 none, 1 fs, 2 gs
    int lock, rep, repne;
    int two;             // 0F escape seen
    uint8_t op;          // opcode byte after escape
    int has_modrm; uint8_t modrm; int mod, reg, rm;
    int is_mem;          // operand-in-memory (mod != 3)
    int m_base, m_index, m_scale; int64_t disp; int rip_rel; int m_hasbase, m_hasindex;
    int rm_reg;          // when mod==3: the r/m register number
    int64_t imm; int imm_bytes;
};

static int op_has_modrm(int two, uint8_t op) {
    if (two) {
        if (op == 0x05) return 0;                       // syscall
        if (op == 0xA2 || op == 0x31 || op == 0x77) return 0;  // cpuid / rdtsc / emms (no modrm)
        if (op >= 0xC8 && op <= 0xCF) return 0;                // bswap reg (encoded in opcode)
        if (op == 0x1E) return 1;                       // endbr (modrm follows)
        if ((op & 0xF0) == 0x80) return 0;              // jcc rel32
        if ((op & 0xF0) == 0x90) return 1;              // setcc
        if ((op & 0xF0) == 0x40) return 1;              // cmovcc
        if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF || op == 0xAF) return 1; // movzx/sx/imul
        if (op == 0x1F) return 1;                        // nop r/m
        if (op == 0x10 || op == 0x11 || op == 0x28 || op == 0x29 || op == 0x6E || op == 0x7E
            || op == 0x6F || op == 0x7F || op == 0xD6 || op == 0xEF || op == 0x57 || op == 0x54) return 1; // SSE
        return 1;
    }
    if (op >= 0x50 && op <= 0x5F) return 0;             // push/pop r
    if (op >= 0x70 && op <= 0x7F) return 0;             // jcc rel8
    if (op == 0xE8 || op == 0xE9 || op == 0xEB || op == 0xE3) return 0; // call/jmp rel, jrcxz
    if (op == 0xC3 || op == 0xC2 || op == 0xC9 || op == 0x90 || op == 0xF4 || op == 0x99 || op == 0x98) return 0;
    if (op >= 0x91 && op <= 0x97) return 0;             // xchg eax, rN
    if (op == 0x9B || op == 0x9C || op == 0x9D || op == 0x9E || op == 0x9F) return 0;  // fwait/pushf/popf/sahf/lahf
    if (op == 0x9C || op == 0x9D || op == 0xFC || op == 0xFD || op == 0xCC || op == 0xF5) return 0; // pushf/popf/cld/std/int3/cmc
    if (op >= 0xA4 && op <= 0xAF) return 0;             // movs/cmps/stos/lods/scas + test al,imm(A8/A9)
    if (op >= 0xB0 && op <= 0xBF) return 0;             // mov r8/r, imm
    if (op < 0x40 && ((op & 7) == 4 || (op & 7) == 5)) return 0;  // ALU al/eAX, imm (04/05,0C/0D,...,3C/3D)
    if (op == 0xA8 || op == 0xA9) return 0;             // test al/eax, imm
    if (op == 0x68 || op == 0x6A) return 0;             // push imm
    if (op == 0xCC || op == 0xF1) return 0;
    // ALU group, mov, lea, test, group1/2/3, etc. all have modrm
    return 1;
}
// immediate size (bytes) for the opcodes we handle; 0 if none.
static int op_imm_bytes(struct insn *I) {
    int two = I->two; uint8_t op = I->op; int os = I->opsize;
    if (two) {
        if ((op & 0xF0) == 0x80) return 4;              // jcc rel32
        if (op == 0xBA) return 1;                        // bt/bts/btr/btc r/m, imm8
        if (op == 0xA4 || op == 0xAC) return 1;          // shld/shrd r/m, r, imm8
        if (op == 0x70 || op == 0x71 || op == 0x72 || op == 0x73 || op == 0xC2 || op == 0xC4 || op == 0xC5 || op == 0xC6) return 1; // SSE imm
        return 0;
    }
    if (op == 0xC2) return 2;                            // ret imm16
    if (op >= 0x70 && op <= 0x7F) return 1;             // jcc rel8
    if (op == 0xEB || op == 0xE3) return 1;             // jmp rel8 / jrcxz rel8
    if (op == 0xE9 || op == 0xE8) return 4;             // jmp/call rel32
    if (op >= 0xB0 && op <= 0xB7) return 1;             // mov r8, imm8
    if (op >= 0xB8 && op <= 0xBF) return os == 8 ? 8 : (os == 2 ? 2 : 4); // mov r,imm (movabs if W)
    if (op < 0x40 && (op & 7) == 4) return 1;                             // ALU al, imm8
    if (op < 0x40 && (op & 7) == 5) return os == 2 ? 2 : 4;               // ALU eAX, imm16/32
    if (op == 0xA8) return 1; if (op == 0xA9) return os == 2 ? 2 : 4;     // test
    if (op == 0x6A) return 1; if (op == 0x68) return os == 2 ? 2 : 4;     // push imm
    if (op == 0x80) return 1;                            // group1 r/m8, ib
    if (op == 0x81) return os == 2 ? 2 : 4;             // group1 r/m, iz
    if (op == 0x83) return 1;                            // group1 r/m, ib (sign-ext)
    if (op == 0xC6) return 1;                            // mov r/m8, ib
    if (op == 0xC7) return os == 2 ? 2 : 4;             // mov r/m, iz
    if (op == 0xC0 || op == 0xC1) return 1;             // shift r/m, ib
    if (op == 0xF6) return (I->reg <= 1) ? 1 : 0;       // test r/m8,ib only for /0,/1
    if (op == 0xF7) return (I->reg <= 1) ? (os == 2 ? 2 : 4) : 0;
    if (op == 0x69) return os == 2 ? 2 : 4;             // imul r,r/m,iz
    if (op == 0x6B) return 1;                            // imul r,r/m,ib
    return 0;
}

// returns instruction length, fills I. On a decode it can't handle for length, returns
// the bytes consumed so far so the reporter can show them.
static int decode(uint64_t pc, struct insn *I) {
    memset(I, 0, sizeof *I);
    const uint8_t *p = (const uint8_t *)pc; int n = 0;
    I->opsize = 4; I->m_scale = 0;
    // legacy prefixes
    for (;;) {
        uint8_t b = p[n];
        if (b == 0x66) { I->opsize = 2; I->p66 = 1; n++; continue; }
        if (b == 0x67) { I->addr32 = 1; n++; continue; }
        if (b == 0xF0) { I->lock = 1; n++; continue; }
        if (b == 0xF2) { I->repne = 1; n++; continue; }
        if (b == 0xF3) { I->rep = 1; n++; continue; }
        if (b == 0x64) { I->seg = 1; n++; continue; }   // fs
        if (b == 0x65) { I->seg = 2; n++; continue; }   // gs
        if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) { n++; continue; }
        break;
    }
    // REX
    if ((p[n] & 0xF0) == 0x40) {
        uint8_t rex = p[n++]; I->has_rex = 1;
        I->rexW = (rex >> 3) & 1; I->rexR = (rex >> 2) & 1; I->rexX = (rex >> 1) & 1; I->rexB = rex & 1;
        if (I->rexW) I->opsize = 8;
    }
    // opcode
    uint8_t op = p[n++];
    if (op == 0x0F) { I->two = 1; op = p[n++]; }
    I->op = op;
    // modrm + sib + disp
    if (op_has_modrm(I->two, op)) {
        uint8_t m = p[n++]; I->has_modrm = 1; I->modrm = m;
        I->mod = m >> 6; I->reg = ((m >> 3) & 7) | (I->rexR << 3); I->rm = m & 7;
        if (I->mod == 3) { I->rm_reg = I->rm | (I->rexB << 3); }
        else {
            I->is_mem = 1;
            int base = I->rm, idx = -1, scale = 0;
            if (I->rm == 4) {                            // SIB
                uint8_t s = p[n++]; scale = s >> 6; idx = ((s >> 3) & 7) | (I->rexX << 3); base = (s & 7);
                if (((s >> 3) & 7) == 4 && !I->rexX) idx = -1;   // no index
                if ((s & 7) == 5 && I->mod == 0) { I->m_hasbase = 0; }
                else { I->m_hasbase = 1; I->m_base = base | (I->rexB << 3); }
            } else if (I->rm == 5 && I->mod == 0) {      // RIP-relative
                I->rip_rel = 1;
            } else { I->m_hasbase = 1; I->m_base = I->rm | (I->rexB << 3); }
            if (idx >= 0) { I->m_hasindex = 1; I->m_index = idx; I->m_scale = scale; }
            // displacement
            if (I->rip_rel) { I->disp = (int32_t)(p[n] | (p[n+1]<<8) | (p[n+2]<<16) | ((uint32_t)p[n+3]<<24)); n += 4; }
            else if (I->mod == 1) { I->disp = (int8_t)p[n]; n += 1; }
            else if (I->mod == 2 || (!I->m_hasbase && I->rm == 4)) {
                I->disp = (int32_t)(p[n] | (p[n+1]<<8) | (p[n+2]<<16) | ((uint32_t)p[n+3]<<24)); n += 4;
            }
        }
    }
    // immediate
    int ib = op_imm_bytes(I);
    I->imm_bytes = ib;
    if (ib) {
        uint64_t v = 0; for (int i = 0; i < ib; i++) v |= (uint64_t)p[n+i] << (8*i);
        I->imm = (ib == 1) ? (int8_t)v : (ib == 2) ? (int16_t)v : (ib == 4) ? (int32_t)v : (int64_t)v;
        n += ib;
    }
    I->len = n;
    return n;
}

// Compute the effective address of a memory operand into host scratch x17.
// (base + index*scale + disp, + fs/gs base; RIP-relative -> a constant.)
static void emit_ea(struct insn *I, uint64_t next_rip) {
    if (I->rip_rel) { e_movconst(17, next_rip + (uint64_t)I->disp); }
    else {
        if (I->m_hasbase) e_mov_rr(17, I->m_base, 1);
        else e_movz(17, 0, 0);
        if (I->m_hasindex) e_rrr(A_ADD, 17, 17, I->m_index, 1, I->m_scale);  // add x17,x17,idx,lsl#scale
        if (I->disp) { e_movconst(16, (uint64_t)I->disp); e_rrr(A_ADD, 17, 17, 16, 1, 0); }
    }
    if (I->seg) { e_ldr(16, 28, I->seg == 1 ? OFF_FS : OFF_GS); e_rrr(A_ADD, 17, 17, 16, 1, 0); }
}

// ---------------- the translator ----------------
static void report_unimpl(uint64_t pc, struct insn *I);

// ALU operation selector from the primary opcode group (00..3D) or group1 /digit.
// returns: 0 ADD 1 OR 2 ADC 3 SBB 4 AND 5 SUB 6 XOR 7 CMP, or -1.
static int alu_kind_primary(uint8_t op) { int k = (op >> 3) & 7; return ((op & 7) <= 5) ? k : -1; }

// 32/64-bit core ALU into `out`, rn<op>rm, setting ARM flags. out=31 -> discard (cmp/test).
static void alu_core(int kind, int out, int rn, int rm, int sf) {
    switch (kind) {
        case 0: e_rrr(A_ADDS, out, rn, rm, sf, 0); break;                 // add
        case 4: e_rrr(A_ANDS, out, rn, rm, sf, 0); break;                 // and / test
        case 5: e_rrr(A_SUBS, out, rn, rm, sf, 0); break;                 // sub / cmp
        case 1: e_rrr(A_ORR, out, rn, rm, sf, 0);                          // or
                emit32((sf ? 0xEA00001Fu : 0x6A00001Fu) | (out << 16) | (out << 5)); break; // tst
        case 6: e_rrr(A_EOR, out, rn, rm, sf, 0);                          // xor
                emit32((sf ? 0xEA00001Fu : 0x6A00001Fu) | (out << 16) | (out << 5)); break;
        default: break;
    }
}
// Byte-register operands: without REX, encodings 4..7 are the HIGH bytes ah/ch/dh/bh
// (bits[15:8] of the first 4 regs); with any REX they're the low bytes spl/bpl/sil/dil.
static int is_hi8(struct insn *I, int regnum) { return !I->has_rex && regnum >= 4 && regnum < 8; }
// value of an 8-bit register operand, in the LOW 8 bits of the returned reg (rest is
// don't-care -- do_alu's <<24 trick keeps only the low byte). hi8 -> extract via >>8.
static int byte_val(struct insn *I, int regnum, int scratch) {
    if (is_hi8(I, regnum)) { e_lsr_i(scratch, regnum - 4, 8, 1); return scratch; }
    return regnum;
}
// write the low byte of `val` into an 8-bit register operand (preserving other bits).
static void byte_wb(struct insn *I, int regnum, int val) {
    if (is_hi8(I, regnum)) e_bfi(regnum - 4, val, 8, 8, 1);
    else e_bfi(regnum, val, 0, 8, 1);
}
// r/m operand: mem -> EA to x17, load value to x16 (returns 16); reg -> value reg.
static void emit_ea(struct insn *I, uint64_t next_rip);
static int rm_load(struct insn *I, uint64_t next, int w, int *mem) {
    if (I->is_mem) { emit_ea(I, next); e_load(w, 16, 17); *mem = 1; return 16; }
    *mem = 0;
    if (w == 1) return byte_val(I, I->rm_reg, 23);   // handle ah/ch/dh/bh
    return I->rm_reg;
}
static void rm_store(struct insn *I, int w, int val) {   // val -> r/m (EA already in x17 if mem)
    if (I->is_mem) { e_store(w, val, 17); return; }
    if (w == 1) { byte_wb(I, I->rm_reg, val); return; }
    if (val != I->rm_reg) { if (w >= 4) e_mov_rr(I->rm_reg, val, w == 8); else e_bfi(I->rm_reg, val, 0, 8 * w, 1); }
}
// Width-correct ALU: dst = a <kind> b, set cpu->nzcv.  dst<0 => cmp/test (no write).
// 4/8-byte: direct ARM op. 1/2-byte: operate in the HIGH bits (<<sh) so ARM NZCV matches
// x86 byte/word flags exactly, then merge the low w bytes back (preserving upper bits).
static void do_alu(int kind, int dst, int a, int b, int w) {
    int sf = w == 8, out = dst < 0 ? 31 : dst;
    int ak = kind == 7 ? 5 : kind;                 // cmp == sub(discard); test == and(discard)
    if (kind == 7) ak = 5;
    if (kind == 2) {                               // ADC: carry-in = x86 CF = NOT stored; result CF stored inverted
        e_nzcv_load_ci();                          // live ARM C = x86 CF
        e_rrr(0x3A000000u, out, a, b, sf, 0);      // adcs
        e_nzcv_save_ci();
        return;
    }
    if (kind == 3) {                               // SBB: borrow convention matches stored C directly
        e_nzcv_load();
        e_rrr(0x7A000000u, out, a, b, sf, 0);      // sbcs
        e_nzcv_save();
        return;
    }
    int logical = (kind == 1 || kind == 4 || kind == 6);   // or/and/xor (and test): x86 clears CF
    if (w >= 4) {
        alu_core(ak, out, a, b, sf);
        if (kind == 0) e_nzcv_save_ci();                   // x86 add -> invert ARM add-carry
        else if (logical) e_nzcv_save_c1();                // x86 CF=0 -> stored C=1
        else e_nzcv_save();
        return;
    }
    int sh = 8 * (4 - w);                           // 24 for byte, 16 for word
    e_lsl_i(21, a, sh, 0);                          // x21 = a << sh
    e_lsl_i(22, b, sh, 0);                          // x22 = b << sh
    alu_core(ak, dst < 0 ? 31 : 21, 21, 22, 0);     // op in high bits -> correct NZCV
    if (kind == 0) e_nzcv_save_ci();
    else if (logical) e_nzcv_save_c1();
    else e_nzcv_save();
    if (dst >= 0) { e_lsr_i(21, 21, sh, 0); e_bfi(dst, 21, 0, 8 * w, 1); }  // merge low w bytes
}

// x86 condition (opcode low nibble) -> ARM cond, or -1 if unsupported (parity).
static int x86cc_to_arm(int cc) {
    // x86 PF (parity, idx 10/11) -> ARM V flag: our FP compares (comis*/fcomi) leave V=1 on
    // unordered (NaN), which is exactly what `jp`/`jnp` after an FP compare test. (Integer
    // parity is not modeled, but is essentially unused outside the FP-compare idiom.)
    static const int t[16] = { 6,7,3,2,0,1,9,8,4,5,6,7,11,10,13,12 };
    return t[cc & 0xF];
}

// Translate the basic block at guest address gpc; returns host entry pointer.
static void *translate_block(uint64_t gpc) {
    uint64_t start = gpc;
    void *host = g_cp;
    emit_prologue();
    void *body = g_cp;
    for (;;) {
        if (g_itrace && gpc != start) { emit_chain_exit(gpc); break; }   // 1 insn/block: per-instruction register dump
        struct insn I;
        decode(gpc, &I);
        uint64_t next = gpc + I.len;
        uint8_t op = I.op;
        int sf = I.opsize == 8;
        if (g_trace) fprintf(stderr, "[dec] %llx %s%02x len=%d mod%d rm%d reg%d mem%d base%d idx%d disp=%lld imm=%lld\n",
            (unsigned long long)gpc, I.two ? "0F " : "", op, I.len, I.mod, I.rm_reg, I.reg, I.is_mem,
            I.m_hasbase ? I.m_base : -1, I.m_hasindex ? I.m_index : -1, (long long)I.disp, (long long)I.imm);

        if (!I.two) {
            // ---- mov r8, imm8 (B0+r) ----
            if (op >= 0xB0 && op <= 0xB7) {
                int rnum = (op - 0xB0) | (I.rexB << 3);
                e_movz(16, (uint32_t)(I.imm & 0xff), 0); byte_wb(&I, rnum, 16);
                gpc = next; continue;
            }
            // ---- mov r, imm (B8+r) ----
            if (op >= 0xB8 && op <= 0xBF) {
                int rd = (op - 0xB8) | (I.rexB << 3);
                e_movconst(rd, sf ? (uint64_t)I.imm : (uint64_t)(uint32_t)I.imm);
                gpc = next; continue;
            }
            // ---- mov r/m, imm (C7 /0, C6 /0) ----
            if (op == 0xC7 || op == 0xC6) {
                int w = op == 0xC6 ? 1 : I.opsize;
                if (I.is_mem) { emit_ea(&I, next); e_movconst(16, (uint64_t)I.imm); e_store(w, 16, 17); }
                else e_movconst(I.rm_reg, sf ? (uint64_t)I.imm : (uint64_t)(uint32_t)I.imm);
                gpc = next; continue;
            }
            // ---- mov r/m,r (88/89) and r,r/m (8A/8B) ----
            if (op == 0x88 || op == 0x89 || op == 0x8A || op == 0x8B) {
                int w = (op & 1) ? I.opsize : 1;
                int to_reg = (op & 2);                       // 8A/8B: dest is reg
                if (I.is_mem) { emit_ea(&I, next);
                    if (to_reg) {                            // mov reg, [mem]
                        if (w == 1) { e_load(1, 16, 17); byte_wb(&I, I.reg, 16); }  // byte dest: ah/bh/ch/dh -> bits 8-15; lo8 preserves upper
                        else e_load(w, I.reg, 17);
                    } else {                                 // mov [mem], reg
                        int sv = (w == 1) ? byte_val(&I, I.reg, 16) : I.reg;        // byte src: ah/bh/ch/dh -> bits 8-15
                        e_store(w, sv, 17);
                    }
                } else {
                    if (to_reg) e_mov_rr(I.reg, I.rm_reg, sf);
                    else        e_mov_rr(I.rm_reg, I.reg, sf);
                }
                gpc = next; continue;
            }
            // ---- lea (8D) ----
            if (op == 0x8D) { emit_ea(&I, next); e_mov_rr(I.reg, 17, sf); gpc = next; continue; }
            // ---- push/pop r (50-5F) ----
            if (op >= 0x50 && op <= 0x57) { int r = (op - 0x50) | (I.rexB << 3);
                e_subi(RSP, RSP, 8, 1); e_store(8, r, RSP); gpc = next; continue; }       // push (64-bit)
            if (op >= 0x58 && op <= 0x5F) { int r = (op - 0x58) | (I.rexB << 3);
                e_load(8, r, RSP); e_addi(RSP, RSP, 8, 1); gpc = next; continue; }         // pop
            // ---- movsxd (0x63): r64 = sign-extend r/m32 ----
            if (op == 0x63) {
                if (I.is_mem) { emit_ea(&I, next); e_ldrs(4, I.reg, 17); }
                else e_sxt(I.reg, I.rm_reg, 4);
                gpc = next; continue;
            }
            // ---- ALU primary (00..3D): /r reg,r/m forms ----
            // gate on op<0x40: bits[7:6]==00 is primary ALU. 0x80-0x83 (group1) handled below.
            if (op < 0x40 && (op & 7) <= 3 && alu_kind_primary(op) >= 0) {
                int k = alu_kind_primary(op), dir = op & 2;    // dir 0: r/m,reg ; 2: reg,r/m
                int w = (op & 1) ? I.opsize : 1, mem;
                if ((k == 2 || k == 3) && w < 4) { report_unimpl(gpc, &I); break; }   // ADC/SBB 8/16: TODO
                int rmv = rm_load(&I, next, w, &mem);
                int regv = (w == 1) ? byte_val(&I, I.reg, 24) : I.reg;          // reg operand value (handle ah/ch)
                if (dir) {                                                       // dst = reg
                    if (k == 7) do_alu(7, -1, regv, rmv, w);                     // cmp: no write
                    else if (w == 1) { do_alu(k, 16, regv, rmv, w); byte_wb(&I, I.reg, 16); }
                    else do_alu(k, I.reg, I.reg, rmv, w);
                } else {                                                         // dst = r/m
                    do_alu(k, (k == 7) ? -1 : 16, rmv, regv, w);
                    if (k != 7) rm_store(&I, w, 16);
                }
                gpc = next; continue;
            }
            // ALU al/eax/rax, imm (04/05 ... 3C/3D)
            if (op < 0x40 && ((op & 7) == 4 || (op & 7) == 5) && alu_kind_primary(op) >= 0) {
                int k = alu_kind_primary(op), w = (op & 7) == 4 ? 1 : I.opsize;
                if (!((k == 2 || k == 3) && w < 4)) { e_movconst(16, (uint64_t)I.imm);
                    do_alu(k, k == 7 ? -1 : RAX, RAX, 16, w); gpc = next; continue; }
            }
            // ---- group1 (80/81/83): ALU r/m, imm ----
            if (op == 0x80 || op == 0x81 || op == 0x83) {
                int k = I.reg & 7, w = op == 0x80 ? 1 : I.opsize, mem;
                if (!((k == 2 || k == 3) && w < 4)) {         // ADC/SBB ok for 32/64-bit
                    int rmv = rm_load(&I, next, w, &mem);     // mem -> x16 (val), x17 (EA)
                    e_movconst(19, (uint64_t)I.imm);          // imm in x19 (x16 holds the loaded value)
                    // compute into scratch x16, then rm_store -> correct dest (handles mem + hi/lo byte regs)
                    do_alu(k, (k == 7) ? -1 : 16, rmv, 19, w);
                    if (k != 7) rm_store(&I, w, 16);
                    gpc = next; continue;
                }
            }
            // ---- test (84/85, A8/A9, F6/F7 /0) ----
            if (op == 0x84 || op == 0x85) {
                int w = (op & 1) ? I.opsize : 1, mem; int rmv = rm_load(&I, next, w, &mem);
                int regv = (w == 1) ? byte_val(&I, I.reg, 24) : I.reg;       // reg operand: handle ah/bh/ch/dh
                do_alu(4, -1, rmv, regv, w); gpc = next; continue;           // test = and(discard)
            }
            if (op == 0xA8 || op == 0xA9) {
                int w = op == 0xA8 ? 1 : I.opsize; e_movconst(16, (uint64_t)I.imm);
                do_alu(4, -1, RAX, 16, w); gpc = next; continue;
            }
            // ---- shifts: group2 (C0/C1 imm, D0/D1 by 1, D2/D3 by CL) ----
            if (op == 0xC0 || op == 0xC1 || op == 0xD0 || op == 0xD1 || op == 0xD2 || op == 0xD3) {
                int k = I.reg & 7; if (k == 6) k = 4;       // SAL == SHL
                int w = (op & 1) ? I.opsize : 1, mem;
                int bycl = (op == 0xD2 || op == 0xD3), by1 = (op == 0xD0 || op == 0xD1);
                if (k != 0 && k != 1 && k != 4 && k != 5 && k != 7) { report_unimpl(gpc, &I); break; } // RCL/RCR defer
                int raw = rm_load(&I, next, w, &mem);
                if ((k == 0 || k == 1) && w < 4) {            // 8/16-bit ROL/ROR -- rotate WITHIN the operand width
                    int width = 8 * w;                        // (a 64-bit ROR would wrap the wrong bits, e.g. rolw $8)
                    e_uxt(16, raw, w);                        // x16 = zero-extended operand (low `width` bits)
                    e_bfi(16, 16, width, width, 0);           // replicate v -> [2w-1:w] (16-bit: now 32 bits = v|v)
                    if (w == 1) e_bfi(16, 16, 16, 16, 0);     // byte: replicate the pair again -> 4 copies fill 32 bits
                    if (bycl) {                               // count = CL masked to the operand width
                        e_movconst(19, width - 1); e_rrr(A_AND, 20, RCX, 19, 0, 0);            // x20 = CL & (width-1)
                        if (k == 0) { e_movconst(19, width); e_rrr(A_SUB, 20, 19, 20, 0, 0);   // ROL by n == ROR by (width-n)
                                      e_movconst(19, width - 1); e_rrr(A_AND, 20, 20, 19, 0, 0); }
                        e_shv(S_RORV, 16, 16, 20, 0);         // 32-bit RORV of the replicated value -> low `width` bits correct
                    } else {
                        int ce = (((int)(I.imm) % width) + width) % width;
                        int rr = (k == 1) ? ce : (width - ce) % width;
                        if (rr) e_ror_i(16, 16, rr, 0);       // 32-bit ROR; low `width` bits are the answer
                    }
                    rm_store(&I, w, 16);                      // stores low w bytes; x86 rotates leave SF/ZF unchanged -> no flag save
                    gpc = next; continue;
                }
                int ssf = (w >= 4) ? sf : 1;                // operate 64-bit on extended byte/word
                // bring the operand into x16, zero/sign-extended for w<4
                if (w < 4) { if (k == 5) e_uxt(16, raw, w); else if (k == 7) e_sxt(16, raw, w); else e_mov_rr(16, raw, 0); }
                else if (raw != 16) e_mov_rr(16, raw, sf);
                int src = 16;
                int cnt = by1 ? 1 : (bycl ? -1 : (int)(I.imm & (ssf ? 63 : 31)));
                // exact x86 CF (last bit shifted out) for SHL/SHR/SAR immediate at 32/64-bit
                int want_cf = (!bycl && w >= 4 && (k == 4 || k == 5 || k == 7) && cnt >= 1);
                if (want_cf) e_mov_rr(19, src, ssf);          // save original operand for CF
                if (bycl) {
                    if (k == 0) { report_unimpl(gpc, &I); break; }      // ROL by CL: defer
                    uint32_t b = k == 4 ? S_LSLV : k == 5 ? S_LSRV : k == 7 ? S_ASRV : S_RORV;
                    e_shv(b, 16, src, RCX, ssf);
                } else {
                    if (cnt == 0) { if (mem) e_store(w, raw, 17); gpc = next; continue; }  // no flags change
                    if (k == 4) e_lsl_i(16, src, cnt, ssf);
                    else if (k == 5) e_lsr_i(16, src, cnt, ssf);
                    else if (k == 7) e_asr_i(16, src, cnt, ssf);
                    else if (k == 1) e_ror_i(16, src, cnt, ssf);
                    else /*k==0 ROL*/ e_ror_i(16, src, (ssf ? 64 : 32) - cnt, ssf);
                }
                // SF/ZF from result (byte/word via high-bits); CF exact for immediate SHL/SHR/SAR, else approximate
                if (w < 4) { e_lsl_i(21, 16, 8 * (4 - w), 0); e_tst(21, 0); } else e_tst(16, sf);
                if (want_cf) {
                    int width = ssf ? 64 : 32, bit = (k == 4) ? (width - cnt) : (cnt - 1);
                    if (bit > width - 1) bit = width - 1;
                    e_lsr_i(19, 19, bit, ssf); e_movconst(23, 1); e_rrr(A_AND, 19, 19, 23, ssf, 0);  // x19 = x86 CF bit
                    e_nzcv_save_setcf(19);
                } else e_nzcv_save();
                rm_store(&I, w, 16);
                gpc = next; continue;
            }
            // ---- group3 (F6/F7): /0 test /2 not /3 neg /4 mul /5 imul /6 div /7 idiv ----
            if (op == 0xF6 || op == 0xF7) {
                int k = I.reg & 7, w = op == 0xF6 ? 1 : I.opsize, mem;
                if (k == 0) { int rmv = rm_load(&I, next, w, &mem); e_movconst(19, (uint64_t)I.imm);
                              do_alu(4, -1, rmv, 19, w); gpc = next; continue; }        // test r/m, imm
                if (k == 2) { int rmv = rm_load(&I, next, w, &mem);                     // not -> x16, then rm_store
                              emit32(0xAA2003E0u | (rmv << 16) | 16); rm_store(&I, w, 16); gpc = next; continue; }
                if (k == 3) { int rmv = rm_load(&I, next, w, &mem);                     // neg -> x16
                              e_rrr(A_SUBS, 16, 31, rmv, w == 8, 0); e_nzcv_save(); rm_store(&I, w, 16); gpc = next; continue; }
                if (w == 4 || w == 8) {
                    if (k == 4 || k == 5) {                                             // mul / imul (rdx:rax = rax * r/m)
                        int rmv = rm_load(&I, next, w, &mem);
                        // zero/sign-extend operands to 64, full product, lo->rax hi->rdx
                        if (k == 4) { e_mul(19, RAX, rmv, 1); e_umulh(RDX, RAX, rmv); }   // unsigned (assumes w==8); for w==4 see below
                        else        { e_mul(19, RAX, rmv, 1); e_smulh(RDX, RAX, rmv); }
                        if (w == 4) { e_lsr_i(RDX, 19, 32, 1); e_mov_rr(RAX, 19, 0); }    // eax=lo32, edx=hi32
                        else e_mov_rr(RAX, 19, 1);
                        gpc = next; continue;
                    }
                    if (k == 6 || k == 7) {                                             // div / idiv
                        int rmv = rm_load(&I, next, w, &mem);
                        if (w == 8) {                          // 128/64: rdx may be nonzero -> exact division in C
                            e_str(rmv, 28, OFF_DIVOP); emit_exit_const(next, k == 6 ? R_DIV : R_IDIV); break;
                        }
                        // 32-bit: dividend = edx:eax (64-bit), 32-bit divisor (zero/sign-extend), 32-bit quotient
                        e_lsl_i(19, RDX, 32, 1); e_bfi(19, RAX, 0, 32, 1);              // x19 = (edx<<32)|eax
                        if (k == 6) { e_uxt(22, rmv, 4); e_udiv(20, 19, 22, 1); }       // unsigned: zero-extend divisor
                        else        { e_sxt(22, rmv, 4); e_sdiv(20, 19, 22, 1); }       // signed: sign-extend divisor (edx:eax already 64-bit signed)
                        e_msub(21, 20, 22, 19, 1);                                      // rem = x19 - q*divisor
                        e_mov_rr(RAX, 20, 0); e_mov_rr(RDX, 21, 0);                      // eax=quot, edx=rem (32-bit)
                        gpc = next; continue;
                    }
                }
                report_unimpl(gpc, &I); break;
            }
            // ---- group4/5 (FE/FF): inc/dec, and FF: call/jmp/push (indirect) ----
            if (op == 0xFE || op == 0xFF) {
                int k = I.reg & 7, w = op == 0xFE ? 1 : I.opsize, mem;
                if (k == 0 || k == 1) {                        // inc / dec: set N/Z/V (OF correct), PRESERVE CF
                    int rmv = rm_load(&I, next, w, &mem); int o = mem ? 16 : I.rm_reg;
                    if (w >= 4) {
                        if (k == 0) e_addi_s(o, rmv, 1, sf); else e_subi_s(o, rmv, 1, sf);
                        e_nzcv_save_keepC();
                        rm_store(&I, w, o);
                    } else {                                   // byte/word: flags from the high bits
                        int sh = 8 * (4 - w); e_lsl_i(21, rmv, sh, 0); e_movconst(19, 1u << sh);
                        if (k == 0) e_rrr(A_ADDS, 21, 21, 19, 0, 0); else e_rrr(A_SUBS, 21, 21, 19, 0, 0);
                        e_nzcv_save_keepC(); e_lsr_i(21, 21, sh, 0); rm_store(&I, w, 21);
                    }
                    gpc = next; continue;
                }
                if (op == 0xFF && (k == 4 || k == 2)) {        // jmp / call r/m (indirect)
                    int mem2; int tgt = rm_load(&I, next, 8, &mem2);
                    if (tgt != 16) e_mov_rr(16, tgt, 1);       // target -> x16
                    e_movconst(19, gpc); e_str(19, 28, OFF_IBSRC);  // debug
                    if (k == 2) { e_subi(RSP, RSP, 8, 1); e_movconst(19, next); e_store(8, 19, RSP); }  // call: push ret
                    emit_ibranch(); break;                          // IBTC inline probe (target in x16)
                }
                if (op == 0xFF && k == 6) {                     // push r/m
                    int mem2; int v = rm_load(&I, next, 8, &mem2); if (v != 16) e_mov_rr(16, v, 1);
                    e_subi(RSP, RSP, 8, 1); e_store(8, 16, RSP); gpc = next; continue;
                }
            }
            // ---- xchg (86/87) ----
            if (op == 0x86 || op == 0x87) {
                int w = (op & 1) ? I.opsize : 1, mem;
                if (I.is_mem) { emit_ea(&I, next); e_load(w, 16, 17);              // x16 = old [mem]
                                int sv = (w == 1) ? byte_val(&I, I.reg, 19) : I.reg;  // reg->mem: handle ah/bh/ch/dh
                                e_store(w, sv, 17);
                                if (w >= 4) e_mov_rr(I.reg, 16, w == 8); else if (w == 1) byte_wb(&I, I.reg, 16); else e_bfi(I.reg, 16, 0, 8 * w, 1); }
                else { e_mov_rr(19, I.rm_reg, sf); e_mov_rr(I.rm_reg, I.reg, sf); e_mov_rr(I.reg, 19, sf); }
                (void)mem; gpc = next; continue;
            }
            // ---- push imm (68 iz, 6A ib) ----
            if (op == 0x68 || op == 0x6A) { e_movconst(16, (uint64_t)I.imm);
                e_subi(RSP, RSP, 8, 1); e_store(8, 16, RSP); gpc = next; continue; }
            // ---- pop r/m (8F /0) ----
            if (op == 0x8F) { e_load(8, 16, RSP); e_addi(RSP, RSP, 8, 1);
                if (I.is_mem) { emit_ea(&I, next); e_store(8, 16, 17); } else e_mov_rr(I.rm_reg, 16, 1);
                gpc = next; continue; }
            // ---- imul reg, r/m, imm (69 iz, 6B ib) ----
            if (op == 0x69 || op == 0x6B) {
                int mem; int rmv = rm_load(&I, next, I.opsize, &mem);
                e_movconst(19, (uint64_t)I.imm); e_mul(I.reg, rmv, 19, sf);
                gpc = next; continue;       // flags (CF/OF on overflow) approximate -> TODO
            }
            // ---- string ops: stos (AA/AB), movs (A4/A5), lods (AC/AD). DF assumed 0 (fwd). ----
            if (op == 0xAA || op == 0xAB || op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
                int w = (op & 1) ? I.opsize : 1;
                int movs = (op == 0xA4 || op == 0xA5), lods = (op == 0xAC || op == 0xAD);
                uint32_t *cbz = NULL, *top = NULL;
                if (I.rep) { top = (uint32_t *)g_cp; cbz = (uint32_t *)g_cp; emit32(0); }  // cbz RCX,done
                if (movs)      { e_load(w, 16, RSI); e_store(w, 16, RDI); e_addi(RSI, RSI, w, 1); e_addi(RDI, RDI, w, 1); }
                else if (lods) { e_load(w, RAX, RSI); e_addi(RSI, RSI, w, 1); }
                else           { e_store(w, RAX, RDI); e_addi(RDI, RDI, w, 1); }            // stos
                if (I.rep) {
                    e_subi(RCX, RCX, 1, 1);
                    int64_t back = (int64_t)(top - (uint32_t *)g_cp); emit32(0x14000000u | ((uint32_t)back & 0x3FFFFFFu)); // b top
                    int64_t d = ((uint32_t *)g_cp - cbz);
                    *cbz = 0xB4000000u | (((uint32_t)d & 0x7FFFF) << 5) | RCX;              // cbz x_rcx,done
                }
                gpc = next; continue;
            }
            if (op == 0xFC) { gpc = next; continue; }       // cld (DF=0): we assume forward already
            if (op == 0xFD) { report_unimpl(gpc, &I); break; } // std (DF=1): backward string ops -> TODO
            // ---- jmp rel (E9/EB) ----
            if (op == 0xE9 || op == 0xEB) { emit_chain_exit(next + (uint64_t)I.imm); break; }
            // ---- call rel32 (E8) ----
            if (op == 0xE8) { e_subi(RSP, RSP, 8, 1); e_movconst(16, next); e_store(8, 16, RSP);
                              emit_chain_exit(next + (uint64_t)I.imm); break; }
            // ---- jrcxz rel8 (E3): jump if RCX == 0 ----
            if (op == 0xE3) {
                uint64_t taken = next + (uint64_t)I.imm;
                uint32_t *patch = (uint32_t *)g_cp; emit32(0);     // cbz x_rcx -> taken
                emit_chain_exit(next);                              // RCX != 0: fall through
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0xB4000000u | (((uint32_t)d & 0x7FFFF) << 5) | RCX;   // cbz x_rcx, taken
                emit_chain_exit(taken); break;
            }
            // ---- jcc rel8 (70-7F) ----
            if (op >= 0x70 && op <= 0x7F) {
                int cc = x86cc_to_arm(op & 0xF); if (cc < 0) { report_unimpl(gpc, &I); break; }
                uint64_t taken = next + (uint64_t)I.imm;
                e_nzcv_load();
                uint32_t *patch = (uint32_t *)g_cp; emit32(0);     // b.cond -> taken
                emit_chain_exit(next);
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (cc & 0xF);
                emit_chain_exit(taken); break;
            }
            // ---- ret (C3) / ret imm16 (C2) ----
            if (op == 0xC3 || op == 0xC2) {
                e_load(8, 16, RSP); e_addi(RSP, RSP, 8, 1);
                if (op == 0xC2) { e_movconst(19, (uint64_t)(uint16_t)I.imm); e_rrr(A_ADD, RSP, RSP, 19, 1, 0); }
                e_movconst(19, gpc); e_str(19, 28, OFF_IBSRC);   // debug
                emit_ibranch(); break;                            // IBTC inline probe (target in x16)
            }
            // ---- leave (C9) ----
            if (op == 0xC9) { e_mov_rr(RSP, RBP, 1); e_load(8, RBP, RSP); e_addi(RSP, RSP, 8, 1); gpc = next; continue; }
            // ---- nop (90) / xchg rAX, rN (91-97) ----
            if (op == 0x90 && !I.rep) { gpc = next; continue; }   // (F3 90 = pause -> also nop)
            if (op == 0x9B) { gpc = next; continue; }             // fwait/wait -> nop (FPU sync)
            // sahf (9E): AH -> flags. We map SF=AH.7, ZF=AH.6, CF=AH.0 into cpu->nzcv (N/Z/C).
            if (op == 0x9E) {
                emit32(0x53083C00u | (RAX << 5) | 16);            // ubfx w16, w_rax, #8, #8  (AH)
                emit32(0x53000000u | (16 << 5) | 17);             // ubfx w17, w16, #0, #1  (CF)
                e_lsl_i(17, 17, 29, 0);
                emit32(0x53061800u | (16 << 5) | 18);             // ubfx w18, w16, #6, #1  (ZF)
                e_lsl_i(18, 18, 30, 0); e_rrr(A_ORR, 17, 17, 18, 0, 0);
                emit32(0x53071C00u | (16 << 5) | 18);             // ubfx w18, w16, #7, #1  (SF)
                e_lsl_i(18, 18, 31, 0); e_rrr(A_ORR, 17, 17, 18, 0, 0);
                e_str(17, 28, OFF_NZCV); gpc = next; continue;
            }
            // lahf (9F): flags -> AH (SF,ZF,--,AF,--,PF,1,CF). We fill SF/ZF/CF + the always-1 bit.
            if (op == 0x9F) {
                e_ldr(16, 28, OFF_NZCV);
                emit32(0x53000000u | (31 << 16) | (31 << 10) | (16 << 5) | 17);   // ubfx w17,w16,#31,#1 (N->SF)
                e_lsl_i(17, 17, 7, 0);
                emit32(0x53000000u | (30 << 16) | (30 << 10) | (16 << 5) | 18);   // ubfx w18,w16,#30,#1 (Z->ZF)
                e_lsl_i(18, 18, 6, 0); e_rrr(A_ORR, 17, 17, 18, 0, 0);
                emit32(0x53000000u | (29 << 16) | (29 << 10) | (16 << 5) | 18);   // ubfx w18,w16,#29,#1 (C->CF)
                e_rrr(A_ORR, 17, 17, 18, 0, 0);
                e_movconst(18, 2); e_rrr(A_ORR, 17, 17, 18, 0, 0);                // bit1 reads as 1
                e_bfi(RAX, 17, 8, 8, 1); gpc = next; continue;                    // AH = w17
            }
            // ===== x87 FPU (D8-DF): double-precision stack emulation =====
            if (op >= 0xD8 && op <= 0xDF) {
                int reg = I.reg & 7, rm = I.rm_reg & 7;
                #define FAd(d,n,m) emit32(0x1E602800u|((m)<<16)|((n)<<5)|(d))   /* fadd d */
                #define FSd(d,n,m) emit32(0x1E603800u|((m)<<16)|((n)<<5)|(d))   /* fsub d */
                #define FMd(d,n,m) emit32(0x1E600800u|((m)<<16)|((n)<<5)|(d))   /* fmul d */
                #define FDd(d,n,m) emit32(0x1E601800u|((m)<<16)|((n)<<5)|(d))   /* fdiv d */
                #define FCMPd(n,m) do { emit32(0x1E602000u|((m)<<16)|((n)<<5)); e_nzcv_save(); } while (0)
                if (I.is_mem) {
                    emit_ea(&I, next); e_mov_rr(19, 17, 1);            // x19 = EA (helpers clobber x17)
                    if (op == 0xD9) {                                 // f32 mem
                        if (reg == 0) { e_ldr_s(16, 19); emit32(0x1E22C000u | (16 << 5) | 16); e_fp_push(16); }       // fld m32
                        else if (reg == 2 || reg == 3) { e_fp_ld(16, 0); emit32(0x1E624000u | (16 << 5) | 16); e_str_s(16, 19); if (reg == 3) e_fp_settop(1); } // fst/fstp
                        else if (reg == 5) { /* fldcw: ignore */ }
                        else if (reg == 7) { e_movconst(16, 0x037f); emit32(0x79000000u | (19 << 5) | 16); }          // fnstcw
                        else { report_unimpl(gpc, &I); break; }
                    } else if (op == 0xDD) {                          // f64 mem
                        if (reg == 0) { e_ldr_d(16, 19); e_fp_push(16); }                                            // fld m64
                        else if (reg == 2 || reg == 3) { e_fp_ld(16, 0); e_str_d(16, 19); if (reg == 3) e_fp_settop(1); } // fst/fstp
                        else if (reg == 7) { e_ldr(16, 28, OFF_FPSW); emit32(0x79000000u | (19 << 5) | 16); }          // fnstsw m16
                        else { report_unimpl(gpc, &I); break; }
                    } else if (op == 0xDB) {                          // i32 mem / m80
                        if (reg == 0) { emit32(0xB9400000u | (19 << 5) | 16); emit32(0x1E620000u | (16 << 5) | 16); e_fp_push(16); }  // fild m32
                        else if (reg == 2 || reg == 3) { e_fp_ld(16, 0); emit32(0x1E780000u | (16 << 5) | 16); emit32(0xB9000000u | (19 << 5) | 16); if (reg == 3) e_fp_settop(1); } // fist/fistp m32
                        else if (reg == 5) { e_str(19, 28, OFF_X87EA); emit_exit_const(next, R_X87FLD); break; }   // fld m80 -> C
                        else if (reg == 7) { e_str(19, 28, OFF_X87EA); emit_exit_const(next, R_X87FSTP); break; }  // fstp m80 -> C
                        else { report_unimpl(gpc, &I); break; }
                    } else if (op == 0xDF) {                          // i16/i64 mem
                        if (reg == 0) { emit32(0x79C00000u | (19 << 5) | 16); emit32(0x1E620000u | (16 << 5) | 16); e_fp_push(16); }   // fild m16 (ldrsh)
                        else if (reg == 3) { e_fp_ld(16, 0); emit32(0x1E780000u | (16 << 5) | 16); emit32(0x79000000u | (19 << 5) | 16); e_fp_settop(1); } // fistp m16
                        else if (reg == 5) { e_ldr(16, 19, 0); emit32(0x9E620000u | (16 << 5) | 16); e_fp_push(16); }    // fild m64
                        else if (reg == 7) { e_fp_ld(16, 0); emit32(0x9E780000u | (16 << 5) | 16); e_str(16, 19, 0); e_fp_settop(1); } // fistp m64
                        else { report_unimpl(gpc, &I); break; }
                    } else {                                          // D8 (f32) / DC (f64) arith with ST0
                        if (op == 0xD8) { e_ldr_s(16, 19); emit32(0x1E22C000u | (16 << 5) | 16); } else e_ldr_d(16, 19);
                        if (reg == 2 || reg == 3) { e_fp_ld(18, 0); e_fcom_setfpsw(18, 16); if (reg == 3) e_fp_settop(1); gpc = next; continue; }  // fcom/fcomp
                        e_fp_ld(18, 0);
                        if (reg == 0) FAd(18, 18, 16); else if (reg == 1) FMd(18, 18, 16);
                        else if (reg == 4) FSd(18, 18, 16); else if (reg == 5) FSd(18, 16, 18);
                        else if (reg == 6) FDd(18, 18, 16); else if (reg == 7) FDd(18, 16, 18);
                        else { report_unimpl(gpc, &I); break; }
                        e_fp_st(18, 0);
                    }
                    gpc = next; continue;
                }
                // ---- register forms (mod=3) ----
                if (op == 0xD9) {
                    if (reg == 0) { e_fp_ld(16, rm); e_fp_push(16); }                                    // fld ST(i)
                    else if (reg == 1) { e_fp_ld(16, 0); e_fp_ld(18, rm); e_fp_st(18, 0); e_fp_st(16, rm); } // fxch
                    else if (reg == 4 && rm == 0) { e_fp_ld(16, 0); emit32(0x1E614000u | (16 << 5) | 16); e_fp_st(16, 0); } // fchs
                    else if (reg == 4 && rm == 1) { e_fp_ld(16, 0); emit32(0x1E60C000u | (16 << 5) | 16); e_fp_st(16, 0); } // fabs
                    else if (reg == 5) {                                                                 // fld const
                        static const uint64_t k[8] = {0x3FF0000000000000ull/*1*/,0x400A934F0979A371ull/*l2t*/,0x3FF71547652B82FEull/*l2e*/,
                            0x400921FB54442D18ull/*pi*/,0x3FD34413509F79FFull/*lg2*/,0x3FE62E42FEFA39EFull/*ln2*/,0x0ull/*0*/,0x0ull};
                        e_movconst(16, k[rm]); e_fmov_to_d(16, 16); e_fp_push(16);
                    }
                    else if (reg == 7 && rm == 2) { e_fp_ld(16, 0); emit32(0x1E61C000u | (16 << 5) | 16); e_fp_st(16, 0); } // fsqrt
                    else { report_unimpl(gpc, &I); break; }
                } else if (op == 0xD8 || op == 0xDC || op == 0xDE) {  // arith ST0/ST(i) [+pop for DE]
                    e_fp_ld(18, 0); e_fp_ld(16, rm);                 // v18=ST0, v16=ST(rm)
                    int dst_i = (op == 0xD8) ? 0 : rm;               // D8 -> ST0; DC/DE -> ST(i)
                    if (reg == 2 || reg == 3) { e_fcom_setfpsw(18, 16); if (op == 0xDE && rm == 1) e_fp_settop(1); if (reg == 3) e_fp_settop(1); gpc = next; continue; } // fcom[p]/fcompp
                    int a = 18, b = 16; if (op != 0xD8) { a = 16; b = 18; }   // DC/DE: dst=ST(i)=v16, other=ST0=v18
                    if (reg == 0) FAd(a, a, b); else if (reg == 1) FMd(a, a, b);
                    else if (reg == 4) { if (op == 0xD8) FSd(a, a, b); else FSd(a, b, a); }   // DC/DE reverse sub
                    else if (reg == 5) { if (op == 0xD8) FSd(a, b, a); else FSd(a, a, b); }
                    else if (reg == 6) { if (op == 0xD8) FDd(a, a, b); else FDd(a, b, a); }
                    else if (reg == 7) { if (op == 0xD8) FDd(a, b, a); else FDd(a, a, b); }
                    else { report_unimpl(gpc, &I); break; }
                    e_fp_st(a, dst_i);
                    if (op == 0xDE) e_fp_settop(1);                  // pop
                } else if (op == 0xDD) {
                    if (reg == 0) { /* ffree: no tag tracking -> nop */ }
                    else if (reg == 2) { e_fp_ld(16, 0); e_fp_st(16, rm); }            // fst ST(i)
                    else if (reg == 3) { e_fp_ld(16, 0); e_fp_st(16, rm); e_fp_settop(1); } // fstp ST(i)
                    else if (reg == 4 || reg == 5) { e_fp_ld(18, 0); e_fp_ld(16, rm); e_fcom_setfpsw(18, 16); if (reg == 5) e_fp_settop(1); } // fucom[p]
                    else { report_unimpl(gpc, &I); break; }
                } else if (op == 0xDB) {
                    if (reg == 4 && rm == 3) { e_movconst(16, 0); e_str(16, 28, OFF_FPTOP); }   // finit -> top=0
                    else if (reg == 4) { /* fclex/etc */ }
                    else if (reg == 5 || reg == 6) { e_fp_ld(18, 0); e_fp_ld(16, rm); FCMPd(18, 16); }   // fucomi/fcomi
                    else { report_unimpl(gpc, &I); break; }
                } else if (op == 0xDF) {
                    if (reg == 4 && rm == 0) { e_ldr(16, 28, OFF_FPSW); e_bfi(RAX, 16, 0, 16, 1); }  // fnstsw ax
                    else if (reg == 5 || reg == 6) { e_fp_ld(18, 0); e_fp_ld(16, rm); FCMPd(18, 16); e_fp_settop(1); } // fucomip/fcomip
                    else { report_unimpl(gpc, &I); break; }
                } else if (op == 0xDA) {                                       // fcmovcc ST0,ST(i) (reg 0/1/2/3 = B/E/BE/U)
                    if (reg <= 3) {                                            // condition from integer EFLAGS
                        int jcc = (reg == 0) ? 2 : (reg == 1) ? 4 : (reg == 2) ? 6 : 10;   // jb/je/jbe/jp
                        int armc = x86cc_to_arm(jcc); e_nzcv_load();
                        e_fp_ld(18, 0); e_fp_ld(16, rm);                       // v18=ST0, v16=ST(i)
                        emit32(0x1E600C00u | (18 << 16) | ((armc & 0xF) << 12) | (16 << 5) | 17);  // fcsel d17, STi, ST0, cond
                        e_fp_st(17, 0);
                    } else if (reg == 5 && rm == 1) {                          // DA E9: fucompp (compare ST0,ST1; pop twice)
                        e_fp_ld(18, 0); e_fp_ld(16, 1); e_fcom_setfpsw(18, 16); e_fp_settop(1); e_fp_settop(1);
                    } else { report_unimpl(gpc, &I); break; }
                } else { report_unimpl(gpc, &I); break; }
                #undef FAd
                #undef FSd
                #undef FMd
                #undef FDd
                #undef FCMPd
                gpc = next; continue;
            }
            if (op == 0x90) { gpc = next; continue; }
            if (op >= 0x91 && op <= 0x97) { int r = (op - 0x90) | (I.rexB << 3);
                e_mov_rr(19, RAX, sf); e_mov_rr(RAX, r, sf); e_mov_rr(r, 19, sf); gpc = next; continue; }
            // ---- cbw/cwde/cdqe (98) and cwd/cdq/cqo (99) ----
            if (op == 0x98) {
                if (sf) e_sxt(RAX, RAX, 4);                              // cdqe: rax = sext32(eax)
                else if (I.p66) emit32(0x13001C00u | (RAX << 5) | RAX);  // cbw: ax = sext8(al) (sxtb Wd,Wn)
                else emit32(0x13003C00u | (RAX << 5) | RAX);             // cwde: eax = sext16(ax) (sxth Wd,Wn)
                gpc = next; continue;
            }
            if (op == 0x99) {
                if (sf) e_asr_i(RDX, RAX, 63, 1);                        // cqo: rdx = rax>>63 (arith)
                else if (I.p66) { e_asr_i(19, RAX, 15, 0); e_bfi(RDX, 19, 0, 16, 1); } // cwd: dx=sign(ax)
                else e_asr_i(RDX, RAX, 31, 0);                           // cdq: edx = eax>>31 (arith)
                gpc = next; continue;
            }
        } else {
            // ===== two-byte (0F xx) =====
            if (op == 0x05) { emit_exit_const(next, R_SYSCALL); break; }   // syscall
            // ===== SSE / SSE2 (guest xmm0..15 == host v0..v15) =====
            // mandatory prefix selects the variant: 66=packed-int/double, F3=scalar-single,
            // F2=scalar-double, none=packed-single. reg/rm fields index xmm directly.
            {
                int handled = 1, mem; int vd = I.reg, vm = I.rm_reg;
                if (op == 0x6E) {                                   // movd/movq xmm, r/m  (66)
                    if (I.is_mem) { emit_ea(&I, next); if (I.rexW) e_ldr_d(vd, 17); else e_ldr_s(vd, 17); }
                    else { if (I.rexW) e_fmov_to_d(vd, I.rm_reg); else e_fmov_to_s(vd, I.rm_reg); }
                } else if (op == 0x7E && I.rep) {                   // F3 0F 7E: movq xmm, xmm/m64
                    if (I.is_mem) { emit_ea(&I, next); e_ldr_d(vd, 17); } else e_vmov8(vd, vm);
                } else if (op == 0x7E) {                            // 66 0F 7E: movd/movq r/m, xmm
                    if (I.is_mem) { emit_ea(&I, next); if (I.rexW) e_str_d(vd, 17); else e_str_s(vd, 17); }
                    else { if (I.rexW) e_fmov_from_d(I.rm_reg, vd); else e_fmov_from_s(I.rm_reg, vd); }
                } else if (op == 0xD6) {                            // 66 0F D6: movq xmm/m64, xmm
                    if (I.is_mem) { emit_ea(&I, next); e_str_d(vd, 17); } else e_vmov8(vm, vd);
                } else if (op == 0x6F || op == 0x28 || (op == 0x10 && !I.rep && !I.repne)) {  // load 128 -> xmm
                    if (I.is_mem) { emit_ea(&I, next); e_ldr_q(vd, 17, 0); } else e_vmov(vd, vm);
                } else if (op == 0x7F || op == 0x29 || (op == 0x11 && !I.rep && !I.repne)) {  // store xmm -> 128
                    if (I.is_mem) { emit_ea(&I, next); e_str_q(vd, 17, 0); } else e_vmov(vm, vd);
                } else if ((op == 0x10 || op == 0x11) && I.rep) {  // movss (32-bit)
                    int st = (op == 0x11);
                    if (I.is_mem) { emit_ea(&I, next); if (st) e_str_s(vd, 17); else e_ldr_s(vd, 17); }
                    else { if (st) emit32(0x6E040420u | (vd << 5) | vm); else emit32(0x6E040420u | (vm << 5) | vd); } // ins .s[0]
                } else if ((op == 0x10 || op == 0x11) && I.repne) { // movsd (64-bit)
                    int st = (op == 0x11);
                    if (I.is_mem) { emit_ea(&I, next); if (st) e_str_d(vd, 17); else e_ldr_d(vd, 17); }
                    else { if (st) emit32(0x6E080420u | (vd << 5) | vm); else emit32(0x6E080420u | (vm << 5) | vd); } // ins .d[0]
                } else if (op == 0x12 || op == 0x16) {              // movlps/movhps (load) or movhlps/movlhps (reg)
                    int lane = (op == 0x16) ? 1 : 0;               // 12->low lane(d[0]), 16->high lane(d[1])
                    if (I.is_mem) { emit_ea(&I, next); e_ldr_d(16, 17); e_ins_d(vd, lane, 16, 0); }
                    else { int srclane = (op == 0x12) ? 1 : 0;     // movhlps: d[0]<-src d[1]; movlhps: d[1]<-src d[0]
                           e_ins_d(vd, lane, vm, srclane); }
                } else if (op == 0x13 || op == 0x17) {              // movlps/movhps store
                    int lane = (op == 0x17) ? 1 : 0;
                    emit_ea(&I, next); e_ins_d(16, 0, vd, lane); e_str_d(16, 17);
                } else if (op == 0x54 || op == 0x55 || op == 0x56 || op == 0x57) {  // andps/andnps/orps/xorps (FP bitwise)
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    if (op == 0x54) e_v3(0x4E201C00u, vd, vd, s);        // and
                    else if (op == 0x55) e_v3(0x4E601C00u, vd, s, vd);  // andn: ~vd & s -> bic vd,s,vd
                    else if (op == 0x56) e_v3(0x4EA01C00u, vd, vd, s);  // or
                    else e_v3(0x6E201C00u, vd, vd, s);                  // xor
                } else if (op == 0xC6 && I.p66) {                       // shufpd: 64-bit lanes (d[0]<-dst, d[1]<-src)
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    unsigned im = (unsigned)I.imm; e_vmov(18, vd);
                    e_ins_d(17, 0, 18, im & 1); e_ins_d(17, 1, s, (im >> 1) & 1);
                    e_vmov(vd, 17);
                } else if (op == 0xC6) {                                // shufps xmm,xmm/m,imm8 (lanes 0,1 from dst; 2,3 from src)
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    unsigned im = (unsigned)I.imm; e_vmov(18, vd);
                    e_ins_s(17, 0, 18, im & 3); e_ins_s(17, 1, 18, (im >> 2) & 3);
                    e_ins_s(17, 2, s, (im >> 4) & 3); e_ins_s(17, 3, s, (im >> 6) & 3);
                    e_vmov(vd, 17);
                } else if (op == 0x71 || op == 0x72 || op == 0x73) {    // psrl/psra/psll w/d/q by imm8; psrldq/pslldq
                    int sub = I.reg & 7, esz = op == 0x71 ? 16 : op == 0x72 ? 32 : 64, sh = (int)(I.imm & 0xff), x = I.rm_reg;
                    if (sub == 2) e_vshr_imm(x, x, esz, sh, 0);          // psrl
                    else if (sub == 4) e_vshr_imm(x, x, esz, sh, 1);     // psra
                    else if (sub == 6) e_vshl_imm(x, x, esz, sh);        // psll
                    else if (op == 0x73 && sub == 3) { e_v3(0x6E201C00u, 18, 18, 18); e_ext(x, x, 18, sh & 0xF); }       // psrldq
                    else if (op == 0x73 && sub == 7) { e_v3(0x6E201C00u, 18, 18, 18); e_ext(x, 18, x, (16 - (sh & 0xF)) & 0xF); } // pslldq
                    else { report_unimpl(gpc, &I); break; }
                } else if (op == 0x70 && I.p66) {                       // pshufd xmm, xmm/m, imm8
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    unsigned im = (unsigned)I.imm;
                    for (int i = 0; i < 4; i++) e_ins_s(17, i, s, (im >> (2 * i)) & 3);  // build in v17
                    e_vmov(vd, 17);
                } else if (op == 0xEF) {                            // pxor
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    e_v3(0x6E201C00u, vd, vd, s);
                } else if (op == 0xDB || op == 0xEB || op == 0xDF) { // pand / por / pandn
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    if (op == 0xDB) e_v3(0x4E201C00u, vd, vd, s);
                    else if (op == 0xEB) e_v3(0x4EA01C00u, vd, vd, s);
                    else e_v3(0x4E601C00u, vd, s, vd);              // pandn: vd = ~vd & s  -> BIC vd, s, vd
                } else if (op == 0x74 || op == 0x75 || op == 0x76) { // pcmpeqb/w/d
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    uint32_t b = op == 0x74 ? 0x6E208C00u : op == 0x75 ? 0x6E608C00u : 0x6EA08C00u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0x64 || op == 0x65 || op == 0x66) { // pcmpgtb/w/d -> CMGT (signed)
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    uint32_t b = op == 0x64 ? 0x4E203400u : op == 0x65 ? 0x4E603400u : 0x4EA03400u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0xDE || op == 0xDA || op == 0xEE || op == 0xEA) { // pmaxub/pminub/pmaxsw/pminsw
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    uint32_t b = op == 0xDE ? 0x6E20A400u : op == 0xDA ? 0x6E20AC00u : op == 0xEE ? 0x4E60A400u : 0x4E60AC00u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0xFC || op == 0xFD || op == 0xFE || op == 0xD4) { // paddb/w/d/q
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    uint32_t b = op == 0xFC ? 0x4E208400u : op == 0xFD ? 0x4E608400u : op == 0xFE ? 0x4EA08400u : 0x4EE08400u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0xF8 || op == 0xF9 || op == 0xFA || op == 0xFB) { // psubb/w/d/q
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    uint32_t b = op == 0xF8 ? 0x6E208400u : op == 0xF9 ? 0x6E608400u : op == 0xFA ? 0x6EA08400u : 0x6EE08400u;
                    e_v3(b, vd, vd, s);
                } else if (op == 0x60 || op == 0x61 || op == 0x62 || op == 0x6C ||
                           op == 0x68 || op == 0x69 || op == 0x6A || op == 0x6D) {   // punpck l/h bw/wd/dq/qdq -> ZIP1/ZIP2
                    int s = I.is_mem ? 16 : vm; if (I.is_mem) { emit_ea(&I, next); e_ldr_q(16, 17, 0); }
                    int hi = (op == 0x68 || op == 0x69 || op == 0x6A || op == 0x6D);  // punpckh*; 0x6C(lqdq) is LOW
                    int sz = (op == 0x60 || op == 0x68) ? 0 : (op == 0x61 || op == 0x69) ? 1 : (op == 0x62 || op == 0x6A) ? 2 : 3;
                    uint32_t b = (hi ? 0x4E007800u : 0x4E003800u) | ((uint32_t)sz << 22);
                    e_v3(b, vd, vd, s);
                } else if (op == 0xD7) {                            // pmovmskb: byte MSBs -> GPR (reg)
                    e_str_q(vm, 28, OFF_MM);                        // spill the 16 bytes to scratch
                    e_addi(17, 28, OFF_MM, 1);                      // x17 = &scratch
                    e_movz(I.reg, 0, 0);                            // result = 0
                    for (int i = 0; i < 16; i++) {
                        emit32(0x39400000u | ((unsigned)i << 10) | (17 << 5) | 16);   // ldrb w16,[x17,#i]
                        emit32(0x53071C00u | (16 << 5) | 16);                          // ubfx w16,w16,#7,#1
                        emit32(0x2A000000u | (16 << 16) | ((unsigned)i << 10) | (I.reg << 5) | I.reg); // orr reg,reg,w16,lsl#i
                    }
                } else if (op == 0x2A) {                           // cvtsi2sd/ss: int r/m -> xmm (F2=double,F3=single)
                    int src; if (I.is_mem) { emit_ea(&I, next); e_load(I.rexW ? 8 : 4, 16, 17); src = 16; } else src = I.rm_reg;
                    emit32(0x1E220000u | (I.rexW ? 0x80000000u : 0) | (I.repne ? 0x00400000u : 0) | (src << 5) | vd);  // scvtf vd,src
                } else if (op == 0x2C || op == 0x2D) {             // cvttsd2si(2C trunc)/cvtsd2si(2D round): xmm/m -> GPR
                    int s = vm; if (I.is_mem) { emit_ea(&I, next); if (I.repne) e_ldr_d(16, 17); else e_ldr_s(16, 17); s = 16; }
                    uint32_t fop = (op == 0x2C) ? 0x1E380000u : 0x1E200000u;   // FCVTZS (trunc) : FCVTNS (round)
                    emit32(fop | (I.rexW ? 0x80000000u : 0) | (I.repne ? 0x00400000u : 0) | (s << 5) | I.reg);
                } else if (op == 0x58 || op == 0x59 || op == 0x5C || op == 0x5E || op == 0x5D || op == 0x5F || op == 0x51) {
                    int s = vm; if (I.is_mem) { emit_ea(&I, next); if (I.repne) e_ldr_d(16, 17); else e_ldr_s(16, 17); s = 16; }
                    uint32_t ty = I.repne ? 0x00400000u : 0;       // F2=double, F3=single
                    uint32_t b = op == 0x58 ? 0x1E202800u : op == 0x59 ? 0x1E200800u : op == 0x5C ? 0x1E203800u :
                                 op == 0x5E ? 0x1E201800u : op == 0x5D ? 0x1E205800u : op == 0x5F ? 0x1E204800u : 0x1E21C000u;
                    if (op == 0x51) emit32(b | ty | (s << 5) | vd);              // FSQRT vd, s
                    else emit32(b | ty | (s << 16) | (vd << 5) | vd);            // FADD/FMUL/FSUB/FDIV/FMIN/FMAX vd,vd,s
                } else if (op == 0x5A) {                           // cvtsd2ss(F2) / cvtss2sd(F3)
                    int s = vm; if (I.is_mem) { emit_ea(&I, next); if (I.repne) e_ldr_d(16, 17); else e_ldr_s(16, 17); s = 16; }
                    if (I.repne) emit32(0x1E624000u | (s << 5) | vd);            // FCVT Sd, Dn (double->single)
                    else emit32(0x1E22C000u | (s << 5) | vd);                    // FCVT Dd, Sn (single->double)
                } else if (op == 0x2E || op == 0x2F) {             // ucomisd/comisd (66=double, none=single) -> FCMP + flags
                    int s = vm; if (I.is_mem) { emit_ea(&I, next); if (I.p66) e_ldr_d(16, 17); else e_ldr_s(16, 17); s = 16; }
                    emit32((I.p66 ? 0x1E602000u : 0x1E202000u) | (s << 16) | (vd << 5));  // FCMP Dvd, Ds  (Rd=0)
                    e_nzcv_save();                                 // CF/ZF/PF substrate: ARM FCMP C/Z align with x86 unsigned cc
                } else handled = 0;
                if (handled) { gpc = next; continue; }
            }
            if (op == 0xA2) { emit_exit_const(next, R_CPUID); break; }      // cpuid -> dispatcher helper
            if (op == 0x31) {                                              // rdtsc: edx:eax = cntvct
                emit32(0xD53BE040u | 16);                                  // mrs x16, cntvct_el0
                e_mov_rr(RAX, 16, 0); e_lsr_i(RDX, 16, 32, 1); gpc = next; continue;
            }
            if (op == 0x01 && I.has_modrm && I.modrm == 0xD0) {            // xgetbv (ecx=0): XCR0 = x87+SSE (no AVX)
                e_movz(RAX, 3, 0); e_movz(RDX, 0, 0); gpc = next; continue;
            }
            if (op == 0x1E && I.imm_bytes == 0) { gpc = next; continue; }  // endbr (modrm consumed)
            if (op == 0x1F) { gpc = next; continue; }                      // nop r/m
            // shld/shrd (0F A4 imm8, 0F A5 cl, 0F AC imm8, 0F AD cl):  dst=r/m, src=reg, count
            if (op == 0xA4 || op == 0xA5 || op == 0xAC || op == 0xAD) {
                int isleft = (op == 0xA4 || op == 0xA5), bycl = (op == 0xA5 || op == 0xAD);
                int w = I.opsize, mem;
                if (w == 2) { report_unimpl(gpc, &I); break; }   // 16-bit shld/shrd: rare, EXTR can't do 16-bit lanes
                int ssf = (w == 8) ? 1 : 0, width = ssf ? 64 : 32;
                int dst = rm_load(&I, next, w, &mem), src = I.reg;
                if (!bycl) {
                    int n = (int)(I.imm & (ssf ? 63 : 31));
                    if (n == 0) { if (mem) e_store(w, dst, 17); gpc = next; continue; }  // count 0 -> no change, flags intact
                    if (isleft) e_extr(16, dst, src, width - n, ssf);   // (dst<<n)|(src>>(W-n))
                    else        e_extr(16, src, dst, n, ssf);           // (dst>>n)|(src<<(W-n))
                } else {
                    e_mov_rr(22, dst, ssf);                             // preserve orig dst for the n==0 select
                    e_movconst(19, ssf ? 63 : 31); e_rrr(A_AND, 17, RCX, 19, ssf, 0);  // n = cl & (W-1)
                    e_movconst(20, width); e_rrr(A_SUB, 20, 20, 17, ssf, 0);           // 20 = W - n
                    if (isleft) { e_shv(S_LSLV, 19, dst, 17, ssf); e_shv(S_LSRV, 20, src, 20, ssf); }
                    else        { e_shv(S_LSRV, 19, dst, 17, ssf); e_shv(S_LSLV, 20, src, 20, ssf); }
                    e_rrr(A_ORR, 16, 19, 20, ssf, 0);                  // combined = t1 | t2
                    e_tst(17, ssf); e_csel(16, 22, 16, 0 /*EQ: n==0*/, ssf);          // n==0 -> dst unchanged
                }
                e_tst(16, ssf); e_nzcv_save();                         // SF/ZF from result (CF/OF approximate)
                rm_store(&I, w, 16); gpc = next; continue;
            }
            // imul reg, r/m (0F AF)
            if (op == 0xAF) { int mem; int rmv = rm_load(&I, next, I.opsize, &mem);
                e_mul(I.reg, I.reg, rmv, sf); gpc = next; continue; }
            // bswap (0F C8+r): byte-reverse a register -> ARM REV
            if (op >= 0xC8 && op <= 0xCF) { int r = (op - 0xC8) | (I.rexB << 3);
                emit32((sf ? 0xDAC00C00u : 0x5AC00800u) | (r << 5) | r); gpc = next; continue; }
            // 0F AE: fences (lfence/mfence/sfence -> dmb), ldmxcsr/stmxcsr, fxsave/fxrstor (xmm area)
            if (op == 0xAE) {
                int sub = I.reg & 7;
                if (sub >= 5) { emit32(0xD5033BBFu); gpc = next; continue; }   // *fence -> dmb ish
                if (sub == 2) { gpc = next; continue; }                        // ldmxcsr: ignore (no SSE rounding/excepts)
                if (sub == 3) { if (I.is_mem) { emit_ea(&I, next); e_movconst(16, 0x1f80); e_store(4, 16, 17); } gpc = next; continue; } // stmxcsr
                if ((sub == 0 || sub == 1) && I.is_mem) {                      // fxsave / fxrstor: XMM0-15 @+160, MXCSR @+24
                    emit_ea(&I, next);
                    for (int i = 0; i < 16; i++) { if (sub == 0) e_str_q(i, 17, 160 + i * 16); else e_ldr_q(i, 17, 160 + i * 16); }
                    gpc = next; continue;   // (x87/MMX + MXCSR areas left as-is; we don't honor them)
                }
            }
            // bsf/tzcnt (0F BC), bsr/lzcnt (0F BD): bit scan -> RBIT+CLZ / CLZ
            if (op == 0xBC || op == 0xBD) {
                int mem; int rmv = rm_load(&I, next, I.opsize, &mem);
                if (op == 0xBC) { e_rbit(I.reg, rmv, sf); e_clz(I.reg, I.reg, sf); }     // bsf = ctz
                else { e_clz(16, rmv, sf); e_movconst(19, sf ? 63 : 31); e_rrr(A_SUB, I.reg, 19, 16, sf, 0); } // bsr = (w-1)-clz
                e_rrr(A_ANDS, 31, rmv, rmv, sf, 0); e_nzcv_save();   // ZF = (src == 0)
                gpc = next; continue;
            }
            // bit ops: BT(A3) BTS(AB) BTR(B3) BTC(BB), and group BA /4..7 with imm8.
            if (op == 0xA3 || op == 0xAB || op == 0xB3 || op == 0xBB || op == 0xBA) {
                int isimm = (op == 0xBA);
                int sub = isimm ? (I.reg & 7) : (op == 0xA3 ? 4 : op == 0xAB ? 5 : op == 0xB3 ? 6 : 7);
                if (sub < 4) { report_unimpl(gpc, &I); break; }
                int w = I.opsize, mem, bits = w * 8;
                int val = rm_load(&I, next, w, &mem);
                if (isimm) e_movconst(19, (uint64_t)(((uint64_t)I.imm) & (bits - 1)));   // idx -> x19
                else { e_movconst(21, bits - 1); e_rrr(A_AND, 19, I.reg, 21, sf, 0); }
                e_shv(S_LSRV, 21, val, 19, sf); e_movconst(22, 1); e_rrr(A_AND, 21, 21, 22, sf, 0); // x21 = bit
                e_rrr(A_SUBS, 31, 31, 21, 1, 0); e_nzcv_save();      // ARM C = !bit  (subs convention -> x86 CF)
                if (sub != 4) {                                       // BTS/BTR/BTC: modify the bit + write back
                    int o = mem ? 16 : I.rm_reg;
                    e_movconst(22, 1); e_shv(S_LSLV, 22, 22, 19, sf); // mask = 1<<idx
                    if (sub == 5) e_rrr(A_ORR, o, val, 22, sf, 0);    // BTS
                    else if (sub == 6) e_rrr(A_BIC, o, val, 22, sf, 0); // BTR
                    else e_rrr(A_EOR, o, val, 22, sf, 0);             // BTC
                    rm_store(&I, w, o);
                }
                gpc = next; continue;
            }
            // cmpxchg (0F B0 byte / B1): compare RAX with r/m; if eq, r/m=reg, ZF=1; else RAX=r/m.
            if (op == 0xB0 || op == 0xB1) {
                int w = op == 0xB0 ? 1 : I.opsize, sf2 = (w == 8);
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_mov_rr(19, RAX, sf2);              // expected
                    e_cas(w, 19, I.reg, 17);            // x19 = old; if old==expected [m]=reg
                    do_alu(7, -1, 19, RAX, w);          // ZF = (old == rax)
                    if (w >= 4) e_mov_rr(RAX, 19, sf2); else e_bfi(RAX, 19, 0, 8 * w, 1);  // rax = old
                } else if (w >= 4) {
                    e_mov_rr(19, I.rm_reg, sf2);
                    do_alu(7, -1, 19, RAX, w);
                    e_csel(I.rm_reg, I.reg, 19, 0, sf2);  // rm = ZF? reg : rm_old
                    e_csel(RAX, RAX, 19, 0, sf2);         // rax = ZF? rax : rm_old
                } else { report_unimpl(gpc, &I); break; }
                gpc = next; continue;
            }
            // xadd (0F C0 byte / C1): tmp=r/m; r/m += reg; reg = tmp (+ flags)
            if (op == 0xC0 || op == 0xC1) {
                int w = op == 0xC0 ? 1 : I.opsize, sf2 = (w == 8);
                if (I.is_mem) {
                    emit_ea(&I, next);
                    e_lse(LSE_LDADD, w, I.reg, 19, 17);  // x19 = old; [m] += reg
                    do_alu(0, -1, 19, I.reg, w);         // flags from old+reg
                    if (w >= 4) e_mov_rr(I.reg, 19, sf2); else e_bfi(I.reg, 19, 0, 8 * w, 1); // reg = old
                } else if (w >= 4) {
                    e_mov_rr(19, I.rm_reg, sf2);          // old
                    e_rrr(A_ADDS, I.rm_reg, I.rm_reg, I.reg, sf2, 0); e_nzcv_save_ci();  // rm += reg (x86 add carry)
                    e_mov_rr(I.reg, 19, sf2);             // reg = old
                } else { report_unimpl(gpc, &I); break; }
                gpc = next; continue;
            }
            // jcc rel32 (0F 80-8F)
            if ((op & 0xF0) == 0x80) {
                int cc = x86cc_to_arm(op & 0xF); if (cc < 0) { report_unimpl(gpc, &I); break; }
                uint64_t taken = next + (uint64_t)I.imm;
                e_nzcv_load();
                uint32_t *patch = (uint32_t *)g_cp; emit32(0);
                emit_chain_exit(next);
                int64_t d = ((uint8_t *)g_cp - (uint8_t *)patch) / 4;
                *patch = 0x54000000u | (((uint32_t)d & 0x7FFFF) << 5) | (cc & 0xF);
                emit_chain_exit(taken); break;
            }
            // setcc (0F 90-9F) -> r/m8 (byte: preserve upper bits / hi-lo byte regs)
            if ((op & 0xF0) == 0x90) {
                int cc = x86cc_to_arm(op & 0xF); if (cc < 0) { report_unimpl(gpc, &I); break; }
                if (I.is_mem) { emit_ea(&I, next);               // EA -> x17 FIRST (emit_ea may clobber x16)
                                e_nzcv_load(); e_cset(16, cc, 0); e_store(1, 16, 17); }
                else { e_nzcv_load(); e_cset(16, cc, 0); byte_wb(&I, I.rm_reg, 16); }
                gpc = next; continue;
            }
            // cmovcc (0F 40-4F), reg or mem source
            if ((op & 0xF0) == 0x40) {
                int cc = x86cc_to_arm(op & 0xF); if (cc < 0) { report_unimpl(gpc, &I); break; }
                int mem; int rmv = rm_load(&I, next, I.opsize, &mem);
                e_nzcv_load(); e_csel(I.reg, rmv, I.reg, cc, sf); gpc = next; continue;
            }
            // movzx/movsx (0F B6/B7 zero, BE/BF sign), movsxd handled as 0x63 one-byte (TODO)
            if (op == 0xB6 || op == 0xB7 || op == 0xBE || op == 0xBF) {
                int w = (op & 1) ? 2 : 1;            // B6/BE byte, B7/BF word
                int signd = (op >= 0xBE);
                if (I.is_mem) { emit_ea(&I, next); if (signd) e_ldrs(w, I.reg, 17); else e_load(w, I.reg, 17); }
                else { int src = (w == 1) ? byte_val(&I, I.rm_reg, 16) : I.rm_reg;   // byte source: ah/bh/ch/dh -> bits 8-15
                       if (signd) e_sxt(I.reg, src, w); else e_uxt(I.reg, src, w); }
                gpc = next; continue;
            }
        }
        report_unimpl(gpc, &I); break;
    }
    map_put(start, host, body);
    patch_links_to(start, body);
    return host;
}

static void report_unimpl(uint64_t pc, struct insn *I) {
    const uint8_t *p = (const uint8_t *)pc;
    fprintf(stderr, "[jit86] UNIMPL %s opcode 0x%02x at rip=%llx  bytes:",
            I->two ? "0F" : "1B", I->op, (unsigned long long)pc);
    for (int i = 0; i < (I->len ? I->len : 8); i++) fprintf(stderr, " %02x", p[i]);
    fprintf(stderr, "\n");
    // emit a clean exit that terminates the guest (so we don't run off into garbage).
    emit_spill();
    e_movconst(16, 0xDEAD0000u | I->op); e_str(16, 28, OFF_RIP);
    e_movconst(16, 99); e_str(16, 28, OFF_RSN);   // reason 99 -> dispatcher aborts
    e_movconst(16, (uint64_t)block_return); e_br(16);
}

// ---------------- host entry trampolines (adapted from jit.c, x86 reg set) ----------------
__attribute__((naked)) static void run_block(struct cpu *cpu, void *code) {
    __asm__ volatile(                          // x0=cpu, x1=code
        "str x19,[x0,#176]\n str x20,[x0,#184]\n str x21,[x0,#192]\n str x22,[x0,#200]\n"
        "str x23,[x0,#208]\n str x24,[x0,#216]\n str x25,[x0,#224]\n str x26,[x0,#232]\n"
        "str x27,[x0,#240]\n str x28,[x0,#248]\n str x29,[x0,#256]\n str x30,[x0,#264]\n"
        "str q8,[x0,#272]\n str q9,[x0,#288]\n str q10,[x0,#304]\n str q11,[x0,#320]\n"
        "str q12,[x0,#336]\n str q13,[x0,#352]\n str q14,[x0,#368]\n str q15,[x0,#384]\n"
        "mov x9, sp\n str x9,[x0,#168]\n"      // host_sp
        "br x1\n");                            // -> emitted prologue (sets x28=cpu)
}
__attribute__((naked)) static void block_return(void) {
    __asm__ volatile(                          // x28 == &cpu (pinned through the block)
        "ldr x19,[x28,#176]\n ldr x20,[x28,#184]\n ldr x21,[x28,#192]\n ldr x22,[x28,#200]\n"
        "ldr x23,[x28,#208]\n ldr x24,[x28,#216]\n ldr x25,[x28,#224]\n ldr x26,[x28,#232]\n"
        "ldr x27,[x28,#240]\n ldr x29,[x28,#256]\n ldr x30,[x28,#264]\n"
        "ldr q8,[x28,#272]\n ldr q9,[x28,#288]\n ldr q10,[x28,#304]\n ldr q11,[x28,#320]\n"
        "ldr q12,[x28,#336]\n ldr q13,[x28,#352]\n ldr q14,[x28,#368]\n ldr q15,[x28,#384]\n"
        "ldr x9,[x28,#168]\n mov sp, x9\n"     // host sp
        "ldr x28,[x28,#248]\n"                 // restore host x28 LAST (was using it as base)
        "ret\n");
}

// ---------------- rootfs path rewriting (ported from jit.c) ----------------
static const char *g_rootfs = NULL;
static char g_cwd[4096] = "/";       // guest cwd (container model). host cwd is kept at xlate(g_cwd).
static unsigned g_hostuid, g_hostgid;            // real host ids -> mapped to container root in stat (userns model)
static unsigned map_uid(unsigned u) { return u == g_hostuid ? 0 : u; }
static unsigned map_gid(unsigned g) { return g == g_hostgid ? 0 : g; }
static const char *xlate(const char *p, char *buf, size_t n);   // fwd: guest path -> host (rootfs-jailed)
int g_ngroups; int g_groups[64];                 // container root's supplementary groups (from rootfs /etc/group)
void build_root_groups(void) {
    static int done; if (done) return; done = 1;
    g_groups[g_ngroups++] = 0;                    // primary gid 0 (root)
    char path[4200]; snprintf(path, sizeof path, "%s/etc/group", g_rootfs ? g_rootfs : "");
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f) && g_ngroups < 64) {     // name:passwd:gid:member,member,...
        char *save, *name = strtok_r(line, ":", &save); (void)strtok_r(NULL, ":", &save);
        char *gids = strtok_r(NULL, ":", &save), *members = strtok_r(NULL, "\n", &save);
        (void)name; if (!gids) continue; int gid = atoi(gids); if (gid == 0) continue;
        int is = 0; if (members) { char *s2; for (char *m = strtok_r(members, ",", &s2); m; m = strtok_r(NULL, ",", &s2)) if (strcmp(m, "root") == 0) { is = 1; break; } }
        if (is && g_ngroups < 64) g_groups[g_ngroups++] = gid;
    }
    fclose(f);
}

// Darwin errno -> Linux errno (values diverge from 35 up; <35 are identical). Used for host
// syscall failures so the Linux guest sees the right code (EAGAIN, EINPROGRESS, ECONNREFUSED...).
static int derr(int e) {
    switch (e) {
        case 35: return 11; case 36: return 115; case 37: return 114; case 38: return 88; case 39: return 89;
        case 40: return 90; case 41: return 91; case 42: return 92; case 43: return 93; case 44: return 94;
        case 45: return 95; case 46: return 96; case 47: return 97; case 48: return 98; case 49: return 99;
        case 50: return 100; case 51: return 101; case 52: return 102; case 53: return 103; case 54: return 104;
        case 55: return 105; case 56: return 106; case 57: return 107; case 58: return 108; case 59: return 109;
        case 60: return 110; case 61: return 111; case 62: return 40; case 63: return 36; case 64: return 112;
        case 65: return 113; case 66: return 39; case 68: return 87; case 69: return 122; case 70: return 116;
        case 78: return 38; case 89: return 125; case 92: return 61; case 93: return 62; case 94: return 67;
        default: return e;
    }
}
// ---- Linux<->Darwin socket translation (jit86 runs on macOS; the guest speaks Linux ABI) ----
// SOL_SOCKET option numbers differ; map the common ones (level 1 == Linux SOL_SOCKET -> Darwin 0xffff).
static int so_l2d(int opt) {
    switch (opt) { case 2: return 0x0004; case 9: return 0x0008; case 5: return 0x0010; case 6: return 0x0020;
        case 13: return 0x0080; case 10: return 0x0100; case 15: return 0x0200; case 7: return 0x1001;
        case 8: return 0x1002; case 21: return 0x1005; case 20: return 0x1006; case 4: return 0x1007;
        case 3: return 0x1008; default: return opt; }
}
static int socktype_l2d(int t) { return t & 0xff; }   // Linux SOCK_NONBLOCK(0x800)/CLOEXEC(0x80000) live in the type field; Darwin sets them via fcntl
// guest (Linux) sockaddr -> host (Darwin); returns host len (0 = unknown family)
static socklen_t l2d_sa(const void *lin, socklen_t llen, struct sockaddr_storage *d) {
    memset(d, 0, sizeof *d);
    if (!lin || llen < 2) return 0;
    int fam = *(const uint16_t *)lin;
    if (fam == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)d; s->sin_len = sizeof *s; s->sin_family = AF_INET;
        memcpy(&s->sin_port, (const char *)lin + 2, 2); memcpy(&s->sin_addr, (const char *)lin + 4, 4); return sizeof *s;
    } else if (fam == 10) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)d; s->sin6_len = sizeof *s; s->sin6_family = AF_INET6;
        memcpy(&s->sin6_port, (const char *)lin + 2, 2); memcpy(&s->sin6_flowinfo, (const char *)lin + 4, 4);
        memcpy(&s->sin6_addr, (const char *)lin + 8, 16); memcpy(&s->sin6_scope_id, (const char *)lin + 24, 4); return sizeof *s;
    } else if (fam == AF_UNIX) {
        struct sockaddr_un *s = (struct sockaddr_un *)d; s->sun_family = AF_UNIX;
        const char *p = (const char *)lin + 2;
        if (p[0] == 0) { socklen_t n = llen - 2; if (n > (socklen_t)sizeof s->sun_path) n = sizeof s->sun_path; memcpy(s->sun_path, p, n); s->sun_len = (uint8_t)llen; return llen; }  // abstract socket
        char pb[4200]; const char *hp = xlate(p, pb, sizeof pb);
        snprintf(s->sun_path, sizeof s->sun_path, "%s", hp); s->sun_len = sizeof *s; return (socklen_t)(2 + strlen(s->sun_path) + 1 + sizeof(struct sockaddr_un) - sizeof s->sun_path - 2);
    }
    return 0;
}
// host (Darwin) sockaddr -> guest (Linux) buffer; updates *glen to the full (untruncated) length
static void d2l_sa(const struct sockaddr *d, void *gout, socklen_t cap, socklen_t *glen) {
    char buf[128]; memset(buf, 0, sizeof buf); socklen_t full = 0;
    if (!d) { if (glen) *glen = 0; return; }
    if (d->sa_family == AF_INET) { struct sockaddr_in *s = (struct sockaddr_in *)d;
        *(uint16_t *)buf = AF_INET; memcpy(buf + 2, &s->sin_port, 2); memcpy(buf + 4, &s->sin_addr, 4); full = 16;
    } else if (d->sa_family == AF_INET6) { struct sockaddr_in6 *s = (struct sockaddr_in6 *)d;
        *(uint16_t *)buf = 10; memcpy(buf + 2, &s->sin6_port, 2); memcpy(buf + 4, &s->sin6_flowinfo, 4);
        memcpy(buf + 8, &s->sin6_addr, 16); memcpy(buf + 24, &s->sin6_scope_id, 4); full = 28;
    } else if (d->sa_family == AF_UNIX) { struct sockaddr_un *s = (struct sockaddr_un *)d;
        *(uint16_t *)buf = AF_UNIX; snprintf(buf + 2, sizeof buf - 2, "%s", s->sun_path); full = (socklen_t)(2 + strlen(s->sun_path) + 1);
    } else { *(uint16_t *)buf = d->sa_family; full = 2; }
    if (gout && cap) memcpy(gout, buf, cap < full ? cap : full);
    if (glen) *glen = full;
}

// ---- private loopback: route the container's 127.0.0.0/8 TCP to AF_UNIX sockets under a per-
// container dir (g_netns). Isolates each container's localhost AND sidesteps the macOS-host
// loopback quirk (a forked child can't connect to the parent's 127.x TCP port on the bridge).
static char g_netns[200];                  // per-container host dir for loopback unix sockets ("" = off)
static uint16_t g_lo_port[1024];           // fd -> loopback port it's bound/connected to (0 = not private-lo)
static uint8_t g_sock_stream[1024];        // fd -> 1 if AF_INET SOCK_STREAM (only those get loopback isolation)
static int g_eventfd_peer[1024];           // eventfd(read-end) -> pipe write-end + 1 (0 = not an eventfd)
static uint8_t g_timerfd[1024];            // fd is a timerfd (kqueue + EVFILT_TIMER) -> read() drains it
static uint8_t g_inotify[1024];            // fd is an inotify (kqueue + EVFILT_VNODE watches) -> read() drains it
// ---- port-map (docker -p H:C): bind(:C) actually binds host :H; getsockname reports :C back ----
static struct { uint16_t cport, hport; } g_portmap[32];
static int g_nportmap = 0;
static uint16_t g_fd_cport[1024];          // fd -> the container port it bound (for getsockname)
static uint16_t pm_host(uint16_t cp) { for (int i = 0; i < g_nportmap; i++) if (g_portmap[i].cport == cp) return g_portmap[i].hport; return cp; }
static void parse_publish(const char *s) {  // "H:C,H:C,..." (docker -p order: host:container)
    while (s && *s && g_nportmap < 32) {
        int h = atoi(s); const char *colon = strchr(s, ':'); if (!colon) break;
        int cc = atoi(colon + 1); if (h > 0 && cc > 0) { g_portmap[g_nportmap].cport = (uint16_t)cc; g_portmap[g_nportmap].hport = (uint16_t)h; g_nportmap++; }
        const char *comma = strchr(s, ','); if (!comma) break; s = comma + 1;
    }
}
// ---- signalfd: a self-pipe poked by a host signal handler (guest reads signalfd_siginfo) ----
// Linux x86-64 signo == Linux aarch64 signo (generic), but macOS differs -> translate at the boundary.
static int g_sigfd_pipe[2] = {-1, -1};     // signalfd self-pipe (write end poked from host_sigh)
static int g_sigfd_read = -1;              // its read end (the guest's signalfd)
static volatile uint64_t g_sigfd_mask;     // signals routed to the signalfd (1<<signo)
static volatile uint64_t g_pending;        // pending-signal bitmask (1<<signo), set by host_sigh
static int sig_is_sync(int s) { return s==4||s==5||s==7||s==8||s==11; }  // ILL TRAP BUS FPE SEGV (Linux nums)
static int sig_l2m(int s) { static const unsigned char T[32] = {0,1,2,3,4,5,6,10,8,9,30,11,31,13,14,15,16,20,19,17,18,21,22,16,24,25,26,27,28,23,30,12}; return (s>=1&&s<=31)?T[s]:s; }
static int sig_m2l(int s) { static const unsigned char T[32] = {0,1,2,3,4,5,6,7,8,9,7,11,31,13,14,15,23,19,20,18,17,21,22,29,24,25,26,27,28,29,10,12}; return (s>=1&&s<=31)?T[s]:s; }
static void host_sigh(int sig) {
    int ls = sig_m2l(sig);                                                             // host(macOS) signo -> Linux
    __atomic_or_fetch(&g_pending, 1ull << ls, __ATOMIC_SEQ_CST);
    if ((g_sigfd_mask & (1ull << ls)) && g_sigfd_pipe[1] >= 0) { char b = (char)ls; if (write(g_sigfd_pipe[1], &b, 1) < 0) {} }  // wake signalfd/epoll
}
// ---- guest signal delivery: build a Linux x86-64 rt_sigframe and redirect to the handler ----
static struct { uint64_t handler, flags, mask; } g_sigact[65];   // per-signal disposition (mask in sigset_t convention)
#define SIGRETURN_PC 0xFFFFFFFFFFF0ull               // sentinel return address: handler ret -> sigreturn
// x86-64 sigcontext gregs index -> guest cpu->r[] index (r8..r15,rdi,rsi,rbp,rbx,rdx,rax,rcx,rsp; then rip,eflags)
static const int GREG2R[16] = {8,9,10,11,12,13,14,15, 7,6,5,3,2,0,1,4};   // gregs[0..15]
static uint64_t nzcv_to_eflags(uint64_t nz) {
    uint64_t f = 0x2;                                                      // bit1 reserved (always 1)
    if (!((nz >> 29) & 1)) f |= 1u << 0;   if ((nz >> 30) & 1) f |= 1u << 6;   // CF (stored inverted), ZF
    if ((nz >> 31) & 1) f |= 1u << 7;      if ((nz >> 28) & 1) f |= 1u << 11;  // SF, OF
    return f;
}
static uint64_t eflags_to_nzcv(uint64_t f) {
    uint64_t nz = 0;
    if (!(f & 1)) nz |= 1u << 29;          if (f & (1u << 6)) nz |= 1u << 30;  // CF (invert), ZF
    if (f & (1u << 7)) nz |= 1u << 31;     if (f & (1u << 11)) nz |= 1u << 28; // SF, OF
    return nz;
}
static void build_signal_frame(struct cpu *c, int sig) {
    uint64_t sp = (c->r[4] - 2048) & ~15ull;                               // 16-aligned frame base; uc lives here
    uint64_t uc = sp, mc = uc + 40, info = uc + 512, xs = uc + 768;        // ucontext / mcontext(gregs) / siginfo / xmm save
    memset((void *)sp, 0, 2048);
    for (int i = 0; i < 16; i++) *(uint64_t *)(mc + i * 8) = c->r[GREG2R[i]];   // gregs[0..15]
    *(uint64_t *)(mc + 16 * 8) = c->rip;                                   // gregs[16] = RIP
    *(uint64_t *)(mc + 17 * 8) = nzcv_to_eflags(c->nzcv);                  // gregs[17] = EFL
    *(uint64_t *)(uc + 296) = c->sigmask;                                  // uc_sigmask (restored on sigreturn)
    memcpy((void *)xs, c->v, sizeof c->v);                                 // preserve guest xmm across the handler
    *(int *)(info + 0) = sig;                                              // siginfo.si_signo
    uint64_t rsp = sp - 8; *(uint64_t *)rsp = SIGRETURN_PC;                // pushed return address
    c->r[7] = (uint64_t)sig; c->r[6] = info; c->r[2] = uc;                 // handler(signo, siginfo*, ucontext*) in rdi,rsi,rdx
    c->r[4] = rsp; c->rip = g_sigact[sig].handler;
    c->sigmask |= g_sigact[sig].mask;
    if (!(g_sigact[sig].flags & 0x40000000)) c->sigmask |= (1ull << (sig - 1));  // SA_NODEFER off -> block this signal
    if (g_trace) fprintf(stderr, "[sig] deliver %d handler=%llx rsp=%llx\n", sig, (unsigned long long)c->rip, (unsigned long long)rsp);
}
static void do_sigreturn(struct cpu *c) {
    uint64_t uc = c->r[4], mc = uc + 40, xs = uc + 768;                    // after the handler's ret, rsp == uc
    for (int i = 0; i < 16; i++) c->r[GREG2R[i]] = *(uint64_t *)(mc + i * 8);
    c->rip  = *(uint64_t *)(mc + 16 * 8);
    c->nzcv = eflags_to_nzcv(*(uint64_t *)(mc + 17 * 8));
    c->sigmask = *(uint64_t *)(uc + 296);
    memcpy(c->v, (void *)xs, sizeof c->v);
}
static void maybe_deliver_signal(struct cpu *c) {
    uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
    for (int sig = 1; sig <= 64; sig++) {
        uint64_t bit = 1ull << sig;
        if (!(p & bit) || (c->sigmask & (1ull << (sig - 1)))) continue;    // pending and not blocked
        uint64_t h = g_sigact[sig].handler;
        if (h <= 1) { __atomic_and_fetch(&g_pending, ~bit, __ATOMIC_SEQ_CST); continue; }   // SIG_DFL/IGN: host already acted
        if (__atomic_fetch_and(&g_pending, ~bit, __ATOMIC_SEQ_CST) & bit) { build_signal_frame(c, sig); return; }
    }
}
// A signal aimed at our own process (raise/abort/kill self): deliver through our machinery
// (host signals into a MAP_JIT thread are fragile) -- custom handler -> pending; else default action.
static void raise_guest_signal(struct cpu *c, int sig) {
    if (sig < 1 || sig > 64) return;
    uint64_t h = g_sigact[sig].handler;
    if (h > 1) { __atomic_or_fetch(&g_pending, 1ull << sig, __ATOMIC_SEQ_CST); return; }   // custom handler
    if (h == 1) return;                                                                    // SIG_IGN
    if (c && (c->sigmask & (1ull << (sig - 1)))) {                                          // blocked -> pending (signalfd/unblock)
        __atomic_or_fetch(&g_pending, 1ull << sig, __ATOMIC_SEQ_CST);
        if ((g_sigfd_mask & (1ull << sig)) && g_sigfd_pipe[1] >= 0) { char b = (char)sig; if (write(g_sigfd_pipe[1], &b, 1) < 0) {} }
        return;
    }
    if (sig == 17 || sig == 18 || sig == 23 || sig == 28) return;                          // SIGCHLD/CONT/URG/WINCH: ignore
    signal(sig_l2m(sig), SIG_DFL); raise(sig_l2m(sig));                                     // default: die by the signal (host signo)
    c->exited = 1; c->exit_code = 128 + sig;                                               // fallback
}
static int lo_on(void) { return g_netns[0] != 0; }
static int lo_is(const uint8_t *sa, socklen_t l) { return sa && l >= 8 && *(uint16_t *)sa == AF_INET && sa[4] == 127; }
static void lo_path(uint16_t port, char *out, size_t n) { snprintf(out, n, "%s/p%u", g_netns, (unsigned)port); }
static int lo_swap(int fd) {               // replace the AF_INET socket at fd with a fresh AF_UNIX one
    int fl = fcntl(fd, F_GETFL), df = fcntl(fd, F_GETFD);
    int u = socket(AF_UNIX, SOCK_STREAM, 0); if (u < 0) return -1;
    if (u != fd) { if (dup2(u, fd) < 0) { close(u); return -1; } close(u); }
    if (fl >= 0 && (fl & O_NONBLOCK)) fcntl(fd, F_SETFL, O_NONBLOCK);
    if (df >= 0 && (df & FD_CLOEXEC)) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}
static void fill_inet_lo(uint8_t *sa, socklen_t *l, uint16_t port) {   // report 127.0.0.1:port to the guest
    if (!sa) return;
    *(uint16_t *)(sa + 0) = AF_INET; *(uint16_t *)(sa + 2) = htons(port);
    *(uint32_t *)(sa + 4) = 0x0100007fu; memset(sa + 8, 0, 8); if (l) *l = 16;
}
// Bind-mount volumes: a guest path prefix -> a host directory (docker -v). Set via JIT86_VOL.
struct vol { char guest[256]; size_t glen; char host[1024]; };
static struct vol g_vols[32]; static int g_nvols;
// Host root for an absolute guest path: a matching volume (longest prefix), else the rootfs.
// *rel receives the path within that root (so root+rel = host path).
static const char *vol_root(const char *abs, const char **rel) {
    int best = -1; size_t bl = 0;
    for (int i = 0; i < g_nvols; i++)
        if (!strncmp(abs, g_vols[i].guest, g_vols[i].glen) && (abs[g_vols[i].glen] == '/' || abs[g_vols[i].glen] == 0)
            && g_vols[i].glen >= bl) { best = i; bl = g_vols[i].glen; }
    if (best >= 0) { *rel = abs[g_vols[best].glen] ? abs + g_vols[best].glen : "/"; return g_vols[best].host; }
    *rel = abs; return g_rootfs;
}
static void add_vol(const char *spec) {            // "guestpath:hostdir"
    if (!spec || g_nvols >= 32) return;
    char t[2048]; snprintf(t, sizeof t, "%s", spec);
    char *colon = strrchr(t, ':'); if (!colon) return; *colon = 0;
    char *gp = t, *hp = colon + 1; if (gp[0] != '/') return;
    char hc[1024]; if (!realpath(hp, hc)) snprintf(hc, sizeof hc, "%s", hp);
    snprintf(g_vols[g_nvols].guest, sizeof g_vols[g_nvols].guest, "%s", gp);
    size_t gl = strlen(g_vols[g_nvols].guest); while (gl > 1 && g_vols[g_nvols].guest[gl - 1] == '/') g_vols[g_nvols].guest[--gl] = 0;
    g_vols[g_nvols].glen = gl; snprintf(g_vols[g_nvols].host, sizeof g_vols[g_nvols].host, "%s", hc); g_nvols++;
}
// ---- Overlay (OCI image layers): --rootfs is the writable UPPER; --lower dirs are read-only,
// searched top->down when a path isn't in the upper. A .wh.NAME whiteout hides a lower entry;
// copy-up brings a lower file into the upper on write. Off entirely when g_nlower==0. ----
struct olayer { char root[1024]; size_t rlen; };
static struct olayer g_lower[8]; static int g_nlower = 0;     // [0] = highest-priority lower (searched first)
static char g_ovldir[1024][256];                              // dir-fd -> its GUEST path (for merged getdents); "" = not overlay
static int g_ovlcur[1024];                                    // dir-fd -> merged-listing cursor (entries already emitted)
static void add_lower(const char *dir) {
    if (g_nlower >= 8 || !dir || !dir[0]) return;
    char rc[1024]; if (!realpath(dir, rc)) snprintf(rc, sizeof rc, "%s", dir);
    snprintf(g_lower[g_nlower].root, sizeof g_lower[g_nlower].root, "%s", rc);
    g_lower[g_nlower].rlen = strlen(rc); g_nlower++;
}
// One layer's host path for an absolute guest path, following symlinks LAYER-relative (like xresolve).
static void layer_path(const char *root, const char *guest, char *buf, size_t n, int follow) {
    char cpath[1024]; snprintf(cpath, sizeof cpath, "%s", guest);
    if (follow) for (int i = 0; i < 40; i++) {
        char h[4200]; snprintf(h, sizeof h, "%s%s", root, cpath);
        char lk[1024]; ssize_t k = readlink(h, lk, sizeof lk - 1); if (k < 0) break; lk[k] = 0;
        if (lk[0] == '/') snprintf(cpath, sizeof cpath, "%s", lk);
        else { char *sl = strrchr(cpath, '/'); int d = sl ? (int)(sl - cpath) : 0;
               char tmp[1024]; snprintf(tmp, sizeof tmp, "%.*s/%s", d, cpath, lk); snprintf(cpath, sizeof cpath, "%s", tmp); }
    }
    snprintf(buf, n, "%s%s", root, cpath);
}
static void wh_path(const char *root, const char *guest, char *buf, size_t n) {   // host path of the .wh.NAME marker
    char par[1024]; snprintf(par, sizeof par, "%s", guest); char *sl = strrchr(par, '/');
    char base[256]; snprintf(base, sizeof base, "%s", sl ? sl + 1 : par); if (sl) *sl = 0;
    snprintf(buf, n, "%s%s/.wh.%s", root, par, base);
}
static int wh_exists(const char *root, const char *guest) { char h[4300]; wh_path(root, guest, h, sizeof h); struct stat st; return lstat(h, &st) == 0; }
// READ resolve: topmost layer that has `guest`. Returns 1 + host on hit; 0 (+ upper path) if absent/whiteout-hidden.
static int overlay_resolve(const char *guest, char *host, size_t hn, int nofollow) {
    char up[4300]; layer_path(g_rootfs, guest, up, sizeof up, !nofollow); struct stat st;
    if (lstat(up, &st) == 0) { snprintf(host, hn, "%s", up); return 1; }                  // upper shadows lowers
    if (wh_exists(g_rootfs, guest)) { snprintf(host, hn, "%s", up); return 0; }            // deleted in upper
    for (int i = 0; i < g_nlower; i++) {
        char lp[4300]; layer_path(g_lower[i].root, guest, lp, sizeof lp, !nofollow);
        if (lstat(lp, &st) == 0) { snprintf(host, hn, "%s", lp); return 1; }
        if (wh_exists(g_lower[i].root, guest)) { snprintf(host, hn, "%s", up); return 0; }
    }
    snprintf(host, hn, "%s", up); return 0;                                               // absent -> upper path (ENOENT / O_CREAT)
}
// Copy-up: bring a lower file into the UPPER so it can be modified; returns the upper host path.
static void overlay_copyup(const char *guest, char *host, size_t hn) {
    layer_path(g_rootfs, guest, host, hn, 1); struct stat st;
    if (lstat(host, &st) == 0) return;                                                    // already writable in upper
    if (wh_exists(g_rootfs, guest)) { char wh[4300]; wh_path(g_rootfs, guest, wh, sizeof wh); unlink(wh); return; }  // recreate
    char src[4300]; int have = 0;
    for (int i = 0; i < g_nlower && !have; i++) { layer_path(g_lower[i].root, guest, src, sizeof src, 1);
        if (lstat(src, &st) == 0 && S_ISREG(st.st_mode)) have = 1;
        else if (wh_exists(g_lower[i].root, guest)) break; }
    if (!have) return;                                                                    // new file -> upper path as-is
    char dir[4300]; snprintf(dir, sizeof dir, "%s", host); char *sl = strrchr(dir, '/'); size_t rl = strlen(g_rootfs);
    if (sl) { *sl = 0; for (char *q = dir + rl + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; } mkdir(dir, 0755); }
    int in = open(src, O_RDONLY), out = open(host, O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 0777);
    if (in >= 0 && out >= 0) { char b[1 << 16]; ssize_t r; while ((r = read(in, b, sizeof b)) > 0) if (write(out, b, r) < 0) break; }
    if (in >= 0) close(in); if (out >= 0) close(out);
}
static int overlay_readdir(const char *gdir, char names[][256], uint8_t *types, int max) {  // merged listing (upper first)
    static char seen[2048][256]; int ns = 0, nout = 0;
    for (int L = -1; L < g_nlower && nout < max; L++) {
        const char *root = (L < 0) ? g_rootfs : g_lower[L].root;
        char host[4300]; layer_path(root, gdir, host, sizeof host, 1);
        DIR *d = opendir(host); if (!d) continue; struct dirent *e;
        while ((e = readdir(d)) && nout < max) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            int wh = !strncmp(e->d_name, ".wh.", 4); const char *name = wh ? e->d_name + 4 : e->d_name;
            int dup = 0; for (int i = 0; i < ns; i++) if (!strcmp(seen[i], name)) { dup = 1; break; }
            if (dup) continue; if (ns < 2048) snprintf(seen[ns++], 256, "%s", name);       // higher layer already decided this name
            if (!wh) { snprintf(names[nout], 256, "%s", name); types[nout] = e->d_type; nout++; }  // whiteout -> hide
        }
        closedir(d);
    }
    return nout;
}
static void overlay_whiteout(const char *guest) {                                          // delete: drop upper copy + .wh marker
    char up[4300]; layer_path(g_rootfs, guest, up, sizeof up, 1); remove(up);
    char wh[4300]; wh_path(g_rootfs, guest, wh, sizeof wh);
    char dir[4300]; snprintf(dir, sizeof dir, "%s", wh); char *s2 = strrchr(dir, '/'); size_t rl = strlen(g_rootfs);
    if (s2) { *s2 = 0; for (char *q = dir + rl + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(dir, 0755); *q = '/'; } mkdir(dir, 0755); }
    int fd = open(wh, O_CREAT | O_WRONLY, 0644); if (fd >= 0) close(fd);
}
static const char *xlate(const char *p, char *buf, size_t n) {
    if (p && p[0] == '/') { const char *rel; const char *root = vol_root(p, &rel);
                            if (root == g_rootfs && g_nlower) { overlay_resolve(p, buf, n, 1); return buf; }  // overlay (nofollow)
                            if (root) { snprintf(buf, n, "%s%s", root, rel); return buf; } }
    return p;
}
static const char *xresolve(const char *p, char *buf, size_t n) {
    if (!p || p[0] != '/') return p;
    const char *rel0; const char *root = vol_root(p, &rel0);
    if (!root) return p;
    if (root == g_rootfs && g_nlower) { overlay_resolve(p, buf, n, 0); return buf; }       // overlay (follow symlinks)
    char cpath[1024]; snprintf(cpath, sizeof cpath, "%s", rel0);
    for (int i = 0; i < 40; i++) {
        char h[4200]; snprintf(h, sizeof h, "%s%s", root, cpath);
        char lk[1024]; ssize_t k = readlink(h, lk, sizeof lk - 1);
        if (k < 0) break; lk[k] = 0;
        if (lk[0] == '/') snprintf(cpath, sizeof cpath, "%s", lk);
        else { char *sl = strrchr(cpath, '/'); int d = sl ? (int)(sl - cpath) : 0;
               char tmp[1024]; snprintf(tmp, sizeof tmp, "%.*s/%s", d, cpath, lk); snprintf(cpath, sizeof cpath, "%s", tmp); }
    }
    snprintf(buf, n, "%s%s", root, cpath); return buf;
}
#define ATFD(a) (((int)(a) == -100) ? AT_FDCWD : (int)(a))
static const char *atpath(const char *raw, char *buf, size_t n) {
    return (raw && raw[0] == '/') ? xresolve(raw, buf, n) : raw;
}
// x86-64 Linux `struct stat` layout (DIFFERS from aarch64): dev@0 ino@8 nlink@16(8B)
// mode@24 uid@28 gid@32 rdev@40 size@48 blksize@56 blocks@64 atime@72 mtime@88 ctime@104.
static void fill_linux_stat(uint8_t *d, const struct stat *s) {
    memset(d, 0, 144);
    *(uint64_t *)(d + 0)  = s->st_dev;
    *(uint64_t *)(d + 8)  = s->st_ino;
    *(uint64_t *)(d + 16) = s->st_nlink ? s->st_nlink : 1;
    *(uint32_t *)(d + 24) = s->st_mode;
    *(uint32_t *)(d + 28) = map_uid(s->st_uid);
    *(uint32_t *)(d + 32) = map_gid(s->st_gid);
    *(uint64_t *)(d + 40) = s->st_rdev;
    *(uint64_t *)(d + 48) = s->st_size;
    *(uint64_t *)(d + 56) = 4096;
    *(uint64_t *)(d + 64) = s->st_blocks;
    *(uint64_t *)(d + 72) = s->st_atime;          // atime sec
    *(uint64_t *)(d + 88) = s->st_mtime;          // mtime sec
    *(uint64_t *)(d + 104) = s->st_ctime;         // ctime sec
}
static int mmap_flags(int lf) {
    int f = 0;
    if (lf & 0x01) f |= MAP_SHARED;
    if (lf & 0x02) f |= MAP_PRIVATE;
    if (lf & 0x10) f |= MAP_FIXED;
    if (lf & 0x20) f |= MAP_ANON;
    return f;
}

// ---------------- syscalls (x86-64 numbers -> reused macOS translations) ----------------
static uint64_t brk_lo, brk_cur, brk_hi;
// Track our anon-RW mappings so guest MAP_FIXED (the dynamic loader placing library
// segments) is emulated in-place (pread/memset) instead of a real macOS MAP_FIXED,
// which would SPLIT the mapping and SIGBUS the rest (the macOS/OrbStack VM quirk).
static struct { uint64_t lo, hi; } g_regions[256]; static int g_nregions;
static int region_has(uint64_t a, uint64_t end) {
    if (a >= brk_lo && end <= brk_hi) return 1;
    for (int i = 0; i < g_nregions; i++) if (a >= g_regions[i].lo && end <= g_regions[i].hi) return 1;
    return 0;
}
static void region_add(uint64_t lo, uint64_t hi) {       // page-round: macOS maps whole pages, and the
    hi = (hi + 4095) & ~(uint64_t)4095;                  // loader's FIXED segment mmaps are page-aligned and
    lo = lo & ~(uint64_t)4095;                           // may extend past the reservation's unrounded end.
    if (g_nregions < 256) { g_regions[g_nregions].lo = lo; g_regions[g_nregions].hi = hi; g_nregions++; }
}

static void service(struct cpu *c) {
    // x86-64 ABI: nr=rax, args rdi,rsi,rdx,r10,r8,r9 ; return -> rax
    uint64_t nr = c->r[RAX];
    uint64_t a0 = c->r[RDI], a1 = c->r[RSI], a2 = c->r[RDX], a3 = c->r[10], a4 = c->r[8], a5 = c->r[9];
    uint64_t ret;
    if (g_trace) fprintf(stderr, "[sys] %llu (%llx,%llx,%llx)", (unsigned long long)nr,
                         (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2);
    switch (nr) {
    case 501: fprintf(stderr,"[w8] --- mallocs done, frees begin ---\n"); ret=0; break;
    case 500: g_w8 = (uint8_t *)a0; g_w8v = g_w8 ? *g_w8 : 0;                          // debug: arm byte-watchpoint
              fprintf(stderr, "[w8] armed @%p = %02x\n", (void *)g_w8, g_w8v); ret = 0; break;
    case 0:  { int rfd = (int)a0;                                                    // read
        if (rfd >= 0 && rfd == g_sigfd_read) {                                        // signalfd read -> struct signalfd_siginfo
            char b; ssize_t pr = read(rfd, &b, 1);                                    // drain one wake byte
            if (pr <= 0) { ret = (uint64_t)(int64_t)(pr < 0 ? -derr(errno) : -11); break; }
            int sig = 0; uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
            for (int s = 1; s < 64; s++) if ((p & (1ull << s)) && (g_sigfd_mask & (1ull << s))) { sig = s; break; }
            if (sig) __atomic_and_fetch(&g_pending, ~(1ull << (unsigned)sig), __ATOMIC_SEQ_CST);
            if (a1 && a2 >= 128) { memset((void *)a1, 0, 128); *(uint32_t *)a1 = (uint32_t)sig; }  // ssi_signo @0
            ret = 128; break; }
        if (rfd >= 0 && rfd < 1024 && (g_inotify[rfd] || g_timerfd[rfd])) {           // inotify/timerfd -> drain kqueue
            struct kevent kv[32]; struct timespec zero = {0, 0}; int nb = fcntl(rfd, F_GETFL) & O_NONBLOCK;
            int n = kevent(rfd, NULL, 0, kv, g_inotify[rfd] ? 32 : 1, nb ? &zero : NULL);
            if (n <= 0) { ret = (uint64_t)(-(n < 0 ? derr(errno) : 11)); break; }     // EAGAIN=11
            if (g_timerfd[rfd]) { if (a1 && a2 >= 8) *(uint64_t *)a1 = (uint64_t)kv[0].data; ret = 8; break; }
            uint8_t *out = (uint8_t *)a1; size_t off = 0;
            for (int i = 0; i < n && off + 16 <= a2; i++) { uint32_t f = kv[i].fflags, m = 0;
                if (f & (NOTE_WRITE | NOTE_EXTEND)) m |= 0x2; if (f & NOTE_ATTRIB) m |= 0x4;
                if (f & NOTE_DELETE) m |= 0x400; if (f & NOTE_RENAME) m |= 0x800;
                *(int32_t *)(out + off) = (int32_t)kv[i].ident; *(uint32_t *)(out + off + 4) = m;
                *(uint32_t *)(out + off + 8) = 0; *(uint32_t *)(out + off + 12) = 0; off += 16; }
            ret = (uint64_t)off; break; }
        ssize_t r = read(rfd, (void *)a1, (size_t)a2); ret = r < 0 ? (uint64_t)(-derr(errno)) : (uint64_t)r; break; }
    case 1:  { int wfd = (int)a0;                                                    // write (eventfd -> its pipe write-end)
        if (wfd >= 0 && wfd < 1024 && g_eventfd_peer[wfd]) wfd = g_eventfd_peer[wfd] - 1;
        ssize_t r = write(wfd, (void *)a1, (size_t)a2); ret = r < 0 ? (uint64_t)(-derr(errno)) : (uint64_t)r; break; }
    case 19: ret = (uint64_t)readv((int)a0, (void *)a1, (int)a2); break;              // readv
    case 20: ret = (uint64_t)writev((int)a0, (void *)a1, (int)a2); break;             // writev
    case 17: { ssize_t r = pread((int)a0, (void *)a1, (size_t)a2, (off_t)a3); ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; } // pread64
    case 18: { ssize_t r = pwrite((int)a0, (void *)a1, (size_t)a2, (off_t)a3); ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }// pwrite64
    case 257: { const char *raw = (const char *)a1; char pb[4200]; const char *p;        // openat
                int lf = (int)a2, mf = lf & 0x3;
                if (lf & 0x40) mf |= O_CREAT;   if (lf & 0x80) mf |= O_EXCL;
                if (lf & 0x200) mf |= O_TRUNC;  if (lf & 0x400) mf |= O_APPEND;
                if (lf & 0x800) mf |= O_NONBLOCK; if (lf & 0x10000) mf |= O_DIRECTORY;
                if (lf & 0x80000) mf |= O_CLOEXEC;
                if (g_nlower && raw && raw[0] == '/') {                                    // OVERLAY: write copies-up, read resolves across layers
                    char host[4300]; if ((mf & 3) != O_RDONLY || (lf & 0x40)) overlay_copyup(raw, host, sizeof host); else overlay_resolve(raw, host, sizeof host, 0);
                    snprintf(pb, sizeof pb, "%s", host); p = pb;
                } else p = atpath(raw, pb, sizeof pb);
                int r = openat(ATFD(a0), p, mf, (mode_t)a3);
                if (r >= 0 && r < 1024 && g_nlower && raw && raw[0] == '/') { snprintf(g_ovldir[r], sizeof g_ovldir[r], "%s", raw); g_ovlcur[r] = 0; }  // guest path + cursor for merged getdents
                ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 2: { const char *raw = (const char *)a0; char pb[4200]; const char *p;           // open
              int lf = (int)a1, mf = lf & 0x3;
              if (lf & 0x40) mf |= O_CREAT; if (lf & 0x200) mf |= O_TRUNC;
              if (g_nlower && raw && raw[0] == '/') {                                       // OVERLAY
                  char host[4300]; if ((mf & 3) != O_RDONLY || (lf & 0x40)) overlay_copyup(raw, host, sizeof host); else overlay_resolve(raw, host, sizeof host, 0);
                  snprintf(pb, sizeof pb, "%s", host); p = pb;
              } else p = atpath(raw, pb, sizeof pb);
              int r = open(p, mf, (mode_t)a2);
              if (r >= 0 && r < 1024 && g_nlower && raw && raw[0] == '/') { snprintf(g_ovldir[r], sizeof g_ovldir[r], "%s", raw); g_ovlcur[r] = 0; }
              ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 3:  { int cf = (int)a0;                                                     // close (reap eventfd peer / timerfd / inotify / loopback state)
        if (cf >= 0 && cf < 1024) { if (g_eventfd_peer[cf]) { close(g_eventfd_peer[cf] - 1); g_eventfd_peer[cf] = 0; }
                                    g_timerfd[cf] = 0; g_inotify[cf] = 0; g_lo_port[cf] = 0; g_sock_stream[cf] = 0; g_fd_cport[cf] = 0; g_ovldir[cf][0] = 0; }
        ret = (uint64_t)close(cf); break; }
    case 8:  ret = (uint64_t)lseek((int)a0, (off_t)a1, (int)a2); break;               // lseek
    case 9:  {                                                                        // mmap
               // All guest memory = anon RW (we translate code, never run guest pages, and
               // treat mprotect as a no-op). File content is pread in (no file mmap). MAP_FIXED
               // within a region we own is emulated in-place (no macOS split -> no SIGBUS).
               int anon = (int)a3 & 0x20, fixed = (int)a3 & 0x10;
               if (fixed && region_has(a0, a0 + a1)) {
                   if (!anon) pread((int)a4, (void *)a0, (size_t)a1, (off_t)a5);  // file segment -> copy into region
                   else if (a2 & 0x2) memset((void *)a0, 0, (size_t)a1);          // anon writable -> zero
                   if (g_trace || g_diag) fprintf(stderr, "[mmap] FIXED-in-region %llx len=%llx (emulated, %s)\n",
                       (unsigned long long)a0, (unsigned long long)a1, anon ? "anon" : "file");
                   ret = a0; break;
               }
               // Non-fixed mmaps get a 64KB guard tail mapped RW so glibc's vectorized
               // string ops (16-byte SSE over-reads past the logical end) land in mapped
               // zero memory instead of an unmapped page (Darwin SIGBUS).
               size_t guard = fixed ? 0 : 0x10000;
               void *r = mmap((void *)(fixed ? a0 : 0), (size_t)a1 + guard, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANON | (fixed ? MAP_FIXED : 0), -1, 0);
               if (r == MAP_FAILED) {
                   if (g_trace || g_diag) fprintf(stderr, "[mmap] FAILED addr=%llx len=%llx flags=%llx fixed=%d errno=%d\n",
                       (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a3, fixed, errno);
                   ret = (uint64_t)(-errno); break; }
               if (!anon) { ssize_t k = pread((int)a4, r, (size_t)a1, (off_t)a5); (void)k; }  // file-backed: read content
               region_add((uint64_t)r, (uint64_t)r + a1 + guard);
               if (g_nochain && anon && a1 == 0x2000) {   // DEBUG (WATCH): auto-arm byte-watchpoint on 2nd small group's slot-12 IB (no guest perturbation)
                   static int n2k; if (++n2k == 2) { g_w8 = (uint8_t *)((uint64_t)r + 0x1ad); g_w8v = *g_w8;
                       fprintf(stderr, "[w8] auto-armed @%p = %02x (group %p)\n", (void *)g_w8, g_w8v, r); }
               }
               if (g_trace || g_diag) fprintf(stderr, "[mmap] addr=%llx len=%llx flags=%llx fd=%lld off=%llx -> %p (%s)\n",
                   (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a3, (long long)(int)a4,
                   (unsigned long long)a5, r, anon ? "anon" : "file");
               ret = (uint64_t)r; break; }
    case 10: ret = 0; break;        // mprotect: NO-OP (can't split anon maps here; we don't enforce guest prot)
    case 11: if (a0 >= brk_lo && a0 < brk_hi) { ret = 0; break; }                     // munmap: skip within arena
             ret = (uint64_t)munmap((void *)a0, (size_t)a1); break;
    case 25: { void *r = mmap(0, (size_t)a2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);  // mremap(old,oldsz,newsz,..) -> copy+grow
               if (r == MAP_FAILED) { ret = (uint64_t)(-errno); break; }
               size_t old = (size_t)a1, n = old < (size_t)a2 ? old : (size_t)a2;
               memcpy(r, (void *)a0, n); ret = (uint64_t)r; break; }
    case 12: { // brk: report a fixed, non-growable break so musl/glibc use their mmap-based
               // allocator path (avoids building a brk heap inside our arena, which the
               // guest then mmap/mprotects -- impossible to split on this macOS VM).
               ret = brk_lo;
               if (g_trace) fprintf(stderr, "[brk] %llx -> %llx (non-growable)\n", (unsigned long long)a0, (unsigned long long)brk_lo); break; }
    case 16: ret = (uint64_t)(-25); break;                                            // ioctl -> ENOTTY
    case 5:  { struct stat s; if (fstat((int)a0, &s) < 0) { ret = (uint64_t)(-errno); break; }  // fstat
               fill_linux_stat((uint8_t *)a1, &s); ret = 0; break; }
    case 262: { struct stat s; char pb[4200];                                         // newfstatat
                const char *raw = (const char *)a1, *p = atpath(raw, pb, sizeof pb);
                int r = fstatat(ATFD(a0), p, &s, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
                if (r < 0) { ret = (uint64_t)(-errno); break; }
                fill_linux_stat((uint8_t *)a2, &s); ret = 0; break; }
    case 4:  { struct stat s; char pb[4200]; const char *p = atpath((const char *)a0, pb, sizeof pb); // stat
               int r = stat(p, &s); if (r < 0) { ret = (uint64_t)(-errno); break; }
               fill_linux_stat((uint8_t *)a1, &s); ret = 0; break; }
    case 158: { int code = (int)a0;                                                   // arch_prctl
                if (code == 0x1002) { c->fs_base = a1; ret = 0; }                      // ARCH_SET_FS
                else if (code == 0x1001) { c->gs_base = a1; ret = 0; }                 // ARCH_SET_GS
                else if (code == 0x1003) { *(uint64_t *)a1 = c->fs_base; ret = 0; }    // ARCH_GET_FS
                else if (code == 0x1004) { *(uint64_t *)a1 = c->gs_base; ret = 0; }    // ARCH_GET_GS
                else ret = (uint64_t)(-22); break; }
    case 218: ret = (uint64_t)getpid(); break;                                        // set_tid_address
    case 273: ret = 0; break;                                                         // set_robust_list
    case 228: { struct timespec ts; clock_gettime((clockid_t)a0, &ts);                // clock_gettime
                uint64_t *g = (uint64_t *)a1; if (g) { g[0] = ts.tv_sec; g[1] = ts.tv_nsec; } ret = 0; break; }
    case 96:  { struct timeval tv; gettimeofday(&tv, 0); uint64_t *g = (uint64_t *)a0; // gettimeofday
                if (g) { g[0] = tv.tv_sec; g[1] = tv.tv_usec; } ret = 0; break; }
    case 318: arc4random_buf((void *)a0, (size_t)a1); ret = a1; break;                // getrandom
    case 63:  { char *u = (char *)a0; memset(u, 0, 6 * 65);                           // uname
                strcpy(u, "Linux"); strcpy(u + 65, "jit86"); strcpy(u + 130, "6.1.0");
                strcpy(u + 195, "#1 jit86"); strcpy(u + 260, "x86_64"); ret = 0; break; }
    case 79: { size_t need = strlen(g_cwd) + 1;                                        // getcwd -> guest cwd
               if ((size_t)a1 < need) { ret = (uint64_t)(-ERANGE); break; }
               memcpy((char *)a0, g_cwd, need); ret = need; break; }
    case 80: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); // chdir
               if (chdir(p) != 0) { ret = (uint64_t)(-errno); break; }
               char real[4200];                                                       // re-derive canonical guest cwd
               if (getcwd(real, sizeof real)) {
                   size_t rl = g_rootfs ? strlen(g_rootfs) : 0;
                   if (rl && strncmp(real, g_rootfs, rl) == 0) { const char *g = real + rl; snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/"); }
                   else snprintf(g_cwd, sizeof g_cwd, "%s", real);
               }
               ret = 0; break; }
    case 81: { if (fchdir((int)a0) != 0) { ret = (uint64_t)(-errno); break; }          // fchdir
               char real[4200];
               if (getcwd(real, sizeof real)) {
                   size_t rl = g_rootfs ? strlen(g_rootfs) : 0;
                   if (rl && strncmp(real, g_rootfs, rl) == 0) { const char *g = real + rl; snprintf(g_cwd, sizeof g_cwd, "%s", g[0] ? g : "/"); }
                   else snprintf(g_cwd, sizeof g_cwd, "%s", real);
               }
               ret = 0; break; }
    case 89: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb);  // readlink
               ssize_t r = readlink(p, (char *)a1, (size_t)a2); ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 267: { char pb[4200]; const char *raw = (const char *)a1, *p = atpath(raw, pb, sizeof pb); // readlinkat
                if (raw && strstr(raw, "/proc/self/exe")) {
                    char rp[1024]; if (!realpath(g_exe_path, rp)) strncpy(rp, g_exe_path, sizeof rp - 1);
                    size_t l = strlen(rp); if (l > a3) l = (size_t)a3; memcpy((void *)a2, rp, l); ret = l; break; }
                ssize_t r = readlink(p, (char *)a2, (size_t)a3); ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    // ---- filesystem mutation (all path args go through the rootfs jail) ----
    case 87: { const char *raw = (const char *)a0;                                        // unlink
               if (g_nlower && raw && raw[0] == '/') { overlay_whiteout(raw); ret = 0; break; }   // OVERLAY: drop upper + .wh marker
               char pb[4200]; const char *p = xlate(raw, pb, sizeof pb); ret = unlink(p) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 84: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); ret = rmdir(p) < 0 ? (uint64_t)(-errno) : 0; break; }           // rmdir
    case 83: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); ret = mkdir(p, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; } // mkdir
    case 82: { char pb[4200], pb2[4200]; const char *o = xlate((const char *)a0, pb, sizeof pb); char b2[4200]; const char *n = xlate((const char *)a1, b2, sizeof b2); (void)pb2; ret = rename(o, n) < 0 ? (uint64_t)(-errno) : 0; break; } // rename
    case 86: { char pb[4200], b2[4200]; const char *o = xlate((const char *)a0, pb, sizeof pb), *n = xlate((const char *)a1, b2, sizeof b2); ret = link(o, n) < 0 ? (uint64_t)(-errno) : 0; break; }   // link
    case 88: { char b2[4200]; const char *t = (const char *)a0, *l = xlate((const char *)a1, b2, sizeof b2); ret = symlink(t, l) < 0 ? (uint64_t)(-errno) : 0; break; }                               // symlink (target kept verbatim)
    case 90: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); ret = chmod(p, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; }   // chmod
    case 91: ret = fchmod((int)a0, (mode_t)a1) < 0 ? (uint64_t)(-errno) : 0; break;                                                                      // fchmod
    case 92: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); ret = chown(p, (uid_t)a1, (gid_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; }  // chown
    case 93: ret = fchown((int)a0, (uid_t)a1, (gid_t)a2) < 0 ? (uint64_t)(-errno) : 0; break;                                                            // fchown
    case 94: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); ret = lchown(p, (uid_t)a1, (gid_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; } // lchown
    case 95: ret = (uint64_t)umask((mode_t)a0); break;                                                                                                   // umask
    case 76: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); ret = truncate(p, (off_t)a1) < 0 ? (uint64_t)(-errno) : 0; break; } // truncate
    case 77: ret = ftruncate((int)a0, (off_t)a1) < 0 ? (uint64_t)(-errno) : 0; break;                                                                    // ftruncate
    case 161: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb); (void)p; ret = 0; break; }                                        // chroot: jail already confines; no-op
    case 263: { const char *raw = (const char *)a1;                                       // unlinkat (AT_REMOVEDIR=0x200)
                if (g_nlower && raw && raw[0] == '/') { overlay_whiteout(raw); ret = 0; break; }   // OVERLAY whiteout
                char pb[4200]; const char *p = atpath(raw, pb, sizeof pb); ret = ((a2 & 0x200) ? rmdir(p) : unlink(p)) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 258: { char pb[4200]; const char *p = atpath((const char *)a1, pb, sizeof pb); ret = mkdir(p, (mode_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; } // mkdirat
    case 264: case 316: { char pb[4200], b2[4200];                                                                                                       // renameat / renameat2
                int od = (nr == 316) ? (int)a0 : (int)a0, nd = (nr == 316) ? (int)a2 : (int)a2;
                const char *o = atpath((const char *)a1, pb, sizeof pb), *n = atpath((const char *)a3, b2, sizeof b2); (void)od; (void)nd;
                ret = rename(o, n) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 265: { char pb[4200], b2[4200]; const char *o = atpath((const char *)a1, pb, sizeof pb), *n = atpath((const char *)a3, b2, sizeof b2); ret = link(o, n) < 0 ? (uint64_t)(-errno) : 0; break; }  // linkat
    case 266: { char b2[4200]; const char *t = (const char *)a0, *l = atpath((const char *)a2, b2, sizeof b2); ret = symlink(t, l) < 0 ? (uint64_t)(-errno) : 0; break; }  // symlinkat
    case 268: { char pb[4200]; const char *p = atpath((const char *)a1, pb, sizeof pb); ret = fchmodat(AT_FDCWD, p, (mode_t)a2, 0) < 0 ? (uint64_t)(-errno) : 0; break; }   // fchmodat
    case 260: { char pb[4200]; const char *p = atpath((const char *)a1, pb, sizeof pb); ret = fchownat(AT_FDCWD, p, (uid_t)a2, (gid_t)a3, (a4 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0) < 0 ? (uint64_t)(-errno) : 0; break; } // fchownat
    case 285: { struct stat st; if (fstat((int)a0, &st) == 0 && st.st_size < (off_t)(a2 + a3)) ret = ftruncate((int)a0, (off_t)(a2 + a3)) < 0 ? (uint64_t)(-errno) : 0; else ret = 0; break; }  // fallocate (extend only)
    case 40: { off_t off = a2 ? *(off_t *)a2 : 0; ssize_t r = 0; char buf[65536]; size_t left = a3;                                                       // sendfile
               if (a2) lseek((int)a1, off, SEEK_SET);
               while (left) { ssize_t k = read((int)a1, buf, left < sizeof buf ? left : sizeof buf); if (k <= 0) break; ssize_t w = write((int)a0, buf, k); if (w < 0) { r = -1; break; } r += w; left -= w; if (w < k) break; }
               if (a2 && r > 0) *(off_t *)a2 = off + r; ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 326: { int fin = (int)a0, fout = (int)a2; off_t *oin = (off_t *)a1, *oout = (off_t *)a3; size_t len = (size_t)a4;  // copy_file_range
                char buf[65536]; size_t done = 0; ssize_t err = 0;
                if (oin) lseek(fin, *oin, SEEK_SET); if (oout) lseek(fout, *oout, SEEK_SET);
                while (done < len) { size_t want = len - done; if (want > sizeof buf) want = sizeof buf;
                    ssize_t k = read(fin, buf, want); if (k < 0) { err = done ? 0 : -1; break; } if (k == 0) break;
                    ssize_t w = write(fout, buf, k); if (w < 0) { err = done ? 0 : -1; break; } done += w; if (w < k) break; }
                if (oin) *oin += done; if (oout) *oout += done;
                ret = err < 0 ? (uint64_t)(-errno) : (uint64_t)done; break; }
    case 280: { if (a1) { char pb[4200]; const char *p = atpath((const char *)a1, pb, sizeof pb);          // utimensat
                          ret = utimensat(AT_FDCWD, p, (struct timespec *)a2, (a3 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0) < 0 ? (uint64_t)(-errno) : 0; }
                else ret = futimens((int)a0, (struct timespec *)a2) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 235: { char pb[4200]; const char *p = xlate((const char *)a0, pb, sizeof pb);                     // utimes(path, tv[2])
                ret = utimes(p, (struct timeval *)a1) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 261: { char pb[4200]; const char *p = atpath((const char *)a1, pb, sizeof pb);                    // futimesat(dfd, path, tv)
                ret = utimes(p, (struct timeval *)a2) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 59: {  // execve -> re-exec this jit86 binary on the new guest program (process replace)
        const char *gp = (const char *)a0; char **gargv = (char **)a1, **genvp = (char **)a2;
        char chk[4200]; const char *rp = xlate(gp, chk, sizeof chk);
        if (access(rp, F_OK) != 0) { ret = (uint64_t)(-errno); break; }
        int gc = 0; while (gargv && gargv[gc]) gc++;
        char **hv = (char **)malloc((gc + 6 + g_nvols * 2 + g_nportmap * 2 + g_nlower * 2) * sizeof *hv); int n = 0;
        hv[n++] = (char *)g_self_path;
        if (g_rootfs) { hv[n++] = (char *)"--rootfs"; hv[n++] = (char *)g_rootfs; }
        for (int vi = 0; vi < g_nvols; vi++) { static char vspec[32][1300]; snprintf(vspec[vi], sizeof vspec[vi], "%s:%s", g_vols[vi].guest, g_vols[vi].host);
                                               hv[n++] = (char *)"--vol"; hv[n++] = vspec[vi]; }
        for (int pi = 0; pi < g_nportmap; pi++) { static char pspec[32][16]; snprintf(pspec[pi], sizeof pspec[pi], "%u:%u", g_portmap[pi].hport, g_portmap[pi].cport);
                                                  hv[n++] = (char *)"-p"; hv[n++] = pspec[pi]; }
        for (int li = 0; li < g_nlower; li++) { hv[n++] = (char *)"--lower"; hv[n++] = g_lower[li].root; }
        hv[n++] = (char *)gp;
        for (int i = 1; i < gc; i++) hv[n++] = gargv[i];
        hv[n] = NULL;
        execve(g_self_path, hv, genvp);                 // replaces the process; returns only on error
        free(hv); ret = (uint64_t)(-errno); break; }
    case 284: case 290: { int fds[2]; if (pipe(fds) < 0) { ret = (uint64_t)(-errno); break; }   // eventfd/eventfd2(initval,flags)->pipe
        int flags = (nr == 290) ? (int)a1 : 0;
        if (flags & 0x800) { fcntl(fds[0], F_SETFL, O_NONBLOCK); fcntl(fds[1], F_SETFL, O_NONBLOCK); }
        if (flags & 0x80000) { fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC); }
        if (a0) { uint64_t v = a0; if (write(fds[1], &v, 8) < 0) {} }
        if (fds[0] < 1024 && fds[1] < 1024) g_eventfd_peer[fds[0]] = fds[1] + 1;
        ret = (uint64_t)fds[0]; break; }
    case 282: case 289: {   // signalfd(fd,mask,sizemask) / signalfd4(fd,mask,sizemask,flags)
        uint64_t lm = a1 ? *(uint64_t *)a1 : 0, pm = 0;                                 // sigset bit (signo-1) -> g_pending bit signo
        for (int s = 1; s < 64; s++) if (lm & (1ull << (s - 1))) pm |= (1ull << s);
        if (g_sigfd_pipe[0] < 0 && pipe(g_sigfd_pipe) < 0) { ret = (uint64_t)(-errno); break; }
        g_sigfd_mask |= pm; g_sigfd_read = g_sigfd_pipe[0];
        for (int s = 1; s < 64; s++) if ((pm & (1ull << s)) && !sig_is_sync(s)) {       // ensure the host delivers them
            struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = host_sigh; sigaction(sig_l2m(s), &sa, NULL); }
        if (nr == 289) { if (a3 & 0x80000) fcntl(g_sigfd_pipe[0], F_SETFD, FD_CLOEXEC); if (a3 & 0x800) fcntl(g_sigfd_pipe[0], F_SETFL, O_NONBLOCK); }
        ret = (uint64_t)g_sigfd_pipe[0]; break; }
    case 253: case 294: { int r = kqueue(); if (r < 0) { ret = (uint64_t)(-errno); break; }      // inotify_init/init1 -> kqueue
        if (r < 1024) g_inotify[r] = 1;
        if (nr == 294) { if ((int)a0 & 0x800) fcntl(r, F_SETFL, O_NONBLOCK); if ((int)a0 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC); }
        ret = (uint64_t)r; break; }
    case 254: { char pb[4200]; const char *p = atpath((const char *)a1, pb, sizeof pb);          // inotify_add_watch(fd,path,mask)
        int wfd = open(p, O_EVTONLY); if (wfd < 0) { ret = (uint64_t)(-derr(errno)); break; }
        struct kevent kv; EV_SET(&kv, wfd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND, 0, (void *)(intptr_t)wfd);
        if (kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0) { int e = errno; close(wfd); ret = (uint64_t)(-derr(e)); break; }
        ret = (uint64_t)wfd; break; }
    case 255: { struct kevent kv; EV_SET(&kv, (int)a1, EVFILT_VNODE, EV_DELETE, 0, 0, NULL);      // inotify_rm_watch(fd,wd)
        kevent((int)a0, &kv, 1, NULL, 0, NULL); close((int)a1); ret = 0; break; }
    case 283: { int r = kqueue(); if (r < 0) { ret = (uint64_t)(-errno); break; }                // timerfd_create(clockid,flags)
        if (r < 1024) g_timerfd[r] = 1; if ((int)a1 & 1) fcntl(r, F_SETFL, O_NONBLOCK); ret = (uint64_t)r; break; }
    case 286: { struct kevent kv; uint64_t iv_s = 0, iv_n = 0, vl_s = 0, vl_n = 0;               // timerfd_settime(fd,flags,new,old)
        if (a2) { memcpy(&iv_s, (void *)a2, 8); memcpy(&iv_n, (void *)(a2 + 8), 8); memcpy(&vl_s, (void *)(a2 + 16), 8); memcpy(&vl_n, (void *)(a2 + 24), 8); }
        int64_t period = (iv_s || iv_n) ? (int64_t)(iv_s * 1000000000ull + iv_n) : (int64_t)(vl_s * 1000000000ull + vl_n);
        if (period <= 0) { EV_SET(&kv, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL); kevent((int)a0, &kv, 1, NULL, 0, NULL); ret = 0; break; }
        uint16_t fl = EV_ADD | ((iv_s || iv_n) ? 0 : EV_ONESHOT); EV_SET(&kv, 1, EVFILT_TIMER, fl, NOTE_NSECONDS, period, NULL);
        ret = kevent((int)a0, &kv, 1, NULL, 0, NULL) < 0 ? (uint64_t)(-derr(errno)) : 0; break; }
    case 287: if (a1) memset((void *)a1, 0, 32); ret = 0; break;                                 // timerfd_gettime -> best-effort 0
    case 213: case 291: { int r = kqueue(); if (r < 0) { ret = (uint64_t)(-errno); break; }      // epoll_create/create1 -> kqueue
        if (nr == 291 && ((int)a0 & 0x80000)) fcntl(r, F_SETFD, FD_CLOEXEC); ret = (uint64_t)r; break; }
    case 233: { int op = (int)a1, fd = (int)a2; uint32_t ev = 0; uint64_t data = (uint64_t)(unsigned)fd;  // epoll_ctl -> kevent
        if (a3) { ev = *(uint32_t *)a3; memcpy(&data, (void *)(a3 + 4), 8); }
        struct kevent kv[2]; int n = 0; uint16_t base = (op == 2) ? EV_DELETE : EV_ADD;
        uint16_t xf = (uint16_t)((ev & 0x80000000u ? EV_CLEAR : 0) | (ev & 0x40000000u ? EV_ONESHOT : 0));
        if (op == 2 || (ev & 0x1)) { EV_SET(&kv[n], fd, EVFILT_READ, base | xf, 0, 0, (void *)data); n++; }
        if (op == 2 || (ev & 0x4)) { EV_SET(&kv[n], fd, EVFILT_WRITE, base | xf, 0, 0, (void *)data); n++; }
        for (int i = 0; i < n; i++) kevent((int)a0, &kv[i], 1, NULL, 0, NULL); ret = 0; break; }
    case 232: case 281: { int maxev = (int)a2; if (maxev > 256) maxev = 256; if (maxev < 0) maxev = 0;   // epoll_wait/pwait -> kevent
        struct kevent kv[256]; struct timespec ts, *tp = NULL;
        if ((int)a3 >= 0) { ts.tv_sec = (int)a3 / 1000; ts.tv_nsec = (long)((int)a3 % 1000) * 1000000L; tp = &ts; }
        int r = kevent((int)a0, NULL, 0, kv, maxev, tp); if (r < 0) { ret = (uint64_t)(-derr(errno)); break; }
        uint8_t *out = (uint8_t *)a1;
        for (int i = 0; i < r; i++) { uint32_t ev = (kv[i].filter == EVFILT_READ) ? 0x1u : (kv[i].filter == EVFILT_WRITE) ? 0x4u : 0u;
            if (kv[i].flags & EV_EOF) ev |= 0x10u; if (kv[i].flags & EV_ERROR) ev |= 0x8u;
            *(uint32_t *)(out + i * 12) = ev; memcpy(out + i * 12 + 4, &kv[i].udata, 8); }
        ret = (uint64_t)r; break; }
    case 74: ret = fsync((int)a0) < 0 ? (uint64_t)(-errno) : 0; break;                 // fsync
    case 75: ret = fsync((int)a0) < 0 ? (uint64_t)(-errno) : 0; break;                 // fdatasync (no macOS fdatasync -> fsync)
    case 162: sync(); ret = 0; break;                                                 // sync
    case 24: ret = (uint64_t)sched_yield(); break;                                    // sched_yield
    case 124: ret = (uint64_t)getsid((pid_t)a0); break;                              // getsid
    case 100: { struct tms t; clock_t r = times(&t); if (a0) memcpy((void *)a0, &t, sizeof t); ret = (uint64_t)r; break; }  // times
    case 62:  if ((int)a0 == getpid() || (int)a0 <= 0) { raise_guest_signal(c, (int)a1); ret = 0; }  // kill: self/pgrp -> guest delivery
              else ret = kill((pid_t)a0, sig_l2m((int)a1)) < 0 ? (uint64_t)(-errno) : 0; break;
    case 200: raise_guest_signal(c, (int)a1); ret = 0; break;                         // tkill(tid,sig) -> self (single-threaded model)
    case 293: { int fds[2]; if (pipe(fds) < 0) { ret = (uint64_t)(-errno); break; }   // pipe2
                if ((int)a1 & 0x800) { fcntl(fds[0], F_SETFL, O_NONBLOCK); fcntl(fds[1], F_SETFL, O_NONBLOCK); }
                if ((int)a1 & 0x80000) { fcntl(fds[0], F_SETFD, FD_CLOEXEC); fcntl(fds[1], F_SETFD, FD_CLOEXEC); }
                ((int *)a0)[0] = fds[0]; ((int *)a0)[1] = fds[1]; ret = 0; break; }
    case 230: { struct timespec *req = (struct timespec *)a2, rel;                     // clock_nanosleep
                if ((int)a1 & 1) { struct timespec now; clock_gettime((int)a0 == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now);
                    rel.tv_sec = req->tv_sec - now.tv_sec; rel.tv_nsec = req->tv_nsec - now.tv_nsec;
                    if (rel.tv_nsec < 0) { rel.tv_sec--; rel.tv_nsec += 1000000000; } if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; } req = &rel; }
                ret = nanosleep(req, NULL) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 271: { int to = -1; if (a2) { struct timespec *ts = (struct timespec *)a2; to = (int)(ts->tv_sec * 1000 + ts->tv_nsec / 1000000); }  // ppoll
                int r = poll((struct pollfd *)a0, (nfds_t)a1, to); ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 274: ret = (uint64_t)(-38); break;                                           // get_robust_list -> ENOSYS
    case 118: if (a0) *(uint32_t *)a0 = 0; if (a1) *(uint32_t *)a1 = 0; if (a2) *(uint32_t *)a2 = 0; ret = 0; break;  // getresuid -> root
    case 120: if (a0) *(uint32_t *)a0 = 0; if (a1) *(uint32_t *)a1 = 0; if (a2) *(uint32_t *)a2 = 0; ret = 0; break;  // getresgid -> root
    case 141: ret = 0; break;                                                         // setpriority -> ok (ignore)
    case 140: ret = 20; break;                                                        // getpriority -> nice 0 (kernel encoding 20-nice)
    case 157: ret = 0; break;                                                         // prctl -> ok (PR_SET_NAME etc. ignored)
    case 131: ret = 0; break;                                                         // sigaltstack -> ok (layout differs; stub)
    case 72: { int r = fcntl((int)a0, (int)a1, a2); ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }  // fcntl
    case 32: ret = (uint64_t)dup((int)a0); break;                                      // dup
    case 33: ret = (uint64_t)dup2((int)a0, (int)a1); break;                            // dup2
    case 234: raise_guest_signal(c, (int)a2); ret = 0; break;                         // tgkill(tgid,tid,sig) -> self (single-threaded model)
    case 292: { int r = dup2((int)a0, (int)a1); if (r >= 0 && (a2 & 0x80000)) fcntl((int)a1, F_SETFD, FD_CLOEXEC);
               ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }                // dup3 (O_CLOEXEC=0x80000)
    case 21: { char pb[4200]; const char *p = atpath((const char *)a0, pb, sizeof pb);  // access
               int r = access(p, (int)a1); ret = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 269: { char pb[4200]; const char *p = atpath((const char *)a1, pb, sizeof pb); // faccessat
                int r = faccessat(ATFD(a0), p, (int)a2, 0); ret = r < 0 ? (uint64_t)(-errno) : 0; break; }
    case 22: { int fds[2]; if (pipe(fds) < 0) { ret = (uint64_t)(-errno); break; }     // pipe
               ((int *)a0)[0] = fds[0]; ((int *)a0)[1] = fds[1]; ret = 0; break; }
    case 13: {                                                  // rt_sigaction(sig, *act, *old) -- x86-64 sigaction: handler@0,flags@8,restorer@16,mask@24
        int sig = (int)a0; if (sig < 1 || sig > 64) { ret = (uint64_t)(-22); break; }
        if (a2) { *(uint64_t *)(a2 + 0) = g_sigact[sig].handler; *(uint64_t *)(a2 + 8) = g_sigact[sig].flags; *(uint64_t *)(a2 + 24) = g_sigact[sig].mask; }
        if (a1) { uint64_t h = *(uint64_t *)(a1 + 0);
            g_sigact[sig].handler = h; g_sigact[sig].flags = *(uint64_t *)(a1 + 8); g_sigact[sig].mask = *(uint64_t *)(a1 + 24);
            if (sig != 9 && sig != 19) { int ms = sig_l2m(sig);                       // SIGKILL/SIGSTOP can't be caught
                if (h == 0) signal(ms, SIG_DFL);
                else if (h == 1) signal(ms, SIG_IGN);
                else if (!sig_is_sync(sig)) { struct sigaction sa; memset(&sa, 0, sizeof sa);  // async: host flags pending, dispatcher delivers
                    sa.sa_handler = host_sigh; sigfillset(&sa.sa_mask); sigaction(ms, &sa, NULL); } } }
        ret = 0; break; }
    case 14: {                                                  // rt_sigprocmask(how, *set, *old)
        if (a2) *(uint64_t *)a2 = c->sigmask;
        if (a1) { uint64_t set = *(uint64_t *)a1;
            if (a0 == 0) c->sigmask |= set; else if (a0 == 1) c->sigmask &= ~set; else c->sigmask = set; }  // BLOCK/UNBLOCK/SETMASK
        ret = 0; break; }
    case 15: do_sigreturn(c); c->redirect = 1; break;                                 // rt_sigreturn (restorer path)
    case 127: { uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST), out = 0;  // rt_sigpending
                for (int s = 1; s <= 64; s++) if (p & (1ull << s)) out |= (1ull << (s - 1));
                if (a0) *(uint64_t *)a0 = out; ret = 0; break; }
    case 105: case 106: case 113: case 114: case 117: case 119: ret = 0; break;       // setuid/setgid/setreuid/setregid/setresuid/setresgid -> ok
    case 39: ret = (uint64_t)getpid(); break;                                         // getpid
    case 102: case 104: case 107: case 108: ret = 0; break;     // getuid/getgid/geteuid/getegid -> container root (docker model)
    case 115: {                                                 // getgroups -> root's supplementary groups (from rootfs /etc/group)
        extern int g_ngroups, g_groups[]; extern void build_root_groups(void); build_root_groups();
        if (a0 == 0) { ret = g_ngroups; break; }                // size query
        if ((int)a0 < g_ngroups) { ret = (uint64_t)(-EINVAL); break; }
        for (int i = 0; i < g_ngroups; i++) ((uint32_t *)a1)[i] = g_groups[i];
        ret = g_ngroups; break; }
    case 116: ret = 0; break;                                   // setgroups -> ok (no-op, container root)
    case 110: ret = (uint64_t)getppid(); break;
    case 186: ret = (uint64_t)getpid(); break;                                        // gettid
    case 202: ret = 0; break;                                                         // futex (stub: single-thread)
    case 217: { // getdents64
        int gfd = (int)a0;
        if (g_nlower && gfd >= 0 && gfd < 1024 && g_ovldir[gfd][0]) {   // OVERLAY: merged listing across layers
            static char onames[2048][256]; static uint8_t otypes[2048];
            int ocnt = overlay_readdir(g_ovldir[gfd], onames, otypes, 2048);   // stable order; persistent per-fd cursor
            int cur = g_ovlcur[gfd]; uint8_t *out = (uint8_t *)a1; size_t o = 0;
            while (cur < ocnt) { size_t nl = strlen(onames[cur]), lr = (19 + nl + 1 + 7) & ~7ull;
                if (o + lr > (size_t)a2) break; uint8_t *ld = out + o;
                *(uint64_t *)(ld + 0) = (uint64_t)(cur + 1); *(uint64_t *)(ld + 8) = o + lr;
                *(uint16_t *)(ld + 16) = (uint16_t)lr; *(ld + 18) = otypes[cur];
                memcpy(ld + 19, onames[cur], nl); ld[19 + nl] = 0; o += lr; cur++; }
            g_ovlcur[gfd] = cur;                                       // remember progress; returns 0 when exhausted -> EOF
            ret = o; break;
        }
        static struct { int fd; DIR *d; } dirs[64]; static int ndirs;
        int fd = (int)a0; DIR *dir = NULL;
        for (int i = 0; i < ndirs; i++) if (dirs[i].fd == fd) { dir = dirs[i].d; break; }
        if (!dir) { dir = fdopendir(dup(fd)); if (!dir) { ret = (uint64_t)(-errno); break; }
                    if (ndirs < 64) { dirs[ndirs].fd = fd; dirs[ndirs].d = dir; ndirs++; } }
        uint8_t *out = (uint8_t *)a1; size_t o = 0; struct dirent *de; long pos = telldir(dir);
        while ((de = readdir(dir))) {
            size_t nl = strlen(de->d_name), lr = (19 + nl + 1 + 7) & ~7ull;
            if (o + lr > (size_t)a2) { seekdir(dir, pos); break; }
            uint8_t *ld = out + o;
            *(uint64_t *)(ld + 0) = de->d_ino; *(uint64_t *)(ld + 8) = o + lr;
            *(uint16_t *)(ld + 16) = (uint16_t)lr; *(ld + 18) = de->d_type;
            memcpy(ld + 19, de->d_name, nl); ld[19 + nl] = 0;
            o += lr; pos = telldir(dir);
        }
        ret = o; break;
    }
    case 28: ret = 0; break;                                                          // madvise
    case 302: ret = 0; break;                                                         // prlimit64 (stub)
    case 334: ret = (uint64_t)(-38); break;                                           // rseq -> ENOSYS (glibc falls back)
    case 111: ret = (uint64_t)getpgrp(); break;                                       // getpgrp
    case 121: ret = (uint64_t)getpgid((pid_t)a0); break;                              // getpgid
    case 109: ret = (uint64_t)setpgid((pid_t)a0, (pid_t)a1); break;                   // setpgid
    case 112: ret = (uint64_t)setsid(); break;                                        // setsid
    case 57: case 58:                                                                 // fork / vfork
    case 56: {                                                                        // clone (process fork; threads unsupported)
        if (nr == 56 && (a0 & 0x100)) { ret = (uint64_t)(-38); break; }               // CLONE_VM (thread) -> ENOSYS
        pid_t p = fork();                                                             // host fork: COW-dup guest mem + JIT cache
        if (p < 0) { ret = (uint64_t)(-errno); break; }
        if (nr == 56) {                                                               // clone tid hand-back
            if (p > 0 && (a0 & 0x100000) && a2) *(int *)a2 = p;                        // CLONE_PARENT_SETTID
            if (p == 0 && (a0 & 0x01000000) && a3) *(int *)a3 = getpid();              // CLONE_CHILD_SETTID
        }
        ret = (uint64_t)p; break;                                                     // parent: child pid; child: 0
    }
    case 61: { int st; pid_t p = wait4((pid_t)a0, a1 ? &st : NULL, (int)a2, NULL);     // wait4
               if (a1) *(int *)a1 = st; ret = p < 0 ? (uint64_t)(-errno) : (uint64_t)p; break; }
    case 221: ret = 0; break;                                                         // fadvise64 -> ok (ignore)
    case 41: { int dom = (int)a0 == 10 ? AF_INET6 : (int)a0;                          // socket(domain,type,protocol)
               int r = socket(dom, socktype_l2d((int)a1), (int)a2);
               if (r < 0) { ret = (uint64_t)(-errno); break; }
               if ((int)a1 & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL, 0) | O_NONBLOCK);   // SOCK_NONBLOCK
               if ((int)a1 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);                         // SOCK_CLOEXEC
               if (r < 1024) { g_sock_stream[r] = ((int)a0 == AF_INET && ((int)a1 & 0xff) == SOCK_STREAM); g_lo_port[r] = 0; }
               ret = (uint64_t)r; break; }
    case 49: { uint8_t *sa = (uint8_t *)a1;                                            // bind
               if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] && lo_is(sa, (socklen_t)a2)) {  // private loopback
                   uint16_t p = ntohs(*(uint16_t *)(sa + 2)); char up[200]; lo_path(p, up, sizeof up);
                   if (lo_swap((int)a0) < 0) { ret = (uint64_t)(-errno); break; }
                   unlink(up); struct sockaddr_un un; memset(&un, 0, sizeof un); un.sun_family = AF_UNIX; un.sun_len = sizeof un; snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
                   int r = bind((int)a0, (struct sockaddr *)&un, sizeof un); if (r == 0) g_lo_port[(int)a0] = p ? p : 1;
                   ret = r < 0 ? (uint64_t)(-errno) : 0; break; }
               struct sockaddr_storage ss; socklen_t l = l2d_sa((void *)a1, (socklen_t)a2, &ss);
               if (g_nportmap && sa && a2 >= 8 && *(uint16_t *)sa == AF_INET) {         // docker -p: bind the published host port :H
                   uint16_t cp = ntohs(*(uint16_t *)(sa + 2));
                   if ((int)a0 >= 0 && (int)a0 < 1024) g_fd_cport[(int)a0] = cp;        // remember :C for getsockname
                   if (l) ((struct sockaddr_in *)&ss)->sin_port = htons(pm_host(cp)); }
               ret = bind((int)a0, l ? (struct sockaddr *)&ss : (void *)a1, l ? l : (socklen_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 42: { uint8_t *sa = (uint8_t *)a1;                                            // connect
               if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] && lo_is(sa, (socklen_t)a2)) {  // private loopback
                   uint16_t p = ntohs(*(uint16_t *)(sa + 2)); char up[200]; lo_path(p, up, sizeof up);
                   if (lo_swap((int)a0) < 0) { ret = (uint64_t)(-errno); break; }
                   struct sockaddr_un un; memset(&un, 0, sizeof un); un.sun_family = AF_UNIX; un.sun_len = sizeof un; snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
                   int r = connect((int)a0, (struct sockaddr *)&un, sizeof un); if (r == 0 || errno == EINPROGRESS) g_lo_port[(int)a0] = p ? p : 1;
                   ret = r < 0 ? (uint64_t)(-errno) : 0; break; }
               struct sockaddr_storage ss; socklen_t l = l2d_sa((void *)a1, (socklen_t)a2, &ss);
               ret = connect((int)a0, l ? (struct sockaddr *)&ss : (void *)a1, l ? l : (socklen_t)a2) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 50: ret = listen((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break;       // listen
    case 43: case 288: { int lfd = (int)a0; int pl = (lfd >= 0 && lfd < 1024) ? g_lo_port[lfd] : 0;  // accept / accept4
               struct sockaddr_storage ss; socklen_t sl = sizeof ss;
               int r = pl ? accept(lfd, NULL, NULL) : accept(lfd, (struct sockaddr *)&ss, &sl);
               if (r < 0) { ret = (uint64_t)(-errno); break; }
               if (nr == 288) { if ((int)a3 & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL, 0) | O_NONBLOCK); if ((int)a3 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC); }
               if (pl) { if (r < 1024) { g_lo_port[r] = pl; g_sock_stream[r] = 1; } fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, pl); }   // peer = 127.0.0.1:lport
               else if (a1 && a2) { socklen_t gl = *(socklen_t *)a2; d2l_sa((struct sockaddr *)&ss, (void *)a1, gl, (socklen_t *)a2); }
               ret = (uint64_t)r; break; }
    case 51: case 52: { int fd = (int)a0;                                              // getsockname / getpeername
               if (fd >= 0 && fd < 1024 && g_lo_port[fd]) { fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]); ret = 0; break; }
               struct sockaddr_storage ss; socklen_t sl = sizeof ss;
               int r = (nr == 51 ? getsockname : getpeername)(fd, (struct sockaddr *)&ss, &sl);
               if (r < 0) { ret = (uint64_t)(-errno); break; }
               if (a1 && a2) { socklen_t gl = *(socklen_t *)a2; d2l_sa((struct sockaddr *)&ss, (void *)a1, gl, (socklen_t *)a2); }
               if (nr == 51 && g_nportmap && a1 && fd >= 0 && fd < 1024 && g_fd_cport[fd])
                   *(uint16_t *)((uint8_t *)a1 + 2) = htons(g_fd_cport[fd]);            // report the :C the app asked for (port @2)
               ret = 0; break; }
    case 44: { struct sockaddr_storage ss; socklen_t l = a4 ? l2d_sa((void *)a4, (socklen_t)a5, &ss) : 0;  // sendto
               ssize_t r = sendto((int)a0, (void *)a1, (size_t)a2, (int)a3, l ? (struct sockaddr *)&ss : (void *)a4, l); ret = r < 0 ? (uint64_t)(-errno) : (uint64_t)r; break; }
    case 45: { struct sockaddr_storage ss; socklen_t sl = sizeof ss;                   // recvfrom
               ssize_t r = recvfrom((int)a0, (void *)a1, (size_t)a2, (int)a3, a4 ? (struct sockaddr *)&ss : NULL, a4 ? &sl : NULL);
               if (r < 0) { ret = (uint64_t)(-errno); break; }
               if (a4 && a5) { socklen_t gl = *(socklen_t *)a5; d2l_sa((struct sockaddr *)&ss, (void *)a4, gl, (socklen_t *)a5); }
               ret = (uint64_t)r; break; }
    case 48: ret = shutdown((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0; break;     // shutdown
    case 53: { int sv[2]; int r = socketpair((int)a0 == 10 ? AF_INET6 : (int)a0, socktype_l2d((int)a1), (int)a2, sv);  // socketpair
               if (r < 0) { ret = (uint64_t)(-errno); break; } ((int *)a3)[0] = sv[0]; ((int *)a3)[1] = sv[1]; ret = 0; break; }
    case 54: { int lvl = (int)a1 == 1 ? 0xffff : (int)a1, opt = (int)a1 == 1 ? so_l2d((int)a2) : (int)a2;  // setsockopt
               ret = setsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t)a4) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 55: { int lvl = (int)a1 == 1 ? 0xffff : (int)a1, opt = (int)a1 == 1 ? so_l2d((int)a2) : (int)a2;  // getsockopt
               ret = getsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t *)a4) < 0 ? (uint64_t)(-errno) : 0; break; }
    case 27: { uint8_t *v = (uint8_t *)a2; size_t n = ((size_t)a1 + 4095) / 4096;      // mincore: report all resident
               if (v) for (size_t i = 0; i < n; i++) v[i] = 1; ret = 0; break; }
    case 204: { if (a2) *(uint64_t *)a2 = 1; ret = (a1 < 8) ? a1 : 8; break; }         // sched_getaffinity (1 cpu)
    case 99: memset((void *)a0, 0, 112); { uint64_t *si = (uint64_t *)a0; if (si) { si[0] = 100; si[1] = 1u << 30; si[2] = 1u << 29; } } ret = 0; break; // sysinfo
    case 332: { struct stat s; char pb[4200];                                         // statx(dfd,path,flags,mask,buf)
        const char *raw = (const char *)a1, *p = atpath(raw, pb, sizeof pb);
        int empty = (raw && !raw[0] && (a2 & 0x1000)), rr;
        rr = empty ? fstat((int)a0, &s) : fstatat(ATFD(a0), p, &s, (a2 & 0x100) ? AT_SYMLINK_NOFOLLOW : 0);
        if (rr < 0) { ret = (uint64_t)(-errno); break; }
        uint8_t *d = (uint8_t *)a4; memset(d, 0, 256);            // correct statx layout
        *(uint32_t *)(d + 0) = 0x7ff; *(uint32_t *)(d + 4) = 4096;  // stx_mask, stx_blksize
        *(uint32_t *)(d + 16) = s.st_nlink ? s.st_nlink : 1;       // stx_nlink
        *(uint32_t *)(d + 20) = map_uid(s.st_uid); *(uint32_t *)(d + 24) = map_gid(s.st_gid);
        *(uint16_t *)(d + 28) = s.st_mode;                        // stx_mode @28 (not 26 -- would clobber gid)
        *(uint64_t *)(d + 32) = s.st_ino; *(uint64_t *)(d + 40) = s.st_size; *(uint64_t *)(d + 48) = s.st_blocks;
        *(uint64_t *)(d + 64) = s.st_atime; *(uint64_t *)(d + 96) = s.st_ctime; *(uint64_t *)(d + 112) = s.st_mtime;
        ret = 0; break; }
    case 137: case 138: { uint8_t *b = (uint8_t *)(nr == 137 ? a1 : a1); memset(b, 0, 120); // statfs/fstatfs
        *(uint64_t *)(b + 0) = 0x01021994; *(uint64_t *)(b + 8) = 4096;
        *(uint64_t *)(b + 16) = 1u << 24; *(uint64_t *)(b + 24) = 1u << 23; *(uint64_t *)(b + 32) = 1u << 23;
        *(uint64_t *)(b + 40) = 1u << 20; *(uint64_t *)(b + 48) = 1u << 19;
        *(uint64_t *)(b + 72) = 255; ret = 0; break; }
    case 60: c->exited = 1; c->exit_code = (int)a0; return;                           // exit
    case 231:                                                                         // exit_group
        if (g_prof) fprintf(stderr, "[prof] dispatcher round-trips=%llu  IBTC fills=%llu  (IBTC %s)\n",
                            (unsigned long long)g_disp_n, (unsigned long long)g_ibtc_fill, g_noibtc ? "OFF" : "ON");
        _exit((int)a0);
    default:
        fprintf(stderr, "[jit86] unhandled syscall %llu (a0=%llx a1=%llx) at rip=%llx\n",
                (unsigned long long)nr, (unsigned long long)a0, (unsigned long long)a1,
                (unsigned long long)c->rip);
        ret = (uint64_t)(-38); break;   // ENOSYS
    }
    // network/socket syscalls report raw host (Darwin) errno -> map to Linux for the guest
    if (((nr >= 41 && nr <= 55) || nr == 288) && (int64_t)ret < 0 && (int64_t)ret >= -134)
        ret = (uint64_t)(-derr((int)(-(int64_t)ret)));
    c->r[RAX] = ret;
    if (g_trace) fprintf(stderr, " = %llx%s\n", (unsigned long long)ret,
                         ((int64_t)ret < 0 && (int64_t)ret > -4096) ? " [ERR]" : "");
}

// CPUID: report an x86-64 baseline CPU with SSE2 ONLY (no SSE3/AVX/BMI), so glibc/musl
// IFUNC resolvers select the SSE2/generic implementations we actually support.
static void do_cpuid(struct cpu *c) {
    uint32_t leaf = (uint32_t)c->r[RAX], a = 0, b = 0, cc = 0, d = 0;
    switch (leaf) {
    case 0:  a = 7; b = 0x756e6547; d = 0x49656e69; cc = 0x6c65746e; break;  // max-leaf=7, "GenuineIntel"
    case 1:  a = 0x000306c3;                                                 // family/model (Haswell-ish id, harmless)
             d = (1u<<0)|(1u<<4)|(1u<<8)|(1u<<11)|(1u<<13)|(1u<<15)|(1u<<19)|(1u<<23)|(1u<<24)|(1u<<25)|(1u<<26); // FPU,TSC,CX8,SEP,PGE,CMOV,CLFSH,MMX,FXSR,SSE,SSE2
             cc = 0; break;                                                  // ecx=0: no SSE3/SSSE3/SSE4/AVX/CX16
    case 7:  a = 0; b = 0; cc = 0; d = 0; break;                             // no AVX2/BMI/etc
    case 0x80000000: a = 0x80000001; break;
    case 0x80000001: d = (1u<<11)|(1u<<29); cc = (1u<<0); break;             // SYSCALL, LM(64-bit), LAHF
    case 0x80000008: a = 0x3027; break;                                      // 48-bit phys, 39-bit virt
    default: break;
    }
    c->r[RAX] = a; c->r[RBX] = b; c->r[RCX] = cc; c->r[RDX] = d;
}

// x87 80-bit extended <-> double conversion (done in C for reliability; libm-free).
// We emulate the ST stack at double precision, so this loses the 80-bit mantissa tail.
static void x87_fld_m80(struct cpu *c) {
    uint8_t *ea = (uint8_t *)c->x87_ea; uint64_t sig; uint16_t se;
    memcpy(&sig, ea, 8); memcpy(&se, ea + 8, 2);
    int s = se >> 15, e = se & 0x7fff; double d;
    if (sig == 0 && e == 0) d = 0.0;
    else {
        d = (double)sig;                                  // ~2^63 (ucvtf)
        int k = e - 16383 - 63;                           // scale exponent
        uint64_t db; memcpy(&db, &d, 8);
        int de = (int)((db >> 52) & 0x7ff) + k;
        if (de <= 0) d = 0.0;
        else if (de >= 0x7ff) { db = (db & (1ull << 63)) | (0x7ffull << 52); memcpy(&d, &db, 8); }
        else { db = (db & ~(0x7ffull << 52)) | ((uint64_t)de << 52); memcpy(&d, &db, 8); }
        if (s) d = -d;
    }
    c->fptop = (c->fptop - 1) & 7; c->st[c->fptop & 7] = d;
}
static void x87_fstp_m80(struct cpu *c) {
    uint8_t *ea = (uint8_t *)c->x87_ea;
    double d = c->st[c->fptop & 7]; c->fptop = (c->fptop + 1) & 7;
    uint64_t db; memcpy(&db, &d, 8);
    int s = (int)(db >> 63), de = (int)((db >> 52) & 0x7ff); uint64_t dm = db & ((1ull << 52) - 1);
    uint64_t sig; uint16_t se;
    if (de == 0) { sig = 0; se = (uint16_t)(s ? 0x8000 : 0); }
    else { sig = (1ull << 63) | (dm << 11); int e80 = de - 1023 + 16383; se = (uint16_t)((s << 15) | (e80 & 0x7fff)); }
    memcpy(ea, &sig, 8); memcpy(ea + 8, &se, 2);
}

// ---------------- dispatcher ----------------
static uint64_t g_prevpc, g_curpc;   // debug: track block transitions for fault diagnosis
static void run_guest(struct cpu *c) {
    pthread_setspecific(g_cpu_key, c);
    while (!c->exited) {
        if (c->rip == SIGRETURN_PC) do_sigreturn(c);                 // handler returned -> restore interrupted context
        if (g_pending) maybe_deliver_signal(c);                      // async signal pending -> redirect to guest handler
        g_prevpc = g_curpc; g_curpc = c->rip; g_disp_n++;
        if (g_trace && g_tracecap && g_disp_n > g_tracecap) {   // bound trace output for runaway guests
            fprintf(stderr, "[jit86] trace cap %llu blocks reached -> stop\n", (unsigned long long)g_tracecap);
            c->exited = 1; c->exit_code = 99; break; }
        if (g_nochain && g_loadbase && c->rip == g_loadbase + 0x2ee0) g_malloc_n++;   // count __libc_malloc_impl entries
        if (g_nochain && g_loadbase) { uint64_t po = g_prevpc - g_loadbase;   // __libc_malloc_impl first-handout: dump the new group's avail_mask (rbp=meta)
            if (po >= 0x32a0 && po <= 0x3340) { uint64_t rbp = c->r[5], rax = c->r[0];
                uint32_t avail = (rbp > 0x10000) ? *(uint32_t *)(rbp + 0x1c) : 0;
                fprintf(stderr, "[av] blk+%llx handout=%llx meta(rbp)=%llx avail_mask[rbp+1c]=%x freed[rbp+18]=%x\n",
                        (unsigned long long)po, (unsigned long long)rax, (unsigned long long)rbp, avail,
                        (rbp > 0x10000) ? *(uint32_t *)(rbp + 0x18) : 0);
            }
        }
        if (g_w8 && *g_w8 != g_w8v) {   // byte-watchpoint: report the block that just changed it
            fprintf(stderr, "[w8] @%p %02x -> %02x  by block +%llx  malloc#=%llu  rsi=%llx\n",
                    (void *)g_w8, g_w8v, *g_w8, (unsigned long long)(g_prevpc - g_loadbase),
                    (unsigned long long)g_malloc_n, (unsigned long long)c->r[6]);
            g_w8v = *g_w8;
        }
        void *code = map_host(c->rip);
        if (!code) {
            if (g_cp + (1u << 16) > g_cache + CACHE_SZ) {
                pthread_jit_write_protect_np(0);
                g_cp = g_cache; memset(g_map, 0, sizeof g_map); g_npend = 0;
                pthread_jit_write_protect_np(1);
                memset(g_ibtc, 0, sizeof g_ibtc);   // body pointers now stale -> drop the cache
            }
            pthread_jit_write_protect_np(0);
            g_emit_start = g_cp;
            code = translate_block(c->rip);
            pthread_jit_write_protect_np(1);
            sys_icache_invalidate(g_emit_start, (size_t)(g_cp - g_emit_start));
        }
        if (c->ic_miss) {                           // IBTC: an indirect branch missed -> cache {target -> body}
            void *body = map_body(c->rip);
            if (body) { uint32_t h = (uint32_t)((c->rip >> 2) & (IBTC_N - 1)); g_ibtc[h].target = c->rip; g_ibtc[h].body = body; g_ibtc_fill++; }
            c->ic_miss = 0;
        }
        if (g_trace) {   // x86 flags derived from cpu->nzcv (convention: stored C = NOT x86 CF)
            unsigned nz = (unsigned)c->nzcv;
            int CF = !((nz >> 29) & 1), ZF = (nz >> 30) & 1, SF = (nz >> 31) & 1, OF = (nz >> 28) & 1;
            fprintf(stderr, "[blk] rip=%llx rax=%llx rbx=%llx rcx=%llx rdx=%llx rsi=%llx rdi=%llx rbp=%llx r8=%llx r9=%llx r10=%llx r11=%llx r12=%llx r13=%llx r14=%llx r15=%llx fl=C%dZ%dS%dO%d\n",
                             (unsigned long long)c->rip, (unsigned long long)c->r[RAX], (unsigned long long)c->r[3], (unsigned long long)c->r[RCX],
                             (unsigned long long)c->r[RDX], (unsigned long long)c->r[RSI],
                             (unsigned long long)c->r[RDI], (unsigned long long)c->r[RBP],
                             (unsigned long long)c->r[8], (unsigned long long)c->r[9],
                             (unsigned long long)c->r[10], (unsigned long long)c->r[11],
                             (unsigned long long)c->r[12], (unsigned long long)c->r[13],
                             (unsigned long long)c->r[14], (unsigned long long)c->r[15], CF, ZF, SF, OF);
        }
        c->reason = 0;
        run_block(c, code);
        if (c->reason == 99) {
            fprintf(stderr, "[jit86] aborting at rip marker %llx (unimplemented opcode)\n",
                    (unsigned long long)c->rip);
            if (g_trace) { for (int rr = 0; rr < 16; rr++) {  // dump heap-pointer regs (meta etc.)
                uint64_t v = c->r[rr];
                if (v > 0x100000000ull && v < 0x200000000ull && (v & 7) == 0) {
                    fprintf(stderr, "  r%d=%llx:", rr, (unsigned long long)v);
                    for (int i = 0; i < 5; i++) fprintf(stderr, " %016llx", (unsigned long long)((uint64_t *)v)[i]);
                    fprintf(stderr, "\n"); } } }
            c->exited = 1; c->exit_code = 70; break;
        }
        if (c->reason == R_CPUID) { do_cpuid(c); continue; }           // rip already = next
        if (c->reason == R_X87FLD)  { x87_fld_m80(c);  continue; }     // fld m80  (rip already = next)
        if (c->reason == R_X87FSTP) { x87_fstp_m80(c); continue; }     // fstp m80
        if (c->reason == R_DIV) {                                      // 128/64 unsigned div (rip already = next)
            uint64_t d = c->divop;
            if (d == 0) { fprintf(stderr, "[jit86] #DE divide-by-zero\n"); c->exited = 1; c->exit_code = 136; break; }
            unsigned __int128 num = ((unsigned __int128)c->r[RDX] << 64) | c->r[RAX];
            c->r[RAX] = (uint64_t)(num / d); c->r[RDX] = (uint64_t)(num % d); continue; }
        if (c->reason == R_IDIV) {                                     // 128/64 signed idiv
            int64_t d = (int64_t)c->divop;
            if (d == 0) { fprintf(stderr, "[jit86] #DE divide-by-zero\n"); c->exited = 1; c->exit_code = 136; break; }
            __int128 num = ((__int128)(int64_t)c->r[RDX] << 64) | c->r[RAX];
            c->r[RAX] = (uint64_t)(num / d); c->r[RDX] = (uint64_t)(num % d); continue; }
        if (c->reason == R_SYSCALL) { service(c); if (c->exited) break;
            if (c->redirect) c->redirect = 0; /* else rip already = next (set at exit) */ }
        // R_BRANCH: c->rip already holds the target
    }
}

// ---------------- minimal ELF loader (load high; copied from jit.c) ----------------
static uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t rd64(const uint8_t *p) { uint64_t v; memcpy(&v, p, 8); return v; }

struct loaded { uint64_t entry, phdr, base; int phent, phnum; };

static int elf_interp(const char *path, char *out, size_t n) {
    int fd = open(path, O_RDONLY); if (fd < 0) return -1;
    struct stat st; fstat(fd, &st);
    uint8_t *f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (f == MAP_FAILED) return -1;
    int r = -1; uint64_t phoff = rd64(f + 32); int phnum = rd16(f + 56), phent = rd16(f + 54);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = f + phoff + (size_t)i * phent;
        if (rd32(ph) == 3) { uint64_t off = rd64(ph + 8), fsz = rd64(ph + 32);
            size_t l = fsz < n ? fsz : n - 1; memcpy(out, f + off, l); out[l] = 0; r = 0; break; }
    }
    munmap(f, st.st_size); return r;
}
static void load_elf(const char *path, struct loaded *out) {
    int fd = open(path, O_RDONLY); if (fd < 0) { perror("open"); exit(1); }
    struct stat st; fstat(fd, &st);
    uint8_t *f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (f == MAP_FAILED) { perror("mmap elf"); exit(1); }
    if (rd16(f + 18) != 0x3E) fprintf(stderr, "[jit86] warning: e_machine=%u (want 62=x86-64)\n", rd16(f + 18));
    uint64_t e_entry = rd64(f + 24), phoff = rd64(f + 32);
    int phnum = rd16(f + 56), phentsize = rd16(f + 54);
    uint64_t minv = ~0ull, maxv = 0;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        uint64_t v = rd64(ph + 16), msz = rd64(ph + 40);
        if (v < minv) minv = v;
        if (v + msz > maxv) maxv = v + msz;
    }
    uint64_t basepage = minv & ~0xFFFull;
    uint64_t span = (maxv - basepage + 0xFFFF) & ~0xFFFFull;
    uint8_t *base = mmap(NULL, span, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED) { perror("mmap base"); exit(1); }
    uint64_t bias = (uint64_t)base - basepage;
    for (int i = 0; i < phnum; i++) {
        uint8_t *ph = f + phoff + (uint64_t)i * phentsize;
        if (rd32(ph) != 1) continue;
        uint64_t off = rd64(ph + 8), v = rd64(ph + 16), fsz = rd64(ph + 32);
        memcpy((void *)(v + bias), f + off, fsz);
    }
    mprotect(base, span, PROT_READ | PROT_WRITE | PROT_EXEC);
    out->entry = e_entry + bias; out->base = (uint64_t)base;
    out->phdr = (uint64_t)base + phoff; out->phent = phentsize; out->phnum = phnum;
    extern int g_diag;
    if (g_trace || g_diag || getenv("JT")) fprintf(stderr, "[LOADED] %s base=%llx span=%llx end=%llx entry=%llx\n", path,
                              (unsigned long long)base, (unsigned long long)span,
                              (unsigned long long)((uint64_t)base + span), (unsigned long long)out->entry);
    munmap(f, st.st_size); close(fd);
}

// Build the SysV x86-64 process stack (identical layout to aarch64). Returns rsp.
static char *g_guest_env[] = { "PATH=/usr/bin:/bin", "HOME=/root", "TERM=dumb", "LANG=C", NULL };
static uint64_t build_stack(int argc, char **argv, struct loaded *lm, uint64_t at_base) {
    size_t SZ = 8u << 20, GUARD = 0x10000;
    // GUARD bytes are mapped ABOVE the logical top: the topmost stack objects are the
    // AT_PLATFORM "x86_64" string and the 16 AT_RANDOM bytes, which glibc strlen/reads
    // with 16-byte SSE loads -> those over-read past the top. Keep that region mapped.
    uint8_t *stk = mmap(NULL, SZ + GUARD, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    uint8_t *top = stk + SZ;
    uint64_t argp[256], envp_[256]; int envc = 0;
    for (int i = 0; i < argc; i++) { size_t l = strlen(argv[i]) + 1; top -= l; memcpy(top, argv[i], l); argp[i] = (uint64_t)top; }
    while (g_guest_env[envc]) envc++;
    for (int i = 0; i < envc; i++) { size_t l = strlen(g_guest_env[i]) + 1; top -= l; memcpy(top, g_guest_env[i], l); envp_[i] = (uint64_t)top; }
    top -= 8; memcpy(top, "x86_64", 7); uint64_t plat = (uint64_t)top;
    top -= 16; arc4random_buf(top, 16); uint64_t rnd = (uint64_t)top;
    top = (uint8_t *)((uint64_t)top & ~15ull);
    uint64_t aux[][2] = {
        {3, lm->phdr}, {4, (uint64_t)lm->phent}, {5, (uint64_t)lm->phnum}, {6, 4096},
        {7, at_base}, {8, 0}, {9, lm->entry}, {11, 0}, {12, 0}, {13, 0},   // AT_UID/EUID/GID -> container root
        {14, 0}, {16, 0}, {15, plat}, {25, rnd}, {23, 0}, {0, 0},          // AT_EGID 0; AT_SECURE 0
    };
    int naux = (int)(sizeof aux / sizeof aux[0]);
    size_t nslots = 1 + (argc + 1) + (envc + 1) + (size_t)naux * 2;
    uint64_t *sp = (uint64_t *)top - nslots;
    sp = (uint64_t *)((uint64_t)sp & ~15ull);
    uint64_t *p = sp;
    *p++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++) *p++ = argp[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++) *p++ = envp_[i];
    *p++ = 0;
    for (int i = 0; i < naux; i++) { *p++ = aux[i][0]; *p++ = aux[i][1]; }
    extern int g_diag;
    if (g_diag) fprintf(stderr, "[stack] base=%p top=%p guard_end=%p sp=%p plat=%llx rnd=%llx\n",
                        (void *)stk, (void *)top, (void *)(stk + SZ + GUARD), (void *)sp,
                        (unsigned long long)plat, (unsigned long long)rnd);
    return (uint64_t)sp;
}

// debug fault handler (only installed under TRACE_ON): print faulting address + guest cpu.
// Lazy-guard fault handler (default): glibc's vectorized string ops (strlen/memchr/
// memcmp) issue 16-byte SSE loads that legitimately over-read past a buffer's end into
// the adjacent page. On Darwin an unmapped page -> SIGBUS. We map the faulting page as
// zero and retry: the zero terminator makes strlen/memchr return the correct result, and
// vectorized loads mask out the bytes past the real end. Bounded so genuine wild
// accesses (a real bug) still abort once the budget is spent.
static _Atomic int g_lazymaps;
void jit86_lazyguard(int sig, siginfo_t *si, void *uc) {
    (void)uc;
    void *a = si ? si->si_addr : NULL;
    if (a && g_lazymaps < 4096) {
        uintptr_t pg = (uintptr_t)a & ~(uintptr_t)0xFFF;
        // macOS won't MAP_FIXED over a sub-range of an existing VM entry (EINVAL); try
        // mprotect first (the page often exists as a PROT_NONE guard), then a fresh map.
        if (mprotect((void *)pg, 0x1000, PROT_READ | PROT_WRITE) == 0) { g_lazymaps++; return; }
        void *r = mmap((void *)pg, 0x1000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (r != MAP_FAILED) { g_lazymaps++; return; }   // retry the faulting instruction
    }
    signal(sig, SIG_DFL); raise(sig);                    // out of budget / mmap failed -> real crash
}
void jit86_faulth(int sig, siginfo_t *si, void *uc) {
    struct cpu *c = (struct cpu *)pthread_getspecific(g_cpu_key);
    static const char *nm[16] = {"rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi","r8","r9","r10","r11","r12","r13","r14","r15"};
    extern uint64_t g_prevpc, g_curpc;
    fprintf(stderr, "[FAULT] sig=%d addr=%p  guest rip(last blk)=%llx  curpc=%llx prevblk=%llx ibranch_src=%llx\n", sig, si ? si->si_addr : 0,
            c ? (unsigned long long)c->rip : 0, (unsigned long long)g_curpc, (unsigned long long)g_prevpc,
            c ? (unsigned long long)c->dbg_ibsrc : 0);
    if (c) for (int i = 0; i < 16; i++) fprintf(stderr, "  %s=%llx%s", nm[i], (unsigned long long)c->r[i], (i % 4 == 3) ? "\n" : "");
    if (c && c->rip) { fprintf(stderr, "  bytes@rip:"); uint8_t *p = (uint8_t *)c->rip;
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", p[i]); fprintf(stderr, "\n"); }
    if (c) { uint64_t pp = c->r[7]; if (pp > 0x100000000ull && pp < 0x200000000ull) {  // rdi: dump chunk header [p-16..p+8)
        fprintf(stderr, "  hdr[rdi-16..p+8):"); uint8_t *b = (uint8_t *)(pp - 16);
        for (int i = 0; i < 24; i++) fprintf(stderr, " %02x", b[i]); fprintf(stderr, "  (p-8 u32=%x p-4 u8=%x p-2 u16=%x)\n",
            *(uint32_t *)(pp - 8), *(uint8_t *)(pp - 4), *(uint16_t *)(pp - 2));
        fprintf(stderr, "  scan-back for group->meta (qword at p-16*off-16):");
        for (int off = 0; off <= 32; off++) { uint64_t bv = *(uint64_t *)(pp - 16 * off - 16);
            if (bv > 0x100000000ull && bv < 0x200000000ull) fprintf(stderr, " off=%d->%llx", off, (unsigned long long)bv); }
        fprintf(stderr, "\n"); } }
    if (c) for (int rr = 0; rr < 16; rr++) {   // dump memory at any reg that looks like a heap pointer
        uint64_t v = c->r[rr];
        if (v > 0x100000000ull && v < 0x200000000ull && (v & 7) == 0) {
            fprintf(stderr, "  mem[%d=%llx]:", rr, (unsigned long long)v);
            for (int i = 0; i < 6; i++) fprintf(stderr, " %016llx", (unsigned long long)((uint64_t *)v)[i]);
            fprintf(stderr, "\n");
            if (rr >= 3) break;   // a couple is enough
        }
    }
    _exit(133);
}

// ---------------- entry ----------------
int jit86_run(const char *rootfs, int argc, char *const argv[]) {
    if (argc < 1 || !argv || !argv[0]) return 2;
    if (rootfs && rootfs[0]) g_rootfs = (char *)rootfs;
    g_hostuid = getuid(); g_hostgid = getgid();   // capture before syscalls fake container root
    { const char *ns = getenv("JIT86_NETNS");      // private-loopback dir: inherit across exec, else create one
      if (ns && ns[0]) snprintf(g_netns, sizeof g_netns, "%s", ns);
      else { char tmpl[64]; snprintf(tmpl, sizeof tmpl, "/tmp/jit86-lo-%d", (int)getpid());
             if (mkdir(tmpl, 0700) == 0 || errno == EEXIST) { snprintf(g_netns, sizeof g_netns, "%s", tmpl); setenv("JIT86_NETNS", g_netns, 1); } } }
    { const char *vs = getenv("JIT86_VOL");        // bind-mount volumes (env path; bridge usually can't pass env, so --vol too)
      if (vs && vs[0]) { char tmp[2048]; snprintf(tmp, sizeof tmp, "%s", vs); char *sv;
        for (char *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) add_vol(t); } }
    { const char *pub = getenv("JIT86_PUBLISH"); if (pub && pub[0] && !g_nportmap) parse_publish(pub); }  // docker -p (inherit across exec)
    { const char *ls = getenv("JIT86_LOWER");      // overlay lower layers (inherit across exec)
      if (ls && ls[0] && !g_nlower) { char tmp[4096]; snprintf(tmp, sizeof tmp, "%s", ls); char *sv;
        for (char *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv)) add_lower(t); } }
    if (g_rootfs) chdir(g_rootfs);   // container model: guest cwd "/" maps to the rootfs root
    const char *prog = argv[0];

    if (pthread_key_create(&g_cpu_key, NULL) != 0) { perror("pthread_key_create"); return 1; }
    g_cache = mmap(NULL, CACHE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (g_cache == MAP_FAILED) { perror("mmap jit"); return 1; }
    g_cp = g_cache;
    g_trace = getenv("JT") != NULL; g_prof = getenv("PROF") != NULL;
    // The OrbStack `mac` bridge does NOT propagate env vars; trace via a trigger file
    // and redirect stderr to a shared log (visible from the Linux side).
    int want_trace = access("runtime/jit86/TRACE_ON", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/TRACE_ON", F_OK) == 0;
    int want_watch = access("runtime/jit86/WATCH", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/WATCH", F_OK) == 0;
    if (want_watch) g_nochain = 1;
    if (access("runtime/jit86/ITRACE_ON", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/ITRACE_ON", F_OK) == 0) { g_itrace = 1; want_trace = 1; }
    if (access("runtime/jit86/PROF", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/PROF", F_OK) == 0) g_prof = 1;
    if (access("runtime/jit86/NOIBTC", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/NOIBTC", F_OK) == 0) g_noibtc = 1;
    int want_fault = want_trace || want_watch || access("runtime/jit86/FAULT_ON", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/FAULT_ON", F_OK) == 0;
    extern int g_diag; g_diag = want_fault;
    if (want_fault) {   // FAULT_ON installs the fault handler WITHOUT slow per-block tracing (chaining stays on)
        if (want_trace) { g_trace = 1; g_tracecap = 200000;   // cap runaway trace volume (override via TRACE_CAP file)
            FILE *cf = fopen("/Users/x/dd/poc/runtime/jit86/TRACE_CAP", "r");
            if (cf) { unsigned long long v = 0; if (fscanf(cf, "%llu", &v) == 1) g_tracecap = v; fclose(cf); } }
        freopen("/Users/x/dd/poc/runtime/jit86/trace.log", "w", stderr); setvbuf(stderr, NULL, _IONBF, 0);
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_flags = SA_SIGINFO;
        extern void jit86_faulth(int, siginfo_t *, void *);
        sa.sa_sigaction = jit86_faulth; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    } else {            // normal runs: lazy-guard handler maps over-read pages and retries
        struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_flags = SA_SIGINFO;
        extern void jit86_lazyguard(int, siginfo_t *, void *);
        sa.sa_sigaction = jit86_lazyguard; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
    }
    g_exe_path = prog;

    char pb[4200]; const char *prog_host = xresolve(prog, pb, sizeof pb);
    struct loaded lm; load_elf(prog_host, &lm);
    g_loadbase = lm.base;

    uint64_t jump = lm.entry, at_base = 0; char interp[256];
    if (elf_interp(prog_host, interp, sizeof interp) == 0) {
        char ib[4200]; const char *ihost = xlate(interp, ib, sizeof ib);
        struct loaded li; load_elf(ihost, &li);
        jump = li.entry; at_base = li.base;
    }

    uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    brk_lo = brk_cur = (uint64_t)heap; brk_hi = brk_lo + (256u << 20);

    struct cpu c; memset(&c, 0, sizeof c);
    c.r[RSP] = build_stack(argc, (char **)argv, &lm, at_base);   // rsp -> argc
    c.r[RDX] = 0;                                                 // rtld_fini = 0
    c.rip = jump;

    run_guest(&c);
    if (g_prof) fprintf(stderr, "[prof] dispatcher round-trips=%llu  IBTC fills=%llu  (IBTC %s)\n",
                        (unsigned long long)g_disp_n, (unsigned long long)g_ibtc_fill, g_noibtc ? "OFF" : "ON");
    return c.exit_code;
}

#ifndef JIT86_LIB
int main(int argc, char **argv) {
    int ai = 1; const char *rootfs = NULL;
    static char self[4200]; if (realpath(argv[0], self)) g_self_path = self; else g_self_path = argv[0];
    while (ai + 1 < argc) {                                  // --rootfs DIR / --vol guest:host (repeatable)
        if (strcmp(argv[ai], "--rootfs") == 0) { rootfs = argv[ai + 1]; ai += 2; }
        else if (strcmp(argv[ai], "--vol") == 0) { add_vol(argv[ai + 1]); ai += 2; }
        else if (strcmp(argv[ai], "--publish") == 0 || strcmp(argv[ai], "-p") == 0) {   // docker -p H:C (port-map)
            parse_publish(argv[ai + 1]); setenv("JIT86_PUBLISH", argv[ai + 1], 1); ai += 2; }
        else if (strcmp(argv[ai], "--lower") == 0) { add_lower(argv[ai + 1]); ai += 2; }  // overlay read-only layer
        else break;
    }
    if (ai >= argc) { fprintf(stderr, "usage: %s [--rootfs DIR] [--vol guest:host]... [-p H:C]... <x86-64-elf> [args...]\n", argv[0]); return 2; }
    return jit86_run(rootfs, argc - ai, argv + ai);
}
#endif
