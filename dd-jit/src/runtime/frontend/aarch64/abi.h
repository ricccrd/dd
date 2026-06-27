// frontend/aarch64/abi.h -- the aarch64 guest's implementation of the cpu interface that os/linux/ uses.
//
// THE CONTRACT. Every Linux-guest frontend provides these so the os/linux/ personality (syscalls,
// container, ELF) stays frontend-agnostic and is genuinely SHARED, not copied:
//   G_NR(c)            the CANONICAL syscall number  [rvalue]   (os/linux switches on the aarch64 ABI
//                                                                numbers; a frontend whose guest numbers
//                                                                differ maps them to canonical here)
//   G_A0(c)..G_A5(c)   the syscall argument registers [rvalue]
//   G_RET(c)           the syscall return register    [lvalue]
//
// Remaining per-arch seams (documented in docs/STRUCTURE.md), provided by the frontend when it wires
// into the shared service: struct layouts (stat/sigaction differ on aarch64 vs x86_64), the §B shadow
// stack (`ssp`, aarch64-only), TLS, and sigaltstack.
//
// aarch64 is the reference: syscall regs are x8 / x0..x5 / x0, and its ABI numbers ARE canonical.
#define G_NR(c) ((c)->x[8])
#define G_A0(c) ((c)->x[0])
#define G_A1(c) ((c)->x[1])
#define G_A2(c) ((c)->x[2])
#define G_A3(c) ((c)->x[3])
#define G_A4(c) ((c)->x[4])
#define G_A5(c) ((c)->x[5])
#define G_RET(c) ((c)->x[0])

// PC / SP / TLS / §B shadow-stack reset — the remaining per-arch cpu state os/linux touches.
#define G_PC(c) ((c)->pc)
#define G_SP(c) ((c)->sp)
#define G_TLS(c) ((c)->tls)
#define G_SHADOW_RESET(c) ((c)->ssp = 0) // reset the §B shadow stack (fork/exec); no-op on engines without it

// Child thread resume PC: aarch64 services a syscall with pc still at the SVC, so advance +4.
#define G_THREAD_RESUME(child, parent) ((child)->pc = (parent)->pc + 4)


// aarch64 guests already use canonical (*at) syscalls -> nothing to normalize.
#define G_NORMALIZE(c) 0

// Zero the integer register file (execve). aarch64 = x[31].
#define G_RESET_REGS(c) memset((c)->x, 0, sizeof (c)->x)

// brk policy: aarch64 grows a real brk heap (works on macOS).
#define G_BRK_GROWABLE 1
