# dd — live gap inventory (what is missing / crashing / not-ok)

Every divergence a test surfaces lands here. A test is written for it (marked `xfail` so the gate stays
green), and a **diagnostic agent** is spawned to root-cause it. When the engine lane fixes it, the test
flips to XPASS and the row moves to "Resolved".

Format: `id` · target(s) · symptom · suspected area · diagnostic agent.

## Open — engine bugs (do NOT fix in the test lane; spawn a diagnostic agent)

| id | targets | symptom | suspected area | diag agent |
|----|---------|---------|----------------|-----------|
| _fork-exec_ | arm-linux | `fork()` then `execve()` of a non-PIE ET_EXEC (gcc/cc1, redis, postgres) → SIGSEGV. Fresh exec OK; post-fork dense layout collides with baked absolute refs. | `execve` doesn't munmap old address space before `load_elf`; non-PIE relocated off fixed vaddr (macOS __PAGEZERO) | _(from project memory; pending)_ |
| _rwx-mmap_ | arm-mac | `mmap(PROT_READ\|WRITE\|EXEC)` → EPERM (macOS W^X, no MAP_JIT). Guest JIT runtimes (JVM/V8/LuaJIT) can't get exec pages. NOTE: the *correct* MAP_JIT + `pthread_jit_write_protect_np` path DOES work under jitdarwin — see `darwin-wx/mmap-jit` (green). Only plain `mmap(PROT_EXEC)` without MAP_JIT is the gap. | W^X / JIT-region handling on darwin | _(pending)_ |
| _darwin-kqueue-signal_ | arm-mac | `EVFILT_SIGNAL` never fires under the darwin engine: register SIGUSR1, `raise()` it, `kevent` blocks the full timeout and returns 0. The other four filters (READ/WRITE/TIMER/VNODE/USER) all work. Case `darwin-kqueue/kq-signal` xfail'd. | jitdarwin/darwinjail signal-disposition → kqueue knote plumbing (signal delivery not reflected to kqueue) | _(pending)_ |
| _darwin-spawn-jail-arch_ | arm-mac | `posix_spawn` of a system binary (`/usr/bin/true`) fails: the guest runs natively under the arm64 `darwinjail.dylib` (`DYLD_INSERT_LIBRARIES`), inherited by the spawned child — but system binaries are arm64e, so dyld aborts the child on an arch mismatch (`incompatible architecture (have 'arm64', need 'arm64e')`) and the spawn never completes. Case `darwin-bsd/bsd-spawn` xfail'd. | darwinjail.dylib built arm64 not arm64e → can't insert into arm64e spawned children (or spawn should drop the insert for non-jailed children) | _(pending)_ |
| _edge-syscalls_ | linux | ~13 obscure syscalls diverge from real Linux (madvise DONTNEED no-op, renameat2 flags, fallocate PUNCH_HOLE, SEEK_HOLE/DATA, O_TMPFILE, pipe2 O_DIRECT, abstract AF_UNIX, F_SETPIPE_SZ, clock_nanosleep ABSTIME, …) | per-syscall service.c handlers | _(tracked in PLAN.md)_ |

## Open — engine/daemon bugs found by recon 2026-06-29 (each needs a diagnostic agent)

| id | targets | symptom | bucket | diag agent |
|----|---------|---------|--------|-----------|
| _exec-loader-noent_ | arm+amd linux | container entry binary → `open: No such file or directory`; blocks hello-world (static non-PIE), nats, golang, rustc, httpd. nginx works → binary-link-shape dependent. Same family as `fork-exec` / the non-PIE class. | A loader/exec | pending |
| _mongo-cpu-topology_ | arm+amd linux | mongod tcmalloc aborts: `NumPossibleCPUs cpus.has_value()(false)` — possible-CPU enumeration empty (sched_getaffinity / /sys cpu). Also `weird/sched-affinity`. | D cpu-topology syscall | pending |
| _realsw-dynamic-glibc_ | arm linux | `realsw/perl-sieve` + `io-churn` (ubuntu): perl run through the engine exits 0 with EMPTY stdout (correct rootfs selected). Pre-existing dynamic-glibc real-software path gap; possibly same family as _dyn-memmove-bus_. | dynamic glibc loader/exec | pending |

### Stale xfail markers to CLEAR (recon saw XPASS — engine already fixed these)
`edge/madvise` [aarch64,x86_64] · `edge/renameat2` [aarch64,x86_64] · `edge/fallocate` [aarch64,x86_64] ·
`edge/lseekhole` [aarch64,x86_64] · `edge/times` [aarch64]. Remove the `.xfail()` so a future regression re-fails.

