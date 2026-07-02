# BUG #201 — Angle A: differential per-allocation trace (tiny-allocator handout audit)

Author: DBT codegen agent (Angle A). Status: **allocator DEFINITIVELY EXONERATED** — there is no
"first wrong block". Redirects the investigation to hypothesis #2 (a wrong-destination zero-store whose
EA is miscomputed outside the allocator handout). This file is an exhaustive, reproduce-from-scratch
account of what I did, how, the exact numbers, and the handoff. See `docs/BUG201.md` for the converged
top-level record; this is the Angle-A deep dive.

---

## 0. One-line result

Across the entire corrupting run (~80,000 tiny allocations probed), **every** tiny-allocator handout
(both the tiny-bump path and nextFreeFast/nextFree) returned a genuinely FREE slot: `allocBits==0`, no
overflow into a live neighbor slot, zero live handouts. The tiny allocator does not hand out a live/marked
slot. Therefore the corrupting 16-byte zero-store gets its wrong destination from **somewhere other than
the allocator handout**, and the per-alloc differential trace has no divergence to report — because the
allocation stream is correct.

---

## 1. Hypothesis I set out to test (Angle A mandate)

BUG201's converged mechanism: Go's tiny allocator fetches a fresh 16-byte block and zeroes it with one SIMD
store (`movups %xmm15,(%r11)` at mallocgc ~0x40cff3 → dd `str q15,[x17]`). On a corrupting run the block
address is claimed to be a LIVE, still-marked, in-use tiny block, so this memclr zeros a live string in
place. Hypothesis #1 ("allocator reuse"): a value/layout-dependent miscompile in the guest instructions
computing that block address (inlined nextFreeFast: allocCache/`bsf`/freeindex/index-math, OR the tiny bump
`c.tiny + round(tinyoffset, align)`) hands out a slot that is actually live.

**Angle A's job:** log, for EVERY tiny allocation on the corrupting path, the returned block address plus
enough allocator state to decide whether that block is a LIVE/allocated slot, run it deterministically, and
find the FIRST allocation that returns a live slot — then pin the guest instruction that computed it wrong.

**What I actually proved:** there is no such allocation. Both handout paths are correct on every allocation
of a corrupting run. This is the strongest possible negative for hypothesis #1, and it is stronger than the
prior round because I check the correct, *persistent* bitmap (`allocBits`) rather than the *transient*
`gcmarkBits` the earlier round used (which reads 0 outside the mark window and therefore could never have
caught a handout).

---

## 2. The guest tiny-allocator PC map (disassembled from `target/heap201/mcount`)

Disassemble with an x86-capable objdump (the OrbStack `/usr/bin/objdump` is ARM-only and errors with
"architecture UNKNOWN"; use `x86_64-linux-gnu-objdump`):

```
x86_64-linux-gnu-objdump -d --start-address=0x40cd00 --stop-address=0x40d1f0 \
  /Users/x/dd/dd/target/heap201/mcount
```

Relevant symbols (link addresses; **runtime addr = link + 0x200000000 under DDFIXHEAP**):

