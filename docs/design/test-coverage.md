# Bulletproof coverage plan

Goal: make dd **bulletproof** — every guest behaves *bit-identically to real Linux/x86*, every landed
optimization is *provably transparent* (gate on == gate off), **every everyday Docker workflow actually
works** on real images (not just "the verb is accepted"), the system survives abuse (limits/leaks/crashes/
concurrency), and nothing silently regresses.

This is **additive** to today's harness (engine×case matrix, `edge_*` guests, `coverage.sh`,
`scenarios/{docker,docker-full,docker-net,compose,macos-container,realsw}.sh`, soak). It closes the gaps
the wave-1–6 sweep exposed and locks in everything it landed.

## Five pillars
1. **Correctness** — differential vs ground truth + edge + fuzz.
2. **Optimization transparency** — every kill-switch on==off, including stacked.
3. **Functional Docker** — real CLI/Compose workflows on real images, incl. interactive `-it`.
4. **Robustness** — limits, leaks, crashes, concurrency, soak.
5. **No-regression** — perf/RSS/coverage gates + a permanent suite for every bug ever found.

---

## Matrix dimensions (apply across all workstreams)
- **engine**: `linux/x86_64` (jit86), `linux/aarch64` (jit), `darwin/aarch64` (jitdarwin)
- **run mode**: standalone binary · daemon (`docker --context dd`) · warm fork-server · `DDJIT_PCACHE` warm
- **ELF type**: PIE · static-PIE · non-PIE `ET_EXEC` · dynamic (glibc & musl)
- **oracle**: `qemu-x86_64` (x86 ground truth) · native aarch64 / real Linux · the VM (perf baseline)
- **gate**: each optimization env switch ON and OFF (see WS1)

---

## WS1 — Optimization-transparency gate sweep  *(P0, highest bulletproofing ROI)*
Every landed optimization is behind an env kill-switch. Bulletproof = **byte-identical output with the gate
ON vs OFF across the whole corpus**, and — critically — **stacked**, since the nightmare is an opt that's only
wrong in combination.

Gates to sweep (keep this list in sync with `docs/PLAN.md`): `NOREP`, `NOREPCMP`, `NOSTITCH`, `NOLAZY*`,
`NOSSEOPT`, `NOEA`-style addressing, `NOTIER2`/`NOTIER2X`/`TIER2`, `NOMTIBTC`, `NOFUTEXQ`, `NODUALMAP`,
`NORWXFIX`/`NOSMC`, `NOLAZYFIX`, `JIT86_NOFASTSYS`, `JIT86_NOSIGINLINE`, `NOEPOLLOPT`, `W4_NOOPENCACHE`,
`S3DB_DURABILITY`, `DDJIT_PCACHE`.

Sweep design:
- **each gate individually** off vs on, over the corpus → byte-exact stdout/stderr/exit/file-effects.
- **all-off (pristine baseline)** vs **all-on (shipping)** → byte-exact.
- **pairwise + randomized N-combinations** (seeded, reproducible) → byte-exact. Pairwise catches most
  interaction bugs at linear-ish cost; random combos add depth.
- runs on every engine. Output: a gate×corpus pass/fail grid; any mismatch is a release blocker.
- New target: `make test-gates`.

## WS2 — Differential oracle harness  *(P0)*
Promote differential testing from ad-hoc (W3-B/W4-C used a qemu oracle) to the **default correctness gate**.
- Run a large corpus through dd-jit and a ground-truth oracle; compare **stdout + stderr + exit code +
  filesystem effects + (where checkable) final register/memory state**, byte-exact.
- Oracles: `qemu-x86_64` for x86 guests; native aarch64 / a real Linux box (or the VM) for aarch64; for
  syscalls, a thin Linux "do the syscall and dump result" probe.
- Corpus: all `dd-tests/guests/*`, every busybox applet, coreutils, plus the realsw binaries (WS6).
- New target: `make test-diff`. Make it required in CI.

## WS3 — Translator fuzzing (decoder/opcode)  *(P1 — catches the 0xF6/pmuludq/cmpsq class systematically)*
- **Instruction fuzzer**: generate random *valid* x86-64 and aarch64 instruction sequences (template +
  constrained random operands), execute one block under dd-jit vs qemu, diff architectural state (GPRs,
  flags/NZCV, touched memory, xmm/NEON). Seeded & reproducible; shrink failing cases to a minimal opcode.
- Prioritize the families the sweep touched: ALU+flags (lazy-flags), SSE/SSE2/SSE4.2, `rep` string ops,
  addressing/SIB, mul/div forms, x87.
