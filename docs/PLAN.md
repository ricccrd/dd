# PLAN — remaining work

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings; `build.rs` builds the guest-arch
binaries), `dd-daemon` (the Docker Engine API), and the desktop surface (`dd-client` / `dd-gui` /
`dd-cli`). This file is the **work list only** — what is missing or not yet implemented.

## ✅ Completed — to validate
Validation entry points (all via the Makefile):
- `make test` — cross-engine matrix (`dd-tests`, **~236 green** over 21 groups × 3 engines: Linux
  aarch64 + x86_64 + **darwin**; portable `port()` guests prove identical behaviour on Linux and macOS).
  Only non-pass: `soak/smc` (RWX, below) + the fork+exec crash (below), both xfail-tracked.
- `make test-docker` — docker-CLI battery (**50/50**); `make test-docker-full` — full per-command
  compliance matrix; `make test-compose` — Docker Compose (skips if compose absent);
  `make test-docker-net` — container-to-container networking (by-name/by-IP/isolation; gaps below).
- The `edge` group in `make test` probes obscure syscall corners (madvise/renameat2/SCM_RIGHTS/
  fallocate-punch/SEEK_HOLE/O_TMPFILE/mprotect/abstract-sockets/…) — 13/14 are xfail-tracked gaps (below).
- `make test-macos` — **macOS-container parity (23/23)**: the *same* `docker` lifecycle (run/logs/exec/
  inspect/ps/stop/rm) runs a Linux container AND a native-macOS container (the `macos` darwinjail image)
  identically — dd's signature capability, validated.
- `make test-realsw` — real pulled software (python ✅; redis/postgres/nats gaps below).
- `make coverage` — syscall/opcode gap report (static switch-diff + dynamic corpus). Source of truth below.

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
- **Process/session:** ~~`setsid`(157), `setregid`(143), `setfsuid/gid`(151/152), `getcpu`(168),
  `waitid`(95)~~ **done** (waitid translates the macOS→Linux siginfo)
- **File I/O:** ~~`flock`(32), `preadv/pwritev`(69/70), `preadv2/pwritev2`(286/287),
  `sync_file_range`(84→fsync), `readahead`(213→noop), `truncate`(45, by-path)~~ **done**
- **Memory:** ~~`memfd_create`(279), `mlockall/munlockall`(230/231), `mlock2`(284)~~ **done** (no-op locks)
- **Timers/clocks:** ~~`getitimer/setitimer`(102/103)~~ **done** (host wrap), ~~`clock_settime`(112)~~ **done**
  (EPERM, no CAP_SYS_TIME), ~~`clock_adjtime`(266)~~ **done** (EPERM)
- **Scheduling:** ~~`sched_get/setscheduler`(119/120), `sched_get/setparam`(118/121),
  `sched_get_priority_max/min`(125/126), `sched_rr_get_interval`(127), `sched_get/setattr`(274/275)~~ **done**
- **Resource:** ~~`getrlimit/setrlimit`(163/164)~~ **done** (aliased to prlimit64)
- **Signals:** ~~`rt_tgsigqueuei`(240)~~ **done** (mirrors rt_sigqueueinfo), ~~`sigqueue` si_value path~~ **done**;
  still `rt_sigtimedwait`(137), `rt_sigsuspend`(133) (need guest-mask aware signal waiting)
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

**Edge cases — obscure syscall corners (the `edge` group; 13/14 diverge from real Linux, all
xfail-tracked, found by reading `os/linux/service.c` + frontends).** Each is a differential (oracle) or
verdict test; fix → XPASS:
- `madvise(MADV_DONTNEED)` is a **no-op** — anon pages aren't dropped, a reread returns stale data
  instead of zeros (this is why **redis/jemalloc** misbehave). Implement DONTNEED (re-`mmap` the range
  `MAP_FIXED|ANON`, or `madvise` the host mapping).
