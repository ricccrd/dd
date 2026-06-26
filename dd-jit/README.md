# dd-jit

The VM-less JIT container runtime (C) plus its Rust bindings. `cargo build` runs `build.rs`, which
compiles and codesigns **one JIT binary per guest architecture** through the macOS toolchain
(`MAP_JIT` + the `allow-jit` entitlement); the library exposes the typed [`SpawnConfig`] launch
contract and the built binaries' paths.

```
cargo build                 # build.rs -> bin per guest arch (aarch64 + x86_64), codesigned
cargo test                  # bindings unit + doc tests
```

On a non-macOS dev host (e.g. OrbStack Linux), `build.rs` drives clang/codesign through the `mac`
bridge automatically; on a real Mac it runs them directly.

## Layout

```
src/lib.rs                       Rust: Guest, SpawnConfig, the built-binary paths
build.rs                         compiles + codesigns ddjit_<arch>.c for each guest
.clang-format                    C style (4-space, 120-col) — `make fmt`
src/runtime/                     the C runtime
  include/cpu_aarch64.h          guest CPU layout (aarch64)
  jit/                           engine: code cache, aarch64 host emitters, dispatcher   (host-ISA)
  os/linux/                      Linux personality: service (one sorted switch), elf, threads, signals
  os/linux/container/            path-jail VFS, overlay, netns, cgroup, /proc synth
  frontend/aarch64/              the aarch64 transliterator (mangle + §B + LSE + depth-gate)
  frontend/x86_64/jit86.c        the x86-64 JIT — brought in WHOLE (see below)
  hal/darwin/                    host primitives (seam; currently inline)
  ddjit_aarch64.c                unity TU -> aarch64 binary
  ddjit_x86_64.c                 unity TU -> x86_64 binary
```

## Two guests, one runtime — current state

The **aarch64** guest is fully decomposed into `jit/` + `os/linux/` + `frontend/aarch64/` (the target
structure; syscalls are one sorted `service.c`, no `.inc` fragments).

The **x86-64** guest (`jit86`) is brought in *whole* for now. It is under active improvement upstream
(`poc/runtime/jit86/jit86.c`) and has a different cpu struct + its own (basic) container layer, so it
isn't yet decomposed onto the shared `jit/` + `os/linux/`. Re-sync the latest with `make sync-jit86`.

**Dedup (next stage):** lift jit86 onto the shared engine + container layer (overlay/cgroup/jail it
currently lacks) via cpu-access accessors + a canonical syscall-id map, then split it the way aarch64
already is. One runtime, two thin frontends.
