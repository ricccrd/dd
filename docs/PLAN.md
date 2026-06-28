# PLAN — remaining work

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings), `dd-daemon` (the Docker Engine
API), and the desktop surface (`dd-client`/`dd-gui`/`dd-cli`). **This file is the work list only — what
is missing or not yet implemented.** Validate via the Makefile: `make test` (cross-engine matrix, **~240
green** / 3 engines), `make test-docker[-full|-net]`, `make test-macos` (23/23), `make test-realsw`,
`make coverage` (syscall/opcode gap report).

## Large subsystems (priority order) — each has an executable design + first-PR roadmap in `docs/design/`

1. **jit86 engine dedup** → `docs/design/engine-dedup.md`. Lift the host `jit/` engine so the code cache +
   dispatcher are cpu-agnostic (the cpu struct + x86 decoder stay per-arch). **PR1 ✅** (x86 on shared
   `jit/cache.c`) + **PR2 ✅** (dispatch.c frontend-hook seam, aarch64 bit-identical). **Remaining: PR3/PR4**
   — x86 hook definitions + swap the x86 target onto `jit/dispatch.c`; also hook the 5th divergence (the
   per-block `g_trace` dump). Gate: matrix green both engines.
2. **Networking Phase-2b — netstack** → `docs/design/netstack.md`. External egress already works; the gap is
   L3 identity + reachability. **PR1 ✅** (daemon IPAM `172.18/12`, per-container IP, `--network` join →
   docker-net 5/7). **Remaining: PR2** (per-network AF_UNIX virtual switch + name→IP DNS → 7/7) *(in
   flight)*; then the optional in-process `smoltcp` stack behind `DD_NETSTACK`.
3. **Untrusted-guest isolation — sentry process-split** → `docs/design/sentry-split.md`. **No PR yet.** The
   trust boundary is one line (`run_guest`→`service(c)`); route it through `syscall_route(c)` on
   `g_untrusted`, split `service()` by authority (compute/mem local, fs/net/proc → sentry over an SPSC
   ring), deny-default Seatbelt. First PR: ring + read/write/open family behind the flag.
4. **Tier-2 trace optimizer** → `docs/design/tier2-optimizer.md`. **No PR yet.** The win is killing the
   stolen-register mangle (x18/x28/x30) via trace-local allocation; §B-safe (own x9–x17 only in call-free
   spans; never drop the `gsp` check); PROF needs loop-back heat. First PR: a 2-block identity trace behind
   `TIER2`, matrix-green when off.
5. **x86 translator → native** → `docs/design/x86-perf.md`. **PR1 ✅** (lazy NZCV for `sub/cmp→Jcc`).
   **Remaining:** extend to add/logical producers + carry-value consumers; `pmovmskb` (48→8 NEON); x87
   `fptop` tracking. Inherits #1 + #4 once they land.

## Deep bugs (root-caused; fixes scoped)

- **fork()+execve() non-PIE crash (gcc toolchain).** `execve` biases a non-PIE `ET_EXEC` off its fixed
  vaddr (`__PAGEZERO` forces it); post-`fork` the dense layout makes its un-relocated absolute refs fault.
  Landed: execve address-space teardown + the dispatcher PC-redirect (advances code-jump fault → data-ref
  fault). `-pagezero_size 0x1000` + pinning the non-PIE low **fully fixes it** but broke the PIE common case
  (reverted). **Achievable fix:** small `__PAGEZERO` + force every *other* mapping (PIE image/heap/stack/
  mmaps) to a high hint so only the non-PIE uses the low region (or an arm64 load/store fault-fixup at
  `+bias`). Unblocks the `compile` group (xfail today).
- **busybox flaky fork+exec tail** → `docs/design/research-busybox-crash.md` + **`docs/design/dual-map-cache.md`**.
  Root cause: W^X/`MAP_JIT` execute-permission fault in the fork child (per-thread APRR isn't reliable across
  fork). First mitigation applied (re-assert execute in the child) — insufficient. **Robust fix (PR-ready):**
  the **dual-mapped RX/RW code cache** (`mach_vm_remap` a RW alias, route cache stores through `cw()`,
  execute stays RX in the page tables → survives fork). Also closes the `soak/smc` RWX gap below.
