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
2. **Networking Phase-2b — userspace netstack.** A real TCP/IP stack for *external* traffic with NET-ns
   interfaces/routes + tunnel egress. Loopback isolation + port-map already cover the common case.
3. **Untrusted-guest isolation — the sentry process-split.** Seccomp/Seatbelt-locked sandbox task +
   trusted sentry over a syscall ring. Required only for untrusted images.
4. **Tier-2 trace optimizer.** Trace formation over `PROF`, cross-trace register allocation (removes the
   per-block spill — the main remaining overhead), monomorphic-comparator inlining, purity-gate
   memoization. Constraint: do not use dead-register §B scratch (unsafe); don't drop the §B gsp check.
5. **Optimize the x86 (jit86) translator toward native.** dd already beats qemu-user emulation on every
   benchmark (`make bench`: int 1.36×, FP 21×, SHA-256 1.15×, SQLite 3.16×), but x86→arm64 translation
   still trails *native* arm on compute (SHA-256 is only ~1.15× over qemu, vs the aarch64 engine running
   at native speed). Close the gap: elide flag synthesis (materialize only the EFLAGS bits a consumer
   actually reads), tighter SSE/x87 lowering, and — once the engine dedup (#1) lands — inherit the
   aarch64 engine's block-chaining / IBTC / §B optimizations plus the tier-2 optimizer (#4). Target: x86
   compute approaching native, beating any VM path (qemu *or* Rosetta), to hold the "beat the VM
   everywhere" bar.

## Docker CLI gaps (`dd-daemon`)
OCI registry **pull/push now works against any registry** — Docker Hub, `ghcr.io`, `quay.io`, ECR, a
plain `localhost:5000` (`dd-daemon/src/registry.rs`; 44/44 docker-CLI scenarios pass). `docker build`
works for the common path (FROM/RUN/COPY/ADD/ENV/WORKDIR/CMD/ENTRYPOINT — RUN executes in the JIT, writes
persist; a non-local `FROM` is auto-pulled; ENV/WORKDIR/ENTRYPOINT are persisted into the image and
inherited at `docker run`, which also honors `-e`). What is still missing on `docker build`:
- **multi-stage builds** and the **BuildKit cache**.

## Remaining JIT gaps
- **IPC namespace** — SysV/POSIX shm/sem/msg per IPC-ns. (The JIT doesn't implement SysV IPC at all yet,
  so this is *implement it + namespace the keys*, not just isolation.)

### Platform limitations (macOS host — need Linux primitives the host can't provide; off the work-list)
Non-PIE **ET_EXEC** (macOS `__PAGEZERO` reserves the low 4 GB the fixed vaddr needs), **cpu/io throttling**
(no cpu/io cgroup — mem+pids ARE enforced via rlimit), **`pidfd`** and **`io_uring`** (no macOS primitive).
These can't be implemented on a macOS host; they'd come for free on a linux→linux build.

## Portability matrix (seams exist; only darwin-host / both-guests built)
Host OS × host ISA × guest ISA × guest OS. The decomposition isolates each:
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux (docker copy), darwin→darwin.

## Final goal
A fully Docker-CLI-compatible daemon: `docker run <image>` pulls/unpacks (done), composes the overlay,
picks the guest arch, launches in the JIT, and behaves like any other container — with resource limits,
networking, and (for untrusted images) the sentry boundary.