- `renameat2` **flags dropped** — `RENAME_NOREPLACE` overwrites, `RENAME_EXCHANGE` doesn't swap.
- **`SCM_RIGHTS` fd-passing over AF_UNIX broken** — `recvmsg` yields no fd (the cmsg control block
  isn't translated). Breaks systemd socket-activation / Docker / D-Bus patterns.
- `fallocate(FALLOC_FL_PUNCH_HOLE)` ignored — region keeps old data (only ftruncate-extend handled).
- `lseek(SEEK_HOLE/SEEK_DATA)` unsupported — no hole/data offsets on a sparse file.
- `open(O_TMPFILE)` **fails** — no unnamed-temp-file support.
- `pipe2(O_DIRECT)` packet mode ignored — writes coalesce instead of preserving message boundaries.
- abstract-namespace AF_UNIX (`sun_path[0]==0`) **bind fails** — Linux-only; X11/D-Bus/systemd use it.
- `F_SETPIPE_SZ`/`F_GETPIPE_SZ` no-op and `dup3(fd,fd,0)` doesn't return EINVAL.
- **`mprotect` is a no-op** — `PROT_NONE` doesn't fault (darwin native DOES — `edge/mprotect` passes on
  darwin, fails on the Linux JIT). RELRO/guard pages and GC write-barriers are unenforced.
- `clock_nanosleep(TIMER_ABSTIME)` treated as **relative** → sleeps for the absolute value (**hangs**).
- `MSG_NOSIGNAL` ignored — a write to a closed socket delivers a fatal **SIGPIPE** instead of EPIPE.
- `/proc/self/fd` not synthesized — readlink/enumerate of the live fd table fails.
- (Works: `recv` `MSG_PEEK` + `MSG_DONTWAIT`/EAGAIN — `edge/msgflags` passes.)

**RWX / guest-JIT pages (`soak/smc`):** `mmap(PROT_READ|WRITE|EXEC)` returns **EPERM** under the JIT
(macOS W^X blocks RWX without `MAP_JIT`). Any guest that JITs its own code — JVM, V8/Node, LuaJIT,
.NET, PyPy — can't get executable pages. Fix needs the runtime to intercept RWX/`PROT_EXEC` maps and
back them with a `MAP_JIT` region or an RW+RX dual-mapping, and to re-translate on writes to executable
pages (the `__builtin___clear_cache` / coherency path the soak test patches 200k×). Endurance otherwise
holds: `soak/{codecache,indirect,threadchurn,forkchurn,allocchurn}` pass on all three engines
(sustained block-chaining/IBTC churn + thread/fork/heap churn over thousands of cycles).

**fork()+execve() crash — DIAGNOSED, main reliability gap.** Root cause: **`execve` (service.c case 221)
never `munmap`s the inherited address space, and `load_elf` (os/linux/elf.c) relocates even a non-PIE
`ET_EXEC` off its fixed link-time vaddr (forced — macOS `__PAGEZERO` reserves the low 4 GB; see Platform
limitations). The bias is survivable in a fresh exec's sparse layout but not in the dense post-`fork()`
layout, where the non-PIE image's baked absolute (un-relocated) references land on live memory → SIGSEGV.**
The marquee victim is the **GCC toolchain** (`compile` group): `gcc-14`/`cc1` are `ET_EXEC` non-PIE
(entry `0x433880`). Evidence (deterministic, gcc-bundle image):
- `sh -c 'gcc-14 --version'` (fork → execve) → **SIGSEGV (rc 139), 6/6**; `env gcc-14 --version` and
  `sh -c 'exec gcc-14 --version'` (execve, **no fork**) and gcc as the initial image → **rc 0**. ⇒ the
  trigger is *fork-then-execve*, not gcc codegen. **`perl` (PIE) fork+exec works** — PIE is fully
  relocatable, so only non-PIE images crash.
- `JT=1` `[LOADED]` traces: the no-fork run spreads gcc to an isolated high base (`0x11ec98000`); the
  crashing fork run packs dash+ld.so+gcc+ld.so into a dense `0x100da0000–0x10124f000` window.
- The **intermittent** tail (`busybox/{find,seq}`, `containersw/nc-loopback`) flakes under full-matrix
  load; `soak/forkchurn` (fork *without* exec) is rock-solid. **But `busybox` is `ET_DYN`/PIE** (verified:
  `e_type == 3`), so the flaky tail is a **DISTINCT bug from the non-PIE gcc crash** — PIE is fully
  relocatable and immune to the bias issue. The tail is some *other* fork+exec race (SIGSEGV, code 139, in
  the exec'd child; `diag_crash` never fires even with `SA_ONSTACK`, so the fault bypasses the POSIX signal
  handler — suspect the Mach exception path). (`container/symlink`'s "failure" is unrelated: leftover `/l`
  in the overlay UPPER from a prior run, an overlay-cleanup issue.)

**Fix — MECHANISM CONFIRMED, partial fix landed, full fix has a tradeoff.** Captured the exact fault (the
`mac` bridge drops the ambient env, so `CRASHDBG` never reached the jit — `SpawnConfig` now forwards it +
the fork child clears its inherited Mach exception port so the POSIX `diag_crash` fires):

    [CRASH] sig=11 fault=0x42b440 pc=0x42b440      (then, after the redirect: pc=biased, fault=0x4c90d9)