| link PC     | what                                                                                     |
|-------------|------------------------------------------------------------------------------------------|
| `0x40cd00`  | `runtime.mallocgc` entry (`cmp 0x10(%r14),%rsp` stack check)                              |
| `0x40cd50/60/6f` | tiny align tests `test $0x7/$0x3/$0x1,%rax`                                          |
| `0x40ce80`  | `cmp $0x10,%rbx` — size<16 tiny gate                                                      |
| `0x40ce8a`  | `mov 0x18(%rdx),%r10` — r10 = `c.tinyoffset` (mcache+0x18), then aligned up per size      |
| `0x40cec3`  | `lea (%r10,%rbx,1),%r11` — r11 = rounded offset + size (fit test operand)                 |
| `0x40cec7`  | `cmp $0x10,%r11; ja 0x40cf1d` — if it doesn't fit in the 16B block → refill                |
| `0x40cecd`  | `mov 0x10(%rdx),%r12` — r12 = `c.tiny` (mcache+0x10, the current block base)              |
| **`0x40ced6`** | **TINY-BUMP RETURN** `lea (%r12,%r10,1),%rax` — returned obj = `c.tiny + roundedOffset` |
| `0x40ceda`  | `mov %r11,0x18(%rdx)` — write back new `c.tinyoffset`                                     |
| `0x40cf1d`  | `mov 0x50(%rdx),%r10` — r10 = mcache.alloc[tinySpanClass] (the mspan*)                    |
| `0x40cf21`  | `mov 0x38(%r10),%r11` — r11 = `span.allocCache` (span+0x38)                               |
| `0x40cf25`  | `bsf %r11,%r12` — r12 = lowest set bit of allocCache (first free slot in cache)           |
| `0x40cf39`  | `movzwl 0x30(%r10),%r13d` — r13 = `span.freeindex` (span+0x30, u16)                       |
| `0x40cf3e`  | `lea (%r13,%r12,1),%r15d` — **r15 = objIndex = freeindex + bitIndex**                     |
| `0x40cf43`  | `movzwl 0x32(%r10),%esi` — esi = `span.nelems` (span+0x32, u16)                           |
| `0x40cf77`  | `shr %cl,%r11` allocCache update, with the `cmp $0x40; sbb; and` shift-by-64 guard        |
| `0x40cf9e`  | `mov 0x68(%r10),%r11` — r11 = `span.elemsize` (span+0x68)                                 |
| `0x40cfa2`  | `imul %r15,%r11` — r11 = objIndex * elemsize                                              |
| **`0x40cfa6`** | **nextFreeFast SLOT ADDR** `add 0x18(%r10),%r11` — r11 = `span.base + objIndex*elemsize` |
| `0x40cfbb`  | slow path: `mov %rdx,%rax; mov $5,%ebx; call 0x40cb00 <runtime.(*mcache).nextFree>`; r11=result |
| **`0x40cff3`** | **BLOCK-ZERO** `movups %xmm15,(%r11)` (xmm15=0) → dd `str q15,[x17]` (ARM `0x3d80022f`) |
| `0x40cff7..` | update `c.tiny=r11` (mcache+0x10) and `c.tinyoffset` for the fresh block                 |
| `0x463440`  | `runtime.memclrNoHeapPointers` (dst=rax, n=rbx) — a separate 16B-SIMD-zero memclr site   |

