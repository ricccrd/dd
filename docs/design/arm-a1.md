# A1 (+A2): Steal host x16/x17 for the engine — kill the per-indirect-branch red-zone stash/restore

**Engine:** dd-jit aarch64 same-ISA transliterator.
**Lever:** `docs/design/arm-sqlite-parity.md` §3 A1 (the P0 lever) + a touch of A2.
**Deliverable:** standalone diff (`arm-a1.diff`), this report, patched tree at `/Users/x/arm-a1`.
**Status:** implemented, gated (`NOSTEAL1617=1`), **bit-exact** vs the unmodified engine on the full
corpus. **Net trade is positive but small** — and crucially it does **not** close the sqlite parity gap,
because the cost A1 removes turns out to be hidden by the host OoO core. See the recommendation.

---

## 1. What the lever does

Today the IBTC inline probe (`emit_ibranch`) uses host `x16`/`x17` as scratch, but they are
**guest-visible** (IP0/IP1), so every indirect branch stashes/restores them through the stack red zone.
A1 makes host `x16`/`x17` **engine-private**: guest `x16`/`x17` live only in `cpu->x[16]/[17]` and are
**mangled on use** exactly like the already-stolen `x18`/`x28`/`x30`. With the host pair free, the probe
needs no stash/restore and the `body_ind` restore stub disappears.

Implementation (4 files, ~120 lines, all behind `g_steal1617`, default on):
- `include/cpu_aarch64.h` — `is_stolen()` now also returns true for 16/17 (gated by `g_steal1617`).
  This single change drives the entire mangle machinery (`uses_x18`/`field_is`/`emit_mangled_x18` are
  register-number-agnostic), the prologue/spill skip-loops, and every `is_stolen` guard in `translate.c`
  (adr/adrp/ldr-literal/mrs/msr/cbz/tbz special cases + the LSE-atomic gate).
- `jit/emit_arm64.c` — `emit_prologue` emits the `body_ind` restore stub only in legacy mode; new
  `emit_ibranch_steal()` is the collapsed probe; the original `emit_ibranch` body is preserved verbatim as
  the legacy path, selected at runtime by the gate.
- `frontend/aarch64/dispatch_hooks.h` — `G_IBTC_FILL` caches `body` (not `body-8`) under the steal gate.
- `targets/linux_aarch64.c` — `NOSTEAL1617=1` reverts to the 3-reg stolen set at startup.

A indirect branch **through** a stolen reg (`br/blr x16|x17|x30`) loads the guest target from
`cpu->x[rn]` into the free host link reg `x30` and runs the generic 3-register probe (target=x30,
scratch=x16/x17) — unifying the old special-cased `x16/x17` path with the `x30` path.

---

## 2. Before / after the hot path (monomorphic IBTC hit)

**Legacy (`NOSTEAL1617=1`)** — generic non-stolen target reg `xRn`:
```
stur x16,[sp,#-16]    ; (mem store)  stash scratch pair
stur x17,[sp,#-24]    ; (mem store)
ldr  x16, Lsite_tgt   ; cached target literal
sub  x16, x16, xRn    ; compare
cbnz x16, Lhash       ; miss -> shared hash
ldr  x16, Lsite_body  ; cached body (= body_ind)
br   x16              ; -> body_ind:
                      ;     ldr x16,[sp,#-16]   (mem load)
                      ;     ldr x17,[sp,#-24]   (mem load)  -> falls into body
```
= **9 instructions, 4 stack mem-ops** (matches the design doc).

**Steal (default)** — `emit_ibranch_steal`, generic `xRn`:
```
ldr  x16, Lsite_tgt   ; cached target literal
sub  x16, x16, xRn    ; compare
cbnz x16, Lhash       ; miss -> shared hash
ldr  x16, Lsite_body  ; cached body (= body, no stub)
br   x16              ; -> body directly
```
= **5 instructions, 0 stack mem-ops** (exactly the design-doc target).

For `rn ∈ {16,17,30}` the steal path prepends one `ldr x30,[cpu,#rn*8]` → 6 instrs / 1 mem-op (the
legacy `x30` path already paid that load, and the legacy `x16/x17` path was *more* convoluted, so steal
wins there too). The shared-hash miss path also loses its two `ldur` restores. Codegen counts are
authoritative from the diff; correctness of the encodings is proven by the bit-exact A/B (§4).

