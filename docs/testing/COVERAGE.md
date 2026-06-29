# dd — Test Coverage Map & Missing-Coverage Inventory

Survey of where dd's tests live, how to run them, and **what coverage is still missing**, prioritized.
Companion to [`CHARTER.md`](CHARTER.md) (why), [`TESTING.md`](TESTING.md) (how), [`IMAGE-MANIFEST.md`](IMAGE-MANIFEST.md)
(the recipe catalog), and [`../STATUS.md`](../STATUS.md) (live gap inventory) / [`../PLAN.md`](../PLAN.md) (open-bug table).
Read-only survey — does not duplicate the PLAN PART A bug table; it references it.

Counts as of this survey: **~411 basics case definitions** + **346 scenario definitions**; each runs on every
target it applies to, so executions are ~2–3× higher. **127 `.xfail()` markers** across the tree.

---

## STEP 1 — THE MAP (where tests live, how to run, what they cover)

### Surface A — Basics (`dd-tests`, in-process JIT, Rust)

Driver: `cargo run -p dd-tests -- [group] [-e arch]` · `make test` · count via the registry in `src/cases/mod.rs`.
Each case compiles a C guest under `dd-tests/guests/**` and runs it through the JIT; `.oracle()` diffs JIT
stdout+exit against the binary run natively (aarch64) / via qemu (x86_64); `.out()/.has()/.exit()` are golden checks.

| Area (group) | Location | Guests | Targets | What it covers |
|---|---|---|---|---|
| base groups (compat, libc, system, net, proc, threads, posix, ipc, clib, linuxsys, heavy, soak, edge, compile, realsw, containersw, perf, busybox, container, sandbox, x86, darwin) | `src/cases/mod.rs` | `guests/*.c` | Linux x2 (+mac via `port`) | core syscalls, libc, fork/threads/IPC/net, edge corners, busybox-in-rootfs, sentry sandbox |
| abi | `src/cases/ext/abi.rs` | `guests/ext_abi/*.c` (~50) | Linux x2 + mac | codegen/ABI: int/float/SIMD, varargs, struct-by-value/HFA, recursion, jump/fnptr tables, int128, longdouble |
| libc | `src/cases/ext/libc.rs` | `guests/ext_libc/*.c` (~48) | Linux x2 + mac | string/mem/stdio/malloc/math/locale/time/regex/glob/scanf/qsort breadth |
| posix | `src/cases/ext/posix.rs` | `guests/ext_posix/*.c` (~44) | Linux x2 + mac | file/dir/mmap/poll/signal/process/fs-metadata syscalls (oracle) |
| linuxsys | `src/cases/ext/linuxsys.rs` | `guests/ext_linuxsys/*.c` (~33) | Linux x2 | epoll/eventfd/timerfd/signalfd/inotify/sendfile/splice/memfd/pidfd/io_uring/sched (oracle) |
| threads | `src/cases/ext/threads.rs` | `guests/ext_threads/*.c` (~27) | Linux x2 + mac | mutex/condvar/rwlock/barrier/atomics(orders)/TLS/once/sem/spinlock/cancel contention |
| ipc | `src/cases/ext/ipc.rs` | `guests/ext_ipc/*.c` (~20) | Linux x2 + mac | pipes/fifo/SysV+POSIX shm·sem·msg/unix sockets/scm_rights/seqpacket |
| net | `src/cases/ext/net.rs` | `guests/ext_net/*.c` (~23) | Linux x2 + mac | tcp/udp/unix sockets, sockopt, nonblock connect, sendmsg/recvmsg, poll-loops, half-close, getaddrinfo |
| soak | `src/cases/ext/soak.rs` | `guests/ext_soak/*.c` (~17) | Linux x2 | endurance: code-cache/IBTC/SMC/alloc/fork/thread/page-fault churn |
| darwin | `src/cases/ext/darwin.rs` | `guests/darwin/*.c` (~52) | **mac only** | kqueue, mach ports/time/host, GCD dispatch, Mach-O ABI (ctor/TLS/HFA/varargs), BSD syscalls, MAP_JIT |
| completeness | `src/cases/ext/completeness.rs` | `guests/completeness/*.c` (96) | per-engine, oracle | **systematic syscall-table + opcode-space probes** (48 syscall, 26 x86, 22 aarch64) |