- **ELF-loader fuzzer**: malformed/edge ELF headers, segment overlaps, weird auxv, PIE/non-PIE/static.
- New target: `make test-fuzz` (bounded iterations in CI, longer nightly).

## WS4 — Syscall conformance + fuzzing  *(P1)*
- Fuzz syscall args/flags/struct contents vs a Linux oracle; focus on the struct-translation surface that
  bit us: `sockaddr_in/in6` (the TCP-cork/AF_UNSPEC bug), `stat/statx`, `dirent`, `epoll_event`,
  `msghdr/cmsg` (SCM_RIGHTS), `termios` (needed for `-it`!), `iovec`, `sigaction/sigset`.
- Negative paths: bad fds, EINVAL/EBADF/ENOENT/EAFNOSUPPORT correctness, errno mapping Linux↔macOS.
- Extend the dynamic side of `coverage.sh` to assert *correct results*, not just "didn't hit UNIMPL".

## WS5 — Functional Docker coverage  *(P0 — the explicit ask: everyday docker must really work)*
Today `docker-full.sh` checks the verb is *accepted*. This proves it **behaves correctly** on real images.
No swarm. Every item is a real behavioral assertion.

- **Interactive `-it` / TTY (must work):** `docker run -it`, **`docker exec -it <c> sh|bash`** → real pty,
  raw mode, prompt, command runs, output streams; **Ctrl-C → SIGINT**, **window resize → SIGWINCH**,
  stdin piping (`echo x | docker exec -i`), correct exit codes, clean detach. Drive via a pty harness
  (e.g. `script`/`expect`/a Rust pty).
- **Lifecycle:** `run` (-d/--rm/--name/--env/-w/-u/--entrypoint/--restart), `start/stop/restart/kill`
  (+ `--time` stop-timeout → SIGTERM then SIGKILL), `pause/unpause`, `wait`, `rename`, `update`.
- **exec:** `-i`, `-it`, `-d`, `-e`, `-w`, `-u`, exit-code propagation, multiple concurrent execs.
- **attach / streams:** `docker attach`, `logs` (`-f` follow, `--tail`, `--since`, timestamps), stdout/stderr
  separation, `--detach-keys`.
- **cp:** host↔container both directions, dirs, symlinks, perms/ownership, stdin/stdout tar streams.
- **volumes:** bind mounts (ro/rw, perms, nested), named volumes (create/inspect/rm/prune), `--tmpfs`,
  data persistence across container restart, mount into the right uid.
- **networking:** `-p` publish (host reachable — the TCP-cork regression lives here), container→container
  by-name DNS + by-IP, cross-network isolation, `--network host`, multiple networks, `/etc/hosts`/resolv.
- **images:** `pull` (multi-arch / manifest-list → correct arch), `build` (Dockerfile: FROM/RUN/COPY/ENV/
  WORKDIR/CMD/ENTRYPOINT/ARG/multi-stage), `tag`, `push`, `images`, `rmi`, `prune`, `history`, `save`/`load`,
  `commit`, `inspect`.
- **introspection:** `ps`/`ps -a`, `inspect` (fields docker/compose rely on), `stats` (live), `events`
  (stream during lifecycle), `top`, `df`, `version`, `info`, `port`.
- **negative/error paths:** missing image, name conflict, bad flag, exec into stopped container, rm running
  without -f → the *exact* error code/message the docker CLI expects.
- **Compose:** `up`/`down`/`ps`/`logs`/`exec`/`build`/`pull`, `depends_on`, **healthchecks** drive ordering,
  multi-service networks + named volumes, `--scale`, env/`.env`, profiles. Multi-container realistic stack
  (e.g. web + redis + postgres).
- New targets: `make test-docker-fn` (functional, incl. pty), extend `compose.sh`.

## WS6 — Real-software corpus (functional)  *(P2)*
Expand `realsw.sh` beyond redis/postgres/python/nats/go with deterministic workloads + **output verified**:
- **language runtimes:** node, ruby, perl, php, and the JIT-heavy ones that exercise the new RWX/SMC path —
  **JVM (java), .NET, LuaJIT, V8/node-jitless-off, PyPy** (these are the smc/guest-JIT validation).
- **toolchains:** gcc/clang **compile-and-run** a program (also exercises the non-PIE/fork+exec path → the
  `gosu`/gcc victims), `make`/`cmake`, `git clone`+build.
- **tools/servers:** openssl, curl, jq, ffmpeg, sqlite3 CLI, nginx (serve+curl), busybox **full applet
  sweep**.