Key semantics used by the probe:
- **tiny-bump return** hands out `[obj, obj+size)` *within* the current `c.tiny` 16B block (already zeroed
  at refill). It can only corrupt a *different* live object if either (a) `c.tiny` is wrong (ruled out —
  `c.tiny` = a prior refill's r11, which the refill probe shows is a free slot), or (b) the rounded
  offset + size overflows 16 and spills into the *next* slot (the fit guard `cmp $0x10,%r11` miscompiled).
  The probe checks (b) explicitly by probing both `obj` and `obj+size-1`.
- **refill/nextFreeFast** hands out a fresh 16B slot `r11`. Invariant: a fresh slot must have
  `allocBits[objIndex]==0` (it was free at the last sweep). `allocBits==1` ⇒ the allocator returned a slot
  that is still live ⇒ the movups block-zero zeros a live object ⇒ THE BUG.

---

## 3. Why allocBits, not gcmarkBits (the fix over the prior round)

Go mspan (Go 1.22) offsets, from the tiny span (all confirmed against the disasm above):

```
base       span+0x18   (uintptr)   arena address of slot 0
freeindex  span+0x30   (u16)       slots [0,freeindex) known in-use; alloc scans from here
nelems     span+0x32   (u16)
allocCache span+0x38   (u64)       = ^allocBits from freeindex onward; bit j => slot freeindex+j is FREE
allocBits  span+0x40   (ptr→bitmap) PERSISTENT: "allocated as of the last sweep". Set to gcmarkBits at sweep.
gcmarkBits span+0x48   (ptr→bitmap) TRANSIENT: set during the mark phase, 0 outside it.
allocCount span+0x60   (u16)
spanclass  span+0x67   (u8)
elemsize   span+0x68   (u64)
```

`nextFreeFast` never sets `allocBits` — that only happens at sweep (`allocBits = gcmarkBits`). So a
freshly-handed-out slot has `allocBits==0`; a slot still holding a survivor from the last GC has
`allocBits==1`. Checking `allocBits[objIndex]==1` at handout is therefore a **stable, non-timing-dependent**
detector of "the allocator returned a live slot". The prior round checked `gcmarkBits` (span+0x48), which is
0 whenever the collector is not mid-mark — so it structurally could not catch a handout and reported 0 hits
for the wrong reason. This distinction is the whole reason I re-ran the experiment.

---

## 4. The instrumentation (exact files, functions, gates)

All changes are **env-gated and INERT by default** (verified: with no `DD*` env the engine behaves and
corrupts identically, ~40%). Base: the existing diagnostic tooling patch
`/Users/x/dd/dd/target/heap201/heap201_tooling_latest.patch` (applied with `patch -p1 --fuzz=3` because it
is stale vs current `origin/main`; only the mem.c mmap-redirect hunk needed fuzz), then my additions.

### 4.1 Determinism — DDFIXHEAP (from the tooling patch)
- `dd-jit/src/runtime/os/linux/syscall/mem.c` — `ddfixheap_next()` + the `svc_mem` mmap redirect: a pure
  NULL-hint anonymous map is placed `MAP_FIXED` at a monotonically-increasing 5 TB base. Note Go's own
  arenas are `MAP_FIXED` at `0xc0..` and are **not** redirected, which is why the victim slots show up at
  `0xc0000120xx`, not in the 5 TB region.
- `dd-jit/src/runtime/translate/x86_64/elf.c` — `load_elf`: if `DDFIXHEAP` and no `g_force_base`, pin the
  image base to `0x0000000200000000` (⇒ non-PIE bias `0x200000000`, so runtime PC = link + 0x200000000).
  `build_stack`: pin AT_RANDOM to `0x5a`×16 so the map/string hash seed is constant.
- Effect: **WHERE** is deterministic (corruption always starts `firsti=1014`, `fails=410/4000`). **WHETHER**
  a run corrupts is still ~40% (GC-interleave/scheduling nondeterminism the fixed layout does not remove).

### 4.2 Per-alloc probe — my additions to `translate/x86_64/elf.c`

`struct slotinfo` + `static struct slotinfo alloc_probe(uint64_t addr)` (added just after
`watch_slot_marked`): walks Go's arena table exactly like the existing `watch_slot_marked`, then reads the
full allocator state:

```
arena L1  = *(uint64_t*)DDARENAS               // biased &mheap_.arenas (env DDARENAS)
ai        = (addr + 0x800000000000) >> 26      // arenaIndex; reject if >= 0x400000
heapArena = *(uint64_t*)(L1 + ai*8)
span      = *(uint64_t*)(heapArena + ((addr>>13)&0x1fff)*8)   // pageIndex = (addr>>13)&0x1fff
base=*(span+0x18); elem=*(span+0x68); gmb=*(span+0x48); abits=*(span+0x40)
objIndex  = (addr - base) / elem
allocBit  = (abits_word >> (objIndex&63)) & 1  // abits_word = *(abits + (objIndex/64)*8)   <-- THE CHECK
markBit   = (gmb_byte  >> (objIndex&7 )) & 1   // transient, informational only
cacheBit  = (objIndex>=freeindex && objIndex-freeindex<64) ? (allocCache>>(objIndex-freeindex))&1 : -1
```

It fills `span/base/elem/freeindex/nelems/allocCache/allocCount/gmb_word/abits_word/objIndex/allocBit/
markBit/cacheBit/valid`. Guarded so any bad pointer just sets `valid=0` (returns cleanly, never faults the
handler).

### 4.3 Two brk probe sites — `translate.c` + `engine_glue.c` + the `jit86_syncguard` handler

- `translate/x86_64/translate.c` `translate_block`: alongside the existing
  `if (gpc==DDUMP) emit brk #0x5202`, I added
  `if (gpc==dump_pc2_val()) emit brk #0x5203`. `dump_pc2_val()` reads env `DDUMP3` (added to
  `engine_glue.c` next to `dump_pc_val()`/`DDUMP`).
- The `SIGTRAP` handler `jit86_syncguard` (elf.c) now branches on the brk immediate:
  - **`brk #0x5202`** = REFILL probe, planted at `DDUMP` = block-zero `0x20040cff3`. The returned slot is
    `r11 = X[11]`, size 16.
  - **`brk #0x5203`** = BUMP probe, planted at `DDUMP3` = tiny-bump return `0x20040ced6`. The brk fires
    *before* the `lea`, so I recompute the returned object as `obj = r12 + r10 = X[12] + X[10] =
    c.tiny + roundedOffset`, size `= rbx = X[3]`.
  - For each fire it calls `alloc_probe(obj)` and `alloc_probe(obj+size-1)`; flags `liveStart` if the start
    slot's `allocBit==1`, `liveEnd` if the end slot is a *different* objIndex with `allocBit==1` (overflow
    into a live neighbor). On any hit it dumps a `[LIVE-HANDOUT#n ...]` line: the path (REFILL/BUMP), obj,
    size, span/base/elem/objIndex, freeindex/nelems/allocCount/allocCache/allocBitsWord, allocBit/markBit/
    cacheBit, and for the BUMP path `c.tiny`, `rawTinyoffset` (`*(c+0x18)`), `roundedOffset(r10)`,
    `size(rbx)`, the computed address, block-end `c.tiny+16`, and an `overflow` flag `r10+rbx>16`, plus the
    dd scratch regs `x16/x17/x20/x22/x23` and `r10/r11/r12/r13`.
  - Counters (`fire_r`, `fire_b`, `nvalid`, `nallocset`, `nmarkset`, `nover`, `ngcactive`, `hits`) are
    printed periodically (`[PROBE-STAT ...]`) and — critically — the `LIVE-HANDOUT` allocBit check runs on
    **every** fire regardless of the periodic gate, so coverage is total even though the informational
    `PROBE-STAT` is sampled.
  - After the check, `pc += 4` to skip the brk and resume the real instruction. Non-perturbing: the earlier
    round already established a brk at the block-zero site does not slow GC enough to suppress the bug.
  - GC phase read from env `DDGCPHASE` (`*(uint32_t*)0x2005933a8`, biased `&runtime.gcphase`; 0=off, 1=mark,
    2=marktermination). **This read returned 0 at every probe — see caveats.**