---

## 3. The measured trade (median-of-9 wall, ON=steal vs OFF=`NOSTEAL1617=1`, vs native = direct aarch64)

| guest | native | steal (ON) | gate-off | **ratio ON** | ratio OFF | ON vs OFF |
|---|---|---|---|---|---|---|
| **sqlite** (600k-row) | 0.327 | 0.6127 | 0.6220 | **1.87×** | 1.90× | **−1.5%** |
| **qsort** (callback)  | 0.776 | 1.2976 | 1.3278 | **1.67×** | 1.71× | **−2.3%** |
| **soak_indirect** (80M megamorphic calls) | 0.092 | 0.2572 | 0.2677 | 2.80× | 2.91× | **−3.9%** |
| sha256 (straight-line NEON) | 0.764 | 0.770 | 0.766 | 1.01× | 1.00× | +0.5% (noise) |
| int-sieve (tight loop) | 0.816 | 0.452 | 0.452 | **0.55× (faster)** | 0.55× | 0.0% |
| mono indirect (100% per-site-cache hit) | 0.279 | 0.282 | 0.282 | 1.01× | 1.01× | 0.0% |

(redis was attempted as the indirect-heavy server case but is blocked in this bare bridge harness — the
overlay/`DD_LOWER` path can't resolve the musl loader without ddockerd's full mount setup; even busybox
from the rootfs fails. `soak_indirect` is the server-dispatch-loop proxy: 80M megamorphic indirect calls.)

### The decisive datapoint: monomorphic indirect is ALREADY native — even gate-off
A 300M-iteration **monomorphic** indirect-call loop runs at **1.01× native with the gate OFF** (0.282 vs
0.279). The legacy 9-instr/4-mem-op probe is already native-speed: on the Apple-Silicon OoO core the four
red-zone stores/loads forward to/from the same slots and issue in the shadow of the call's real work, so
removing them frees ~0 cycles. **A1 therefore yields essentially nothing on monomorphic dispatch.**

The win only appears where the probe runs the **shared-hash** path (polymorphic/megamorphic dispatch:
sqlite's VDBE jump table, qsort's churned site, soak_indirect): a steady **−2% to −4%**. Even there the
dominant cost is the *indirect-branch misprediction* (which A1 does not touch), so shortening the probe is
a small fraction.

### Mangle cost (the downside)
Static guest `x16/x17` usage (objdump): **sqlite 672/318313 = 0.21%**, **qsort 458/93091 = 0.49%**,
sha256 0.50%, int-sieve 0.49%. These uses are concentrated in PLT/veneer/ld.so startup (cold), not hot
loops — which is why there is **zero net regression anywhere**, including the straight-line guards that do
contain a few `x16/x17` setup instructions (sha256/int-sieve flat). Net = (indirect savings) − (mangle
cost) ≥ 0 on every workload measured; strictly > 0 on polymorphic dispatch.

### A second, smaller downside found: LSE suppression
`try_lse_atomic` bails (`is_stolen(...)→return 0`) when an atomic loop uses `x16/x17`, so those sites no
longer upgrade to FEAT_LSE — they fall back to the (correct, mangled) `ldxr/stxr` loop. PROF shows sqlite
`lse=2 → 0`: two atomic sites use `x16/x17`. Cold in single-threaded sqlite (no perf change observed), but
worth noting for atomics-heavy multithreaded guests that happen to address through IP0/IP1.

### PROF deltas (steal vs gate-off) — engine behaviour unchanged
sqlite: `crossings 7971/7964 · ibtc_miss 1107/1107 · translations 4998/4990`. qsort: `ibtc_miss 160/160`.
Identical IBTC dynamics; the tiny crossings/translations jitter is block-layout noise.

---

## 4. Correctness

- **Bit-exact vs the unmodified engine: 87/87** static-PIE guest corpus (`dd-tests/guests/*`) — every
  guest produces **byte-identical stdout+exit** under steal == base(committed) == `NOSTEAL1617=1`,
  including `soak_indirect` (80M megamorphic indirect calls), `threads`, `forkwait`, `soak_smc`,
  `soak_forkchurn`, `soak_threadchurn`, `longjmp`, `sigjmp`.
