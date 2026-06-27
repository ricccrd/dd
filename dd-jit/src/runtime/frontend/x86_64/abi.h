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

// brk policy: x86 reports a fixed break so glibc uses its mmap allocator -- a brk heap the guest then
// mmap/mprotects cannot be split on the macOS VM. (jit86 learned this the hard way.)
#define G_BRK_GROWABLE 0
