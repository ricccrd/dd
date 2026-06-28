# dd-jit optimization plan

Consolidated from a parallel optimization sweep (8 wave-1 agents, each an isolated
copy + bit-exact PoC + measured A/B). Every item below was **measured** and verified
**byte-identical** against the unmodified engine on its workloads. Each carries an env
kill-switch so it can be A/B'd and disabled in place.

Full per-lever writeups (mechanism, PoC diff, measured A/B, correctness evidence) live in
[`docs/optimization-research/`](docs/optimization-research/) — each Tier item links its report below.

Two findings frame the whole plan:
- **Engine-C build tuning is a dead end.** `-O3`/`-mcpu`/LTO/hot-cold/PGO all landed
  within ±1–3% noise — steady-state cycles live in *JIT-emitted* code in the cache and
  in syscalls, not in the dispatcher/translate C. Wins must come from the emitters,
  dispatch elimination, and the syscall layer.
- **Indirect-branch dispatch is *not* the awk/sort gap.** A dispatcher round-trip is
  ~35 L1 cycles; busybox awk burns ~2,800 cycles of interpreter *compute* per bytecode.
  The lever is **translated-code quality** (lazy flags, idioms, addressing), confirmed
  by the −22% lazy-flag win.

---

## Tier 1 — land these (high confidence, measured, low risk)

### 1. x86 `rep movs`/`rep stos` → host `memcpy`/`memset`  ·  gate `NOREP=1`
> Research: [`opt5-idioms.md`](docs/optimization-research/opt5-idioms.md)
- **Engine:** jit86 (x86). **Files:** `frontend/x86_64/{translate.c,decode.c}` (+ 2-line
  `rep_upgrade` PROF counter in `os/linux/service.c`, re-apply on integration).
- **Impact:** **2.50×** on copy/fill-bound code; **1.02× faster than native** aarch64.
  Fires on *every* musl libc copy/fill (musl x86 `memcpy.s`/`memset.s` are `rep movsq`/
  `rep stosq`) — 312k× even on hashing via stdio buffering. Real busybox: small, never
  a regression.
- **Why it wins:** generalizes the existing LSE "beat-native" idiom-upgrade — does work
  the static binary couldn't (HP Dynamo principle).
- **Correctness:** bit-exact incl. forward-overlap smear (memmove-wrong case handled by
  an explicit forward branch), lengths 0…4096, unaligned. Validated vs native/baseline/`NOREP=1`.
- **Next:** extend to `rep cmps`/`rep scas` (strcmp/memchr); aarch64 has no direct
  analogue (already native memcpy) so x86-only.

### 2. x86 lazy-flag completion  ·  gate `NOLAZY`-style (see report)
> Research: [`opt3-lazyflags.md`](docs/optimization-research/opt3-lazyflags.md)
- **Engine:** jit86. **Files:** `frontend/x86_64/translate.c` (~80 lines; `cpu->nzcv`
  boundary ABI untouched).
- **Impact:** **b_int sieve −22%** (0.90→0.70s), **300M `add;test;jcc` loop −18.7%**,
  hottest block −20% emitted instructions. awk −2.2% (interpreter-bound).
- **What it adds over the existing partial scheme:** (a) **dead-flag elimination** — drop
  a producer's NZCV when the next op fully overwrites flags and reads none; (b)
  **generalized live-NZCV deferral** for `add`/`and`/`or`/`xor`/`test` (not just sub/cmp),
  Jcc branches off live flags (drops redundant `ldr;msr`).
- **Correctness:** bit-exact across 22 flag-heavy applets, `adc/sbb __int128` chains,
  signed-overflow/carry/signed-compare, compiled C kernels.
- **Notes:** integer PF/AF aren't modeled at all (nothing to defer there). `adc/sbb` left
  correct-but-inline — deferral is future work.

### 3. x86 memory-addressing fast path  ·  gate (see report)  ·  **SUPERSEDES Opt6**
> Research: [`opt7-memory.md`](docs/optimization-research/opt7-memory.md), diff [`opt7.diff`](docs/optimization-research/opt7.diff) · superseded: [`opt6-peephole.md`](docs/optimization-research/opt6-peephole.md)
- **Engine:** jit86. **Files:** `frontend/x86_64/{emit.c,decode.c,translate.c}`.
- **Impact:** **grep −12.8%, wc −7.1%, sha1 −4.0%**; SIMD-bound hashes at noise floor.
  Emitted host insns −2.3…2.7% engine-wide.
- **What it does:** rewrote `emit_ea` (shifted-`add` for `index<<scale`, add/sub-imm for
  disp, movconst only ≥2²⁴) **and** folded `[base+disp]` straight into `ldr/str [base,#imm]`
  (scaled imm12) / `ldur/stur` (signed imm9) on the hot `mov r/m,r` paths. `[rbp-8]` went
  6 host insns → 1. Exact original-path fallback for unencodable / fs-gs / RIP-rel / RMW / SSE.
