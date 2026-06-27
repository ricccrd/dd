# PLAN — remaining work

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings; `build.rs` builds the guest-arch
binaries), `dd-daemon` (the Docker Engine API), and the desktop surface (`dd-client` / `dd-gui` /
`dd-cli`). This file is the **work list only** — what is missing or not yet implemented.

## ✅ Completed — to validate
Run `bash dd-tests/scenarios/docker.sh` (the docker-CLI battery, **50/50**) and
`cargo run -p dd-tests` (the cross-engine matrix, **~236 green**; the only non-pass is `soak/smc`
(RWX limitation) plus the intermittent fork+exec crash — both documented below) to validate all of the below.

Harness bugs (all fixed; matrix/battery green):
- [x] aarch64 open-flag bits were x86 values → every symlink open ELOOPd (`cat /etc/os-release` on alpine).
- [x] ELF loader resolved the exe in the overlay UPPER only, not through `--lower`s.
- [x] jitdarwin didn't relocate `adrp` under the segment slide (darwin/adrp test).
- [x] the listed jit86 codegen bugs were already stale — the x86 engine is 26/0.

Docker CLI (`dd-daemon`; scenarios in `dd-tests/scenarios/docker.sh`):
- [x] `docker cp` — GET/PUT/HEAD `/archive`, bind-volume aware (scenarios `cp-*`).
- [x] `docker build` — FROM/RUN/COPY/ADD/ENV/WORKDIR/CMD/ENTRYPOINT, **multi-stage** `COPY --from`,
      **auto-pull** non-local base, metadata persisted + inherited at run (scenarios `build-*`).
- [x] `docker run -e` + image ENV/WORKDIR/ENTRYPOINT honored, no host-env leak (scenarios `*env*`).
- [x] `--network none` enforces egress isolation (loopback still allowed) (scenario `egress-none-blocked`).
- [x] `docker pause`/`unpause` via SIGSTOP/SIGCONT (state transitions; freeze relies on standard SIGSTOP).

JIT:
- [x] **IPC namespace** — SysV shared memory, semaphores, message queues all work, keys namespaced
      per-container by `DD_NETNS`; tested both engines (matrix `system/{shm,sem,msg}`). Only
      `*ctl(IPC_STAT/IPC_SET)` introspection is deferred (returns ENOSYS, graceful).
- [x] **Syscall/opcode gaps from the growing matrix** (all both-engine unless noted):
      `eventfd` real accumulating counter + **EFD_SEMAPHORE** (decrement-by-1), `pselect6`(72),
      `inotify` create/delete with filename (kqueue + dir snapshot/diff), x86 `poll`(7)→ppoll +
      x86 `epoll_wait`(232)→epoll_pwait, **SSE `PACKUSWB`/`PACKSSWB`/`PACKSSDW`** (`66 0F 67…` →
      SQXTUN/SQXTN; unblocked `heavy/bigmem` + `posix/mmapshared`), `prctl PR_SET/GET_NAME`,
      `sigqueue` si_value payload (rt_sigqueueinfo → sigframe `si_code`/`si_value`),
      POSIX `shm_open` (`/dev/shm` → host-file backing), `memfd_create`(279, unlinked tmpfile),
      `glob("*")` (stale getdents `DIR*` cache invalidated on close + dropped in fork children).
      Matrix is **~236 green**; the only non-pass is `soak/smc` (RWX limitation, below) plus the
      intermittent fork+exec crash (below).

## Next work (priority order) — large subsystems
1. **Finish the jit86 engine dedup.** *Half done.* The x86-64 guest **already shares the entire
   `os/linux/` personality** through the cpu-interface seam (`frontend/x86_64/abi.h` `G_*` + `sysmap.h`).
   **What's left is the host `jit/` engine:** x86_64 still carries its own
   `frontend/x86_64/{cache,emit,dispatch}.c` instead of the shared `jit/{cache,emit_arm64,dispatch}.c`
   that aarch64 uses. Lift it on via cpu-access accessors so the code cache + dispatcher are cpu-agnostic
   (the cpu struct and the x86 decoder stay genuinely per-arch). Payoff: one engine, two thin frontends.
