# BUG #201 — GC mark/sweep span-metadata probe (investigation account)

Author: the "GC mark/sweep span-metadata" agent (worktree `agent-ad2b475d941d18e38`).
Scope: x86_64 guest → ARM64 macOS, dd JIT. Canonical repro `target/heap201/mcount` under `DDFIXHEAP=1`
(deterministic: `firsti=1014`, `fails=410/4000`, ~35% of runs corrupt).

This document is the EXHAUSTIVE record of the mark/sweep-metadata angle: every probe, the raw data, the
verdict, the full ruled-out list, the prime suspect, and the reusable machinery (especially
`resolve_guest_block` host-PC→guest-block reverse lookup) for the hardware-watchpoint agent.

--------------------------------------------------------------------------------------------------------
## 0. TL;DR VERDICT
--------------------------------------------------------------------------------------------------------
**NEITHER mark-miss NOR sweep.** The victim tiny span (base `0xc000102000`, 512×16B slots, 410 live
noscan 13-byte string backings for `keys[1014..]`) has `gcmarkBits_pop = 410` AND `allocBits_pop = 410`
**stable across ALL 7 forced GCs, including the corrupting one**, and `zeroed_marked_live = 0` at every
sweep (all 410 live slots hold intact bytes at every sweep). The span is never unmarked, never loses
allocBits, never freed. The GC metadata is healthy.

At the moment corruption is reported, the **same span struct** (`0x5000000b7c8`, i.e. NOT freed+reused)
still shows `allocBits_pop = 410`, yet **all 512 slots read zero**. So the live (allocBits-set) slots are
overwritten with zeros while the span stays allocated: a **mutator-phase, wrong-destination 16-byte SIMD
zero-store that bulk-zeroes the live span**, during pass ~619's `make([]*int,0,128)` + 128× `&x` churn
(no GC runs between the last clean check at pass 618 and the failing check at pass 619).

This **REFUTES** BUG201.md's prior "SUPERSEDING CONCLUSION" (that the span loses allocBits → is freed →
reused). allocBits is never lost; the span is never freed.

--------------------------------------------------------------------------------------------------------
## 1. Repro + methodology invariants
--------------------------------------------------------------------------------------------------------
- Repro: `/Users/x/dd/dd/target/heap201/mcount` (linux/amd64 Go 1.22.12, engine-independent). Source
  `src_mcount.go`: builds `map[string]int` of 4000 `"key-<i>-tail"` strings, then loops 2000 passes;
  each pass allocates `junk:=make([]*int,0,128)` + 128× `&x`, checks all 4000 keys, and `runtime.GC()`
  every 100 passes. Prints `CORRUPT pass=.. fails=N/4000 firsti=..` then RETURNS. **mcount has NO
  VICTIM-dump loop** (that is `mvic3`); the victim address must be confirmed engine-side.
- The nondeterminism is TIMING (whether the triggering GC interleaves wrong), not WHERE: firsti is always
  1014 under DDFIXHEAP. A corrupt run is a reproducible target.
- Guest env does NOT inherit host env; only `DD_GUEST_ENV` + fixed PATH/HOME/TERM/LANG reach the guest
  (`translate/x86_64/elf.c` build_stack). All my probes are HOST-side `getenv` in the engine, so they are
  passed directly in the `exec env ...` prefix and are visible.
- Runtime addr = link addr + `0x200000000` under DDFIXHEAP (non-PIE bias). e.g. sweep link `0x422e0d` →
  brk PC `0x200422e0d`. **`DDUMP` must be passed with a `0x` prefix** — the engine parses it with
  `strtoull(.,.,16)` but the value has hex letters; without `0x` a base-0 parse truncates at the first
  non-decimal digit. (Cost me the first run: `DDUMP=200422e0d` parsed as decimal `200422`.)
- Layout note: the mspan STRUCT address differs per engine build (e.g. `0x5000000b7c8` in this build);
  only the data base `0xc000102000` is stable. A DONE-run once showed an 80-byte span at that base — a
  red herring; on corrupt runs it is the 16B tiny span with 410 live.

