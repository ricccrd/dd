// dd/runtime/targets -- darwin_aarch64: the macOS-guest JIT runner (unity TU). Currently includes the
// whole jitdarwin implementation; will share jit/ + frontend/aarch64 with the linux targets after the
// dedup, keeping only os/darwin (Mach-O loader + Mach traps + BSD syscall map).
#include "../os/darwin/jitdarwin.c"
