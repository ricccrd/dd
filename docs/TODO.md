# dd — todo

Shipped **v0.9.0 → v0.9.5**. **v0.9.6 batch = 32 commits (~25 fixes) staged since v0.9.5, NOT yet
tagged** (holding for a clean full-matrix gate at low load once the fix-agent pipeline drains).

## Proven working this session (both arches, real software, correct output — not just "no crash")
postgres · mariadb · nginx (1000/1000 req) · redis (500k ops) · sqlite (WAL, 100k rows) · ruby · perl ·
python + multiprocessing · pip install (six, cryptography — AES vectors correct) · make -j4 · cmake ·
npm · node (incl. heavy heaps) · php (json_encode 50k) · gcc/cc1 (C compile→run, arm) · R (Rscript,
OpenBLAS) · numpy/scipy/pandas (arm) · tmux · git · openssl · tar · sed/awk/grep · coreutils · full
basics matrix (fs, net, mmap, fork/exec, signals, eventfd/epoll) 0-failed both arches.

## Landed this session (highlights)
- **execve-churn (#204)** — exec was scanning the whole fd table (~184K fcntl/exec = 22ms); now O(open
  fds) via proc_pidinfo → ~70× faster exec. Cleared the npm/go/pip/make apparent-hangs.
- **tier-2 code-cache overrun (fd412af)** — tier2_promote emitted past the 64MB arena into GUEST memory
  → wild guest store. Root of a cluster of intermittent arm crashes (php-heavy, clang-O2, flaky class).
- **mremap flags-contract (#211)** — no-move mremap must not relocate (was a use-after-free; php crash).
- **engine-fd protection (#209+#216)** — guest dup2/dup3/close/close_range could clobber the engine's
  pinned low fds (g_root_fd) → EBADF everywhere (erlang, and general).
- **musl ioctl (#219)** — read-direction ioctls sign-extended under musl → ENOTTY (tmux/openpty/pty).
- **NUMA no-ops (#217)** — mbind/set_mempolicy unhandled → R/OpenBLAS/numpy hung. Whole sci-stack.
- **guest-pointer hardening (#194/#203/#214)** — bad syscall pointer returns EFAULT, never crashes engine.
- Plus: pip .pyc coherence (#200), deep-find loop (#199), POSIX sem over fork (#192), single-file bind
  mount (#196), IPv6 + nc-u UDP loopback (#159/#206), munmap gmap-split (#212), the x86 fork+exec
  IBTC/#176 class, and the v0.9.5-tail perf/opcode work.

## GA BLOCKERS (the real, re-verified open bugs)
- [ ] **#201 — x86 indirect/virtual-dispatch codegen miscompile** *(agent active; THE #1 GA blocker)*.
  A GC'd managed runtime's heap-address→metadata machinery miscompiles on x86: a base pointer loads as
  ~0/truncated, then `base+offset` faults (caddy 0x1d8, dotnet 0x2f8). CRASHES **go build, .NET, clang
  -O2, CPython-x86** (and almost certainly **rustc #210**); plain C++ vtables PASS. Smallest repro: a Go
  `map[string]int` hot loop (`target/tsordr201/mapt`). Nondeterminism = macOS guest-heap ASLR. Plan:
  deterministic-heap mode (mem.c) → instruction-trace mapt → fix in translate/x86_64. One fix cascades
  to 5+ runtimes.
- [ ] **#158 — memcached-arm crash on client connect** *(parked, real, deterministic)*. Not a generic
  arm-threading race (generic threads + valkey io-threads are solid); memcached/libevent-connection-
  specific; likely a deterministic aarch64 codegen miscompile (bad/truncated pointer). translate/aarch64.
- [ ] **#222 — ruby fork crash** *(agent active)*. Child dies in fork CFUNC; engine `[ASYNC BUG] close
  event_fd EBADF` → SIGSEGV. Fork/child-fd + eventfd-across-fork family.
- [ ] **#188 — CPUID extended brand-string empty** (maxext=0x80000001 too low) → **java-x86** only
  ("SSE2 not supported" is a fallback, SSE2 bit IS set). numpy/feature-bit libs unaffected. Fix: raise
  maxext ≥0x80000004 + brand string (+ synth /proc/cpuinfo flags). Behind #201 (translate/x86_64).
- [ ] **#224 — python-x86 startup** — (a) `_Py_HashRandomization_Init` getrandom fails, (b) segfault
  (likely #201). Blocks x86 python/numpy.
- [ ] **#215 — erlang full boot** — multithreaded-fork (beam.smp) JIT corruption (fd EBADF now fixed).

## Tail (lower priority)
#221 CRASHDBG false-crashes non-PIE binaries *(agent active — diagnostic-integrity)* · #223 pty-master
poll/EOF (script hangs) · #135 PyPy-x86 · #119 mongosh SEA · #104 V8 large-array · #190 MOVNTPS (dnf/rpm)
· #183 x86 0x8c · #161 pg fast-shutdown SIGSYS · #167 amd jobctl · #218 vDSO ptr-check · #208 syscall-65573
spam · #193 procfs follow-ups · #170 mkdir-EPERM (re-confirm) · #171 docker.sh gate · #178 PCACHE execve ·
#220 xfail-sync (c-primes/cpp-stl now XPASS) · #93 encoder de-dup · #78 fixture.

## Process (what's working)
dev-day = permanent discovery agent (real software, **normal-run crash detection** — CRASHDBG false-
crashes non-PIE binaries; verify **correct output**, not just no-crash; pin engine md5, repeat runs, note
load+free-mem, cross-check a 2nd workload before naming a trigger). Manager delegates each real bug to a
disjoint isolated-build-dir agent, **merges by extracting only the agent's hunks** (worktree bases often
predate HEAD — blind-copy reverts recent fixes), verifies fresh-binary md5, then commits. Gate + tag when
builders idle at low load. No push without explicit OK.
