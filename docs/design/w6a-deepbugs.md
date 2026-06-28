# W6A — deep-bug cluster (4 items)

**Engines:** jit86 (x86→arm64) + shared aarch64. **Host:** macOS 26.3.1, Apple M4 (16KB pages).
**Patched tree:** `/Users/x/w6a-bugs` (mirror `scratchpad/opt-work/w6a-bugs`). **Diff:** `w6a-deepbugs.diff`.
**Build (mac bridge):**
```
clang -O2 -o ddjit-x86 src/runtime/targets/linux_x86_64.c && codesign -s - --entitlements jit.entitlements -f ddjit-x86
clang -O2 -o ddjit-arm src/runtime/targets/linux_aarch64.c && codesign -s - --entitlements jit.entitlements -f ddjit-arm
```
Baseline for bit-exact A/B = `ddjit-x86-base` built from the **unmodified canonical** `/Users/x/dd/dd/dd-jit`.

Every change is behind a kill-switch and **inert by default for the existing test matrix** (the new code
paths only fire for the specific guest classes each item targets), so PIE/normal guests are byte-identical.

| # | item | status | gate |
|---|------|--------|------|
| 1 | non-PIE ET_EXEC fork+execve crash | **PARTIAL (fault-fixup; major facets fixed)** | `NONPIE_NOFIXUP=1` |
| 2 | activate dual-mapped RX/RW code cache | **FIXED** | `NODUALMAP=1` |
| 3 | RWX guest-JIT pages (+SMC) | **FIXED** | `NORWXFIX=1`, `NOSMC=1` |
| 4 | x86 lazy-fault budget never resets | **FIXED** | `NOLAZYFIX=1`, `LAZYBUDGET=n` |

---

## Item 2 — Activate dual-mapped RX/RW code cache  · **FIXED**  · gate `NODUALMAP=1`

**Starting point (verified):** the committed tree still toggles `pthread_jit_write_protect_np` per block —
dual map was NOT in the tree (only a PoC diff `w3c-dualmap.diff`). I rebuilt + activated the **RW-canonical**
variant (per `w3c-dualmap.md`'s recommendation — strictly simpler than wrapping ~20 emit sites with `cw()`:
because `emit32` already writes through `g_cp` and `g_cp` is made the **RW** writer alias, *all* cache stores
already land on the writable alias by construction; only the handful of **absolute** handoffs convert RW↔RX).

