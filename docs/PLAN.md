# PLAN — remaining work

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings; `build.rs` builds the guest-arch
binaries), `dd-daemon` (the Docker Engine API), and the desktop surface (`dd-client` / `dd-gui` /
`dd-cli`). This file is the **work list only** — what is missing or not yet implemented.

## Next work (priority order)
1. **Finish the jit86 engine dedup.** *Half done.* The x86-64 guest **already shares the entire
   `os/linux/` personality** — `service.c` (the syscall layer), the container overlay/jail/cgroup/netns
   (`container/vfs.c`+`netns.c`+`state.c`), `signal.c`, `thread.c`, `fscache.c` — through the
   cpu-interface seam (`frontend/x86_64/abi.h` `G_*` contract + the `sysmap.h` syscall-number→canonical
   map). Its old duplicate `service.c`+`container.c` are deleted, so it has overlay/cgroup/jail already.
   **What's left is the host `jit/` engine:** x86_64 still carries its own
   `frontend/x86_64/{cache,emit,dispatch}.c` instead of the shared `jit/{cache,emit_arm64,dispatch}.c`
   that aarch64 uses. Lift it on via cpu-access accessors so the code cache + dispatcher are
   cpu-agnostic (the cpu struct and the x86 decoder stay genuinely per-arch). Payoff: one engine, two
   thin frontends.
2. **Rare fork/command-sub `Bus error`.** ~1/15 intermittent fault in the fork + nested command-sub path
   under full container flags (pre-existing in the engine). Likely a shadow-stack/teardown race;
   reproduce under load and fix.
3. **Networking Phase-2b — userspace netstack.** A real TCP/IP stack for *external* traffic with NET-ns
   interfaces/routes + tunnel egress. Loopback isolation + port-map already cover the common case.
4. **Untrusted-guest isolation — the sentry process-split.** Seccomp/Seatbelt-locked sandbox task +
   trusted sentry over a syscall ring. Required only for untrusted images.
5. **Tier-2 trace optimizer.** Trace formation over `PROF`, cross-trace register allocation (removes the
   per-block spill — the main remaining overhead), monomorphic-comparator inlining, purity-gate
   memoization. Constraint: do not use dead-register §B scratch (unsafe); don't drop the §B gsp check.
6. **Optimize the x86 (jit86) translator toward native.** dd already beats qemu-user emulation on every
   benchmark (`make bench`: int 1.36×, FP 21×, SHA-256 1.15×, SQLite 3.16×), but x86→arm64 translation
   still trails *native* arm on compute (SHA-256 is only ~1.15× over qemu, vs the aarch64 engine running
   at native speed). Close the gap: elide flag synthesis (materialize only the EFLAGS bits a consumer
   actually reads), tighter SSE/x87 lowering, and — once the engine dedup (#1) lands — inherit the
   aarch64 engine's block-chaining / IBTC / §B optimizations plus the tier-2 optimizer (#5). Target: x86
   compute approaching native, beating any VM path (qemu *or* Rosetta), to hold the "beat the VM
   everywhere" bar.

## Docker CLI gaps (`dd-daemon`)
OCI registry **pull/push now works against any registry** — Docker Hub, `ghcr.io`, `quay.io`, ECR, a
plain `localhost:5000` (`dd-daemon/src/registry.rs`; 38/38 docker-CLI scenarios pass). So
`DD_IMAGES` no longer has to point at a pre-extracted rootfs. What is still missing:
- **`docker build`** — needs a BuildKit-compatible builder.
- **`docker cp`** — the `/archive` tar endpoints aren't implemented.
- **freezer `pause`/`unpause`** — accepted but no-ops (dd has no freezer cgroup on macOS).

## Bugs found by the test harness
- **jit86 (x86-64) translator/service bugs (xfail in dd-tests)** — surfaced by running the aarch64 test
  programs on the x86 engine via the cross-compiler: `math`/`floatmath` (float/libm codegen → crash),
  `heap` (malloc churn → crash), `threads`/`atomics` (hang). Plus the known `base32` (NEON) and
  `sha256sum`. The aarch64 engine passes all; these are jit86-specific.
- **jitdarwin: adrp/`__cstring` literal not relocated under the segment slide** — a guest that reads a
  string literal (via adrp) gets zeros; stack/SP-relative data works. (Found by dd-tests darwin group.)
- **ELF loader resolves the executable in the overlay UPPER only, not through lowers** — running a
  program from an image given purely as `--lower` (empty upper) fails with "open: No such file". The
  program loader needs to go through `overlay_resolve` like the syscall paths do. (A registry `pull`
  unpacks into a flat rootfs, so this isn't hit there — only on a pure read-only-lower mount.)

## Remaining JIT gaps
- **`--network none` egress isolation** — dd-daemon can pass `DD_NET_ISOLATE=1`, but the JIT doesn't yet
  enforce it (Seatbelt egress block). Stub only today.
- **ET_EXEC loader** (non-PIE static) — platform-blocked by macOS `__PAGEZERO`; needs a fixed-vaddr map.
- **IPC namespace** — SysV/POSIX shm/sem/msg per IPC-ns.
- **cpu/io cgroup** — only mem+pids enforced (cpu/io best-effort on macOS).
- **`pidfd`, `io_uring`** — no macOS primitive; defer.

## Portability matrix (seams exist; only darwin-host / both-guests built)
Host OS × host ISA × guest ISA × guest OS. The decomposition isolates each:
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux (docker copy), darwin→darwin.

## Final goal
A fully Docker-CLI-compatible daemon: `docker run <image>` pulls/unpacks (done), composes the overlay,
picks the guest arch, launches in the JIT, and behaves like any other container — with resource limits,
networking, and (for untrusted images) the sentry boundary.