- **package managers:** `apk add`, `apt-get install`, `pip install`, `npm install` (these hammer fork+exec,
  network, fs — and the apt/dpkg path that stalled W6-A).
- Multi-arch: run x86 **and** arm variants of each.

## WS7 — Robustness / limits / chaos  *(P1)*
- **Leak gates:** monitor RSS + open-fd count across a long run; **fail on unbounded growth** — this is the
  gate that would have caught the `mremap` leak (30 GB to sort 60 MB). Per-container and daemon-wide.
- **Large inputs:** huge files, 2M-line sort (the historical SIGSEGV), directories with 100k entries
  (getdents), >4096 distinct guard pages (the lazy-fault budget), multi-GB mmaps.
- **Resource pressure:** memory pressure/OOM, fd exhaustion, pipe-buffer limits, disk-full.
- **Signals:** storms, signal-during-syscall, restart (EINTR), mask edges (the sigprocmask inline path),
  SIGPIPE, `stop --time` escalation.
- **Crash/corruption recovery:** corrupt `DDJIT_PCACHE` → clean rebuild, malformed ELF → graceful error,
  daemon restart with live containers, **duplicate-daemon-on-socket detection** (the split-brain we hit).
- **Startup cross-product:** standalone × daemon × fork-server × pcache-warm, each correct + measured.

## WS8 — Concurrency / soak / endurance  *(P1)*
Extend soak (`codecache/indirect/threadchurn/forkchurn/allocchurn`):
- **High thread counts** (16/32/64) + core oversubscription; **threaded-IBTC + futex-queue race stress**
  (verify byte-exact + zero torn dispatch — the W5-C invariant) ; **dual-map fork-safety under load** (the
  flaky fork+exec tail); **SMC under concurrent writers**.
- **Multi-hour endurance** with RSS/fd leak monitoring (nightly); fork+exec churn at scale (gcc-bundle style).

## WS9 — Security / isolation (sentry)  *(P2, gated on sentry integration)*
- Untrusted-guest **escape attempts**; **Seatbelt** deny-default enforcement (worker can't touch host fs/net
  except via sentry); **path-jail escapes** (`../`, symlink, absolute, `/proc/self`, abstract sockets,
  SCM_RIGHTS fd passing); sentry ring under hostile/oversized input (no deadlock/torn/leak); capability
  boundaries. New target `make test-sec`.

## WS10 — Permanent regression suite (every bug ever found)  *(P0)*
A `make test-regress` suite where each entry is a fail-before/pass-after test, so nothing returns:
mremap-leak (RSS gate on big sort) · **TCP setsockopt cork** (keepalive server round-trips) · `0xF6 /4..7`
8-bit MUL/DIV · `pmuludq` · `cmpsq` OF · `strrchr_sse2` tail · `sockaddr_in/in6` translation · `EPOLL_CTL_MOD`
stale filter · non-PIE fork+execve · `0F 18` prefetch · `time` (201) · busybox-sort SIGSEGV · AF_UNIX
scmrights/abstract · signal/pipe edges · duplicate-daemon socket · postgres startup.

## WS11 — Coverage measurement + CI gates  *(P2)*
- Extend `coverage.sh`: report **syscall % and opcode % per engine** (x86 + aarch64), and **fail CI on any
  drop**; auto-grow the corpus toward uncovered opcodes/syscalls.
- **C code coverage** (llvm-cov/gcov) of `src/runtime` — surface untested branches.
- **Perf regression gates**: lock in the headline numbers (path-cache 40×, openat 4.3×, rep-string 36–42×,
  sqlite 3.3× vs VM, fork-server 2.2×, threading 7×) with a tolerance band; alert on regression.
- CI tiers: **per-PR** = test-gates(individual+all) + test-diff + test-docker-fn + test-regress + coverage-no-drop;
  **nightly** = fuzz(long) + soak(multi-hour) + pairwise gate combos + realsw(full) + perf gates.

---

## New Makefile targets (summary)
`test-gates` · `test-diff` · `test-fuzz` · `test-docker-fn` · `test-sec` · `test-regress` ·
`test-soak-long` · extend `coverage` (per-engine %, fail-on-drop) · extend `realsw`/`compose`.

## Phasing
- **P0 (do first — locks in the sweep, biggest bulletproofing):** WS1 gate-sweep, WS2 differential,
  WS5 functional Docker (incl. `-it`), WS10 regression suite.
- **P1:** WS3 fuzzing, WS4 syscall conformance, WS7 robustness, WS8 soak.
- **P2:** WS6 realsw expansion, WS9 security, WS11 coverage+perf gates.