2. **Networking Phase-2b — userspace netstack.** A real TCP/IP stack for *external* traffic with NET-ns
   interfaces/routes + tunnel egress. Loopback isolation + port-map already cover the common case.
3. **Untrusted-guest isolation — the sentry process-split.** Seccomp/Seatbelt-locked sandbox task +
   trusted sentry over a syscall ring. Required only for untrusted images.
4. **Tier-2 trace optimizer.** Trace formation over `PROF`, cross-trace register allocation (removes the
   per-block spill — the main remaining overhead), monomorphic-comparator inlining, purity-gate
   memoization. Constraint: do not use dead-register §B scratch (unsafe); don't drop the §B gsp check.
5. **Optimize the x86 (jit86) translator toward native.** Close the gap to native arm on compute: elide
   flag synthesis (materialize only the EFLAGS bits a consumer reads), tighter SSE/x87 lowering, and —
   once the engine dedup (#1) lands — inherit the aarch64 engine's block-chaining / IBTC / §B optimizations
   plus the tier-2 optimizer (#4).

## Smaller remaining items
- **`docker build` BuildKit cache** — layer/step caching (today every build re-runs from the base).
- **IPC `*ctl(IPC_STAT/IPC_SET)`** — the macOS `*_ds` layouts differ from the guest ABI; rare
  introspection, returns ENOSYS today.

## Coverage gaps — unimplemented syscalls / opcodes / Docker API
*Source of truth: `make coverage` (static switch-diff of `os/linux/service.c` + `frontend/x86_64/sysmap.h`
against the kernel ABI, plus a dynamic corpus run that aggregates the engine's own
`unhandled syscall N` / `UNIMPL opcode 0xNN` diagnostics). Snapshot below; re-run to refresh.*
Static: **178/323 canonical syscalls handled, 145 missing.** Surfaced by the dd-tests `linuxsys`/`ipc`
groups + the dynamic corpus.

**Syscalls worth implementing (macOS has the primitive — mostly a wire-up):**
- **Process/session:** `setsid`(157), `waitid`(95), `setregid`(143), `setfsuid/gid`(151/152), `getcpu`(168)
- **File I/O:** `flock`(32) *(macOS has flock(2))*, `truncate`(45, by-path), `preadv/pwritev`(69/70) +
  `preadv2/pwritev2`(286/287), `sync_file_range`(84→fsync), `readahead`(213→noop/fadvise)
- **Memory:** ~~`memfd_create`(279)~~ **done** (unlinked tmpfile), `mlockall/munlockall`(230/231→noop),
  `mlock2`(284)
- **Timers/clocks:** `getitimer/setitimer`(102/103), `clock_settime`(112), `clock_adjtime`(266)
- **Resource:** `getrlimit/setrlimit`(163/164) *(prlimit64=261 already handled — alias these to it)*
- **Signals:** `rt_sigtimedwait`(137), `rt_sigsuspend`(133), `rt_tgsigqueuei`(240); the **`sigqueue`
  si_value payload path** is **done** (`rt_sigqueueinfo`→sigframe `si_code`/`si_value`)
- **Scheduling (stub sane values):** `sched_get/setscheduler`(119/120), `sched_get/setparam`(118/121),
  `sched_get_priority_max/min`(125/126), `sched_rr_get_interval`(127), `sched_get/setattr`(274/275)
- **Misc (all done):** ~~`prctl PR_GET_NAME`~~ (stores the set name), ~~POSIX `shm_open`~~ (`/dev/shm`→
  host-file backing), ~~`glob("*")`~~ (it was a stale getdents `DIR*` cache, not `d_type` — invalidated
  on `close`, dropped in fork children)

**Host-limited (no macOS primitive — emulate or leave ENOSYS):** POSIX mqueue `mq_*`(180-185),
`timer_create`/`timer_*`(107-111) *(could ride kqueue)*, plus the already-listed `pidfd`/`io_uring`.
NUMA/keyring/module/`ptrace` are out of scope. (`eventfd` **EFD_SEMAPHORE** and `inotify` create/delete
**with filename** are now **done** — a real accumulating counter with decrement-by-1, and a kqueue
`EVFILT_VNODE` watch backed by a directory snapshot/diff to recover the changed name.)

**Opcodes:** the dynamic corpus currently hits **0 `UNIMPL`** (the SSE `packuswb` `66 0F 67` gap that
`heavy/bigmem` + `posix/mmapshared` exposed is fixed). `make coverage dynamic` is the way to catch new ones.

**RWX / guest-JIT pages (`soak/smc`):** `mmap(PROT_READ|WRITE|EXEC)` returns **EPERM** under the JIT
(macOS W^X blocks RWX without `MAP_JIT`). Any guest that JITs its own code — JVM, V8/Node, LuaJIT,
.NET, PyPy — can't get executable pages. Fix needs the runtime to intercept RWX/`PROT_EXEC` maps and
back them with a `MAP_JIT` region or an RW+RX dual-mapping, and to re-translate on writes to executable
pages (the `__builtin___clear_cache` / coherency path the soak test patches 200k×). Endurance otherwise
holds: `soak/{codecache,indirect,threadchurn,forkchurn,allocchurn}` pass on all three engines
(sustained block-chaining/IBTC churn + thread/fork/heap churn over thousands of cycles).

**Intermittent fork+exec Bus error (the flaky matrix tail) — main reliability gap.** A shell running
several commands (`sh -c 'a; b | c'`) flake-crashes: ~1–2 of the fork+exec-heavy tests per *full* matrix
run produce empty output (`busybox/{find,seq}`, `containersw/nc-loopback`, `container/symlink`). It is
**fork+exec-specific** (`soak/forkchurn` — fork *without* exec — is rock-solid over thousands of cycles;
`container/symlink` uses no pipe and no getdents, just three forked commands, and still flakes),
**load-dependent** (reproduces only under the concurrent full-matrix run, not a standalone daemon loop),
and **pre-existing** (not introduced by the syscall fixes above). Suspects: host `fork()` interacting with
the MAP_JIT/W^X state + the per-block cache-flush-on-`execve`. Next step is a crash capture under matrix
load (the macOS DiagnosticReports backtrace + faulting address) — the JIT installs its own SIGSEGV/SIGBUS
handler, so it likely needs disarming for the child to drop a core.

**Docker API compliance** (from `scenarios/docker-full.sh`, 33/49): missing endpoints/behaviour —
`/system/df`, `/containers/{id}/changes` (`docker diff`), `/commit`, `/images/get`+`/images/load` (405:
`save`/`load`), `/images/{name}/history`, `/containers/prune` (405), `docker export` tar incomplete,
`docker import` doesn't register the image; inspect omits `.NetworkSettings` (breaks `docker port` → CLI
panic, and network-membership reporting); `--user`, `--label`, and `exec -e/-w` not honoured.

### Platform limitations (macOS host — need Linux primitives the host can't provide; off the work-list)
Non-PIE **ET_EXEC** (macOS `__PAGEZERO` reserves the low 4 GB the fixed vaddr needs), **cpu/io throttling**
(no cpu/io cgroup — mem+pids ARE enforced via rlimit), **`pidfd`** and **`io_uring`** (no macOS primitive).
These can't be implemented on a macOS host; they'd come for free on a linux→linux build.

## Portability matrix (seams exist; only darwin-host / both-guests built)
Host OS × host ISA × guest ISA × guest OS. The decomposition isolates each:
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux (docker copy), darwin→darwin.
