# Architecture

## The one idea

`dd` runs a Linux container by **being its kernel in userspace**. There is no VM and no Linux kernel. A
JIT translates the guest's machine code and traps every syscall instruction; the trap handler —
`service()` in `dd-jit/src/runtime/os/linux/` — *is* the Linux syscall ABI, implemented against the
macOS host. Everything a real kernel owns (the filesystem view, PIDs, namespaces, cgroups, networking)
is therefore ordinary state we keep in this process. This is the gVisor/PRoot lineage: a userspace
kernel, with none of a VM's virtualization cost — the guest's *compute* runs as native host
instructions; only its *syscalls* are interpreted.

## Why a JIT (even same-ISA)

For the aarch64 guest the host is the same ISA, so most instructions are copied verbatim
("transliteration"). We still translate in order to (1) **trap** the syscall/branch boundary, (2)
**steal** a few host registers for the engine and rewrite ("mangle") the guest instructions that touch
them, and (3) **optimize** idioms the original binary couldn't assume. For the x86-64 guest, the JIT
additionally **decodes** x86 and **synthesizes** its flags and SSE/x87 semantics on arm64.

## Workspace

```
dd-jit/      the JIT runtime (C) + Rust bindings; build.rs compiles + codesigns one binary per guest arch
dd-daemon/   the Docker Engine API daemon; depends on dd-jit
docs/        ARCHITECTURE, OPTIMIZATIONS, SYSCALLS, PLAN
```

`dd-jit` is where the engine lives. `build.rs` turns each `src/runtime/ddjit_<arch>.c` unity TU into a
codesigned executable (the `MAP_JIT` entitlement is required for W^X JIT memory on macOS) and exports
its path to the crate; `src/lib.rs` exposes `Guest` + the `SpawnConfig` launch contract. The daemon
detects an image's arch from its ELF and spawns the matching binary.

## The layers (the axes that vary independently)

The C is decomposed along the four axes you'd re-target when porting: host-OS × host-ISA × guest-ISA ×
guest-OS.

```
dd-jit/src/runtime/
  frontend/aarch64/   guest ISA  — decode + mangle + §B + LSE + depth-gate          (per guest ISA)
  frontend/x86_64/    guest ISA  — jit86: decode + flag synth + SSE/x87 (whole)      (per guest ISA)
  jit/                host ISA   — code cache, arm64 host emitters, dispatcher        (per host ISA)
  os/linux/           guest OS   — syscalls (one sorted service.c), ELF, threads, signals, fscache
  os/linux/container/ guest OS   — path-jail VFS, overlay, netns, cgroup, /proc synth
  hal/darwin/         host OS    — MAP_JIT, W^X, Mach exceptions, icache  (seam; currently inline)
  ddjit_aarch64.c     unity TU -> aarch64 binary
  ddjit_x86_64.c      unity TU -> x86_64 binary
```

`jit/` (host engine) and `os/linux/` (Linux personality) are **guest-ISA-agnostic** — the goal is for
both frontends to share them. The aarch64 guest already does. The x86-64 guest (jit86) has a different
cpu struct and its own basic runtime, so it's brought in whole for now; the **dedup** (see `PLAN.md`)
lifts it onto the shared layer via cpu-access accessors + a syscall-number→canonical-id map.

## Execution flow

1. **Load** (`os/linux/elf.c`): map the guest ELF (static-PIE, or dynamic via its ld.so), build the
   initial stack (argv/envp/auxv with the container uid/gid).
2. **Dispatch** (`jit/dispatch.c`): look up the guest PC in the block map; on a miss, `translate_block`;
   enter the translated block through the host trampoline with the guest registers live.
3. **Run**: the block runs as native host code until a terminator — a direct branch (chained straight to
   the next block), an indirect branch (IBTC / per-site inline cache), or a syscall.
4. **Syscall**: exit to `service()`, which switches on the number, translates the ABI to macOS, and
   passes every path through the container VFS jail. Return to the dispatcher.
5. **Repeat** until `exit_group`.

## The container is kernel state

Because we *are* the kernel, container features are how the syscalls behave: the **VFS jail** confines
every path to the rootfs (TOCTOU-free) across **overlay** layers (copy-up, `.wh.` whiteout, merged
`getdents`); the **PID** ns makes init PID 1; **UTS** drives `uname`; **USER** runs as uid 0 by default;
**cgroups** charge `mmap`/`brk` against `memory.max` (OOM at the limit) and cap `pids.max`; the **net**
ns reroutes the container's `127.0.0.0/8` to private AF_UNIX sockets and `-p` publishes ports.

## Trust model

Single-process today — fine for **trusted** images. For **untrusted** images the design splits into a
seccomp/Seatbelt-locked *sandbox* task (runs guest code) and a *sentry* (holds kernel state, services
syscalls over a ring): the guest runs unchecked native loads/stores, so only a separate process it
cannot corrupt is a real boundary. See `PLAN.md`.
