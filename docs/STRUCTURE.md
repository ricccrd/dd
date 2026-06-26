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

## Dedup — foundation complete

The cpu-interface SEAM is built and verified:
- `os/linux/service.c` is fully cpu-agnostic (the `G_NR/A0..A5/RET/PC/SP/TLS/SHADOW_RESET` contract; 0
  raw register/field references) — the shared Linux syscall layer.
- `frontend/aarch64/abi.h` + `frontend/x86_64/abi.h` implement the contract for each guest.
- `frontend/x86_64/sysmap.h` maps x86 syscall numbers -> the canonical (aarch64) numbers.

## Finishing the dedup (the concrete remaining work)

The seam is done (above). The remaining swap, to make x86-64 *use* shared `os/linux/` instead of its copy:

1. **Legacy-syscall shim** — x86-64 has 58 legacy syscalls aarch64 lacks (`open`/`stat`/`pipe`/`access`/
   `dup2`/`getdents`/`rename`/`mkdir`/…). A thin x86 entry translates each to its `*at` canonical form
   (e.g. `open(p,fl,m)` → `openat(AT_FDCWD,p,fl,m)`) before delegating to the shared `service()`.
2. **Per-arch struct fills** — move `fill_linux_stat` (+ statx/sigaction) out of the shared layer into
   each frontend (both already define it with the same signature, just different layouts).
3. **Wire** `targets/linux_x86_64.c` to shared `os/linux/` + `jit/` + only x86-specific
   `frontend/x86_64/{cpu,decode,translate,emit,abi,sysmap,sigframe}`; delete the x86 service/container/
   cache/elf copies.

**The verification gate is now satisfied:** the dense matrix (x86 cross-compiler + qemu oracle) runs
the same ~25 guest programs on x86, exercising the common syscall + struct-fill paths — so the swap is
checked, not hopeful. (The 5 `xfail`'d jit86 codegen bugs are orthogonal and stay tracked.)

The same pattern lifts **jitdarwin** onto the shared `jit/` engine — it already shares the entire
aarch64 host codegen; only the Mach-O loader, the Darwin syscall map, and Mach traps are darwin-specific.