mspan offsets (Go 1.22): base `+0x18`, freeindex `+0x30`(u16), nelems `+0x32`(u16), allocCache `+0x38`,
allocBits `+0x40`(ptr), gcmarkBits `+0x48`(ptr), allocCount `+0x60`(u16), spanclass `+0x67`, elemsize
`+0x68`. allocBits/gcmarkBits point into the `0x5000..` gcBits region (readable from the handler).
Arenas L1 for the walk: mcount link `0x54c7b8` → biased `0x20054c7b8` (env `DDARENAS`).

--------------------------------------------------------------------------------------------------------
## 2. Instrumentation (all env-gated, INERT by default; x86 target only)
--------------------------------------------------------------------------------------------------------
Built on top of the existing `heap201_tooling_latest.patch` (DDFIXHEAP + DDUMP brk + DDWATCH). All my
additions live in the worktree files listed in §7. Every probe is driven by a `brk #0x5202` planted at a
chosen guest PC (`DDUMP=<hexPC>`, translate.c) whose SIGTRAP is handled in `jit86_syncguard` (elf.c), or
by a syscall-boundary check in `mem.c`/`io.c`. Host regs are read from the ARM ucontext:
`X[0..15]` = rax,rcx,rdx,rbx,rsp,rbp,rsi,rdi,r8..r15; `X[16]`/`X[17]` = the emitter's scratch/EA regs.