- **Bench guests: 10/10** (`.bench-cache/aarch64`) byte-identical across steal/base/nosteal **and** match
  native directly.
- **vs native: 82/87** match; the 5 that differ (`edge_mprotect/otmpfile/pipepacket/procfd/statfs`) are
  syscall-edge guests that differ **identically on the committed baseline** (steal==base) — pre-existing
  JIT-on-macOS vs Linux env differences, not introduced by A1.
- **Targeted IP0/IP1 mangle stress** (`gsrc/ip2.c`: explicit `mov x16 / eor x17` inline asm interleaved
  with function-pointer indirect calls and `setjmp`/`longjmp` unwinds, 500k iters): steal == base ==
  nosteal == **native** (`ip2 5956753301782823687`). This directly exercises the warned cases (IP0/IP1
  data use + veneer/longjmp interaction) and confirms the mangle is correct.
- The diff applies cleanly to a fresh `HEAD` checkout and that tree **builds and runs** (sqlite=2325248).

Note: a *more* adversarial test (`x16/x17` pinned as long-lived `register __asm__` variables held across
a 20-deep `longjmp`) crashes — **but identically on the committed baseline** (steal==base==nosteal, all
SIGSEGV). That is a pre-existing engine limitation, independent of A1. (Also: non-PIE `ET_EXEC` static
guests crash under the baseline too — the known open issue in MEMORY; all corpus testing used static-PIE.)

A1 also **removes a latent risk**: previously guest `x16/x17` lived in host IP0/IP1 during block
execution, which the host linker's veneers (and any host code path) may clobber; now they always live in
`cpu->x[]`, so there is nothing for a host veneer to corrupt.

---

## 5. Recommendation

**Ship — but as a cheap, correctness-positive cleanup, NOT as the sqlite parity lever.**

- It is bit-exact, strictly ≥ baseline on every workload, neutral on straight-line code, and it removes
  the IP0/IP1 host-veneer-clobber latent risk. The cost is ~120 lines behind a clean gate.
- **But it does not close the sqlite gap** (1.90× → 1.87×). The design doc's premise — that the four
  red-zone mem-ops dominate — is **contradicted by the data**: the monomorphic probe is already 1.01×
  native gate-off, i.e. the OoO core hides those mem-ops. The residual sqlite/qsort overhead is therefore
  **elsewhere** (most likely the indirect-branch *misprediction* on the VDBE jump table + the
  per-iteration VDBE dispatch itself, plus `bl/ret` chains), which A1 cannot address.
- **Redirect the parity effort** to levers that attack misprediction / dispatch, not probe length:
  A3 (widen §B return-stack coverage so sqlite's `bl/ret` chains stay RAS-predicted) and especially
  **B1 (meta-trace the VDBE opcode stream into a threaded superblock)** — eliminate the data-dependent
  dispatch `br` entirely rather than shave its probe. That is where the 1.0× (and sub-1.0×) lives.

If a 2–4% win on polymorphic-dispatch-heavy real software (interpreters, redis-class event loops, qsort)
is wanted now at near-zero risk, land A1 default-on with the `NOSTEAL1617` kill-switch.

---

## 6. Reproduce
```
# build (macOS arm64 via the bridge)
mac bash -lc "cd /Users/x/arm-a1 && clang -O2 -o ddjit-arm src/runtime/targets/linux_aarch64.c && \
  codesign -s - --entitlements jit.entitlements -f ddjit-arm"
# A/B (default = steal on; NOSTEAL1617=1 = legacy)
mac bash -lc "cd /Users/x/arm-a1 && bash bench_one.sh 9 ./ddjit-arm <guest>"
mac bash -lc "cd /Users/x/arm-a1 && NOSTEAL1617=1 bash bench_one.sh 9 ./ddjit-arm <guest>"
# native baseline (direct on this aarch64 Linux host)
bash /Users/x/arm-a1/bench_one.sh 9 <guest>
# PROF: PROF=1 ./ddjit-arm <guest>   (ibtc_miss/crossings/lse deltas)
```
