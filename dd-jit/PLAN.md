# dd-jit optimization plan — remaining (unimplemented) work

The wave-1–6 optimization sweep is **landed** (20 opts + the wave-5 server bugs + the W6A deep-bug cluster,
all matrix-240-green, each behind an env kill-switch). Per-lever writeups (mechanism, PoC diff, measured A/B,
correctness evidence) live in [`docs/optimization-research/`](docs/optimization-research/). **This file lists
ONLY what is not yet built.**

---

## Correctness gaps (guest aborts / wrong output)

- **x86 `rep cmps`/`rep scas` → host `memcmp`/`memchr`** · gate `NOREPCMP=1` ·
  [`w4c-sse42.md`](docs/optimization-research/w4c-sse42.md). Currently **UNIMPL → guest abort** (a correctness
  fix, not just perf): `rep cmpsb`→memcmp 36.5×, `repne scasb`→strlen 41.4×, memchr 42.2×; exact RCX/RSI/RDI +
  ZF/SF/CF/OF end-state, qword `cmpsq` OF fixed via the qemu oracle. *(The SSE4.2 `pcmp{i,e}str{i,m}` evaluator
  is fully built but kept **DORMANT** — the CPUID A/B says do NOT advertise SSE4.2: 0% win + a 14.8× strcmp
  regression vs the SSE2 `pmovmskb` path already landed. Leave dormant.)*
- **`strrchr_sse2` tail bug** (W3-B) — misses the last match in the final ~50 bytes (~7 of thousands of cases);
  reproduces on stock baseline + `NOSSEOPT=1` (so it's not the SSE opt) — a flag/mask edge in the
  `pmovmskb`-consuming tail. Forward `strchr` is correct.
- **non-PIE `ET_EXEC` fork+execve — PARTIAL** · gate `NONPIE_NOFIXUP=1` ·
  [`w6a-deepbugs.md`](docs/optimization-research/w6a-deepbugs.md). Landed: a no-relink SIGSEGV fault-fixup
  (redirect absolute code jumps + emulate faulting integer ld/st at `+bias`; non-PIE fork+execve(self) → exit 77,
  0 PIE regression). **Remaining:** SIMD-Q/`ldr q` + LSE-atomic-RMW absolute forms, and syscall pointer-args into
  low `.rodata`/`.data`, aren't redirected (they take a clean abort, not silent corruption). The
  host-`__PAGEZERO`-shrink path is a confirmed dead end. Victims: gcc toolchain, postgres `gosu`.
- **Minor:** 64 KB guard-tail leak per `munmap` (215); ignored `MREMAP_*` flags (correct for musl in practice).

## Security capability (not perf)

- **Untrusted-guest sentry process-split** · gate `g_untrusted` / `DDJIT_SANDBOX` (OFF default) ·
  [`w6b-sentry.md`](docs/optimization-research/w6b-sentry.md), [`w6b-sentry.diff`](docs/optimization-research/w6b-sentry.diff).
  New `sentry.c`: SPSC shared-mem mailbox + a forked **sentry** process holding all fs/net/proc authority + real
  fds; `syscall_route()` replaces the single `service(c)` call. Worker keeps compute/mem; guest memory is never
  shared (only marshaled bytes cross), deny-default Seatbelt on the worker. **First PR done** (in the research
  diff, not yet integrated): read/write/open(at)/close/lseek forwarded byte-identically, ~375 ns/syscall, trusted
  path (gate OFF) a one-branch passthrough. **Remaining for full isolation:** per-context rings (a forking
  `busybox sh` stalls at the first `clone`), the complete fs/net/proc set (stat/iovec/sockets/execve/fork),
  SCM_RIGHTS for file-backed mmap, futex wakeup vs spin, a real allow/deny policy layer.

## Perf — remaining squeeze (not yet built)

- **redis throughput** — the jitted server event loop is 98.5% CPU-bound, ~18× off native (transport is fine —
  native-mac redis 1.6M rps, so the gap is the translated server). x86 trace/superblock + x86 tier-2 + 2-way IBTC
  are landed but need **exercising and tuning on the redis command-dispatch/dict hot path** specifically.
  ← highest-ceiling lever.
- **Tier-2 multi-block traces spanning call-free spans** — real busybox forms ~0 stolen-register traces because
  the x30 mangle lives in call prologues/epilogues (excluded by W6-C's call-free constraint, not in tight
  self-loops). Multi-block traces over the call-free spans *between* calls would capture the x30 prologue mangle.
  High-value follow-up to W6-C.
- **x86 lazy flags — carry-value consumers** (`adc`/`sbb` deferral; Opt3 left them correct-but-inline) + **x87
  `fptop` tracking**.
- **sqlite temp-file spill = 97% `pread64`/`pwrite64`** to a host temp file → back the sorter/index temp with an
  in-memory/tmpfs fd → turn ~2,834 host I/O syscalls into memcpy. (The remaining sqlite lever after S1/S2/S3.)
- **Sub-millisecond startup** — the fork-server is at 1.22 ms; the residual is `fork()` of a large-VM-reservation
  process + the guest's own ld.so. Attack the VM-reservation size + ld.so translation next.

## Validation blocked on inputs

- **Run real redis / postgres / nats end-to-end** — sockets (AF_INET/INET6) + epoll + futex + the wave-5
  server-bug fixes make the server-DB class runnable; needs **x86_64 images** (only arm64 are local) and the
  non-PIE `-static` residual above resolved.