### 2.1 DDSPAN span-watch (the DECISIVE probe) — `jit86_syncguard`, brk at sweep entry
- Plant `DDUMP=0x200422e0d` = `runtime.(*sweepLocked).sweep` at guest link `0x422e0d`, the point right
  AFTER `MOVQ 0(AX),DX` (0x422dff) loads the `*mspan` being swept into **rdx = X[2]**. (Receiver
  `*sweepLocked` is in AX; `sl.mspan` = `*(AX)` = DX.) Read BEFORE the allocBits/gcmarkBits swap that
  happens later in the function, so `gcmarkBits` = what THIS cycle's mark produced and `allocBits` =
  pre-sweep (== last cycle's marks).
- Handler: `span = X[2]`; `base = *(span+0x18)`. Match victim (`DDSPAN`, default 0xc000102000) exactly, or
  auto-detect any 16B span with `allocCount>=300`. For a match, popcount the 8 words (`nelems=512`) of
  gcmarkBits(`*(span+0x48)`) and allocBits(`*(span+0x40)`), report `freeidx/allocCount/firstclr`, and the
  8 raw words of each bitmap.
- `zeroed_marked` extension: for each MARKED slot, read its 16 payload bytes at `base+idx*16`; count
  marked-but-all-zero slots. This is what proved the live slots are INTACT at every sweep.
- Non-perturbing: fires once per span per GC (~a few thousand brks/run total, only fprintf on the victim
  ~7×/run). Corruption still reproduces with it active.
- It also ARMS the RO-watch (§2.6) and stashes `g_vic_span`/`g_vic_base` on first sighting.

### 2.2 DDBZ / DDBZ2 — tiny block-zero EA probe (`str q15,[x17]` @0x40cff3)
The tiny block-zero is `MOVUPS X15,(R11)` at mallocgc link `0x40cff3` (block starts `0x40cfc8`, right
after `CALL runtime.(*mcache).nextFree`; `r11 = AX =` the slot nextFree returned). It lowers to
`str q15,[x17]` (ARM `0x3d80022f`), x15 = Go's zero vector.
- **DDBZ** (`DDUMP=0x20040cff3`): reads guest **r11 = X[11]** (the intended dest). If in the victim span,
  arena-walks (`watch_slot_bits`, §2.7) to read markBit+allocBit for that slot. Reports only when the slot
  is NON-free. Result: **0 hits** across many corrupt runs — block-zero's guest intent is always a free
  slot.
- **DDBZ2** (`DDUMP=0x20040cff7`, the instruction AFTER the store; host **x17 = X[17]** still holds the
  ACTUAL store EA there): flags when `X[17] != X[11]` and `X[17]` is in the victim span. Result: **0
  hits** — the block-zero's real ARM EA equals guest r11 and never lands on the victim. This EXONERATES
  0x40cff3 on BOTH the guest register and dd's lowered EA, overturning the earlier `str q15` attribution.
- Non-perturbing: the block-zero brk fires ~80k×/run and is known not to suppress the bug.

### 2.3 DDMCLR — memclrNoHeapPointers cover probe (`jit86_syncguard`)
`DDUMP=0x200463440` = `runtime.memclrNoHeapPointers` entry (AX=X0=dst, BX=X3=n; caller ret = `*(rsp=X4)`).
Reports only calls whose `[dst,dst+n)` covers the victim base, with the caller link addr. Result: **0
hits** on firsti=1014 corrupt runs — memclr never zeroes a range covering the victim.

### 2.4 DDMADV / DDMSYS — memory-syscall cover probes (`mem.c`, syscall boundary, fully non-perturbing)
Syscall numbers in `svc_mem` are ARM64-normalized: mmap=222, munmap=215, mremap=216, mprotect=226,
madvise=233; brk=214.
- **DDMADV**: logs any `madvise` adv∈{4 DONTNEED, 8 FREE, 9 REMOVE} whose `[a0,a0+a1)` covers the victim.
  Result: **0 hits**. (dd's DONTNEED handler MAP_FIXED-remaps → zeroes; if Go scavenged the live pages
  this would fire. It never does.)
- **DDMSYS**: logs any of {222,215,216,226,233} covering the victim, PLUS reads the victim's slot0 to show
  whether it is currently populated. Result: on both DONE and CORRUPT runs, exactly **2 hits — both at
  startup** (arena reserve 64MB PROT_NONE flags=0x22 + the initial 4MB RW MAP_FIXED commit flags=0x32 at
  `c000000000`, guest_rip link `0x464604`), with `victim_slot0_now=''` (empty = before the strings exist).
  No mid-run memory syscall ever touches the victim.

### 2.5 DDVICDUMP — victim state at the CORRUPT print (`io.c`, write syscall, non-perturbing)
Hook the `write(fd, buf, len)` path (case 64): when `buf` starts with `"CORRUPT"` and fd∈{1,2}, scan the
victim span `[base, base+512*16)` for all-zero 16B slots, AND arena-walk (using `DDARENAS`) to report the
CURRENT span struct backing `base`, its gcmarkBits/allocBits popcounts, and `marked_but_zero`. This is
what proved: same span struct (not freed), allocBits still 410, gcmark 0 (normal between GCs), all 512
zero.

### 2.6 DDARM — RO-watch of the victim span + `resolve_guest_block` (PERTURBS; see §5)
On first span-watch sighting of the victim, call `watch_add_range(base, 0x2000)` (mprotects the 2 data
pages READ-ONLY; the span is FULL so no legit tiny-alloc writes into it during churn — only the culprit
should fault). The SIGSEGV lands in `jit86_lazyguard`, whose DDARM path:
- computes `objIdx=(va-base)/16`, reads mark/alloc bits from `g_vic_span` directly (no arena walk → fast);
- for the FIRST store into a MARKED-live slot, prints the guest block, ARM insn, EA (x17), guest r11, and
  all regs, then emulates the store and continues (`watch_emulate_store`);
- `ARMSKIP=N` late-arms (skip N sightings ≈ N forced GCs) to cover only the passes just before corruption
  and cut the number of legit free-slot faults.

**`resolve_guest_block(hpc, *off)`** (host-PC → guest-block reverse lookup — REUSE THIS): scans the shared
code-cache map `g_map[]` (defined in `engine/cache.c`, in scope in `elf.c` via the unity TU order:
cache.c is `#include`d before elf.c). Each live entry is `{uint64_t gpc; void *host; void *body;}`
(host = block entry pointer). The function returns the `gpc` of the entry whose `host` is the greatest
`<= hpc` (the block currently executing) and sets `*off = hpc - host`. Subtract `0x200000000` to get the
guest LINK address. Implementation:
```c
static uint64_t resolve_guest_block(uint64_t hpc, uint64_t *off) {
    uint64_t best_host = 0, best_gpc = 0;
    for (int i = 0; i < JIT_MAP_N; i++) {
        if (!g_map[i].host) continue;
        uint64_t h = (uint64_t)g_map[i].host;
        if (h <= hpc && h > best_host) { best_host = h; best_gpc = g_map[i].gpc; }
    }
    if (off) *off = best_host ? hpc - best_host : 0;
    return best_gpc;
}
```
The HW-watchpoint agent should copy this verbatim to translate a trapped store's host PC back to a guest
block, then `go tool objdump` that block to find the exact x86 instruction. (`JIT_MAP_N`, `g_map`,
`map_idx` all live in `engine/cache.c`.)

Caveat: `g_map.host` is the block ENTRY, not per-instruction; the trapped store is at `gpc + off` host
bytes into the block, so disassemble forward from the block's guest start to find the store.

### 2.7 watch_slot_bits / watch_slot_marked — arena walk (elf.c)
Given a guest heap addr, walk Go's arena table: `arenaIdx=(a+0x800000000000)>>26`;
`L1=*(DDARENAS)`; `heapArena=*(L1+arenaIdx*8)`; `pageIdx=(a>>13)&0x1fff`; `span=*(heapArena+pageIdx*8)`;
then `oi=(a-base)/elem` and read the gcmarkBits(`+0x48`) and allocBits(`+0x40`) bits.
`watch_slot_bits` returns `(markBit<<1)|allocBit` or -1. `watch_slot_marked` returns just the mark bit.

### 2.8 DDRAC — refillAllocCache correctness probe (`jit86_syncguard`)
`DDUMP=0x200412754` = `runtime.(*mspan).refillAllocCache` RET. `AX=X0=span`, and crucially **`rbx=X3` still
holds the `whichByte` arg** (the function reads it into DX at 0x412746 and never rewrites BX). For the
victim span, compute `expected = ^(*(uint64_t*)(allocBits + whichByte))` (an UNALIGNED 8-byte read at
byte offset `whichByte`) and compare to the freshly-stored `allocCache` (`*(span+0x38)`). Result: **0
mismatches** even on corrupt runs → `allocCache == ^allocBits` is computed correctly.
  - PROBE BUG WARNING: my FIRST version derived the word index from the post-call `freeindex` (`freeidx/64`)
    instead of from `whichByte=rbx`. Because `refillAllocCache` does NOT update freeindex (nextFreeIndex
    does, later) and `whichByte` is based on the (sfreeindex+64)&^63 aligned value, that read the WRONG
    allocBits word and produced a **false "42 MISCOMPILE" on the corrupt run**. Reading at `rbx` byte
    offset gave 0 mismatches. Do not repeat this: `whichByte` is the arg in rbx, not `freeindex/64`.

--------------------------------------------------------------------------------------------------------
## 3. THE DATA
--------------------------------------------------------------------------------------------------------
### 3.1 Victim span metadata per forced GC (corrupt run, firsti=1014), from the sweep hook
Forced GCs run at pass 0,100,200,...,600 (7 total). Each GC's sweep of the victim (base=0xc000102000,
struct=0x5000000b7c8, nelems=512, elem=16, freeidx=512, allocCount=512):