Engine mapping (`src/lib.rs`): `src()` → both Linux engines; `port()` → all 3 (incl. **DarwinAarch64**);
`darwin_src`/`darwin_libc` → mac only; `fixture()` → prebuilt x86; `in_rootfs()` → aarch64 only.
x86-source guests run on the x86 engine only, oracle vs qemu.

### Surface B — Scenarios (real software via the daemon, Rust)

Driver: `cargo run -p dd-tests --bin scenarios -- [--backend dd|real] [-c cat] [-t arm|amd] [--long] [--count]`
· `make scenarios CAT= TGT=`. Boots ONE `dd-daemon`, pulls popular images, runs deterministic workloads
(`--backend real` = host docker oracle to prove the test correct; `--backend dd` = the JIT under test).

| Category | Location | ~defs | What it covers (real software) |
|---|---|---|---|
| distros | `src/scenarios/distros/mod.rs` | 56 | ubuntu/debian/alpine/fedora/rocky/alma/arch/amazonlinux/busybox: os-release, coreutils, pkg-mgr presence, fork-heavy shell (glibc vs musl) |
| databases | `src/scenarios/databases/mod.rs` | 43 | postgres/mysql/mariadb/redis/valkey/mongo/nats/etcd/memcached/couchdb/influxdb — real client over loopback |
| languages | `src/scenarios/languages/{python,node,ruby,golang,java,php,rust,scripting,dotnet}.rs` | 51 | compute/json/REPL across runtimes; JVM/V8/CoreCLR = JIT-in-JIT |
| web | `src/scenarios/web/mod.rs` | 35 | nginx/httpd/caddy/traefik/haproxy/varnish — loopback curl/version |
| toolchains | `src/scenarios/toolchains/mod.rs` | 40 | gcc/clang/go/rust/make/cmake compile+link+run inside container |
| utilities | `src/scenarios/utilities/mod.rs` | 58 | jq/openssl/git/socat/bash/vim/curl/coreutils/busybox |
| weird | `src/scenarios/weird/mod.rs` | 34 | JIT-in-JIT (LuaJIT/PyPy/YJIT/BEAM/Julia/RyuJIT), exotic syscalls, codegen-heavy, unusual langs |
| terminal | `src/scenarios/terminal/mod.rs` | 29 | **PTY path** (`docker exec -it`): isatty/termios/TIOCGWINSZ/job-control |

Default targets = **both Linux arches**. `Target::ArmMac` and `.plus_mac()` exist in `src/scenario.rs` but
**no scenario opts into mac** → the entire real-software surface is Linux-only today.

### Surface C — Bash end-to-end (Docker CLI/API & daemon), under `dd-tests/scenarios/`

| Script | `make` target | Covers |
|---|---|---|
| `docker.sh` | `test-docker` | run/logs/stop/kill/volumes/networks lifecycle |
| `docker-full.sh` | `test-docker-full` | full Docker CLI/API compliance matrix (maps each non-compliant verb) |
| `compose.sh` | `test-compose` | compose up/ps/logs/exec/down |
| `docker-net.sh` | `test-docker-net` | **container-to-container networking — currently expected to FAIL** (no shared bridge yet; executable spec) |
| `macos-container.sh` | `test-macos` | macOS-container parity (same lifecycle, Linux + native-mac container) |
| `realsw.sh` / `smoke-realimage.sh` | `test-realsw` / `test-smoke` | fresh-pull real glibc distro on both arches (the `libc.so.6` guard) |

### Coverage tooling — `make coverage` (`dd-tests/tools/coverage.sh [static|dynamic|all]`)

- **static** — parses the engine's syscall switch (`os/linux/service.c` `switch(nr)`) and diffs vs the full
  asm-generic ABI (`<asm-generic/unistd.h>`) → MISSING canonical syscalls by name; and parses
  `frontend/x86_64/sysmap.h` → x86 syscalls whose canonical target hits the default (DEAD-MAP → -ENOSYS).
