# PLAN — remaining work

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings; `build.rs` builds both guest-arch
binaries) and `dd-daemon` (Docker Engine API). This file is the **work list only**.

## Next work (priority order)
1. **Dedup jit86 onto the shared layer.** The x86-64 guest is brought in whole (`frontend/x86_64/jit86.c`)
   with its own cpu struct + a *basic* container layer (no overlay/cgroup/jail). Lift it onto the shared
   `jit/` + `os/linux/` via (a) cpu-access accessors (`SYSARG0..5(c)`, `SYSRET(c,v)`, PC/SP getters) so
   the syscall + dispatch logic is cpu-agnostic, and (b) a syscall-number→canonical-id map per frontend
   (x86 `read`=0 vs aarch64 63). Then decompose it the way aarch64 is. Payoff: one runtime, two thin
   frontends; jit86 gains overlay/cgroup/jail for free.
2. **OCI image pull/unpack** (in `dd-daemon`, a `dd-image` module): registry pull (manifest+config+layer
   blobs), verify digests, untar each layer to a content-addressed dir, hand the stack to `SpawnConfig`
   as `lowers` (ro) + a writable `rootfs` (upper). Today `DD_IMAGES` must point at pre-extracted rootfs.
3. **Rare fork/command-sub `Bus error`.** ~1/15 intermittent fault in the fork + nested command-sub path
   under full container flags (pre-existing in the engine). Likely a shadow-stack/teardown race;
   reproduce under load and fix.
4. **Networking Phase-2b — userspace netstack.** A real TCP/IP stack for *external* traffic with NET-ns
   interfaces/routes + tunnel egress. Loopback isolation + port-map already cover the common case.
5. **Untrusted-guest isolation — the sentry process-split.** Seccomp/Seatbelt-locked sandbox task +
   trusted sentry over a syscall ring. Required only for untrusted images.
6. **Tier-2 trace optimizer.** Trace formation over `PROF`, cross-trace register allocation (removes the
   per-block spill — the main remaining overhead), monomorphic-comparator inlining, purity-gate
   memoization. Constraint: do not use dead-register §B scratch (unsafe); don't drop the §B gsp check.

## Remaining JIT gaps
- **ET_EXEC loader** (non-PIE static) — platform-blocked by macOS `__PAGEZERO`; needs a fixed-vaddr map.
- **IPC namespace** — SysV/POSIX shm/sem/msg per IPC-ns.
- **cpu/io cgroup** — only mem+pids enforced (cpu/io best-effort on macOS).
- **`pidfd`, `io_uring`** — no macOS primitive; defer.

## Build / packaging
- `make jit` (= `cargo build --release`) builds both JIT binaries via `build.rs` and both crates.
- `make fmt` runs clang-format on the decomposed C (skips the whole-imported jit86.c).
- `make sync-jit86` pulls the latest improved jit86 from the upstream tree.
- For distribution: package the codesigned binaries + the daemon; a `make install` target and a release
  workflow are TODO.

## Portability matrix (seams exist; only darwin-host / both-guests built)
Host OS × host ISA × guest ISA × guest OS. The decomposition isolates each:
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux (docker copy), darwin→darwin.

## Final goal
A fully Docker-CLI-compatible daemon: `docker run <image>` pulls/unpacks, composes the overlay, picks
the guest arch, launches in the JIT, and behaves like any other container — with resource limits,
networking, and (for untrusted images) the sentry boundary.