### 4.4 Exact env values (mcount)

```
DDFIXHEAP=1
DDUMP=20040cff3        # block-zero movups (refill probe, brk 0x5202)   link 0x40cff3 + 0x200000000
DDUMP3=20040ced6       # tiny-bump return  (bump probe,   brk 0x5203)   link 0x40ced6 + 0x200000000
DDARENAS=20054c7b8     # biased &mheap_.arenas L1 ptr (link 0x54c7b8 = runtime.mheap_+0x101d8)
DDGCPHASE=2005933a8    # biased &runtime.gcphase (link 0x5933a8)   [returned 0 — inconclusive]
GOMAXPROCS=1
```

---

## 5. Build & run (standalone, does not touch canonical)

Direct unity-TU compile via the mac bridge (mirrors `dd-jit/build.rs`: `clang -O2 -o <bin> <tu>` then
codesign with `dd-jit/jit.entitlements`). `mac bash -lc` does NOT forward ambient env — every `DD*` var must
be passed in the `exec env` prefix.

```bash
WT=/Users/x/dd/dd/.claude/worktrees/agent-aad774950ecdddc2d          # my worktree
SCR=/Users/x/dd/dd/target/heap201angleA_aad                          # my scratch (mac-visible, under target/)
mac bash -lc "clang -O2 -o $SCR/ddjit-x86i \
   $WT/dd-jit/src/runtime/targets/linux_x86_64.c && \
   codesign -s - --entitlements $WT/dd-jit/jit.entitlements -f $SCR/ddjit-x86i"
# (the aarch64/darwin targets are irrelevant here; only linux_x86_64 is needed)

# One run:
timeout 22 mac bash -lc "exec env DDFIXHEAP=1 DDUMP=20040cff3 DDUMP3=20040ced6 \
   DDGCPHASE=2005933a8 DDARENAS=20054c7b8 GOMAXPROCS=1 \
   $SCR/ddjit-x86i /Users/x/dd/dd/target/heap201/mcount" 2>&1

# REAP (mac-side JIT is not killed by timeout); reap by MY unique slug only, never by binary name:
mac bash -lc "pkill -f heap201angleA_aad"
```

