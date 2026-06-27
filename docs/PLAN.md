# PLAN — remaining work

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings; `build.rs` builds the guest-arch
binaries), `dd-daemon` (the Docker Engine API), and the desktop surface (`dd-client` / `dd-gui` /
`dd-cli`). This file is the **work list only** — what is missing or not yet implemented.

## Next work (priority order)
1. **Dedup jit86 onto the shared layer.** The x86-64 guest is brought in whole (`frontend/x86_64/jit86.c`)
   with its own cpu struct + a *basic* container layer (no overlay/cgroup/jail). Lift it onto the shared
   `jit/` + `os/linux/` via (a) cpu-access accessors (`SYSARG0..5(c)`, `SYSRET(c,v)`, PC/SP getters) so
   the syscall + dispatch logic is cpu-agnostic, and (b) a syscall-number→canonical-id map per frontend
   (x86 `read`=0 vs aarch64 63). Then decompose it the way aarch64 is. Payoff: one runtime, two thin
   frontends; jit86 gains overlay/cgroup/jail for free.
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

## Docker CLI gaps (`dd-daemon`)
OCI registry **pull/push now works against any registry** — Docker Hub, `ghcr.io`, `quay.io`, ECR, a
plain `localhost:5000` (`dd-daemon/src/registry.rs`; see [`DOCKER.md`](DOCKER.md), 38/38 scenarios). So
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
- **Relative symlink in the jail mis-resolves as "Symbolic link loop"** — alpine's
  `/etc/os-release -> ../usr/lib/os-release` fails to read through the JIT (`cat /etc/os-release` →
  symlink loop), though the target exists. Breaks tools that read os-release. In the overlay/jail
  symlink-follow (`layer_follow`). (Found by dd-tests sandbox/jail group.)

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
