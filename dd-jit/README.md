# dd-jit

The VM-less JIT container runtime (C) plus its Rust bindings. `cargo build` runs `build.rs`, which
compiles and codesigns **one JIT binary per guest target** through the macOS toolchain (`MAP_JIT` + the
`allow-jit` entitlement); the library exposes `Guest`, the typed [`SpawnConfig`] launch contract, and
the built binaries' paths.

```
cargo build                 # build.rs -> one binary per target (linux/aarch64, linux/x86_64, darwin/aarch64), codesigned
cargo test                  # bindings unit + doc tests
```

On a non-macOS dev host (e.g. OrbStack Linux), `build.rs` drives clang/codesign through the `mac`
bridge automatically; on a real Mac it runs them directly.

> **What lives where, and why** is documented in [`docs/STRUCTURE.md`](../docs/STRUCTURE.md) — the
> engine/frontend/OS-personality split and the cpu-interface contract that keeps `os/linux/` shared.

## The matrix: guest-OS × guest-ISA

A *target* is a `(guest OS, guest ISA)` pair. Three are built today, all hosted on darwin/arm64:

| target | what | guest binaries |
|---|---|---|
| `linux/aarch64` | jit — same-ISA transliteration | Linux ELF aarch64 |
| `linux/x86_64`  | jit86 — x86→arm64 translation | Linux ELF x86-64 |
| `darwin/aarch64`| jitdarwin — native macOS, no VM | macOS Mach-O arm64 |

## Layout

```
src/lib.rs                       Rust: Guest (3 targets), SpawnConfig, the built-binary paths
build.rs                         compiles + codesigns targets/<target>.c for each target
.clang-format                    C style (4-space, 120-col) — `make fmt`
src/runtime/
  include/cpu_{aarch64,x86_64}.h guest CPU layouts
  jit/                           host-aarch64 engine: code cache, emitters, dispatcher        (host ISA)
  frontend/aarch64/              aarch64 transliterator (mangle + §B + LSE + depth-gate + PAC) (guest ISA)
  frontend/x86_64/               x86-64 JIT (jit86): cache, emit+SSE+x87, decode, translate,
                                 container, service, dispatch, elf                            (guest ISA)
  os/linux/                      Linux personality: service (one sorted switch), elf, threads, signals,
  os/linux/container/              path-jail VFS, overlay, netns, cgroup, /proc synth          (guest OS)
  os/darwin/jitdarwin.c          macOS personality (jitdarwin, whole-imported)                (guest OS)
  hal/darwin/                    host primitives (seam; currently inline)                      (host OS)
  targets/{linux_aarch64,linux_x86_64,darwin_aarch64}.c   one unity TU per target
```

## Decomposition state

- **linux/aarch64** — fully decomposed into `jit/` + `os/linux/` + `frontend/aarch64/`; syscalls are one
  sorted `service.c` (no `.inc` fragments); clang-formatted, comments above the code.
- **linux/x86_64 (jit86)** — decomposed into `frontend/x86_64/` modules (mirrors the aarch64 split), but
  still carries its *own* cpu struct + container/syscall layer (it isn't yet sharing `jit/` + `os/linux/`).
  Re-pull the latest from poc with `make sync-jit86` (re-slices by section banner).
- **darwin/aarch64 (jitdarwin)** — minimal POC, brought in *whole* under `os/darwin/`. `make sync-darwin`.

**Dedup (the remaining refactor):** lift jit86 and jitdarwin onto the shared `jit/` engine + (for jit86)
`os/linux/` via cpu-access accessors + a syscall-number→canonical-id map per frontend. The aarch64 host
codegen is identical across all three — only the decoder, the OS personality, and the syscall numbers
differ. End state: one engine, thin per-target frontends.