**What landed (6 files):**
- `jit/cache.c` — `dualmap_alloc()` (anon RW + `mach_vm_remap` to a 2nd VA + `mach_vm_protect` RX),
  `J_RX`/`J_RW`, `jit_wprot()` (no-op under dual map), `jit_after_fork()` (rebuild the child's aliased map).
- `jit/dispatch.c` — 4 raw toggles → `jit_wprot`; `run_block(c, J_RX(code))`; icache flush over `J_RX`.
- `jit/emit_arm64.c` — **the load-bearing fix**: `e_adrp_add` page-immediate computed against `J_RX(g_cp)`
  (else every inline IBTC lookup reads `&g_ibtc + g_rw2rx` → 100% miss — invisible to output, caught by PROF).
- `frontend/aarch64/dispatch_hooks.h` — `G_IBTC_FILL` body literal stored as `J_RX`, per-site literal written
  via `J_RW(ic_site)`.
- `targets/linux_aarch64.c` — cache alloc via `dualmap_alloc` (default), `NODUALMAP=1` → single MAP_JIT.
- `os/linux/service.c` — `jit_after_fork()` in the clone(220)/clone3(435) children; PROF line adds
  `wx_toggles`/`dualmap`/`xlate_ms`.

**Why this fixes the fork+exec flake (mechanism):** the busybox flaky fork+exec tail is a MAP_JIT
execute-permission fault in the fork child — per-thread APRR is unreliable across `fork()`. Under dual map the
RX permission lives in the **page tables** (inherited by the child), and `jit_after_fork()` rebuilds a fresh,
correctly-aliased map (vm_remap aliases COW independently). No per-thread permission state is relied on.

**Proof — activation + toggle elimination (`PROF=1 ./ddjit-arm ./big2`):**
```
dualmap : wx_toggles=0      dualmap=1
fallback: wx_toggles=73096  dualmap=0     (NODUALMAP=1)
```
**Proof — byte-identical & no IBTC regression** (dualmap vs `NODUALMAP=1` fallback), 8 guests:
`big big2 compute icheavy libcheavy threads thic forkc` → **all stdout/exit-code identical**, and
`ibtc_miss` **matches the fallback exactly** (e.g. big2 12117=12117, icheavy 149=149, thic 4000291=4000291) —
confirming the adrp fix is correct. Plus a **16-applet busybox battery** (echo/ls/sort/uniq/grep/wc/md5sum/
sha256sum/sed/tr/head/cut/awk/printf/date/du) dualmap==fallback, **0 diffs**.

**Proof — fork+exec stable under load:** 6-way × 40 = 240 concurrent runs (~600 `fork+execve`/run) → **0
failures** under dual map; and an 8-way × 30 = 240-run `fork+exec+pipe` A/B (`loadtest.sh`): **dual-map 0,
fallback 0**. The known flake is intermittent (and likely dearer on M1 than this M4); it did **not** reproduce
in either mode here, so the result is "**dual map is at least as stable, and structurally removes the
across-fork per-thread-APRR hazard**" (RX permission inherited via page tables + `jit_after_fork` rebuild) —
rather than a before/after crash count on this machine.

**Note:** threaded IBTC fill (`g_threaded ? NULL` guard) left as-is — that is W5-C's separate inline-reader
ordering follow-up; dual map *unblocks* it but enabling it is out of scope here (keeps the PoC byte-identical).
x86 engine: shares `jit/cache.c` so the helpers exist, but x86 still allocates MAP_JIT → `g_dualmap=0` → fully
inert (bit-exact); activating x86 dual map is a clean follow-up.

---

## Item 4 — x86 lazy-fault budget never resets  · **FIXED**  · gate `NOLAZYFIX=1`

**Confirmed mechanism (repro):** `jit86_lazyguard` maps each lazily-faulted page but only while a single
**global, monotonic, never-reset** counter `g_lazymaps < 4096`. The audit's hypothesis that large
`sort`/`ls -lR` exhaust it does **not** reproduce (the 256MB pre-reserved heap + 64KB mmap guard tails + 8MB
stack cover normal busybox; 3M-line sort never faults). It *does* bite for **>4096 distinct guard pages**:
a synthetic guest (`mkoverread.py`: mmap a region at a high fixed addr, then over-read N pages past it) with
**N=8192** (16KB stride = one macOS page) →
```
NOLAZYFIX=1 (legacy): SIGSEGV, exit 139   (budget 4096 exhausted -> fatal re-raise)
fix (default):        exit 0 "OK"  [lazy] grow=8192 wild=0
```

**Fix:** classify each fault by **adjacency to a live mapping** (via `mach_vm_region` — `mincore` is useless on
macOS, it returns 0 for *any* address). A fault whose neighbour (byte below, or the 16KB page above) is mapped
is legitimate growth/over-read → served from a large **grow** budget (256K pages); an **isolated** fault (both
neighbours unmapped) is a candidate wild pointer → still bounded by the small **wild** budget (4096) and
crashes. Page contents + retry are unchanged → **bit-identical for any workload the old code completed.**

**Proof — safety net preserved:** a *scattered* wild guest (`mkoverread.py` start +256MB, stride 256KB →
isolated faults) **still SIGSEGVs (exit 139)** under the fix — genuine wild access remains bounded.
**Proof — bit-exact:** x86 battery (18 workloads incl. big sort / ls -lR / find / md5sum / sha256sum)
`ddjit-x86` vs unmodified `ddjit-x86-base` → **failures=0**.

---

## Item 3 — RWX guest-JIT pages + SMC  · **FIXED**  · gates `NORWXFIX=1`, `NOSMC=1`

**Confirmed mechanism:** the guest mmap path passed `prot = a2 | R | W`, so a guest PROT_EXEC (RWX) request
became R|W|X → macOS hardened W^X returns **EPERM** → JVM/V8/LuaJIT/.NET/PyPy can't allocate their code arena.
Repro (`mkjit.py`: mmap RWX, write `mov eax,42;ret`, `call` it): `ddjit-x86-base` → **SIGSEGV exit 139**.

**Key insight:** this is a DBT — the host **never executes guest pages natively**; "execution" is
`translate_block` reading the page's bytes. So PROT_EXEC on a guest mapping is meaningless to the host and only
triggers the EPERM. **Fix:** strip PROT_EXEC from guest anon mmaps (map R+W); the guest writes its code,
"executes" it (guest PC enters the page → translate), runs.
```
RWX JIT guest:  base/NORWXFIX=1 -> exit 139    fix (default) -> exit 42   (generated code ran)
```

**SMC (self-modifying code):** a guest that *overwrites* already-translated code returned the **stale**
translation. Implemented write-fault invalidation (gated, inert unless a RWX guest is present → `g_rwx_guest`):
after translating, `mprotect` the source 16KB page read-only; a guest write traps in `jit86_lazyguard`, which
drops `g_map`+`g_ibtc`+pending chains (**without** resetting `g_cp` — the running block's host code stays
intact; orphaned translations are reclaimed by the normal wholesale flush), unprotects the page, and lets the
write land + re-translate. x86 uses only the shared `g_ibtc` (no per-site ICs), so clearing it suffices.
```
SMC guest (write 42-code, call; overwrite to 7-code, call): NOSMC=1 -> 42 (stale)   fix -> 7 (correct)
```
**Bit-exact:** `smc_protect` is `if(!g_rwx_guest) return;` first → zero effect on the matrix; post-SMC x86
battery vs baseline **0 diffs**.

**Residual (honest):** (a) syscall *pointer args* that point into the **low non-PIE image data** are read 1:1
in `service.c` and not redirected (real gcc/gosu pass stack/heap pointers — fine; a non-PIE passing a low
`.rodata` pointer to a syscall would still fault in the handler — shared with item 1). (b) SMC invalidation is
page-wholesale and doesn't unpatch existing **direct-jump chains** into overwritten code (indirect-call JITs —
the norm — are covered); (c) only integer ld/st absolute refs are fixed up (see item 1).

