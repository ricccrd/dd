# Memory-leak audit — dd JIT engine core (READ-ONLY scan)

Scope audited: `dd-jit/src/runtime/jit/`, `dd-jit/src/runtime/frontend/{x86_64,aarch64}/`, `dd-jit/src/runtime/include/`.
Method: enumerated every `malloc/calloc/realloc/strdup/mmap/aligned_alloc` and every table-append (`[n++]`, `g_n…++`) site, then traced each one's free/bound lifecycle including error/early-return paths. Quotes are `file:line`.

Engine layout note (matters for every finding below):
- `jit/cache.c` + `jit/dispatch.c` = the **aarch64→aarch64** same-ISA transliterator engine.
- `frontend/x86_64/{cache.c,dispatch.c,…}` = the **x86-64→aarch64** engine; it supplies its OWN code cache + dispatcher and does NOT use the `jit/` stop-the-world machinery.
The two engines are built into separate binaries; a given guest process runs exactly one.

---

## TL;DR ranking (likelihood × impact)

| # | Site | Verdict | Visibility | Conf |
|---|------|---------|-----------|------|
| L1 | `jit/cache.c:556` `jit_flush_to_fresh` retains every old 64MB cache (×2 dual-map = 128MB) forever | **Unbounded host RSS growth** under a multithreaded, code-churning aarch64 guest | ENGINE-INTERNAL | **high** |
| L2 | `frontend/x86_64/elf.c:586` lazy grow-page `mmap` (budget `g_growmaps < 256<<10`) | Bounded (~1GB) but monotonic & never reset; guest-owned pages | GUEST-VISIBLE | med |
| L3 | `frontend/x86_64/dispatch.c:90` threaded cache-full → `_exit(70)` | Not a leak — a hard abort; the x86 twin of L1's condition | n/a | high |
| L4 | `frontend/x86_64/emit.c:42` `g_reloc` not reset on in-place flush (DDJIT_PCACHE=1 only) | Bounded table; **correctness** risk for pcache, not RSS | ENGINE-INTERNAL | low |

Everything else enumerated is a fixed-size static table or a one-time-init / guest-lifetime / process-exit allocation — listed under "Confirmed bounded" so no test effort is wasted on them.

---

## L1 — Retained-forever old code caches on stop-the-world flush  ★ top candidate

- **Where:** `jit/cache.c:556` `jit_flush_to_fresh()`, called from `jit/cache.c:583` `stw_flush()`, called from `jit/dispatch.c:113`.
- **What's allocated:** a fresh 64MB cache. Under the dual-map JIT scheme that is **two** 64MB VM mappings (RW alias + `mach_vm_remap` RX alias) per flush — `dualmap_alloc()` at `jit/cache.c:36/40`. So **~128MB per flush**.
- **Hot path that triggers it:** dispatcher miss → cache near-full (`g_cp + 64KB > g_cache + CACHE_SZ`, `jit/dispatch.c:108`) → **and ≥1 peer guest thread is live** (`stw_peers_live()`, `jit/dispatch.c:109`) → `stw_flush()` → `jit_flush_to_fresh()` swaps `g_cache`/`g_cp`/`g_rw2rx` to the new mapping.
- **Why unbounded:** the comment at `jit/cache.c:551-555` is explicit — "The OLD cache is left mapped and UNMODIFIED … retained for the process lifetime." There is **no `munmap` of the old cache** on this path (grep confirms `munmap` in `jit/cache.c` occurs only at `:44` the alloc-error path and `:648-649` `jit_after_fork`). There is **no cap on the number of retained old caches** and no reference-count that ever lets one be reclaimed. The "bounded by the few times a fully-threaded 64MB cache fills" claim is an assumption about the workload, not an invariant enforced in code. A guest that keeps generating >64MB of distinct translated blocks while ≥2 threads stay live leaks 128MB on each fill, without limit.
- **What re-fills the cache:** any of (a) a large distinct-block working set (>~186K blocks), (b) **self-modifying / RWX-mmap guest code** that forces re-translation of the same PCs (`g_rwx_guest` path), (c) a guest JIT (JVM/V8/LuaJIT) emitting fresh native code continuously.
- **Visibility:** **ENGINE-INTERNAL.** These mappings are the engine's translated host code; they never appear in the guest's own `getrusage`/`/proc/self/statm`. Only the host process RSS grows. The old pages were executed → resident → stay resident after abandonment.
- **Concrete workload to expose (writable as a test):** run an **aarch64** guest that (1) spawns ≥2 long-lived threads (so `stw_peers_live()>0` at flush time) and (2) drives the code cache to repeated exhaustion. Simplest reproducer: a guest that takes an RWX `mmap` and repeatedly writes+executes fresh code in a loop (SMC re-translation churn) with a second thread spinning, OR a guest that walks a >128MB distinct-code working set in a loop. Sample host RSS every N seconds; L1 shows a staircase of ~+128MB per `g_stw_flushes` increment (PROF counter `g_stw_flushes`, `jit/cache.c:488`, is a ready-made instrument). Control: same workload single-threaded takes the in-place flush (`jit/dispatch.c:116-124`) and RSS stays flat — that delta is the proof.
- **Confidence:** **high** (mechanism and the missing `munmap` both confirmed by source).