| sweep (GC pass) | gcmarkBits_pop | allocBits_pop | firstclr(gcmark) | zeroed_marked_live |
|-----------------|----------------|---------------|------------------|--------------------|
| #0  (pass 0)    | 410            | 0  (pre-first-swap) | 3          | 0                  |
| #1  (pass 100)  | 410            | 410           | 3                | 0                  |
| #2  (pass 200)  | 410            | 410           | 3                | 0                  |
| #3  (pass 300)  | 410            | 410           | 3                | 0                  |
| #4  (pass 400)  | 410            | 410           | 3                | 0                  |
| #5  (pass 500)  | 410            | 410           | 3                | 0                  |
| #6  (pass 600)  | 410            | 410           | 3                | 0                  |  ← the corrupting GC

(Earlier fuller capture keyed the per-alloc sweep index #113 to the pass-600 GC's victim sweep; identical
numbers. The gcmarkBits words are the fixed pattern `7bdef7bdef7bdef7 f7bdef7bdef7bdef ...`, i.e. 410 of
512 set; allocBits after the first swap mirror it. `allocBits_pop=0` at #0 is normal: the span had never
been swept before, so its allocBits was still 0 until this first sweep's swap.)

INTERPRETATION: the 410 live string backings are correctly greyed/marked every cycle (gcmark=410), the
sweep correctly carries them into allocBits (410), the span is never freed (allocCount stays 512, struct
address constant), and every marked slot holds intact string bytes at every sweep (zeroed_marked=0).

### 3.2 Victim at the CORRUPT print (DDVICDUMP, same corrupt run)
```
[VICDUMP] base=c000102000 zero16=512 nonzero16=0 span=5000000b7c8 gcmark_pop=0 allocBits_pop=410 marked_but_zero=0 slot0=''
```
- `span=5000000b7c8` == the struct seen at every sweep → NOT freed+reused.
- `allocBits_pop=410` → still allocated (unchanged).
- `gcmark_pop=0` → normal: no GC is running at pass ~619, marks are transient/cleared between GCs.
- `zero16=512 nonzero16=0` → the ENTIRE 8KB span (all 512 slots, incl. the 410 allocBits-set live ones)
  reads zero. `slot0=''` (the `keys[1014]` backing "key-1014-tail" is gone).
- `marked_but_zero=0` only because gcmark_pop is 0 at this instant (no marked slots to compare).

Timing: mcount checks all keys EVERY pass and returns on the first failure. Observed `CORRUPT pass=` in the
600s (601/611/617/619/622/625 across runs), always AFTER the pass-600 GC, with fails jumping 0→410 in a
single pass. So the whole-span zeroing happens during ONE pass's mutator work
(`make([]*int,0,128)` + 128× `&x`), with no GC in between.

--------------------------------------------------------------------------------------------------------
## 4. VERDICT + RULED-OUT LIST
--------------------------------------------------------------------------------------------------------
VERDICT: **Not a GC mark-miss and not a sweep bug.** The span's gcmarkBits and allocBits are correct and
complete at every sweep including the corrupting GC; the span is never freed. The corruption is a
**mutator-phase, wrong-DESTINATION 16-byte SIMD zero-store (`str q15`) that bulk-zeroes the live victim
span** (~8KB, all 512 slots) during pass ~619's `make([]*int,0,128)` churn, while the GC metadata stays
healthy. This overturns BUG201.md's "span loses allocBits → freed → reused" superseding conclusion.

RULED OUT (all non-perturbing, verified on firsti=1014 corrupt runs unless noted):
- **Mark-miss**: gcmarkBits_pop=410 every GC; the 410 live slots are always marked.
- **Sweep bug**: allocBits_pop=410 every post-swap; span never freed; struct address constant.
- **Tiny block-zero `str q15,[x17]` @0x40cff3**: DDBZ (guest r11) AND DDBZ2 (actual ARM EA x17) both 0
  hits into a non-free victim slot; x17==r11 always. Fully innocent.
- **memclrNoHeapPointers @0x463440**: 0 calls cover the victim (DDMCLR).
- **madvise DONTNEED/FREE/REMOVE**: 0 cover the victim (DDMADV). (Not the scavenger.)
- **mmap/munmap/mremap/mprotect (222/215/216/226)**: only the 2 STARTUP arena maps cover the victim, both
  while it is empty (DDMSYS). No mid-run mapping change; not a page-remap-to-zero.
- **refillAllocCache**: `allocCache == ^allocBits[whichByte]` with 0 mismatches (DDRAC). The allocator
  bitmap math is correct. (The transient "42 mismatch" was a probe bug: wrong word index; see §2.8.)
- **duffzero @0x462f20 (full-entry)**: 0 destination hits in the victim on the runs sampled (DDDST, rdi).
  Note duffzero maxes at 1024 bytes anyway — too small for the ~8KB whole-span zero, and it is entered at
  variable offsets so a brk at its top misses partial entries.

--------------------------------------------------------------------------------------------------------
## 5. THE OBSTACLE (why the exact instruction is not yet pinned)
--------------------------------------------------------------------------------------------------------
The culprit is a `str q15` (16-byte zero) store at an UNIDENTIFIED guest PC (NOT the tiny block-zero) that
writes the victim's live slots. Catching it requires observing a store TO those slots. The only method
that catches an arbitrary store into a region — **RO page-protection (DDARM)** — PERTURBS: protecting the
victim's 2 pages either (a) faults on every legit free-slot reuse over the run and the emulate-and-continue
is far too slow (times out), or (b) shifts the manifestation so corruption drops to ~0%. Over a 40-run
batch: 5 DONE + 6 timeouts + 0 catches. The single store I did catch under DDARM was the legit tiny
block-zero to a FREE slot (objIdx 3, allocBit=0), `str q15,[x17]`, x17==r11==c000102030, guest block
link 0x40cfc8. Late-arming (`ARMSKIP`) did not help enough. This is the same perturbation wall the prior
multi-agent effort hit; page-protection cannot pin this culprit.

--------------------------------------------------------------------------------------------------------
## 6. PRIME SUSPECT + RECOMMENDED NEXT STEP
--------------------------------------------------------------------------------------------------------
PRIME SUSPECT: a bulk inline 16-byte-zero sequence whose destination EA scratch (x16/x17) is clobbered
across an EA-fold / translation-block boundary, so `str q15,[x17]` lands on the victim span instead of the
intended fresh object. Candidates: **`runtime.duffzero` @0x462f20 (base=rdi)** invoked at a partial entry
offset (so a top-of-function brk misses it), or a compiler-inlined `MOVUPS X15` zeroing loop for the
`make([]*int,0,128)` backing (1024B) / an adjacent allocation. The x86 SSE store lowering (`movups`→
`str q`, in the SSE/legacy emitter — NOT the interpreted `avx.c` path) is where the EA is computed; a
scratch-reg reuse there would produce a value/layout-dependent wrong EA (explains the ~35% ASLR/interleave
dependence and why every static audit of the individual instruction passed).

RECOMMENDED (two non-perturbing options; the coordinator's HW-watchpoint agent is pursuing #2):
1. **Emitter ring-buffer** (BUG201's own recommendation): at every emitted `str q` of the zero vector,
   emit an inline UNCONDITIONAL record of `(guest_pc, EA)` to a ring buffer (one store, no branch, no
   signal). At the `CORRUPT` write, dump entries whose EA ∈ [0xc000102000, 0xc000104000) → the culprit
   guest PC. Low overhead; should not perturb.
2. **Hardware watchpoint (ARM DBGWCR/DBGWVR via `thread_set_state`)**: arm a single-address watchpoint on
   one live victim slot (e.g. 0xc000102000, 8 bytes). If the culprit bulk-zeroes the whole span it will
   hit that slot; the watchpoint traps ONLY that store (zero perturbation on unrelated stores, unlike
   mprotect). On the trap, use `resolve_guest_block(host_pc, &off)` (§2.6) to map back to the guest block,
   then `go tool objdump` the block and read x16/x17 vs the guest src reg to identify the wrong-EA
   lowering. Arm it AFTER strings are written (e.g. from the first victim sweep-watch sighting, reusing
   `g_vic_span`/`g_vic_base`).

--------------------------------------------------------------------------------------------------------
## 7. BUILD, ARTIFACTS, SOURCE LOCATIONS
--------------------------------------------------------------------------------------------------------
Worktree: `/Users/x/dd/dd/.claude/worktrees/agent-ad2b475d941d18e38`  (canonical tree untouched; changes
uncommitted per protocol). Diagnostic edits (all env-gated INERT):
- `dd-jit/src/runtime/translate/x86_64/elf.c` — `jit86_syncguard` brk handler (DDSPAN span-watch + DDBZ/
  DDBZ2 + DDMCLR + DDRAC + DDDST); `jit86_lazyguard` DDARM RO-watch; **`resolve_guest_block`** (§2.6);
  `watch_slot_bits` (§2.7); `g_vic_span`/`g_vic_base` globals; arming in the span-watch block.
- `dd-jit/src/runtime/os/linux/syscall/mem.c` — DDMADV + DDMSYS covers at `svc_mem` entry.
- `dd-jit/src/runtime/os/linux/syscall/io.c` — DDVICDUMP in the write path (case 64), with the arena walk.
- `dd-jit/src/runtime/translate/x86_64/{engine_glue.c,translate.c}` — from the base tooling patch
  (g_dumplo/hi, brk plant at `gpc==DDUMP`).
- `resolve_guest_block` uses `g_map[]`/`JIT_MAP_N` from `dd-jit/src/runtime/engine/cache.c` (in scope
  because the unity TU `targets/linux_x86_64.c` includes cache.c before elf.c).

Reusable diagnostic diff: `/Users/x/dd/dd/target/heap201/heap201_span_probe_ad.patch` (my full `git diff`).

Build (mac bridge, mirrors build.rs flags; `ent` = `dd-jit/jit.entitlements`):
```
WT=/Users/x/dd/dd/.claude/worktrees/agent-ad2b475d941d18e38
OUT=/Users/x/dd/dd/target/heap201_ad ; mkdir -p $OUT
mac bash -lc "env X=1 clang -O2 -o $OUT/ddjit-x86 $WT/dd-jit/src/runtime/targets/linux_x86_64.c \
  && codesign -s - --entitlements $WT/dd-jit/jit.entitlements -f $OUT/ddjit-x86"
```
(The `ATFD macro redefined` warning is pre-existing/benign. Grep the output for `error:` only.)
Engine artifact: `/Users/x/dd/dd/target/heap201_ad/ddjit-x86`.

Run examples (unique marker in the env prefix; reap by that marker — never pkill by a shared name):
```
E=/Users/x/dd/dd/target/heap201_ad/ddjit-x86 ; R=/Users/x/dd/dd/target/heap201/mcount
# span-watch + victim dump (the decisive combo):
timeout 55 mac bash -lc "exec env DD201ad=1 DDFIXHEAP=1 DDVICDUMP=1 DDSPAN=0xc000102000 \
  DDARENAS=0x20054c7b8 DDUMP=0x200422e0d $E $R"
# block-zero EA exoneration:  DDBZ=1 DDBZ2=1 DDUMP=0x20040cff7
# memclr cover:               DDMCLR=1 DDUMP=0x200463440
# syscall covers:             DDMADV=1  or  DDMSYS=1  (no DDUMP needed; syscall-boundary)
# refillAllocCache check:     DDRAC=1 DDUMP=0x200412754
# RO-watch (perturbs):        DDARM=1 [ARMSKIP=6] DDUMP=0x200422e0d
reap: mac bash -lc "pkill -f heap201_ad"
```

Guest PC map (mcount, runtime = link + 0x200000000):
`runtime.(*sweepLocked).sweep` 0x422dc0 (span-in-rdx point 0x422e0d) · `refillAllocCache` 0x412740
(RET 0x412754) · `nextFreeIndex` 0x412760 · `(*mcache).nextFree` 0x40cb00 · mallocgc tiny block-zero
`MOVUPS X15,(R11)` 0x40cff3 (block 0x40cfc8, next-insn 0x40cff7) · `memclrNoHeapPointers` 0x463440 ·
`memmove` 0x463740 · `duffzero` 0x462f20 (base=rdi).