`mcount` prints `CORRUPT pass=N fails=410/4000 firsti=1014` on a corrupting run, else `DONE`. A build
warning `ATFD macro redefined` is benign; `grep -iE 'error:'` must be empty (a C compile failure otherwise
ships a stale engine — the classic trap).

---

## 6. RESULTS — the actual numbers

Batch of 10–20 runs each, `DDFIXHEAP` deterministic layout, both probes active. Representative
(`fire_r`=refill fires, `fire_b`=bump fires; `valid`=probes whose arena walk resolved; all cumulative
end-of-run):

```
r7  [CORRUPT pass=613] LH=0 | fire_r=40801  fire_b=39199  valid=80000   allocBitSet=0 markBitSet=0 overflowLive=0
r9  [CORRUPT pass=614] LH=0 | fire_r=40801  fire_b=39199  valid=80000   allocBitSet=0 markBitSet=0 overflowLive=0
r3  [CORRUPT pass=611] LH=0 | fire_r=40801  fire_b=39199  valid=80000   allocBitSet=0 markBitSet=0 overflowLive=0
r1  [DONE]             LH=0 | fire_r=130801 fire_b=129199 valid=260000  allocBitSet=0 markBitSet=0 overflowLive=0
...
```

- **Corrupting runs: ~80,000 tiny allocations probed** (40,801 refill + 39,199 bump). The run exits early
  (return at first corrupt pass, ~pass 601–620), so a corrupt run has fewer fires than a DONE run
  (~260,000). `valid == fire_r + fire_b` exactly ⇒ **100% arena-walk coverage**, every allocation checked.
- `allocBitSet = 0` — no returned block's slot was allocated/live as of the last sweep (START slot).
- `overflowLive = 0` — the tiny-bump `c.tiny + round(tinyoffset, align)` never overflowed the 16B block
  into a live neighbor slot (both `obj` and `obj+size-1` probed).
- `markBitSet = 0` — no marked slot handed out either (informational, transient bitmap).
- `LIVE-HANDOUT = 0` — the unconditional per-fire allocBit check never tripped, on any run.
- **Non-perturbing:** with probes active the corrupt rate stays ~40% and `firsti=1014`/`fails=410` are
  preserved (identical to the uninstrumented engine). The block-zero brk does not suppress the bug.
- **Env-gated:** the same instrumented binary with no `DD*` env corrupts ~2/10 with zero probe output.

### First-fires sanity (build phase), confirming the probe reads real state
```
[PROBE-STAT fire_r=1 fire_b=0] valid=1 ... this=REFILL obj=c000012000 size=16 objIdx=0 freeidx=1 allocBit=0
[PROBE-STAT fire_r=2 fire_b=0] valid=2 ... this=REFILL obj=c000012010 size=16 objIdx=1 freeidx=2 allocBit=0
[PROBE-STAT fire_r=3 fire_b=0] valid=3 ... this=REFILL obj=c000012020 size=16 objIdx=2 freeidx=3 allocBit=0
[PROBE-STAT fire_r=3 fire_b=1] valid=4 ... this=BUMP   obj=c000012008 size=6  objIdx=0 freeidx=3 allocBit=0
```
i.e. slot base `0xc0000120xx`, sequential objIndex, bump within slot 0 — the arena walk and offsets are
correct and the returned addresses are the real Go tiny arena.

---

## 7. What this PROVES