- **Memory model confirmed:** direct 1:1 host pointers (no softMMU/guest-base) — so EA
  arithmetic *is* the whole cost.
- **Correctness:** bit-exact on 6 benchmarks + 20-applet battery (FAIL=0).
- **⚠ Merge note:** **Opt6 (peephole) and Opt7 both rewrote `emit_ea`.** Land **Opt7 only**
  — it is the strict superset (Opt6 was EA strength-reduction; Opt7 adds the addressing-mode
  fold; grep +12.8% vs Opt6's +9.7% on the same workload). Do **not** stack them.

### 4. Trace / superblock formation  ·  gate `NOSTITCH=1`
> Research: [`opt4-trace.md`](docs/optimization-research/opt4-trace.md)
- **Engine:** shared aarch64 (jit / jitdarwin). **Files:** `frontend/aarch64/translate.c`
  (+81 lines).
- **Impact:** **awk +14.5%, grep −15.3% wall**; sort +3.2%, gzip/sha256 neutral.
  Dispatcher round-trips **−50%**, translations **−43%**.
- **What it does:** greedy superblock formation — follow unconditional `b` edges inline and
  lay conditional fall-through successors inline (condition inverted so the taken side is a
  tiny out-of-line exit). Region bounded to 16 KB / 16 blocks; intermediate blocks left
  unregistered so any mid-region entry self-heals via the existing back-patch path.
- **Correctness:** bit-exact on 8 workloads; `NOSTITCH=1` reproduces baseline PROF exactly.
- **Next (high value):** **port to the x86 engine** — `frontend/x86_64/translate.c` has the
  identical block-tail shape (`emit_chain_exit(taken/next)`); the recipe ports verbatim and
  would compound with items 1–3 on x86. *Not yet built.*

### 5. Persistent translated-code cache  ·  gate `DDJIT_PCACHE=1`
> Research: [`opt8-coldstart.md`](docs/optimization-research/opt8-coldstart.md), diff [`opt8-coldstart.diff`](docs/optimization-research/opt8-coldstart.diff)
- **Engine:** jit86. **Files:** new `frontend/x86_64/pcache.c` + `frontend/x86_64/{translate.c,
  emit.c,dispatch.c,elf.c,engine_glue.c}`, `targets/linux_x86_64.c`, 2-line `service.c`
  (+341/−10).
- **Impact:** **internal cold start ~1.1→0.68 ms (−40%)**; translation 0.43 ms → **0** on
  2nd+ run; wall −12% (bounded by the ~2 ms `posix_spawn`+dyld floor). First-run build
  overhead +0.23 ms one-time.
- **What it does:** persists the translated arena + block map (arena-relative offsets) +
  a relocation list for the only two baked host pointers (`block_return`, `&g_ibtc`).
  Guest image+interp loaded at fixed VAs for byte-identical bias. Keyed by engine version +
  cpu-struct size + bases + FNV1a(dev/ino/size/mtime) of binary & interp.
- **Correctness:** 8 applets cached==fresh; corrupt/truncated cache → graceful MISS + rebuild.
  One `/bin/busybox` cache serves all applets; args/env don't invalidate.
- **Next (the big one):** a **warm resident `ddjitd` fork-server** that pre-loads workers +
  this cache → pushes 2nd+ launch wall toward **sub-millisecond**. This is the decisive
  VM-domination metric for short-lived containers. *Design only.*

### 7. x86 inline `clock_gettime`/`gettimeofday` time fast path  ·  gate `JIT86_NOFASTSYS=1`
> Research: [`s1-vdso-fastpath.md`](docs/optimization-research/s1-vdso-fastpath.md), patch [`s1-vdso-fastpath.patch`](docs/optimization-research/s1-vdso-fastpath.patch)
- **Engine:** jit86 (x86). **Files:** `frontend/x86_64/{translate.c,emit.c}` (+ startup
  calibration in `targets/linux_x86_64.c`).
- **Impact:** **clock_gettime 28.69→3.79 ns/call (7.57×)**, REALTIME 5.43×, gettimeofday
  3.81×; **100% of guest time-syscalls eliminated** (2,000,000 → 0 reaching `service()`).
- **What it does (vDSO-equivalent, no kernel needed):** at the `0F 05` site, if `rax` is a
  handled time syscall, emit inline ARM64 that reads the EL0-readable **`CNTVCT_EL0`** virtual
  counter, converts to a Linux `timespec`/`timeval` via a one-time startup calibration
  (overflow-safe 128-bit Q30 multiply), writes the guest buffer, sets `rax=0`, and **falls
  through without ever exiting the block or entering `service()`**. Unhandled clockids fall
  back to the full path.
- **Correctness:** monotonic verified; REALTIME bracketed by host wall-clock (not stale);
  busybox `uname`/`echo`/`ls`/`sh`-loop + sqlite byte-identical FAST vs SLOW.
- **Caveat (honest):** a timestamped-logging workload is only **1.05×** — `snprintf` (~250 ns)
  dwarfs the ~25 ns saving. The win scales with time-syscall *density*: full 4–8× on
  timing/rate-limit/metrics/benchmark loops, marginal when formatting dominates.
- **Reusable:** `emit_fast_syscall` (save-flags → `rax` compare-ladder → {serve inline & continue
  | restore & full exit}) is the inline-dispatch skeleton S2's identity/path-cache hit-arms
  plug into.

### 8. Configurable `fsync` durability mode  ·  gate `S3DB_DURABILITY=none|fast|strict`
> Research: [`s3-db-killers.md`](docs/optimization-research/s3-db-killers.md)
- **Engine:** jit86 (shared `os/linux/service.c` + `thread.c`). **Files:** `os/linux/service.c`
  (`fsync`/`fdatasync`/`sync_file_range`/`msync` cases).
- **Impact (sqlite `synchronous=FULL` insert tps):** `fast` (default) 3576 → **`none` 10385
  (2.9×)**; plain fsync **19.0 µs → 725 ns (26× cheaper)** in `none`.
- **What it does:** `fast` (default, **unchanged** = plain `fsync()`, the macOS fast path);
  `none` = no-op barrier for ephemeral containers (page-cache-coherent, not host-crash-durable);
  `strict` = `fcntl(F_FULLFSYNC)` for real durability.
- **⚠ Critical finding — do NOT naively "fix" fsync:** mapping `fsync`→`F_FULLFSYNC` (the
  obvious "correct" macOS port) collapses sqlite to **103 tps (35–100× slower)**; a single
  `F_FULLFSYNC` is **3.05 ms (160× a plain fsync)**. The shipped default is already correct;
  the value here is the **opt-in `none` mode** for throwaway/CI containers and documenting the
  trap. `none` is an explicit, opt-in durability tradeoff — keep `fast` the default.
- **Correctness:** query results byte-identical across base/fast/none/strict; MAP_SHARED
  persistence PASS in all modes.
- **mmap:** already a real host mmap with correct MAP_SHARED write-back — **no change needed.**

### 9. VFS-jail path-resolution cache  ·  gate (kill-switch) · **profiler's #1 ROI**
> Research: [`s2-cache-dispatch.md`](docs/optimization-research/s2-cache-dispatch.md), [`s4-syscall-profile.md`](docs/optimization-research/s4-syscall-profile.md)
- **Engine:** shared `os/linux/`. **Files:** `os/linux/fscache.c` (cache + `atpath` hook),
  `os/linux/service.c` (epoch bump on mutating syscalls + fork reset), `os/linux/container/state.c`.
- **Impact:** **`stat` same path ×1M: 3450 → 85 ms (40.3×)**, path-cache hit-rate 100%
  (1,000,000 resolutions → 1; ~3M host `lstat` round-trips → ~0). S4 identified this as the
  highest-ROI lever of the whole sweep: jail path resolution is ~6.7 µs/call (~500× the
  dispatch floor) and open/stat/getdents are 70–95% of file-heavy workloads.
- **What it does:** memoizes only the guest-path→host-path *string resolution* (the real
  syscall always still runs); a global **epoch** bumped on every mutating syscall invalidates
  the cache; fork resets it.
- **Correctness:** 9/9 invalidation cases pass (create-after-negative, delete-after-positive,
  rename, mkdir/rmdir); byte-identical busybox output. No regression on unique-path walks
  (`find /` is noise — 0% hit by design).
- **⚠ Scope notes:** **`openat` is NOT cached** — it uses the TOCTOU-safe `resolve_at` path,
  not `atpath`; extending the cache there safely is future work (could pick up the
  open-heavy half). **Identity-syscall memoization was a dead end** — `getpid` memo is
  +3.7% (macOS `getpid` is already commpage-free); **drop it.**

### 10. x86 SSE2 `pmovmskb` → branch-free NEON (+ `pmuludq`)  ·  gate `NOSSEOPT=1`
> Research: [`w3b-sse-simd.md`](docs/optimization-research/w3b-sse-simd.md), diff [`w3b-sse.diff`](docs/optimization-research/w3b-sse.diff)
- **Engine:** jit86. **Files:** `frontend/x86_64/` (3 files, +47/−2).
- **Impact:** `pmovmskb` (`0F D7`) was the **single scalar fallback** in an otherwise 1:1-NEON
  SSE engine — a ~51-insn per-byte spill loop — and the hot-loop bottleneck of *every* glibc/musl
  **SSE2** `strlen`/`strcmp`/`memchr`/`memcmp`/`strchr` (CPUID advertises SSE2, so glibc IFUNC
  picks `*_sse2`; 971 sites in the test binaries). Replaced with a **7-insn branch-free NEON
  cascade** (proven sse2neon `_mm_movemask_epi8`).
  - **strlen 2.73×, strcmp 3.21×, memchr 1.97×, memcmp 1.78×**, text tokenizer 1.24×. vs native
    arm64 the gap collapses from 2.7–5.6× to 1.2–2.1× (not beat-native — native arm64 string
    funcs are bespoke NEON).
- **Bonus correctness fix:** implemented `pmuludq` (`0F F4`, 3 NEON insns) — was an **UNIMPL that
  aborted the guest** in glibc `strchr`/`strrchr` byte-broadcast.
- **Correctness:** bit-exact vs `NOSSEOPT`/baseline on an exhaustive alignment/length/edge matrix,
  AND **byte-identical to a `qemu-x86_64` oracle** on the same binary.
- **Next:** the remaining SSE4.2 string ops (`pcmpistri`) + `rep cmps/scas` idiom (from the
  original W3-B brief) were not reached — still open squeeze.

### 11. x86 trace/superblock formation (port of item 4)  ·  gate `NOSTITCH=1`
> Research: [`w3a-x86-trace.md`](docs/optimization-research/w3a-x86-trace.md), diff [`w3a-x86-trace.diff`](docs/optimization-research/w3a-x86-trace.diff)
- **Engine:** jit86. **Files:** `frontend/x86_64/translate.c` (+~70 lines).
- **Impact:** **translations −40…43%** every workload (awk 1868→1110, grep 1777→1010,
  wc 1116→646); dispatcher round-trips −40% awk / −41% sort. **Wall: wc −11.1%, grep −7.0%,
  tr −5.9%, awk −3.1%** — none regress.
- **Composes with Opt3 lazy flags:** `jmp` stitch is flag-clean (top-of-loop materializes
  before non-Jcc); `jcc` fall-through reuses the existing producer `e_nzcv_save` before laying
  the OOL taken exit + inline fall-through, so `cpu->nzcv` is correct at every stitched boundary.
  Added a `trap_head()` guard so musl's `cmp;jbe;hlt` alloca check isn't stitched into.
- **Correctness:** 17-workload battery byte-identical; `NOSTITCH=1` reproduces baseline PROF
  to the integer.

### 12. x86 `epoll` change-batching → kqueue  ·  gate `NOEPOLLOPT=1`
> Research: [`w3e-epoll.md`](docs/optimization-research/w3e-epoll.md), diff [`w3e-epoll.diff`](docs/optimization-research/w3e-epoll.diff)
- **Engine:** shared `os/linux/service.c` (187 lines, service.c only).
- **Impact:** baseline issues a real `kevent()` per `epoll_ctl`, so the scalable-server pattern
  (EPOLLONESHOT rearm / EPOLLOUT toggle) pays an extra kqueue syscall per request. Classic
  libevent-style batching folds buffered `epoll_ctl` changes into the next `epoll_wait`'s single
  `kevent()` (O(1) armed-filter map). **14–15× fewer kqueue syscalls** (~100k eliminated on a
  rearm-heavy run); **+3.6% rps** (throughput modest — loopback ping-pong is latency-bound and
  rearm is off the critical path; the robust win is the syscall/CPU/scalability reduction).
- **Bonus fix:** `EPOLL_CTL_MOD` was leaving a stale `EVFILT_WRITE` armed (real baseline bug).
- **Correctness:** byte-identical echo across LT/ET/ONESHOT × 400 fds + a 4 MB edge-triggered
  pipelined drain (`bad=0`).

### 13. Warm resident `ddjitd` fork-server (next step of item 5)  ·  opt-in server mode
> Research: [`w3d-forkserver.md`](docs/optimization-research/w3d-forkserver.md), diff [`w3d-forkserver.diff`](docs/optimization-research/w3d-forkserver.diff)
- **Engine:** jit86. **Files:** new `forkserver.c` (364 lines) + `fclient.c` (102 lines) +
  small engine edits (`g_noexit` prewarm unwinding, image `span` pristine-restore, `jit86_run`
  split into idempotent init/run). Standalone path unchanged.
- **Impact:** **warm launch 1.22 ms vs standalone 2.69 ms (2.2×, −55%).** A resident `ddjitd`
  pays dyld + engine init + pre-translation *once*, then `fork()`s a COW exec-less worker per
  launch over AF_UNIX (argv + stdio via `SCM_RIGHTS`, exit code returned). Cold (no prewarm)
  1.75× (removes the ~2 ms posix_spawn+dyld+codesign floor); warm adds 1.26× (removes translation
  — workers translate a measured **0 bytes**).
- **Correctness/isolation:** 10 applets byte-identical (stdout+stderr+exit codes); **MAP_JIT
  executes post-fork** (re-assert `pthread_jit_write_protect_np(1)`); COW isolation proven
  (heavy `awk` worker doesn't disturb siblings); soak 30/30.
- **Integration:** fork-server on by default; layer Opt8 `DDJIT_PCACHE` (item 5) to back the
  arena for server-restart durability + curate the warm set. Residual ~1.2 ms is `fork()` of a
  large-VM-reservation process + the guest's own ld.so — sub-ms needs attacking those next.
- **Huge pages: dropped for startup** — Apple Silicon is already 16 KB pages, startup working
  set <1 MB, and there's no usable arm64 macOS superpage path. (Only relevant to a future
  database working-set lever, not startup.)

### 14. Inline pure-userspace syscalls (`rt_sigprocmask`, `sched_yield`)  ·  gate `JIT86_NOSIGINLINE=1`
> Research: [`w4f-sysinline.md`](docs/optimization-research/w4f-sysinline.md), diff [`w4f-sysinline.diff`](docs/optimization-research/w4f-sysinline.diff)
- **Engine:** jit86 (builds on S1's `emit_fast_syscall`). **Files:** 4 (+103/−6).
- **Impact:** `rt_sigprocmask` serves from `c->sigmask` inline (read/update, write `*oldset`,
  SIG_BLOCK/UNBLOCK/SETMASK + NULL cases) and falls through without dispatching — **13.33→3.10
  ns/call (4.30×), 100% of round-trips eliminated**. `sched_yield` inlined too. Real busybox is
  noise (only ~6 sigprocmask/process) — win scales with signal-mask density.
- **Safety (the subtle part):** `c->sigmask` has no async reader — `host_sigh` only sets
  `g_pending`; the sole consumer is `maybe_deliver_signal` (synchronous, dispatcher loop top).
  The only observable difference is delivery *timing* when a pending signal is unblocked, so the
  inline path is **gated on `g_pending==0`** → provably bit-exact; pending ⟹ exact-timing slow
  exit. Validated: byte-identical signal test; counters prove unblock-while-pending takes the
  slow path.
- **Left on the slow path** (low value / staleness risk): `set_tid_address`, `rt_sigaction` query,
  `rt_sigpending`.

### 15. AF_INET/AF_INET6 sockaddr translation — real TCP servers work  ·  gate `NOSOCKADDR=1`
> Research: [`w4a-sockaddr.md`](docs/optimization-research/w4a-sockaddr.md), diff [`w4a-sockaddr.diff`](docs/optimization-research/w4a-sockaddr.diff) · resolves the 🚩 bug
- **Engine:** shared `os/linux/`. **Files:** `os/linux/container/netns.c` (~80-line
  `af_l2m`/`sa_l2m`/`sa_m2l` helpers) wired into **11 syscalls** in `service.c` (socket,
  socketpair, bind, connect, accept/accept4, getsockname, getpeername, sendto, recvfrom,
  sendmsg/recvmsg, sendmmsg/recvmmsg) + `JIT86_NONETNS=1` opt-out.
- **Impact:** **first time a real TCP server binds + serves under jit86.** Fixes the layout
  mismatch (Linux `{u16 sin_family}` vs macOS `{u8 sin_len; u8 sin_family}` → guest `AF_INET`
  was becoming `AF_UNSPEC`) and the AF_INET6 family-value difference (10 vs 30 → was
  `EAFNOSUPPORT`).
- **Proven end-to-end:** AF_INET + AF_INET6 echo byte-exact; W3-E epoll echo server 4000
  concurrent round-trips 0 mismatches; a real HTTP/1.0 server served macOS `curl`. AF_UNIX/netns
  isolation paths untouched (run first), regression-clean.
- **Unblocks the entire network-server workload class** — running real redis/postgres/nats now
  needs only an x86_64 binary (only arm64 images are local). With item 12 (epoll) + Tier-2 item 7
  (futex), the server DB story is now measurable.

### 16. `openat` resolution cache (extends item 9 to the open-heavy half)  ·  gate `W4_NOOPENCACHE=1`
> Research: [`w4d-openat.md`](docs/optimization-research/w4d-openat.md)
- **Engine:** shared `os/linux/fscache.c` (builds on item 9). **Impact:** repeated-file
  `open+fstat+close` ×1M **19158 → 4500 ms (4.26×)**, 100% hit. `openat` uses the TOCTOU-safe
  `resolve_at`/`jail_at` per-component walk (~6 host syscalls) that item 9 left uncached; the new
  `oc_*` cache memoizes guest-path → canonical symlink-free host path (`F_GETPATH`), so a hit
  collapses the walk to **one `open(host, O_NOFOLLOW)`** — and the real `open()` always runs.
- **TOCTOU safety:** reuses item 9's epoch (any mutating syscall bumps it → whole cache misses);
  same threat model. **Excludes `O_CREAT/O_EXCL/O_TRUNC` and `O_DIRECTORY`** (directory-open
  caching regressed −21% — a deep host path is *more* macOS path-walk than the pinned-root
  `openat`, and excluded).
- **`getdents`/readdir: neutral** (already caches the `DIR*` per fd — no per-call path resolution
  to remove; honest null).
- **Correctness:** 8/8 open-invalidation (delete/recreate/rename verified by reading *contents*
  through the cached path) + item 9's 9/9 still pass; byte-identical on find/ls/grep/cat/md5sum.
  Real-world: md5sum-repeats 60% hit; find/grep 0% but no regression.

### 17. x86 `rep cmps`/`rep scas` → host `memcmp`/`memchr`  ·  gate `NOREPCMP=1`
> Research: [`w4c-sse42.md`](docs/optimization-research/w4c-sse42.md), diff [`w4c-sse42.diff`](docs/optimization-research/w4c-sse42.diff)
- **Engine:** jit86. **Files:** 7 (+284/−100). Was **UNIMPL → guest abort**, so also a correctness fix.
- **Impact (1 MiB×2000, median-of-5):** memcmp (`rep cmpsb`) **36.5×**, strlen (`repne scasb`) **41.4×**,
  memchr **42.2×**, dword `rep cmpsd` 8.9× over the byte-loop — **runs at native-arm speed, beats the
  qemu VM 22–89×.** Exact x86 RCX/RSI/RDI + ZF/SF/CF/OF end-state (RCX=0 leaves flags untouched).
- **Bonus:** re-derived the `pmuludq` fix (removes a strchr/memchr abort); **found+fixed a 64-bit
  `cmpsq` overflow-flag UB** via the qemu oracle (self-consistent but diverged from real x86 on qword OF).
- **SSE4.2 `pcmp{i,e}str{i,m}`:** full bit-exact evaluator implemented (3-byte `0F 38`/`0F 3A` decode,
  all aggregations/polarities/widths) **but kept dormant** — the CPUID A/B says **do NOT advertise
  SSE4.2**: 0% win on strlen/memchr/memcmp and a **14.8× strcmp regression** (glibc switches to
  `pcmpistri`, which even optimally inlined can't beat the SSE2 `pmovmskb` path from item 10).
- **Correctness:** three-way verified `qemu-x86_64` == JIT-default == JIT-gated.

### 18. Adaptive tier-up (hot-loop recompile + live swap-in)  ·  gate `NOTIER2=1`
> Research: [`w4e-tier2.md`](docs/optimization-research/w4e-tier2.md), diff [`w4e-tier2.diff`](docs/optimization-research/w4e-tier2.diff)
- **Engine:** shared `jit/` (aarch64). **Files:** 6 (+216). The first adaptive/tiered codegen in the engine.
- **Chosen transform:** on the same-ISA aarch64 engine, guest regs are already in host regs and NZCV is
  native, so flag-breaking/regalloc/scheduling are inapplicable; the one per-iteration redundancy is the
  conditional back-edge trampoline (`b.cond Ltaken; b body` = 2 taken branches/iter) → tier-2 folds it to
  a single `b.cond body`. **Hot branch-bound loops 1.55×**; dependency-bound loops neutral (honest).
- **Adaptive mechanism (the reusable substrate):** hot code never returns to the dispatcher, so hotness
  is measured **in-cache** via a flag-free decrementing back-edge counter (`sub`-imm + `cbnz`, preserves
  guest NZCV); at threshold it exits `R_TIER2`, the dispatcher recompiles and **swaps in under live
  execution** (icache → overwrite the old body's first insn with `b new_body` → repoint map/chains/IBTC);
  the recompile drops the counter → **zero steady-state cost**.
- **Cold/short-lived: zero tier-ups, no regression** (the cost is fully amortized).
- **Hazard found + guarded:** folding can tighten a loop into an Apple-Silicon store-forwarding replay on
  volatile same-slot RMW (−73% unguarded); `loop_has_rmw_hazard()` keeps only those on tier-1.
- **Correctness:** byte-identical vs `NOTIER2` on 9 kernels + busybox + 4 awk scripts; 60-region soak 12/12.
- **Biggest value is the substrate, not this transform:** tier-1 on the same-ISA engine is already
  near-native (small ceiling). The hotness+live-swap-in machinery **ports directly to the x86 engine**,
  which is the real high-ceiling tier-2 target (combine with multi-block superblocks + `adrp`/literal
  hoisting + the cross-ISA regalloc/flag levers that don't apply to aarch64). ← top future lever.

---

## Tier 2 — keep / situational

### 6. x86 IBTC 2-way set-associative  ·  separate `g_xibtc` table
> Research: [`opt2-indirect.md`](docs/optimization-research/opt2-indirect.md)
- **Engine:** jit86. **Impact:** IBTC misses **4,000,403 → 403 (≈9,925×)**, dispatcher
  round-trips ≈2,050× fewer, hot path 10→8 insns — **but wall is flat** (dispatch is ~1% of
  awk time). **Keep it:** strictly-better hot path + defends against pathological
  conflict-miss blowup at ~zero cost. Don't expect a wall-time headline from it.

### 8. Dual-mapped RX/RW code cache (threading enabler, not a perf win)  ·  gate `NODUALMAP=1`
> Research: [`w3c-dualmap.md`](docs/optimization-research/w3c-dualmap.md), diff [`w3c-dualmap.diff`](docs/optimization-research/w3c-dualmap.diff)
- **Engine:** shared `jit/` (aarch64). **Eliminates 100% of `pthread_jit_write_protect_np`
  toggles** (73,812 → 0 on big2) — **but wall-neutral (±1%)**: the M4 APRR toggle is only
  ~10.9 ns, so removing tens of thousands saves ~0.8 ms of a ~20 ms run, offset by ~1,846 extra
  minor page-faults from the double-aliased VA.
- **Why keep it anyway (strategic):** it's the **prerequisite to race-free threaded IBTC fill**
  (removes the process-wide permission flip; the current threaded bottleneck is 4,000,291
  dispatcher round-trips because threaded IBTC fill is disabled). Necessary but **not
  sufficient** — the inline IBTC reader still needs load-load ordering hardening (scoped
  follow-up). Also a cleaner design than the RX-canonical variant currently in the tree.
- **Feasibility:** `MAP_JIT` **cannot** be re-aliased RX; the working route is plain anon-RW +
  `mach_vm_remap` to a 2nd VA + `vm_protect` RX under the existing `allow-jit` entitlement.
  RW-canonical design keeps the diff tiny (+146/−32, 6 files) since intra-cache PC-relative
  patches cancel the alias delta.
- **Bugs fixed en route:** an `adrp` that must reference the RX-alias PC (silent 100% IBTC miss),
  and a fork COW-divergence (`jit_after_fork()` rebuild — without it dual-map breaks fork+exec).
- **⚠ Conflict:** a different **RX-canonical "PR-A" dual-map is already in the live tree** (larger,
  still toggles). This (toggle-free, RW-canonical) is the cleaner variant — **replace, don't
  stack.**

### 7. Uncontended-futex fast path  ·  gate `S3DB_FUTEX_FAST`
> Research: [`s3-db-killers.md`](docs/optimization-research/s3-db-killers.md)
- **Engine:** shared `os/linux/thread.c`. Waiter counter so `FUTEX_WAKE` with no sleeper
  skips the global mutex + broadcast; lock-free `FUTEX_WAIT` EAGAIN pre-check. Lost-wakeup
  race kept closed. **Safe, neutral-to-positive** (ping-pong ns/handoff ~3%, within noise) —
  keep it, but the real multi-thread win needs **per-address wait queues** (out of scope here;
  the current global-mutex futex is the actual threaded-DB bottleneck). Worth a follow-up.

---

## Not worth pursuing

- **Engine-C build flags / PGO (Opt1):** within noise. The flag/hint changes are harmless
  hygiene only — adopt with **no perf claim**, or skip.
  Research: [`opt1-buildflags.md`](docs/optimization-research/opt1-buildflags.md).
- **Opt6 peephole as a separate change:** subsumed by Opt7 (item 3).
  Research: [`opt6-peephole.md`](docs/optimization-research/opt6-peephole.md).

---

## Landing order & conflict map

x86 `translate.c` is touched by items 1, 2, 3, 5; `emit.c` by 3 and 5. Land sequentially,
rebuild + re-run the bit-exact battery after each:

1. **Opt7 addressing** (item 3) — base emitter/EA change, biggest x86 footprint.
2. **Opt3 lazy flags** (item 2) — independent flag logic in translate.c.
3. **Opt5 rep-string** (item 1) — independent decode path.
4. **Opt2 2-way IBTC** (item 6) — separate table, low conflict.
5. **Opt8 pcache** (item 5) — touches the most files; land last so its hashing/relocation
   sees the final emitted-code shape.
6. **Opt4 trace** (item 4) — aarch64 engine, **no x86 conflict**; land anytime. Then do the
   x86 port.

Re-apply each report's `service.c` PROF counter lines after the upstream networking edits.

---

## Bugs (open)

- **x86 `-static` (non-PIE `ET_EXEC`) guests crash the JIT** (S4) — `-static-pie` works. Known non-PIE
  absolute-jump issue (the dispatcher has a `g_nonpie_*` bias redirect; this case still faults). Platform
  limitation: shrinking `__PAGEZERO` to pin the non-PIE low crashes the JIT host binary (see `docs/PLAN.md`);
  needs a host-layout-safe approach.
- **Pre-existing `strrchr_sse2` tail bug** (W3-B) — misses the last match in the final ~50 bytes (~7 of
  thousands of cases); reproduces on stock baseline + `NOSSEOPT=1` alike (so it's not the SSE opt), a
  flag/mask edge in the `pmovmskb`-consuming tail. Forward `strchr` is correct.
- **Minor:** 64 KB guard-tail leak per `munmap` (syscall 215); the 1024-entry `gmap` cap; ignored
  `MREMAP_*` flags (correct for musl in practice).

---

## Open follow-ups / remaining squeeze (not yet built)

- **Port trace/superblock x86 lazy-NZCV interplay review** — done (item 11); no action.
- **SSE4.2 `pcmpistri`/`pcmpestri` + `rep cmps`/`rep scas` idioms** — W3-B reached `pmovmskb`+
  `pmuludq` only; the string-scan idioms are still open (would compound item 10).
- **`openat`/`resolve_at` path-cache extension** — item 9 caches `atpath` only; the open-heavy
  half is uncached (needs TOCTOU-safe caching).
- **Threaded IBTC fill** — unblocked by dual-map (Tier-2 item 8) but needs inline-reader
  load-load ordering; current threaded bottleneck is 4M dispatcher round-trips.
- **Sub-millisecond startup** — fork-server (item 13) is at 1.22 ms; residual is `fork()` of a
  large-VM process + guest ld.so. Attack VM-reservation size + ld.so translation next.
- **Run real redis/postgres/nats** — sockets (AF_INET/INET6) + epoll + futex + the mremap/xchg crash fixes
  now make the server-DB story runnable; needs x86_64 images (only arm64 are local). Remaining engine
  blocker is just non-PIE `-static` (see Bugs).

## Wave 2 + 3 — completed (syscall / database / server / startup)

Rationale: **there is no Linux kernel** — every guest syscall is a userspace `service()`
call. Confirmed gaps: **no vDSO**; `getuid` falls through to a real host call;
`fsync` durability policy unreviewed.

### Profiler results (S4 — done) · Research: [`s4-syscall-profile.md`](docs/optimization-research/s4-syscall-profile.md)

**The dispatch round-trip is NOT the bottleneck — host work + the rootfs-jail path rewrite are.**
- Fixed JIT syscall overhead floor = **12.7 ns** (getpid). gettimeofday 16.6 ns,
  clock_gettime 25.2 ns, **getuid 115 ns** (hits real host getuid), read(64B) 268 ns.
- **`open+fstat+close` = 20.2 µs/iter, ≈6.7 µs of it VFS-jail path resolution — ~500× the
  dispatch floor.** This is the dominant cost of the broadest syscall-intensive class.
- Hottest syscalls by workload: **find** newfstatat 46% / getdents64 18% / openat 9%;
  **grep -r** read 59%; **sha256sum** read 59% / openat+close 34%; **cat** sendfile 47%;
  **sqlite (600k ins+idx+agg)** pread64 49% + pwrite64 49%.
- futex did not surface (workloads single-threaded) — still a gap for threaded DBs.
  redis SKIPPED (only an **arm64** redis image is present — incompatible with the x86 engine);
  postgres/pgbench profiled from knowledge (pread/pwrite + fsync/fdatasync + futex).

**Re-prioritized wave-2 order (per S4):**
- **S2 (DONE → Tier 1 item 9):** path-resolution cache — `stat` ×1M **40.3×**, 100% hit,
  ship default-on. Identity memo dropped (no win on macOS); `openat`/`resolve_at` path still
  uncached (future work, the open-heavy half).
- **S1 (DONE → Tier 1 item 7):** inline `clock_gettime`/`gettimeofday` via `CNTVCT_EL0`,
  7.57×/3.81×, time-syscalls 100% eliminated. Identity-family memoization (getpid/getuid,
  115 ns to host) handed to S2 via the `emit_fast_syscall` mechanism.
- **S3 (DONE → Tier 1 item 8 + Tier 2 item 7):** `fsync` durability knob (`none` = 2.9×
  sqlite; never map fsync→F_FULLFSYNC — 35–100× slower); uncontended-futex fast path (safe,
  neutral; real win needs per-address queues); mmap already correct. **Remaining sqlite lever
  is the pread/pwrite path itself** (98% of its syscalls) — left for a future pass.

S1/S2/S3 reports will be added to `docs/optimization-research/` and folded into Tier 1/2
as they land.