- **dynamic** — runs the compiled guests + busybox applets through each engine and harvests the engine's own
  diagnostics: `[jit] unhandled syscall N` and `[jit86] UNIMPL <0F|1B> opcode 0xNN`, resolving numbers to names.
  Output is to stdout (no persisted artifact). Needs `make jit` + reaches the mac JIT via the `mac` bridge.

### The run matrix (surface × run-class × target)

| | linux/amd64 (`LinuxX86_64`) | linux/arm64 (`LinuxAarch64`) | darwin/arm64 (`DarwinAarch64`) |
|---|---|---|---|
| **Basics: `port`** | ✅ | ✅ | ✅ |
| **Basics: `src` (incl. oracle, syscall completeness)** | ✅ | ✅ | ❌ |
| **Basics: x86 opcode completeness** | ✅ | n/a | ❌ |
| **Basics: aarch64 opcode completeness** | n/a | ✅ | ❌ |
| **Basics: darwin guests** | ❌ | ❌ | ✅ |
| **Scenarios (all 8 categories)** | ✅ | ✅ | ❌ **none** |
| **Run-classes** | quick (cache-only) + long (`--long`, pulls images) | same | basics only |

---

## STEP 2 — WHAT'S MISSING (prioritized)

### Top gaps (most impactful first)