1. **Hypothesis #1 (allocator reuse / wrong block address) is REFUTED.** Both the inlined nextFreeFast
   (allocCache/`bsf`/freeindex/`imul`/`add` index math) and the tiny-bump (`c.tiny + round(tinyoffset,
   align)` with its AND/ADD alignment rounding and the `cmp $0x10` fit guard) return a genuinely FREE,
   non-overlapping slot on **every one of ~80,000 allocations of a corrupting run.** Checked against the
   correct persistent bitmap `allocBits`, so it is not a timing artifact.
2. This closes the **one gap** the bit-scan/single-instruction audits did not exercise in context: the
   tiny-bump alignment rounding is also correct (0 overflow-into-live across 39,199 bump ops/corrupt run).
3. **There is no "first wrong block" to pin.** The allocation stream is correct, so Angle A's
   divergence-search has nothing to diverge. The block-zero `movups %xmm15,(%r11)` at `0x40cff3` is **not**
   the culprit: its destination `r11` is a verified-free slot every single time.
4. Therefore the corrupting 16-byte zero-store's wrong destination comes from **outside** the allocator
   handout ⇒ **hypothesis #2** (a direct wrong-EA `memclr`/SIMD-zero into live data). The culprit `str q`/
   memclr is at a **different, uninstrumented guest PC** whose EA is miscomputed for the corrupt layout —
   consistent with the doc's round-2 conclusion and the "individually-correct instruction, context/ordering
   interaction" read from the bit-scan angle. This matches msweep7's ground truth (victim stays marked,
   never freed, bytes zeroed in place) far better than an allocator-reuse story.

---

## 8. Caveats / honest limits

- **gcphase read is inconclusive.** `DDGCPHASE=2005933a8` (`*(uint32_t*)&runtime.gcphase`) returned 0 at
  **every** probe (`gcActiveFires=0` across 260,000 fires). Either the address/offset is off, or no probed
  tiny alloc coincided with a nonzero phase. I do **not** claim "allocs avoid the mark window"; I only claim
  the allocBit rule-out, which does not depend on gcphase (the LIVE-HANDOUT check runs on every fire). A
  follow-up should re-derive `&runtime.gcphase` (RIP-relative target of `cmpl $0x2,...(%rip)` at
  `0x40cd12`) and sanity-read a known-changing global before trusting it.
- The probe verifies the **returned r11/obj value in the guest CPU** (X[11], X[12]+X[10]). It does **not**
  verify that dd's emitted ARM `str q15,[x17]` actually used `x17 == r11` — the brk fires *before* the EA
  is materialized, so I could not compare `x17` to `r11` at the store. If the miscompile is a scratch-reg
  (x16/x17) clobber between EA-compute and store, the guest r11 would read correct (allocBit=0, as observed)
  while the hardware wrote elsewhere. The doc claims `x17==r11` was verified previously, but re-verifying it
  *on a corrupting run at the store itself* is the natural next probe (see handoff).
- `DDFIXHEAP` fixes WHERE but not WHETHER (~40% corrupt), so a corrupting run must be caught by re-running,
  not forced. The determinism of `firsti=1014`/`fails=410` makes the caught run a stable target.

---

## 9. Handoff / recommended next probes (for the hardware-watchpoint agent)

The corrupting 16-byte zero-store is a wrong-EA memclr/SIMD-zero, at a PC I did not instrument. To pin it:

1. **Verify x17 vs r11 at the block-zero store on a corrupting run.** Move the check to fire *after* the EA
   is formed (or read the emitted store's base reg), and compare dd's `x17` to guest `r11`. If they differ,
   the miscompile is the block-zero EA lowering after all (a scratch clobber), not a foreign store.
2. **Emitter-level catch of every zero-store.** In dd's lowering of `str q`-of-zero (`movups %xmm(0)`) and
   `runtime.memclrNoHeapPointers` (@`0x463440`), emit a cheap inline check that traps only when the dest EA
   has `allocBits` set — and arm it only when the EA is in Go's arena (`0xc0..`). The trapping store's guest
   PC + operands + `x17`-vs-intended is the answer. NB: `memclr@0x463440` is still UNRESOLVED because the
   prior round's *range-sampling* of it perturbed the bug to 0%; a **start-slot-only** single arena-walk per
   memclr call is the non-perturbing form to try.
3. Hardware watchpoints: since the layout is deterministic, capture the victim data address (`keys[1014].ptr`
   — header stays valid per msweep7) on a corrupting run, then set a HW watchpoint on those bytes and let it
   fault at the real store, yielding the guest PC directly (avoids the mprotect-perturbs-the-victim problem
   that defeated page-protection). This is the most promising path and why Angle A's negative matters: it
   guarantees the watchpoint hit will be a foreign store, not an allocator handout.

Candidate set for the culprit store: a 16-byte SIMD zero (`str q` of a zeroed V / `movups %xmm(0)`) or
`memclrNoHeapPointers` whose destination EA is miscomputed for the corrupt heap-pointer bit pattern, landing
on a live tiny slot — with the wrong EA arising from an instruction that is individually correct in
isolation (⇒ a context/ordering interaction: scratch-reg reuse across an EA-fold or translation-block
boundary, or a stale value consumed).

---

## 10. Artifacts (all under `target/`, gitignored, mac-visible)

- **Instrumented engine:** `/Users/x/dd/dd/target/heap201angleA_aad/ddjit-x86i`
- **Run logs:** `/Users/x/dd/dd/target/heap201angleA_aad/runs{2,3,5}/r_*.txt` (per-run stderr with
  `PROBE-STAT` lines and results). `runs4/` is a KNOWN-BAD batch — I momentarily added `gcActiveFires=%ld`
  to the format string without adding the matching vararg, which shifted all `printf` fields to garbage
  (`gcphase=36608459`, `valid=0`); fixed in `runs5/`. Ignore `runs4/`.
- **Disassembly:** `/Users/x/dd/dd/target/heap201angleA_aad/mcount.dis` (from `x86_64-linux-gnu-objdump`).
- **Repro + oracle harness:** `/Users/x/dd/dd/target/heap201/{mcount,verify.sh,src_mcount.go}` (shared).
- **Source changes (uncommitted, diagnostic only, env-gated):** in worktree
  `/Users/x/dd/dd/.claude/worktrees/agent-aad774950ecdddc2d/`:
  - `dd-jit/src/runtime/os/linux/syscall/mem.c` — DDFIXHEAP mmap redirect (from tooling patch).
  - `dd-jit/src/runtime/translate/x86_64/elf.c` — DDFIXHEAP image/AT_RANDOM pin, `struct slotinfo` +
    `alloc_probe`, dual-brk (`0x5202`/`0x5203`) per-alloc handler in `jit86_syncguard`, tally counters.
  - `dd-jit/src/runtime/translate/x86_64/engine_glue.c` — `dump_pc2_val()` for `DDUMP3`.
  - `dd-jit/src/runtime/translate/x86_64/translate.c` — plant `brk #0x5203` at `DDUMP3`.
  These are diagnostic scaffolding, not a fix; there is no allocator fix to make because the allocator is
  correct.

## 11. Dead-ends / obstacles encountered

- OrbStack `/usr/bin/objdump` cannot disassemble x86-64 ("architecture UNKNOWN") — use
  `x86_64-linux-gnu-objdump`; `objdump -d mcount` alone yields only the ELF header line.
- The tooling patch is stale vs `origin/main` (`git apply` fails at mem.c:276); `patch -p1 --fuzz=3` applies
  it (only the mmap-redirect hunk fuzzes). `patch` leaves a `mem.c.orig` backup — delete it.
- `atexit(final_tally)` never runs — the guest's exit path bypasses libc atexit in the engine, so a final
  cumulative line cannot be emitted that way; I switched to a periodic `PROBE-STAT` at a low interval and
  relied on the unconditional per-fire LIVE-HANDOUT check for total coverage instead.
- The periodic `PROBE-STAT` interval must be low enough (I used 20,000) that a corrupt run — which exits at
  ~80,000 fires — prints a late cumulative line; the initial 300,000 interval only ever showed `fire<=4`
  and hid the totals.
- The vararg/format-string mismatch described in §10 (`runs4/`) — a reminder to keep printf args in lockstep.
