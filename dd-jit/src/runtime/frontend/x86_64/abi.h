// frontend/x86_64/abi.h -- the x86-64 guest's implementation of the cpu interface os/linux/ is written
// against (the contract documented in frontend/aarch64/abi.h). With this + sysmap.h + a per-arch
// fill_linux_stat, the x86 target can drop its own service.c/container.c and share os/linux/.
//
// x86-64 cpu: r[16] = rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15. Linux x86-64 syscall ABI:
//   number = rax ; args = rdi,rsi,rdx,r10,r8,r9 ; return = rax.
#include "sysmap.h" // canon_x86(): x86 syscall number -> canonical (aarch64) number

#define G_NR(c) canon_x86((long)(c)->r[0]) // rax -> canonical number os/linux/service.c switches on
#define G_A0(c) ((c)->r[7])                // rdi
#define G_A1(c) ((c)->r[6])                // rsi
#define G_A2(c) ((c)->r[2])                // rdx
#define G_A3(c) ((c)->r[10])               // r10
#define G_A4(c) ((c)->r[8])                // r8
#define G_A5(c) ((c)->r[9])                // r9
#define G_RET(c) ((c)->r[0])               // rax

// Engine seam: the shared jit/cache.c hashes the guest PC as (gpc >> G_GPC_HASH_SHIFT). x86 PCs are
// byte-granular, so do not shift (>>0) -- matches the original frontend/x86_64/cache.c hash.
#define G_GPC_HASH_SHIFT 0

#define G_PC(c) ((c)->rip)
#define G_SP(c) ((c)->r[4])                // rsp
#define G_TLS(c) ((c)->fs_base)            // x86 TLS base (arch_prctl SET_FS)
#define G_SHADOW_RESET(c) ((void)0)        // no §B shadow stack on the x86 frontend

// Child thread resume PC: x86 pre-advances rip past `syscall` before servicing, so the copy is correct.
#define G_THREAD_RESUME(child, parent) ((void)0)


// Syscall normalization: x86 rewrites legacy syscalls to their *at form (frontend/x86_64/legacy.c).
#define G_NORMALIZE(c) x86_normalize(c)

// Zero the integer register file (execve). x86 = r[16].
#define G_RESET_REGS(c) memset((c)->r, 0, sizeof (c)->r)

// uname(2) `machine` field — per guest ISA, so an x86-64 guest reports "x86_64" (not the host "aarch64").
#define G_UNAME_MACHINE "x86_64"

// brk policy: x86 reports a fixed break so glibc uses its mmap allocator -- a brk heap the guest then
// mmap/mprotects cannot be split on the macOS VM. (jit86 learned this the hard way.)
#define G_BRK_GROWABLE 0

// Open-flag bits that DIFFER by guest arch (the high O_* group): x86-64 has its own values for the
// O_DIRECTORY/O_NOFOLLOW group, distinct from aarch64's asm-generic ones (see frontend/aarch64/abi.h).
#define G_O_DIRECTORY 0x10000 // x86-64 O_DIRECTORY = 0200000
#define G_O_NOFOLLOW  0x20000 // x86-64 O_NOFOLLOW  = 0400000

// W5B tier-2: extra PROF line at exit_group (x86 engine only; aarch64 leaves G_PROF_EXTRA undefined).
// g_prof_t2 lives in the shared jit/cache.c, g_prof_t2fold in frontend/x86_64/engine_glue.c -- both
// defined before the shared service.c is #included in the x86 unity TU.
#define G_PROF_EXTRA                                                                                                    \
    do {                                                                                                                \
        if (getenv("PROF"))                                                                                            \
            fprintf(stderr, "[prof] tier2=%llu tier2_fold_elide=%llu\n", (unsigned long long)g_prof_t2,               \
                    (unsigned long long)g_prof_t2fold);                                                                 \
    } while (0)