- **`soak/smc` / RWX guest-JIT pages.** `mmap(PROT_READ|WRITE|EXEC)` returns EPERM (W^X). Any guest that JITs
  its own code (JVM/V8/LuaJIT/.NET/PyPy) needs executable pages — fixed by the same dual-map cache (intercept
  RWX/`PROT_EXEC` maps, back with MAP_JIT, re-translate on writes). *(Endurance otherwise holds:
  `soak/{codecache,indirect,threadchurn,forkchurn,allocchurn}` pass.)*
- **postgres:alpine** — root-caused: the initial loader `load_elf` had no `#!` check (entry is a shebang
  script). **Fix landed** (shared `parse_shebang` in `jit_run`); next gap downstream is daemonize/initdb.
  Daemon-side: the local `postgres` image's `dd-image.json` lacks `entrypoint`/`env`.

## Coverage gaps — syscalls

*Source: `make coverage`. Static **178/323 handled** at last snapshot (many since landed).*
- **Signals:** `rt_sigtimedwait`(137), `rt_sigsuspend`(133) — need guest-mask-aware signal waiting.
- **Edge corners** (`edge` group; xfail-tracked, fix → XPASS) *(batch in flight: renameat2 flags, fallocate
  PUNCH_HOLE, lseek SEEK_HOLE/DATA, O_TMPFILE, statfs/fstatfs layout, times, madvise MADV_DONTNEED, mprotect
  PROT_NONE)*. Likely still after: **SCM_RIGHTS fd-passing** over AF_UNIX (cmsg translation — breaks
  systemd/Docker/D-Bus), **abstract-namespace AF_UNIX** (`sun_path[0]==0` — X11/D-Bus/systemd),
  `clock_nanosleep(TIMER_ABSTIME)` treated as relative (hangs), `MSG_NOSIGNAL` ignored (SIGPIPE instead of
  EPIPE), `pipe2(O_DIRECT)` packet mode, `F_SETPIPE_SZ`, `/proc/self/fd` synth.
- **Host-limited (emulate or leave ENOSYS):** POSIX mqueue `mq_*`(180-185), `timer_create`/`timer_*`(107-111)
  *(could ride kqueue)*; `pidfd`/`io_uring`/NUMA/keyring/module/`ptrace` out of scope.

## Docker API — remaining field/behaviour fidelity (Engine API v1.43, single-node; Swarm out of scope)

| Area | What's left | Pri |
|------|-------------|:---:|
| `docker stats` | real-ish CPU/mem + streaming *(in flight; runtime has no cgroup metrics → best-effort)* | P1 |
| `docker logs` | `-f`/follow, `--since` (tail/timestamps/raw done) | P1 |
| `docker ps` | `--size`, remaining `--filter` keys (status/name/label done) | P1 |
| `docker exec` | `-u` (needs a `SpawnConfig` uid field), `--privileged` (`-e`/`-w`/`-d` done) | P1 |
| `docker run` opts | `--user` (uid not applied), wider `HostConfig`: restart policy, `--cap-add`, `--device`, `--mount`, `--privileged` (`--label` done) | P2 |
| `docker build` | BuildKit/layer cache (every build re-runs from base) — args/target/nocache/labels/digest-IDs done | P2 |
| image inspect | surface `Config.Labels` (stored at build; inspect still emits `{}`) | P2 |
| `docker cp` | non-default-driver named volumes (default `<volumes_dir>/<name>` *in flight*) | P3 |
| IPC | `*ctl(IPC_STAT/IPC_SET)` introspection (macOS `*_ds` layout differs; ENOSYS today) | P3 |

## Platform limitations (macOS host — can't provide the Linux primitive; off the work-list)
Non-PIE `ET_EXEC` fixed-vaddr (the `__PAGEZERO` low-4GB reservation — see the deep bug above for the
achievable workaround), cpu/io throttling (no cpu/io cgroup; mem+pids *are* enforced via rlimit), `pidfd`,
`io_uring`. These would come for free on a linux→linux build.

## Portability matrix (seams exist; only darwin-host / both-guests built)
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux, darwin→darwin.
