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

## Dedup state

- **linux/aarch64** is the **canonical reference**: fully decomposed, and `os/linux/service.c` is written
  against the cpu-interface contract (`frontend/aarch64/abi.h`) — it names no aarch64 register. This is
  the shared Linux personality.
- **linux/x86_64 (jit86)** is decomposed into `frontend/x86_64/` but still carries **parallel copies** of
  the OS layer (`service.c`, `container.c`, `cache.c`, `elf.c`). Wiring it onto the shared `os/linux/` is
  the remaining dedup (next section).
- **darwin/aarch64 (jitdarwin)** is a minimal POC, whole-imported under `os/darwin/`.

## Finishing the dedup (the concrete remaining work)

To make x86-64 *use* the shared `os/linux/` instead of its copy, the x86 frontend must supply:

1. **`frontend/x86_64/abi.h`** — `G_NR` mapping x86 `rax` → the canonical (aarch64) syscall number via a
   number table; `G_A0..G_A5` = `rdi/rsi/rdx/r10/r8/r9`; `G_RET` = `rax`.
2. **Per-arch struct fills** — `fill_stat`/`fill_statx`/sigaction in `frontend/x86_64/` (x86_64 layouts),
   called by `os/linux/service.c` through a small hook (the aarch64 fills move behind the same hook).
3. **A few per-arch hooks** — the §B shadow-stack reset is a no-op for x86 (no `ssp`); TLS reads
   `fs_base`; sigaltstack uses common fields.

Then `targets/linux_x86_64.c` includes the shared `os/linux/` + `jit/` + only the x86-specific
`frontend/x86_64/{cpu,decode,translate,emit,abi,sigframe}`, and the x86 copies of service/container/
cache/elf are deleted. **Gate:** this swap needs broader x86 test coverage first (today: 5 bare
fixtures, no container cases) so the change is verifiable rather than hopeful — jit86 is also under
active bug-fixing upstream (the base32 NEON path), so the swap lands cleanly after it stabilizes.

The same pattern lifts **jitdarwin** onto the shared `jit/` engine — it already shares the entire
aarch64 host codegen; only the Mach-O loader, the Darwin syscall map, and Mach traps are darwin-specific.