### Harness fact (not a bug — drives the framework design)
On the linux dev host the daemon is a **Mach-O binary run mac-side via the `mac` bridge**: env is dropped
crossing the bridge and the daemon socket lives mac-side (a linux `docker` can't reach it). Therefore the
Rust scenario harness drives the daemon **and** `docker` through `mac bash <script-file>` (env inline,
socket/state under a `/Users` shared path), mirroring `lib.rs`. On a real macOS host it runs direct.

## Open — concurrency/IPC/net gaps found by threads/ipc/net agent 2026-06-29 (each needs a diagnostic agent)

All surfaced by the new `ext_threads` / `ext_ipc` / `ext_net` basics groups. Each is xfail-marked on the
affected target(s); every one passes on the native-on-macOS path (or native oracle), so the divergence is
Linux-engine emulation. Same-binary native-vs-JIT diff isolates each.

| id | targets | symptom | suspected area | diag agent |
|----|---------|---------|----------------|-----------|
_xproc-futex-fork RESOLVED (sem/eventfd/lockf all fixed — see Resolved)._

## Open — weird/edge corners (category: weird, 2026-06-29) — each needs a diagnostic agent

All rows below PASS on the real oracle (verified, arm64); the listed targets are *suspected* dd
divergences and are `.xfail()`'d in `src/scenarios/weird/mod.rs`. Markers are pinned/deterministic.

RESOLVED in this section: _weird-rwx-exec_/_weird-smc-retranslate_ (aarch64 SMC, commit a5cba9d), _weird-signal-on-fault_ (0b48e04), _weird-seccomp-eperm_/_weird-seccomp-install_/_weird-ptrace_/_weird-cpu-topology2_ (1566613). Still OPEN:

| id | targets | symptom (suspected on dd) | bucket | diag agent |
|----|---------|---------------------------|--------|-----------|
| _weird-toolchain-suite_ | arm linux | gcc DRIVER + cc1 + as now work (commits); full `cc /m.c -o /m` blocked at the LINK stage (collect2/ld plugin hang — see _collect2-link_). Once link works the ~21 `weird/*` deeper probes (tsc/vdso/hwcap/...) unlock and reveal their next layer. | A loader/exec → now D link | pending |
| _collect2-link_ | arm linux | gcc link stage: collect2 (with -plugin liblto_plugin.so) infinite-loops after installing signal handlers (~1090 blocks, 100% CPU; repeatedly bl's a cmp helper at guest 0x..86b78). `ld --version` alone works. Last gate to gcc→/m→42. | aarch64 dispatch/translate (maybe shares root w/ LDAR) | pending |
| _weird-build-fork_ | arm+amd linux | `weird/haskell-ghc` (ghc forks as/ld) + `weird/dotnet-ryujit` (dotnet SDK) — same toolchain family; recheck after gcc link works. | A loader/exec (toolchain) | pending |
| _weird-tsc/vdso/hwcap_ | arm+amd linux | tsc-counter / vdso-clock / auxval-hwcap / simd-probe — were gated by the toolchain compile; recheck now that cc1+as work (only the link is left). | K/L/M | recheck |
| _weird-python-slim-amd_ | amd linux | python:3.12-slim probes were xfail for jit86-opcode-1c (byte ADC/SBB), now FIXED upstream — recheck; non-slim python already passes (discovery). | B amd64 opcode (likely resolved) | recheck |

## Open — syscall/opcode COMPLETENESS gaps found by completeness agent 2026-06-29 (each needs a diagnostic agent)

Surfaced by `cases/ext/completeness.rs` (96 compiled guests: 48 syscall, 26 x86-64 opcode, 22 aarch64
opcode — no docker images). Each guest drives one syscall/instruction-family with deterministic args and
`.oracle()`-diffs JIT vs native (aarch64 direct, x86_64 via qemu). 107 green / 37 xfail. The 22 aarch64
opcode guests (NEON base + crypto/CRC/LSE/FP16/dotprod/i8mm/bf16) all PASS — the aarch64 backend's
instruction coverage is complete for everything probed. The gaps below are x86_64-translator UNIMPL
aborts and missing/wrong syscall handlers. NB `comp-sys-proc/clone3` xfails on x86_64 ONLY because
qemu-user (the oracle) lacks clone3 — the dd engine itself handles it (aarch64 passes); not an engine gap.

### Syscall handlers — missing / error / wrong value (engine, not oracle)

| id | targets | symptom | suspected area | diag agent |
|----|---------|---------|----------------|-----------|
_All syscall-handler completeness gaps in this batch are RESOLVED (see Resolved section): openat2,
name_to_handle_at, sched_get_priority_min, adjtimex, syncfs, mincore, membarrier, process_vm_readv,
close_range, pidfd, auxv-pagesz, auxv-clktck._

### jit86 (x86_64→ARM64) translator — UNIMPL opcode aborts (exit 70, empty stdout)

Each row is a confirmed `[jit86] UNIMPL … opcode 0x…` abort; aarch64 has no equivalent (all aarch64
opcode guests pass). The whole VEX/AVX lane is the biggest single hole. Bytes are the captured fault bytes.

| id | opcode | instruction (ext) | case | diag agent |
|----|--------|-------------------|------|-----------|
| _jit86-vex_ | `C5` (2-byte) / `C4` (3-byte) VEX | AVX/AVX2/FMA/F16C lane: VEX/EVEX decode + avx.c landed, but `comp-x86-avx/{avx,avx2,fma,f16c}` still xfail — finish the VEX op coverage | `comp-x86-avx/{avx,avx2,fma,f16c}` | pending |

## Open — coverage gaps (categories not yet authored)

_Populated as category agents report. Each row: category · what's not yet covered · why._

| category | not yet covered | why |
|----------|-----------------|-----|
| databases | etcd put/get round-trip; nats pub/sub round-trip | The `quay.io/coreos/etcd` and `nats:latest` images are FROM scratch (no `/bin/sh`), so the harness `exec` bring-up path is impossible — only run-form `--version` banners are expressible in one container. A live multi-process round-trip would need either a busybox-based etcd/nats image or multi-container scenario support. |
| databases | mysql/mariadb/mongo markers verified live | postgres/redis/valkey/memcached/nats/etcd/couchdb/influxdb were verified against the Real docker oracle; mysql/mariadb/mongo (huge images, slow boot) rely on the IMAGE-MANIFEST §3 pinned markers + standard SQL/mongosh syntax. Verify live on next nightly. |
| web (servers/proxies) | authored — 35 scenarios / 70 cases, all green on the Real oracle (arm) | nginx (musl+glibc), caddy, traefik, haproxy, varnish covered; loopback-only/hermetic. **httpd (alpine+glibc) is `.xfail(BOTH)`** reusing the `exec-loader-noent` row above (apache entry binary fails to load under dd; nginx works → binary-link-shape dependent). caddy/traefik (Go runtimes, goroutine schedulers) + varnish (VCL→C dlopen codegen) are NEW dd stress paths not previously exercised — watch the first `--backend dd` run for divergences. |
| languages | authored — 51 scenarios / 102 cases, all green on the Real oracle (arm); py-fib-312-slim also spot-checked on amd64 | python/node/ruby/go/java/php/rust/perl/elixir/.NET. xfail rows reuse existing GAPS: python:3.12-slim + node:20-slim `.xfail(AmdLinux)` (`jit86-opcode-1c`); golang + rustc `.xfail(BOTH)` (`exec-loader-noent`). JIT-in-JIT runtimes (JVM/V8/CoreCLR — javac/dotnet SDK builds + V8 hot loops) are PRIME dd engine stress, NEW paths — watch first `--backend dd` run, esp. .NET SDK (RyuJIT, RWX heaps) which is **not** yet xfail'd. amd64 markers unverified live (Docker Hub pull rate-limit) but are arch-invariant deterministic arithmetic + language-serializer JSON. |
| languages | JIT-in-JIT on **arm-mac** (JVM/V8/CoreCLR); extra runtimes (Kotlin/Scala, Deno/Bun, Crystal, GHC, OCaml, Zig, Swift, R, Julia, LuaJIT) | Linux-only (no `.plus_mac()`) to avoid the known `rwx-mmap` macOS W^X gap until MAP_JIT lands. openjdk:*-slim recipes from MANIFEST §2 dropped — those tags were REMOVED from Docker Hub (repo deprecated) → substituted eclipse-temurin JDKs. |

## Open — newly found by the engine-fix lane (2026-06-29 night)

Real-software smoke discovery (2026-06-29 night). PASS: distros (deb/ubuntu/alpine), busybox (musl+glibc), python (both arches incl sqlite/hashlib), php, memcached, nginx, psql, mariadb client, curl/openssl/git/perl, httpd, gcc/go *driver*, nats-server --help. OPEN:

| id | targets | symptom | suspected area | status |
|----|---------|---------|----------------|--------|
| _sse2-packed-int_ | amd linux | redis-server aborts on UNIMPL `0F F2` (PSLLD), valkey on `0F E4` (PMULHUW) — whole SSE2 packed-int shift/mul/add family missing. | frontend/x86_64 translate/avx | WIP (#28) |
| _ud2-sigill_ | amd linux | `0F 0B` (UD2) prints UNIMPL and CONTINUES instead of SIGILL → ruby -e diverges into a bogus HLT abort. | frontend/x86_64 translate | WIP (#28) |
| _ldar-excl_ | arm linux | LDAR mis-detected as LDXR exclusive region (mask omits bit23) → in_excl sticks → defer[64] overflow on >64 conditionals after an LDAR (C++/glibc atomics). | frontend/aarch64/translate.c | WIP (#29) |
| _node-e_ | arm+amd linux | node -e fails: x86 V8 `Check failed: 0==munmap(addr,size)` (partial/range munmap); aarch64 worker-thread futex busy-spin hang. (node --version works.) | service/mem.c munmap-range + thread.c futex | WIP (#30) |
| _collect2-link_ | arm linux | gcc cc1+as work; LINK stage collect2 (-plugin liblto_plugin.so) infinite-loops after installing signal handlers. Last gate to full gcc compile. | aarch64 dispatch/translate | pending (#23) |
| _go-arm-runtime_ | arm linux | `go version` SIGSEGV (NPGUARD low-VA ~0x75b360 SIMD load) / SIGABRT — Go heap-arena MAP_FIXED / signal setup. Blocks Go on aarch64. | service/mem.c MAP_FIXED, frontend/aarch64 | pending (#31) |
| _glibc-grep-simd_ | amd linux | glibc `grep` (debian) SIGSEGV in a vectorized string routine (AVX2 memchr/strchr); busybox/musl grep works. | frontend/x86_64 AVX2 string path | pending |
| _mongod-sigill_ | arm linux | mongod --version SIGILL (unimpl aarch64 insn in the large C++ binary); mongosh hangs. | frontend/aarch64/translate.c | pending (#32) |
| _nonpie-native-vaddr_ | arm+amd linux | PLATFORM LIMITATION (not fixable): native low-vaddr non-PIE load is impossible (macOS mandates ~4GB __PAGEZERO). The bias+fixup+dispatch-redirect path is the supported approach; per-fault fixups serve absolute data, adr/adrp emit LOW, Go pclntab is rebased. | — | wontfix (documented) |

## Resolved (XPASS — gap closed by the engine lane, 2026-06-29 night)

- _ext-shmstat-arm_ — shmctl IPC_STAT now fills shmid64_ds with the requested segsz (commit `1fc8589`).
- _comp-mincore_ / _comp-membarrier_ / _comp-process_vm_readv_ — implemented in service/mem.c (commit `96828ae`).
- _ext-condvar-timedwait_ — futex FUTEX_WAIT_BITSET absolute deadline (commit `7429c15`).
- _comp-auxv-pagesz_ — SUPERSEDED: AT_PAGESZ must equal the host mmap granularity (16K on Apple Silicon) for ld.so segment maps to work (commit `654295b`); on a 16K-page host the guest's page size IS 16K. The `4096` expectation is a PLATFORM LIMITATION, not achievable while hosting on 16K pages — the test should accept the host page size.
- _dynamic-elf-loading_ — AT_PAGESZ=host page fixes the ld.so segment-map SIGBUS in _platform_memmove; bullseye/alpine dynamic binaries (/bin/*, python3) now load (commit `654295b`). [bookworm/ubuntu newer-glibc still crash in translate_block — separate open gap.]
- _ext-condvar-timedwait_ sibling — _posix-sem-named_ cross-process futex over fork (shared bucket table, commit `86db1a6`).
- _weird-signal-on-fault_ — guest signal delivery from a synchronous fault in translated code (commit `0b48e04`); siglongjmp-recover works both arches.
- _weird-smc-retranslate_ / _weird-rwx-exec_ — aarch64 self-modifying-code detection via ic ivau + CTR_EL0 synth (commit `a5cba9d`); soak/smc XPASS. [The docker `cc()` weird scenarios remain gated by the toolchain fork-exec gap.]
- _mongo-cpu-topology_ / _weird-cpu-topology2_ / _weird-seccomp-*_ / _weird-ptrace_ — CPU topology + seccomp/ptrace parity (commit `1566613`).
- _jit86-vex_ — AVX/AVX2/FMA/F16C VEX lane (commit `186f529`); comp-x86-avx/* XPASS.
- _amd64-image-register_ / _docker-build-register_ — daemon arch-detect fallback + full-tag register + rescan (commit, daemon).
- _jit86-wcschr-bool_ — byte reg-to-reg mov full-width copy (commit `7ba0317`, upstream).
- _jit86-opcode-1c_ — byte ADC/SBB (commit `d414a8f`, upstream).
- _jit86-{movmskps,cvtdq2ps,psadbw,rol-cl,parity,lrint-round,fcmp-unordered,pabsb,dpps,crc32-rm,crc32-rm8,aesni,pclmul,sha-ni,movbe,cmpxchg16b,rdtscp,movntdq}_ — 0F38/0F3A 3-byte map (do_sse3b) + missing SSE opcodes + flag fixes (PF lane, MXCSR, COMISD unordered) (commit `abbfab4`). 21 x86_64 cases XPASS.
- _lstat-nofollow_ — atpath nofollow; lstat/unlinkat honor AT_SYMLINK_NOFOLLOW (commit `bb18274`).
- _aarch64-nonpie-fixup_ — aarch64 non-PIE absolute data-ref fault handler; static non-PIE runs (commit `9126a1f`). Dynamic non-PIE (gcc) still blocked by _dyn-memmove-bus_.
- _comp-{openat2,name_to_handle_at,sched_prio_min,adjtimex,syncfs,close_range,pidfd}_ + _ext-{seqpacket,peercred,mq}_ + _ext-sem-open_ (non-fork) + statx-nofollow — 11 syscalls + statx in service.c/fscache.c (commit `d2d431b`). XPASS both arches.
- _ext-spinlock-x86_ — atomic LOCK inc/dec (group4/5) in translate.c (commit `ec36f61`). XPASS x86_64.
- _darwin-spawn-jail-arch_ + _rwx-mmap_ — drop DYLD insert for arm64e children + plain RWX mmap via MAP_JIT (commit `a98bebb`). bsd-spawn XPASS.
- _busybox-find-empty_ / _gcc-image-rootfs-leak_ — were test-harness rootfs-selection artifacts (wrong-arch image picked); fixed by arch-aware `rootfs_path` (dd-tests/src/lib.rs, uncommitted WIP). busybox 20/20, container 13/13.
- _comp-auxv-clktck_ — already 100 on x86_64 (fixed upstream `d414a8f`)._comp-mincore/membarrier/process_vm_readv_ listed above.
- _daemon-namespaced-route_ — namespaced image routes + rmi disk cleanup (commit `d7ad373`).
- _xproc-eventfd-lockf_ — eventfd counter MAP_SHARED across fork + svc_io boundary errno translation (lockf, eventfd-nonblock, pipe2) (commit `cbac247`).
- _go-nonpie pclntab_ — rebase Go firstmoduledata/pclntab for non-PIE (commit `2a365d5`); nats past nil-deref.
- _newglibc-translate_ — FALSE ALARM (bookworm/ubuntu images are x86_64; arch-mismatch guard added, commit `7638d07`).
- _darwin-kqueue-signal_ — raise()→process kill so EVFILT_SIGNAL fires (commit, darwin).
- _go-greg/storm_ — bsf/bsr/tzcnt dest==src clobber (broad bytes.IndexByte bug) + POPCNT + STD/DF (commit, x86_64); nats runs full runtime init.
- _go-nats-runtime_ — x86-64 clone tls/ctid ABI swap + non-PIE type-pointer LOW consistency + RCL/RCR (commit); nats-server --help runs.
- _jit86-jcc-parity/PF/rep-DF_ — jp/jnp via PF lane, PF on shifts/neg/sahf, backward rep cmps/scas (commit); soakext/bitchurn XPASS.
- _gpr-field-mask_ — aarch64 FP→GPR converts (FCVTZS/SCVTF/FMOV-general) + UMOV/SMOV/DUP/INS no longer corrupt stolen host regs; arch-general correctness (commits); cc1 compiles to asm.
- _adrp-low_ — aarch64 non-PIE adr/adrp materialize LOW guest address (commit); gcc 16.1.0 driver + cc1 + as work.
- _mutex-errorcheck_ — already passing (stale row; userspace glibc + stable tid).
- _amd64 modern distros_ — bookworm/ubuntu x86_64 dynamic binaries run on the x86_64 engine (discovery).

---

### Bug protocol (for builder agents)
When your case fails on a target and it's an engine issue (not a flaky test):
1. Add `.xfail(&[Target::ArmLinux])` (or the affected target) to the `scen(...)` builder, with a comment
   referencing the GAPS row, so the gate stays green and XPASS fires when it's fixed.
2. Add a row under **Open — engine bugs** with id/target/symptom/suspected-area.
3. Report the bug in your final message so the orchestrator spawns a diagnostic agent.
Never edit dd-jit engine C. Tests document; diagnostic agents root-cause; the engine lane fixes.
