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

**What lives where, and why** — the engine/frontend/OS-personality split and the cpu-interface contract
that keeps `os/linux/` shared — is laid out in *Layout* and *Decomposition state* below.

## The matrix: guest-OS × guest-ISA

A *target* is a `(guest OS, guest ISA)` pair. Three are built today, all hosted on darwin/arm64:

| target | what | guest binaries |
|---|---|---|
| `linux/aarch64` | jit — same-ISA transliteration | Linux ELF aarch64 |
| `linux/x86_64`  | jit86 — x86→arm64 translation | Linux ELF x86-64 |
| `darwin/aarch64`| jitdarwin — native macOS, no VM | macOS Mach-O arm64 |

## Layout

```
src/lib.rs                        Rust: Guest (3 targets), SpawnConfig, the built-binary paths
build.rs                          compiles + codesigns targets/<target>.c for each target
.clang-format                     C style (4-space, 120-col) — `make fmt`
src/runtime/
  include/cpu_{aarch64,x86_64}.h  guest CPU layouts
  engine/                         host-ISA-agnostic JIT core: code cache, dispatcher, stubs   (engine)
  host/arm64/asm.c                ARM64 assembler — emit32 + the e_* encoders                 (host ISA)
  translate/aarch64/              aarch64→ARM64 frontend (mangle + §B + LSE + depth-gate + PAC)(guest ISA)
  translate/x86_64/               x86-64→ARM64 frontend (jit86): decode, emit, avx, x86_ops,
                                  elf, pcache + translate/{alu,mov,shift,repstr,x87,trace}.c   (guest ISA)
  os/linux/                       Linux personality: syscall/ (split dispatcher), elf, thread,
  os/linux/container/               signal; path-jail VFS, overlay, netns, /proc synth         (guest OS)
  os/darwin/                      macOS personality: jitdarwin DBT + jail/                     (guest OS)
  targets/{linux_aarch64,linux_x86_64,darwin_aarch64}.c   one unity TU per target, entry dd_run
```

The two **axes** are now distinct directories: an `engine/` (host-ISA-agnostic JIT core) and a
`host/arm64/` assembler form the *host* side; `translate/<isa>/` frontends and `os/<os>/` personalities
form the *guest* side. A `targets/<os>_<isa>.c` unity TU `#include`s one frontend + one OS personality
+ the shared engine/host, and exposes a single entry, `dd_run`.

## Decomposition state

- **linux/aarch64** — fully decomposed into `engine/` + `host/arm64/` + `translate/aarch64/` +
  `os/linux/`. The syscall dispatcher is split by family under `os/linux/syscall/` (`fs.c`, `io.c`,
  `mem.c`, `net.c`, `proc.c`, `signal.c`, `sysv.c`, `time.c`, `event.c`, …) behind `dispatch.c` — no
  `.inc` fragments; clang-formatted, comments above the code.
- **linux/x86_64 (jit86)** — decomposed into `translate/x86_64/` modules (mirrors the aarch64 split, with
  the big instruction switch further split into `translate/{alu,mov,shift,repstr,x87,trace}.c`) and
  **already sharing the whole `os/linux/` personality** (the syscall split, the container overlay/jail/
  netns, `signal.c`, `thread.c`, `fscache.c`) through the `abi.h` `G_*` seam + the `sysmap.h`
  syscall-number→canonical map. It still carries its *own* cpu struct (genuinely per-ISA) and its *own*
  engine glue (`translate/x86_64/engine_glue.c`, `pcache.c`) rather than the shared `engine/`.
- **darwin/aarch64 (jitdarwin)** — minimal native-macOS DBT + the DYLD-interpose `jail/`, under `os/darwin/`.

**Dedup (the remaining refactor):** the `os/linux/` half is done for jit86; what's left is lifting its
engine glue onto the shared `engine/` (and jitdarwin onto `engine/` + the darwin personality) via
cpu-access accessors. The `host/arm64/` codegen is identical across all three — only the decoder, the OS
personality, and the syscall numbers differ. End state: one engine, thin per-target frontends.