---

## L2 — Lazy grow-page mapper: monotonic, never-reset, large budget

- **Where:** `frontend/x86_64/elf.c:586` `mmap(...MAP_FIXED...)` (and the `mprotect` fast-path at `:581`) inside the SIGSEGV/SIGBUS lazy guard.
- **What's allocated:** one 4KB guest page per faulting address, on demand.
- **Hot path:** any guest page fault classified "adjacent to a live mapping" (stack grow-down or SSE over-read) → `g_growmaps++` against budget `g_growmaps < (256<<10)` ≈ **1GB** (`elf.c:575,582,588`). Isolated/wild faults use the small `lazy_budget()` (4096, `elf.c:385`).
- **Why noteworthy:** the counters `g_growmaps`/`g_lazymaps` are **monotonic and never reset** even if the guest later `munmap`s the region. So the budget tracks *cumulative distinct fault pages over the whole process lifetime*, not currently-live pages. This is not a host-side leak (the pages are real guest memory the OS frees at process exit, and they ARE the guest's RSS), but: (a) it caps a legitimately long-running large-working-set guest at ~1GB of lazily-grown pages, after which the next legitimate fault is re-raised fatal (`elf.c:607-608`); (b) it is a per-fault `mmap` on a fault hot path.
- **Visibility:** **GUEST-VISIBLE** — these become the guest's own committed pages.
- **Workload to expose:** an x86 guest with a sustained grow/free cycle on a large region (e.g., repeatedly `mmap` a big buffer, touch it via vectorized libc, `munmap`, repeat across >256K distinct pages). It will eventually exhaust `g_growmaps` and crash (exit 139) rather than leak host memory unboundedly — so the test asserts a *fatal abort after ~1GB of cumulative grow-faults*, not RSS runaway.
- **Confidence:** med (bounded by design; flagged because the bound is cumulative, not live-set).

---

## L3 — x86 engine: threaded cache-full is a hard abort (not a leak, but the L1 sibling)

- **Where:** `frontend/x86_64/dispatch.c:88-92` — when the x86 code cache fills and `g_threaded`, the engine prints `code cache full with threads (unsupported)` and `_exit(70)`.
- **Why listed:** it is the same triggering condition as L1 but the x86 engine has no stop-the-world fresh-cache path, so instead of leaking it kills the guest. For a long-lived daemon hosting many execs this is a *liveness* bug (a multithreaded, code-churning x86 guest dies) rather than a memory bug. Single-threaded x86 takes the safe in-place flush (`dispatch.c:93-100`) — no leak, no abort.
- **Visibility:** n/a. **Confidence:** high (it is an explicit `_exit`).

---

## L4 — `g_reloc` table not reset on in-place flush (DDJIT_PCACHE only)

- **Where:** append at `frontend/x86_64/emit.c:42-45` (`g_reloc[g_nreloc++]`, fixed `g_reloc[1<<16]` at `engine_glue.c:78`); recording is **gated on `g_pcache`** (default OFF, `emit.c:38`).
- **Why noteworthy:** the x86 in-place cache flush (`dispatch.c:93-99`) resets `g_map`/`g_npend`/`g_ibtc`/`g_xibtc` but **not `g_nreloc`**. With `DDJIT_PCACHE=1`, after a flush the stale offsets accumulate and `g_nreloc` climbs toward 65536, at which point recording silently stops (`emit.c:42` guard prevents overflow). This is **not a host-RSS leak** (`g_reloc` is a fixed-size static array), but it would corrupt a subsequently-saved pcache (offsets point into overwritten arena bytes) and silently stop relocating new baked pointers. Default config (pcache off) is unaffected.
- **Visibility:** ENGINE-INTERNAL. **Confidence:** low (correctness, not memory; only with a non-default env flag).

---

## The 13-vs-9 frontend alloc/free imbalance — classified (all benign)

Every unmatched frontend allocation is one-time-init, guest-lifetime (OS reclaims at exit), or a standalone tool — none is a per-operation host leak:

| Alloc site | Class | Disposition |
|---|---|---|
| `pcache.c:125` `me`, `:126` `pe`, `:137` `abuf` | one-time (pcache load) | freed on **every** path: `:152`, `:156-157`, `:168-169` (incl. error early-returns) — balanced |
| `pcache.c:215` `buf` | one-time (pcache save, at guest exit) | freed `:239` — balanced |
| `forkserver.c:224` `g_wsnap_main`, `:227` `g_wsnap_interp` | **one-time** prewarm snapshot | intentionally retained for process life (worker COW restore source); allocated once in `--prewarm` init, never on a hot path |
| `elf.c:32`/`:175` ELF header `mmap` | one-time per exec | `munmap` `:49`/`:246` — balanced |
| `elf.c:199`/`:203` guest image base, `:257` guest stack | guest-lifetime | the guest's own address space; OS frees at process exit (correct) |
| `elf.c:586` lazy grow page | per-fault, **bounded** | see L2 (guest-visible, budget-capped) |
| `fclient.c:88` `ts` | standalone benchmark `main()` | not the engine; process exits immediately after (`fclient.c` is the `--bench` client tool) |

So the imbalance is fully explained by one-time-init + guest-owned mappings + a bench tool. No action needed.

---

## Confirmed BOUNDED — do NOT spend test effort here

Code-cache / translation (`jit/cache.c`, `frontend/x86_64/cache.c`):
- **Code cache itself** — single fixed 64MB arena (`CACHE_SZ`, `cache.c:5`/`:4`); on fill it is flushed **in place** (bump pointer reset, `dispatch.c:117`/`:94`) — bytes reused, never grown. (Multithreaded aarch64 is the L1 exception.)
- **`g_map` block map** — fixed `JIT_MAP_N=1<<19` (aarch64) / `MAP_N=65536` (x86), zeroed on flush (`dispatch.c:118`). Open-addressed, never grows; sized so the arena fills first (`cache.c:76-83`). No per-block metadata struct is heap-allocated — block info lives in this fixed table.
- **Translation scratch** — `seen[]`, `stolen[]`, `stores[]`, `sc[]` in `translate.c` (both arches) are **stack-local** fixed arrays (e.g. `aarch64/translate.c:1182`, `x86_64/translate.c:614`), freed on function return. No per-block heap alloc anywhere in the translate/emit units (grep for malloc in `translate*.c`/`emit*.c`/`avx.c`/`x86_ops.c`/`x87.c`/`repstr.c`/`decode.c` finds only comments).
- **`emit.c` patch lists** — `to_slow[]`/`after[]` are stack-local (`emit.c:463-600`).
- Self-modifying code: re-translation reuses `g_map` slots / in-place arena; no accumulation.

IBTC / inline caches / indirect-branch tables:
- **`g_ibtc`** fixed `IBTC_N=8192`, zeroed on flush (`cache.c:577`, `dispatch.c:121`). Direct-mapped → self-evicting, never grows.
- **`g_xibtc`** (x86 2-way) fixed `XIBTC_SETS*XIBTC_WAYS` (`engine_glue.c:41`), 2-way set-associative → evicts in place (`dispatch.c:118-125`), zeroed on flush.
- **`g_pend`** direct-branch backpatch list, fixed `1<<16`, swap-removed as resolved (`cache.c:453`/`cache.c:93`), reset to 0 on flush. Append is guarded (`add_pend2`, `cache.c:431`).
- **IBPROF / VDBETRACE tables** (`g_ibsite`, `g_ibtrans{1,2,3}`, `g_k{1,2,3}`, `cache.c:184-219`) — fixed open-addressed; **gated behind `IBPROF`/`VDBETRACE` env** (off by default), degrade-don't-grow on full.
- **`g_reloc`** fixed `1<<16`, append-guarded (see L4 for its only caveat).

Per-thread / per-context state:
- **Stop-the-world thread registry** `g_stw_threads` — fixed `STW_MAXTHREAD=4096`; each thread frees its slot in `stw_unregister()` (`cache.c:527`) on guest-thread exit; `stw_after_fork` clears it in the child (`cache.c:620`). Bounded and reclaimed.
- **`g_t2cnt`/`g_t2gpc`** tier-2 counters — fixed `T2_MAX=8192`, `t2_slot` dedups by gpc and returns -1 when full (`cache.c:412-420`). No growth on re-translation.
- **`g_smc_pg`** SMC protected-page set — fixed `SMC_MAX=8192`, entries removed on write (`dispatch.c:35`), append-guarded (`dispatch.c:25`).
- The per-guest-thread `struct cpu` is allocated in `os/linux/thread.c` (**outside this audit's scope**) — flagged for a separate check that it's freed on guest-thread exit; nothing in the audited units allocates it per-op.

fork() paths:
- `jit_after_fork` (`cache.c:636`) builds a fresh dual map AND `munmap`s the old RW+RX (`cache.c:648-649`) — correctly balanced, no child leak.
- `dualmap_alloc` error path `munmap`s the partial RW (`cache.c:44`) — balanced.

Signal handling:
- `build_signal_frame`/`do_sigreturn` (`sigframe.c`) build the rt_sigframe **on the guest's own stack** (`sigframe.c:23-25`) — zero host allocation per signal.

---

## Final note for the test author
Only **L1** is a true unbounded host-RSS leak, and it is narrow: it needs the **aarch64** engine + **≥2 live guest threads** + **repeated 64MB cache exhaustion**. The PROF counter `g_stw_flushes` (`jit/cache.c:488`) is the exact trip-wire — assert `host_RSS ≈ baseline + 128MB × g_stw_flushes`. L2/L3 are robustness/liveness issues at scale (bounded-abort, not RSS runaway). L4 is pcache-only correctness. The 9-vs-13 imbalance is a false alarm.