| # | Area | What's missing | Bug-class it would catch | Priority |
|---|---|---|---|---|
| 1 | **x87 transcendentals** | `completeness/x86_x87.c` exercises only `FSQRT`+`FYL2X`. **No test drives `FPREM/FPREM1/FSIN/FCOS/FPTAN/FPATAN/F2XM1/FSCALE`** — exactly the family behind the live `ruby-x87-fprem` bug (PLAN PART A #3). | x87 UNIMPL / wrong-result on real interpreters (ruby, perl, old glibc `printf` long-double) | **P0** |
| 2 | **Scenarios on darwin** | Zero real-software scenarios run on `ArmMac` (`.plus_mac()` unused). Darwin is exercised only by ~52 darwin basics guests + portable `port()` cases. JIT-in-JIT on mac (MAP_JIT/RWX) is untested by any real workload. | mac-only MAP_JIT/codegen/dyld divergences; the "lighter-touch but proven" charter goal is unmet for real software | **P0** |
| 3 | **Multithread corruption under stress** | Thread coverage is broad but **deterministic single-shot** (golden contention sums). No repeat-N / randomized-schedule / long-soak stress that surfaces *races*. The live `go-tool-compile` crash (multithreaded copystack/morestack, PLAN #2) is a guest-runtime threading bug no C thread test reproduces. | intermittent memory corruption, TLS/atomics ordering, copystack/signal-on-thread bugs | **P1** |
| 4 | **Toolchain link path (have spec, still red)** | `go build`/`go run`, `cc/ld`, rustc, ghc, dotnet build are all authored **and xfail'd on LINUX** — they ARE the executable spec for `toolchain-link-hang` (PLAN #1) and `go-tool-compile` (#2). Gap is the *fix*, not the test. Watch for XPASS. | nested fork+exec+pipe link hang; internal-linker codegen | **P1** (test exists; tracked) |
| 5 | **Networking — multi-container** | The only multi-container/bridge test is `docker-net.sh`, which is **expected-to-fail today** (no shared bridge/IPAM/embedded DNS). No Rust scenario covers container↔container by-name DNS or cross-network isolation. Basics `net` covers loopback only. | service-to-service nets (compose web+db); DNS/IPAM regressions | **P1** |
| 6 | **AVX/VEX & advanced x86 SIMD on real software** | `completeness/` probes SSE/AVX/crypto/BMI directly, but **no real-software scenario forces AVX2/AVX-512/SHA-NI/PCLMUL paths** (openssl/ffmpeg/numpy/zstd built `-march=native`). If completeness comments (`UNIMPL VEX 0xC5/0xC4`) are still live, broad real images would hit them. | VEX-encoded opcode UNIMPL aborts in optimized libs (openssl, numpy, video/codec) | **P1** |
| 7 | **Security / DD_* validation** | The only sandbox coverage is the `sentry` split (3 guests ×2 = trusted+`.untrusted()`/`DDJIT_UNTRUSTED`) + 3 jail-containment `sh` cases. **`DDJIT_SANDBOX` (Seatbelt) is explicitly OFF; no test for `DD_*` env validation / arg sanitization / path-escape attempts.** | sandbox-escape, env/arg injection, rootfs path-traversal regressions | **P2** |
| 8 | **Performance regression gate** | `make bench` exists (`src/bin/bench.rs`) but there is **no perf threshold/baseline gate** — `perf` group is a single correctness case (`sortbig`). Nothing fails on a slowdown. | silent throughput/codegen regressions (e.g. an opt revert) | **P2** |

### Secondary / breadth gaps

| Area | What's missing | Why it matters | Priority |
|---|---|---|---|
| Fuzzing | No fuzz/proptest/quickcheck anywhere (instruction-stream or syscall-arg fuzzing). | Random opcode/arg corners the curated corpus misses. | P2 |
| Syscall completeness comments stale | `completeness.rs` carries `GAP` comments (openat2/mincore/membarrier/pidfd/adjtimex/syncfs/close_range/sched-attr/name_to_handle_at) with **no `.xfail()`** — either fixed (comments stale) or gate-relevant. Reconcile against live `make coverage static`. | Drift between doc and gate; a real gap could be unmarked. | P2 |
| Stale xfail markers | STATUS.md lists XPASS-but-not-cleared: `edge/madvise`, `edge/renameat2`, `edge/fallocate`, `edge/lseekhole` `[both]`, `edge/times` `[arm]`. Clearing them re-arms regression detection. | A future regression silently stays "expected fail". | P2 |
| Recheck backlog | `weird-python-slim-amd` (jit86-opcode-1c now fixed), `weird-tsc/vdso/hwcap`, `weird-toolchain-suite` — gated cases to re-run + reclassify. | Hidden XPASS = unrealized coverage. | P3 |
| etcd/nats round-trips | FROM-scratch images (no `/bin/sh`) → only `--version` banner cases; no live put/get / pub/sub. Needs busybox-based image or multi-container support. | DB protocol round-trip under JIT untested for these two. | P3 |
| Extra runtimes | Kotlin/Scala, Deno/Bun, Crystal, GHC, OCaml, Zig, Swift, R, Julia, LuaJIT — most Linux-only or absent. | Broader JIT-in-JIT / codegen surface. | P3 |
| io_uring / async I/O at scale | `linuxsys` has an `io_uring` probe but no real-software async-I/O workload (e.g. a server using io_uring). | Modern high-perf I/O path under JIT. | P3 |

### Did the recent bug classes have a test that would have caught them?

| Bug class | PLAN id | Test that would have caught it? |
|---|---|---|
| **x87 / m80 (80-bit) float** | #3 `ruby-x87-fprem` | **NO (direct).** `x86_x87.c` covers only FSQRT+FYL2X; `ext_abi/longdouble.c` is per-arch golden (not oracle-diffed for the FPREM path). The ruby scenarios run on x86 and *may* surface it incidentally, but no probe targets `FPREM`/transcendentals. → **Gap #1.** |
| **go link-spin** | #1 `toolchain-link-hang` | **YES.** `toolchains/mod.rs` go build/cc/ld + `weird` `cc()` cases exist, xfail'd on LINUX — the executable spec; XPASS fires when fixed. |
| **go tool compile crash** | #2 `go-tool-compile` | **YES (as a workflow).** `golang.rs` `go run`/`go build` cases catch it (xfail'd). But the *root* (multithreaded copystack) has no isolated reproducer → see Gap #3. |
| **epoll-abi** | (not in PART A head) | **YES.** `epoll.c` + `epoll_{oneshot,mod,pwait,et}.c` are oracle-diffed against native → behavioral/ABI divergence caught. Well-covered. |
| **multithread corruption** | adjacent to #2 | **PARTIAL.** 27 `th_*` guests + `threads_many` (64-way) + soak `threadpool`/`threadchurn` give broad coverage, but all are deterministic golden checks; flaky/race corruption is not reliably triggered (no repeat-N / randomized stress). → **Gap #3.** |

---

*Generated by a read-only coverage survey. Engine fixes and the PART A bug table are owned by `../PLAN.md`;
this file only maps tests and enumerates missing coverage.*
</content>
</invoke>