So `pc == fault == a low non-PIE link vaddr`: the guest takes an **absolute jump to a link vaddr** (the
un-relocated ref), the JIT reads guest code there 1:1, but the image is biased high → unmapped → SIGSEGV.
Landed so far:
- **execve address-space teardown** — kernel-like reset, fixes a real per-exec leak (256MB heap + image +
  stack leaked every exec). Necessary but not sufficient.
- **dispatcher PC-redirect** (`g_nonpie_*` set by `load_elf` for `ET_EXEC`) — redirects absolute *code*
  jumps from the link vaddr into the biased image. This advances the crash from a code-jump fault to a
  **data-ref fault** (pc now real biased code; fault still a low link vaddr = an un-relocated data pointer).
- **`-pagezero_size 0x1000` + pinning the non-PIE at its link vaddr (MAP_FIXED, bias 0)** — **fully fixes
  the non-PIE crash** (`gcc-14 --version`: SIGSEGV 6/6 → **rc 0**; code+data refs resolve natively). **But
  it broke the PIE common case hard** (43/195 — basic PIE guests exit 255), so it was **reverted**. Too
  global: shrinking __PAGEZERO perturbs every PIE load + the heap/stack placement.

**The achievable full fix** is the shrunk-__PAGEZERO approach made safe: keep __PAGEZERO small so the
non-PIE can pin low, but force **every other** guest mapping (PIE image+interp, heap, stack, anon mmaps)
to a **high hint** so the PIE world is unchanged and only the non-PIE uses the low region. (Alternative:
an arm64 load/store fault-fixup handler that re-does a link-range data access at `+bias` — heavier.) Either
way the diagnostic plumbing (`CRASHDBG` forwarding + `[CRASH]`) is now in place to iterate.

**Separately, the flaky busybox tail is a DISTINCT, still-open bug** (busybox is PIE, immune to all the
above) — a different fork+exec SIGSEGV under load, not yet root-caused.

**Docker API compliance** — goal is a faithful Engine API **v1.43** for the **everyday developer
workflow** so the stock `docker` CLI + bollard work unmodified. **Swarm / services / nodes / tasks /
secrets / configs / managed plugins are out of scope** (single-node, local dev — `/info` reports
`Swarm: inactive`).

