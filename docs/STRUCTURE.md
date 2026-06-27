# Structure — what lives where, and why

dd runs a guest by JIT-translating its code and servicing its syscalls in userspace. A **target** is a
`(guest OS, guest ISA)` pair. Everything is organized along the axes that vary independently between
targets, so each file has exactly one reason to change.

```
dd-jit/src/runtime/
  jit/            host engine        — varies by HOST ISA (always arm64): code cache, host emitters, dispatch
  os/linux/       Linux personality  — varies by GUEST OS: syscalls, container, ELF, threads, signals
  os/darwin/      Darwin personality — varies by GUEST OS: jitdarwin (Mach-O + Mach traps)
  frontend/<isa>/ guest decoder      — varies by GUEST ISA: cpu state, decode, translate, ABI seam
  include/        shared headers     — cpu layouts + the cpu-interface contract
  targets/        one unity TU per target = host engine + guest frontend + guest OS personality
```

A target's `.c` is just a list of `#include`s that assembles those three layers. That is the whole
mental model: **engine + frontend + OS personality**.

## What is SHARED vs PER-FRONTEND, and why

| layer | shared? | why |
|---|---|---|
| `jit/` (code cache, host emitters, dispatch) | shared across guest ISAs | the host is always arm64; emitters take register *numbers*, not guest state |
| `os/linux/` (syscalls, container, ELF, FS-cache) | **the goal — shared across guest ISAs** | the Linux ABI is the same regardless of how the guest encodes it |
| `frontend/<isa>/cpu_*.h` | per-ISA | the guest register file differs (aarch64 `x[31]` vs x86 `r[16]` + xmm + x87) |
| `frontend/<isa>/decode/translate/emit` | per-ISA | decoding x86 ≠ aarch64; flag synthesis, SSE/x87 are x86-only |
| `frontend/<isa>/abi.h` | per-ISA | the seam that lets `os/linux/` be shared (below) |
| `stat`/`sigaction` struct fills | per-ISA | **Linux struct layouts differ on aarch64 vs x86_64** |

## The cpu-interface contract (the seam)

`os/linux/` never names a guest register directly. It talks to the guest CPU only through the macros a
frontend provides in `frontend/<isa>/abi.h`, included by the target TU before `os/linux/`:

```c
G_NR(c)            the canonical syscall number   (rvalue)
G_A0(c)..G_A5(c)   the syscall argument registers (rvalue)
G_RET(c)           the syscall return register    (lvalue)
```

`os/linux/service.c` switches on the **aarch64 syscall numbers** (the canonical set, since aarch64 is
the reference). A frontend whose guest uses different numbers (x86-64) maps them to canonical in
`G_NR`. So one `service()` serves every Linux guest; the frontend only supplies the mapping.

## Dedup state — DONE for Linux

- **linux/aarch64** and **linux/x86_64** now **share the entire `os/linux/` personality** (`service.c`,
  `container/`, `signal.c`, `fscache.c`). Each frontend supplies only what is genuinely per-arch:
  - the engine + decoder (`cache/emit/decode/translate/dispatch`, x86-only by nature),
  - `abi.h` (the cpu-interface seam + the x86→canonical syscall map `sysmap.h`),
  - `sigframe.c` (rt_sigframe register layout), `fill_stat.c` (struct-stat byte layout),
  - `legacy.c` (`G_NORMALIZE`: legacy→`*at` rewrite + arch_prctl), `x86_ops.c` (cpuid/x87), `elf.c`.
  - jit86's old `service.c` + `container.c` (2277 lines) are **deleted**.
- **darwin/aarch64 (jitdarwin)** is a minimal POC, whole-imported under `os/darwin/` — the same pattern
  would lift it onto the shared `jit/` engine next.

The contract (`G_*`) that makes one `os/linux/` serve both: `NR/A0..A5/RET` (syscall ABI), `PC/SP/TLS`
(named registers), `SHADOW_RESET`/`THREAD_RESUME` (§B + thread entry), `RESET_REGS` (execve),
`NORMALIZE` (legacy-syscall rewrite), `BRK_GROWABLE` (brk policy). Three macOS-isms live in the shared
service, harmless to aarch64: mprotect is a no-op, anon mmaps get a guard tail, brk is non-growable on x86.