---

## Item 1 — non-PIE ET_EXEC fork+execve crash  · **PARTIAL (fault-fixup)**  · gate `NONPIE_NOFIXUP=1`

**Confirmed mechanism:** a non-PIE ET_EXEC links at a fixed low vaddr (0x400000); macOS `__PAGEZERO` reserves
the low 4GB so the loader must bias it to a high mmap. Its un-relocated **absolute** refs then resolve to the
original low vaddr (in `__PAGEZERO`) and fault. Minimal repro (`mknonpie.py`: `movzx edi,[0x401000]` disp32
absolute) → **SIGSEGV exit 139** (native would read 55).

**Host-layout-safe approach (A) re-evaluated and rejected:** linking the x86 host with
`-Wl,-pagezero_size,0x4000` frees the low band but **kills normal PIE guests (rc=137)** — exactly the prior
finding. The blocker is fundamental: with a small `__PAGEZERO`, NULL-hint mmaps (PIE image/heap/stack/cache)
and the **host's own low Mach-O segments** land in the band a non-PIE needs, and macOS does not honour mmap
hints, so forcing everything high requires whole-address-space MAP_FIXED management *and* still risks the
non-PIE's fixed 0x400000 colliding with the host. This is the twice-failed path; I did not pursue it.

**Approach taken — self-contained fault-fixup (B), no relink, no PIE regression:**
1. `frontend/x86_64/elf.c load_elf`: set `g_nonpie_lo/hi/bias` for ET_EXEC (etype==2) — the x86 loader never
   did (the redirect existed only on the aarch64 engine).
2. `frontend/x86_64/dispatch.c`: redirect absolute **code** jumps — if `c->rip ∈ [lo,hi)` then `+= bias`
   (mirrors `jit/dispatch.c`).
3. `frontend/x86_64/elf.c jit86_lazyguard`: `nonpie_fixup()` — on a fault into the low link range, decode the
   faulting emitted arm64 **integer load/store** (scaled-imm + unscaled, signed/unsigned, b/h/w/x), perform the
   access at `si_addr + bias`, advance the host PC. Self-contained in the SIGSEGV handler; gated on `g_nonpie`
   (only set for ET_EXEC) → PIE/static-PIE untouched.

**Proof:**
```
non-PIE absolute data load:     fixup -> exit 55     NONPIE_NOFIXUP=1 -> exit 139
non-PIE fork + execve(self):    fixup -> exit 77     NONPIE_NOFIXUP=1 -> exit 0 (child crashes)
```
The fork+execve case (`mkforkexec.py`, stack-built argv like real gcc/gosu) exercises the full path: fork →
child `execve(self)` (case 221 resets then re-sets `g_nonpie` for the re-loaded non-PIE) → child's absolute
data load fixed up → parent `wait4` → exit 77. **PIE regression: 0** (18-workload x86 battery vs baseline).

**Why PARTIAL (residual, documented):** the fixup covers the **integer ld/st** absolute-ref forms (the bulk of
non-PIE data refs) but **not**: SIMD-Q/`ldr q` (SSE) and LSE atomic RMW absolute accesses (they take the normal
path → clean abort, not silent wrong data); and **syscall pointer args** pointing into low `.rodata`/`.data`
(read 1:1 in `service.c`; real toolchain binaries pass stack/heap pointers, but a non-PIE handing a global
string straight to e.g. `open` would fault in the handler — would need a `g2h()` redirect on non-PIE pointer
args). A real gcc/`gosu` (Go, non-PIE, mostly stack/heap syscall args + integer absolute data refs) is expected
to work for the dominant paths; a full guarantee needs the SIMD/RMW fixup forms + the syscall-arg redirect.
**Could not run the real gcc/gosu binaries** (no container network for amd64 image pull, no cross-toolchain on
the mac) — guests were emitted as raw x86-64 ELFs via Python (`scratchpad/opt-work/w6a-bugs/guests-gen/`).

---

## Reproducers (all in `scratchpad/opt-work/w6a-bugs/guests-gen/`, emit raw x86-64 ELFs — no toolchain needed)
- `mkoverread.py` — item 4 budget exhaustion (adjacent over-read) + wild-scatter safety test.
- `mkjit.py [smc]` — item 3 RWX JIT guest + SMC overwrite.
- `mknonpie.py` — item 1 non-PIE absolute data ref.
- `mkforkexec.py` — item 1 non-PIE fork+execve(self).

## Net status
- **FIXED:** item 2 (dual-map activated, fork+exec stable, byte-identical), item 3 (RWX EPERM + SMC),
  item 4 (lazy budget, safety net preserved).
- **PARTIAL:** item 1 (fault-fixup handles absolute integer data refs + code jumps across fork+execve; SIMD/RMW
  + low syscall-arg pointers remain; host-`__PAGEZERO`-shrink confirmed a platform dead-end).