_Done 2026-06-27:_ `dd-daemon` decomposed (1805-line `main.rs` → `model`/`util`/`system`/`images`/
`containers`/`build`/`archive`/`volumes`/`networks`/`runtime` modules); `dd-client` rewrapped on
**bollard** (GUI + CLI share it). Added routes/behaviour: global Docker headers (`Api-Version`… → CLI
handshake + `GET`/`HEAD /_ping`), `POST /auth`, `GET /system/df`, `GET /events` (open stream), prune
(containers/volumes/networks/images/build), `/containers/{id}/{changes,export,update}`, image
`history`/`search`/`distribution`, `VirtualSize` on `images/json` (bollard strict-deserialize fix),
`tag`-query honoring, `version`/`info` fills. _(every everyday route now returns a Docker-shaped
response — no 404s; what's left is field/behaviour fidelity.)_

Remaining — what still needs to be figured out / built (priority):

| Area | What needs work | Pri |
|------|-----------------|:---:|
| `docker inspect` (container) | ~~`NetworkSettings` (ports → fixes `docker port`, + membership), `Name`, `Mounts`, `Config.Image/Env`~~ **done**; still: `State.{Pid,StartedAt,FinishedAt}` (not tracked yet) | P1 |
| `docker logs` | ~~`--tail`/`--timestamps`, **raw** stream for `-t`~~ **done**; still `-f`/follow, `--since` | P1 |
| `docker ps` | ~~human `Status` (`Up 3 minutes`)~~ **done**; still `--filter`/`--size`, `Labels`/`ImageID`/network info | P1 |
| `docker exec` | ~~`-e`/`-w`~~ **done**; still `-u` (needs a `SpawnConfig` uid field), `--privileged`, `exec -d` → 200 | P1 |
| `docker events` | wire a real lifecycle event bus into the open stream (compose + GUI watch it) | P1 |
| `docker stats` | real CPU/mem accounting from the JIT runtime + streaming *(design: runtime has no cgroup metrics)* | P1 |
| networks | **container-to-container connectivity** (`scenarios/docker-net.sh`, 3/7): two containers on a user network can't reach each other — no per-container IP is assigned (inspect/`network inspect` show no member IP, `"Containers": {}`), and there's no embedded DNS (`nc <name>` → "bad address"). Needs a real bridge + IPAM (subnet/gateway, per-endpoint IP/MAC) + name→IP resolution *(design: dd uses a per-container loopback netns, no bridge — see Networking Phase-2b #2)*. Cross-network isolation trivially holds today (nothing reaches anything). | P2 |
| `docker build` | ~~`buildargs`/`target`/`nocache`~~ **done**; still `labels`, real content-digest image IDs; (BuildKit cache, above) | P2 |
| `docker run` opts | ~~`--label` (stored → `Config.Labels`)~~ **done**; still `--user` (uid not applied — needs a `SpawnConfig` uid field), wider `HostConfig`: restart policy, `--cap-add`, `--device`, `--mount`, `--privileged` | P2 |
| volumes | ~~`409` when in use, RFC3339 `CreatedAt`~~ **done**; still persist `--driver`/`--opt`/`--label` | P2 |
| `docker pull`/`push` | per-layer progress bars; push returns final `aux{Digest}` | P2 |
| image inspect/history | ~~real `Size`/`Entrypoint`/`Env`/`Cmd`/`WorkingDir`~~ **done** (from the Image struct); still real `Created` (needs an `Image.created` field) + `Labels` | P2 |
| `docker cp` | ~~redirect a `cp` into a bind-mount path~~ **done** (longest-prefix bind match); still named-volume paths (handlers lack the volume list) | P2 |
| `save`/`load`/`import` | ~~`images/get` + `images/load` + `fromSrc`~~ **done** (dd-native rootfs tar + manifest) | P3 |
| stop/kill/restart | honor `signal`/`t` + real signal delivery (containers run synchronously today) | P3 |

Hard problems (not plumbing): **live resource metrics** for `stats`, an **event bus**, and a real
**network/IPAM model** over the loopback-netns isolation.

## Real software — `make test-realsw` (pulled from Docker Hub) + the `compile` group
Real, syscall/fork/thread/mmap-heavy production binaries run with deterministic workloads. Snapshot:
- **python:alpine** ✅ — a mixed workload (lru_cache fib(35), dict aggregation, sort) runs correctly; a
  large C runtime (bytecode VM, import machinery) is solid.
- **GCC-14 toolchain** (`compile` group, gcc-bundle image) ❌ — `gcc`/`g++`/`cc1` **segfault**: this IS
  the fork()+execve()/non-PIE crash diagnosed above (driver is always fork+exec'd by the shell).
  xfail-tracked (`compile/{hello,c-primes,cpp-stl}`); XPASS fires when the execve address-space-reset lands.
- **redis:alpine** ❌ — DISTINCT bug (redis-server is **PIE**, so *not* the non-PIE crash): the server
  **crashes during startup** — prints its config banner (+ jemalloc `MADV_DONTNEED` unsupported / overcommit
  warnings) then exits *before* binding (nothing on the port; `redis-cli` → refused). Dies in bootstrap
  (post-config, pre-listener) — suspect the `madvise`/jemalloc or an eventloop/thread-init path; needs a
  faulting-address capture.
- **postgres:alpine** ❌ — DISTINCT bug (postgres is **PIE**): never reaches "ready to accept connections".
  Its `setuid`/`setgid` drop *works* (146/144/145/147 are handled) — the gap is the daemonize, i.e. the
  still-missing **`setsid`(157)**, plus possibly the fork+exec path during initdb.
- **nats:latest** ❌ — won't even **pull**: `dd-daemon` reports "could not detect the image architecture"
  on a distroless/scratch image (the arch sniffer in `containers_create`/`detect_arch` scans for an ELF
  where there is none — needs a manifest-`platform` fallback). A `dd-daemon` image-arch-detection gap.

### Platform limitations (macOS host — need Linux primitives the host can't provide; off the work-list)
Non-PIE **ET_EXEC** (macOS `__PAGEZERO` reserves the low 4 GB the fixed vaddr needs), **cpu/io throttling**
(no cpu/io cgroup — mem+pids ARE enforced via rlimit), **`pidfd`** and **`io_uring`** (no macOS primitive).
These can't be implemented on a macOS host; they'd come for free on a linux→linux build.

## Portability matrix (seams exist; only darwin-host / both-guests built)
Host OS × host ISA × guest ISA × guest OS. The decomposition isolates each:
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux (docker copy), darwin→darwin.
