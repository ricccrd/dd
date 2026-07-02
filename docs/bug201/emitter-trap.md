# BUG #201 — Emitter-level allocBits trap investigation

Knowledge capture for the emitter-level allocBits-trap angle of BUG #201
(x86_64 → ARM64 GC-runtime heap corruption). This documents the instrumentation,
the raw data it produced, and the (later-superseded) conclusion it led to.
**Docs-only record — the source changes described here were diagnostic and left
uncommitted.**

> TL;DR — The emitter trap proved the corrupting 16-byte zero-store is the
> **ordinary tiny block-zero re-zeroing a span that (per allocBits) is FREE**,
> not a wrong-EA `memclrNoHeapPointers` into a still-live slot. That refuted
> BUG201.md's "wrong-destination memclr" pivot. It led me to a
> **"premature free of a live span" (mark/sweep)** inference — which a later
> hardware-watchpoint / mark-sweep agent **in turn refuted** (mark & sweep are
> healthy; the true bug is a wrong-DESTINATION bulk zero elsewhere). See
> "Status / what superseded this" at the bottom.

---

## 1. Goal & premise

Prior angles (BUG201.md) had converged on: the tiny allocator is exonerated
(Angle A per-alloc trace: every handout `allocBit==0`, genuinely free), the
bit-scan/arith is value-correct (Angle B), and the block-zero
`movups %xmm15,(%r11)` @guest `0x40cff3` was declared NOT the culprit because its
destination `r11` was verified free every time. BUG201.md therefore hypothesised
the corrupting zero-store was a **different** `str q`/memclr whose EA is
miscomputed on the corrupt layout and lands on a LIVE tiny slot — prime suspect
`memclrNoHeapPointers` @guest `0x463440`.

The assigned probe: instrument dd's **emitter** so every zeroing store carries an
inline guard that reads the destination slot's persistent `allocBit` and traps
when it stores into a live (`allocBit==1`) slot — a non-breakpoint,
non-page-protection method that survives the GC hot path.

The result **inverted the premise** (see §5).

---

## 2. The instrumentation

All changes gated behind env vars; **zero bytes emitted unless enabled**, so the
default engine is byte-for-byte unchanged (proven in §6).

### 2.1 Where it hooks — `translate/x86_64/emit.c`

The EA of every guest memory operand is computed into host **x17** by
`emit_ea_core()` (`decode.c`). A guest store then writes `[x17]`. So the store
emitters are the choke point. `emit_bug201_trap(int valreg)` is called right
after the store bytes are emitted, from:

- `e_str_q(t, rn, off)`  — SIMD 128-bit store (the block-zero `str q15,[x17]`
  and memclr's `MOVDQU X15` unrolled loop). Hooked when `rn==17 && off==0`,
  `valreg=-1`.
- `e_stp_q(t1, t2, rn, off)` — SIMD store-pair. `rn==17 && off==0`, `valreg=-1`.
- `e_str_d(t, rn)` / `e_str_s(t, rn)` — SIMD 64/32-bit store. `rn==17`,
  `valreg=-1`.
- `e_store(w, rt, rn)` — GPR store. Hooked for **all** `rn==17`, passing
  `valreg=rt` (so the guard can skip nonzero stores inline — see §2.3). This is
  what catches `memclr`'s small-size tail `MOVQ AX,(DI)` with `rax==0`.

`off==0` is required because `emit_ea` folds any displacement into x17 (e.g.
`MOVDQU X15,0x10(DI)` becomes `str q15,[x17]` with `x17 = DI+0x10`), so a guest
memory store always has the full EA in x17 with `off==0`. Nonzero `off` is dd's
own spill/scratch use (not a guest EA) — excluded.

`g_strap_gpc` is set in `translate.c` (next to the DDUMP brk planting:
`g_strap_gpc = gpc;`) to the guest PC of the instruction being lowered, so the
guard can materialise that PC on a hit.

### 2.2 The inline ARM64 guard (allocBit mode)

Emitted after the store, it walks Go's mheap arena for `EA` and reads the
persistent `allocBit` (span+0x40) — the correct liveness invariant (unlike
`gcmarkBits` which is transiently 0 outside the mark window). Register plan:
`x16 = EA` (saved copy), `x17/x19/x21` = walk scratch, `x20` = saved NZCV.
Guest flags are saved (`mrs x20,nzcv`) and restored on **every** exit path
(`msr nzcv,x20` at the shared `DONE:` label); all guard-fail branches are
back-patched to `DONE`.

Sequence (each line is one emitted A64 insn unless noted):

```
mrs   x20, nzcv                     ; save guest flags
[cbnz val, DONE]                    ; (GPR only) skip nonzero stores — cheap filter
mov   x16, x17                      ; x16 = EA
ubfx  x17, x16, #32, #32            ; EA>>32
cmp   x17, #0xc0 ; b.ne DONE        ; only the Go arena (0xc0.. region) proceeds
movconst x17, <DDARENAS>            ; &mheap_.arenas L1 ptr (biased link addr)
ldr   x17, [x17]                    ; L1
cmp   x17,#0xfff ; b.ls DONE
movz  x19, #0x8000, lsl #32         ; 1<<47
add   x19, x16, x19
ubfx  x19, x19, #26, #38            ; arenaIdx = (EA + 1<<47) >> 26
ldr   x17, [x17, x19, lsl #3]       ; heapArena
cmp   x17,#0xfff ; b.ls DONE
ubfx  x19, x16, #13, #13            ; pageIdx = (EA>>13)&0x1fff
ldr   x17, [x17, x19, lsl #3]       ; span
cmp   x17,#0xfff ; b.ls DONE
ldr   x19, [x17,#0x18]              ; span.base
ldr   x21, [x17,#0x68]              ; span.elemsize
ldr   x17, [x17,#0x40]              ; span.allocBits
cbz   x19, DONE ; cbz x21, DONE     ; base/elem sane
cmp   x17,#0xfff ; b.ls DONE
cmp   x21, #512 ; b.hi DONE         ; ELEM FILTER (see §2.3) — skip large spans inline
cmp   x16, x19 ; b.lo DONE          ; EA<base guard
sub   x19, x16, x19                 ; EA-base
udiv  x19, x19, x21                 ; objIndex
ubfx  x21, x19, #3, #61             ; objIndex/8
add   x17, x17, x21                 ; &allocBits[objIndex/8]
ldrb  w17, [x17]
and   x19, x19, #7                  ; bit-in-byte
lsrv  x17, x17, x19
tbz   x17, #0, DONE                 ; allocBit==0 -> ok, skip
movconst x19, <g_strap_gpc>         ; x19 = the store's guest PC
brk   #0x5211                       ; LIVE-slot zero-store! (handler in elf.c)
DONE:
msr   nzcv, x20
```

Key: `brk` fires **only** on a genuine allocBit==1 hit, so it is not a hot-path
breakpoint — no kernel round-trip flood, no GC-timing perturbation from the
trap itself (the per-store arena walk is straight-line userspace ALU/loads;
single-threaded + DDFIXHEAP-deterministic ⇒ uniform slowdown doesn't move the
bug).

### 2.3 Env gates & filters

- `DDSTRAP` — master enable. Unset ⇒ `emit_bug201_trap()` returns immediately,
  emitting nothing.
- `DDARENAS=<hex>` — biased addr of `mheap_.arenas` L1 ptr (mcount:
  `link 0x54c7b8 → 0x20054c7b8` under the DDFIXHEAP 0x200000000 non-PIE bias).
  Required; absent ⇒ trap disabled.
- **Value filter (inline):** for GPR stores, `cbnz x[valreg], DONE` bails before
  the arena walk if the stored value is nonzero. Only ZEROING stores can
  corrupt, so this keeps the common nonzero heap store to ~4 extra insns. xzr
  (rt==31) and SIMD (valreg=-1) always proceed.
- **Elem filter (inline):** `cmp elem,#512 ; b.hi DONE`. Large objects
  (`mallocgc` large path) are marked allocated **before** `memclr` zeroes them,
  so a zeroing store into a large allocated slot is LEGIT and would `brk`
  constantly; the flood perturbs the GC-window bug away. This filter (moved
  INLINE after an early version filtered only in the handler and killed repro to
  0/12) restored the corrupt rate. `STRAP_SMALL` env (handler-side) tunes the
  reporting threshold; I narrowed to `STRAP_SMALL=16` to isolate the tiny/string
  class.
- **Victim-window mode:** `DDVICLO`/`DDVICHI` (hex). When set, the guard becomes
  a minimal range check — `brk #0x5211` for EVERY store (any value, any
  allocBit) whose EA ∈ `[DDVICLO,DDVICHI)`. Used to watch a KNOWN victim span and
  name every PC that writes it.

### 2.4 Handler + ring buffer — `translate/x86_64/elf.c`

`jit86_syncguard()` handles `brk #0x5211`:
- Reads `x16` (EA) and `x19` (guest PC) from the ARM ucontext; re-runs
  `alloc_probe(EA)` for span/base/elem/objIndex/allocBit/markBit.
- allocBit mode: elem>`STRAP_SMALL` ⇒ count as legit ("big"); else print a
  `SMALL LIVE-SLOT ZERO-STORE` line + guest GPRs.
- Victim-window mode: append `{gpc, ea, cur(8 bytes at EA), rdi, rsp, allocBit,
  markBit, elem}` to a 2048-entry file-scope ring `g_vicring` (the LATE
  corrupting zero survives; early creation writes scroll out). Dumped in program
  order by `bug201_vicwin_dump()`.
- Advances host PC past the brk (`__pc += 4`) and resumes — non-fatal, so a run
  collects many hits and still completes.

Because guest `exit_group` bypasses host `atexit`, the ring dump is triggered
from the **exit_group syscall** handler: `os/linux/syscall/proc.c` case 94 calls
`bug201_vicwin_dump()` when `DDVICLO` is set.

### 2.5 Files touched (all uncommitted, worktree
`.claude/worktrees/agent-a6a41416b8e91685a`)

```
translate/x86_64/emit.c        +104  emit_bug201_trap() + 5 store-emitter hooks + g_strap_gpc
translate/x86_64/elf.c         +...  brk#0x5211 handler, vicring, bug201_vicwin_dump, VICSPAN
translate/x86_64/translate.c   +1    g_strap_gpc = gpc
os/linux/syscall/proc.c        +1    exit_group -> bug201_vicwin_dump
os/linux/syscall/mem.c         (copied DDFIXHEAP scaffolding)
translate/x86_64/engine_glue.c (copied DDUMP/DDUMP3 scaffolding)
```

---

## 3. Build & run

```
# build the instrumented x86 engine from the worktree unity TU via the mac bridge
MINE=<worktree>
OUT=/Users/x/dd/dd/target/heap201_a6/ddjit-x86-strap
mac bash -lc "clang -O2 -o $OUT $MINE/dd-jit/src/runtime/targets/linux_x86_64.c \
  && codesign -s - --entitlements $MINE/dd-jit/jit.entitlements -f $OUT"

# baseline (uninstrumented) engine for corrupt-rate comparison:
#   /Users/x/dd/dd/target/heap201_a6/ddjit-x86

# allocBit-trap run (tiny/string class only)
mac bash -lc "exec env DDFIXHEAP=1 DDSTRAP=1 STRAP_SMALL=16 DDARENAS=0x20054c7b8 \
  GOMAXPROCS=1 $OUT /Users/x/dd/dd/target/heap201/mcount"

# victim-window watch (span found in §4)
mac bash -lc "exec env DDFIXHEAP=1 DDSTRAP=1 DDARENAS=0x20054c7b8 \
  DDVICLO=0xc000102000 DDVICHI=0xc000104000 GOMAXPROCS=1 $OUT \
  /Users/x/dd/dd/target/heap201/mvic3"
```

Artifacts:
- Engines: `/Users/x/dd/dd/target/heap201_a6/ddjit-x86` (baseline),
  `.../ddjit-x86-strap` (instrumented).
- Repros: `/Users/x/dd/dd/target/heap201/mcount` (canonical),
  `.../mvic3` (mcount + `unsafe.StringData` victim-pointer prints; built from
  `src_mvic3.go`, corrupts identically — firsti=1014, fails=410).
- Reap: `mac bash -lc "pkill -f heap201_a6"`.

Guest-PC map (mcount, runtime = link + 0x200000000 under DDFIXHEAP):
`mallocgc` block-zero `0x40cff3` = `runtime.mallocgc+0x2f3` (biased `0x20040cff3`);
`memclrNoHeapPointers` `0x463440`; reuse writer `0x282c15` (main-program code,
biased `0x200482c15`); a legit small clear at `0x251da8`.

---

## 4. Data

### 4.1 allocBit==1 trap catches only LEGIT zeroing

Across many corrupt AND done runs, the only allocBit==1 zeroing stores were
legitimate small/large object clears — identical on both outcomes:

```
[STRAP#0 SMALL LIVE-SLOT ZERO-STORE] guestPC=200451da8 EA=c000128018
  span=50046b55228 base=c000128000 elem=8 objIdx=3 freeidx=0
  allocBit=1 markBit=0 allocCount=4 allocCache=fffffffffffffff0
```

`0x251da8` (elem=8) appears on DONE runs (9×) and the CORRUPT run (2×, different
EA) with the same shape — a legit clear of an allocated 8-byte object, NOT the
corruption. **The 13-byte string victim never trapped** ⇒ its slots are
`allocBit=0` at zero-time.

### 4.2 The victim is exactly ONE 8 KB tiny span

`mvic3` prints `unsafe.StringData(keys[i])` on corruption (deterministic under
DDFIXHEAP):

```
CORRUPT pass=621 fails=410/4000 firsti=1014
VICTIM i=1014 dataPtr=0xc000102000 len=13 bytes="\x00\x00\x00...\x00"
VICTIM i=1015 dataPtr=0xc000102010 len=13 bytes=0
VICTIM i=1016 dataPtr=0xc000102020 len=13 bytes=0
VICTIM i=1219 dataPtr=0xc000103000 len=13 bytes=0
VICTIM i=1423 dataPtr=0xc000103ff0 len=13 bytes=0
```

Span `0xc000102000 – 0xc000104000` = 8 KB = one Go tiny span = 512 × 16 B slots.
410 held live 13-byte `"key-<n>-tail"` strings (matches BUG201.md
`allocCount=512, setbits=410`). The **entire span** reads back zero — not a
single miscomputed store.

### 4.3 Who zeroes the victim span — REUSE

Victim-window watch, last writes before exit (ring buffer, program order):

```
#4393 gpc=20040cff3 EA=c000103fc0 elem=0 allocBit=0 markBit=0 curQ=0000000000000000 rdi=64 rsp=c00020fee8
#4394 gpc=200482c15 EA=c000103fc0 elem=0 allocBit=0 markBit=0 curQ=0000000000000064 rdi=0  rsp=c00020ff98
#4395 gpc=200482c15 EA=c000103fc8 elem=0 allocBit=0 markBit=0 curQ=0000000000000065 rdi=65 rsp=c00020ff98
```

Two writers, `allocBit=0` on **every** write:
- `gpc=0x40cff3` = `mallocgc+0x2f3` — the tiny **block-zero** (`str q15,[x17]`),
  `curQ=0` (zeroing fresh 16-byte blocks).
- `gpc=0x282c15` = main-program code writing fresh sequential ints
  (`curQ=0x58,0x59,0x5a…` = 88,89,90… incrementing), `rsp` far away
  (`0xc00020ff…`, NOT in the victim) ⇒ heap reuse, not stack reuse.

The earlier window writes (creation phase) show the same span first populated
with ASCII string bytes (`curQ` = `-tail`, `1125`, …), then later fully
re-handed-out. i.e. the tiny allocator is **correctly reusing a span that
allocBits says is free**, zeroing it via the ordinary block-zero.

---

## 5. Root-cause statement (as concluded from THIS angle)

1. **The corrupting 16-byte zero-store is NOT a wrong-EA memclr.** It is the
   ordinary tiny **block-zero** (`mallocgc+0x2f3` @`0x40cff3`) — the very store
   BUG201.md had "exonerated." It zeroes a slot whose persistent `allocBits`
   legitimately reads **free** (`allocBit=0`).
2. Both the block-zero AND `memclrNoHeapPointers` are therefore **innocent** —
   they act on memory that (per allocBits) is free. BUG201.md's
   "`memclrNoHeapPointers` wrong-EA into a live slot" pivot is **wrong**: the
   allocBit==1 trap never fired on the victim; the victim is reused free memory.
3. Consistent with Angle A ("allocBit=0 at handout, allocator exonerated") — but
   the inference "therefore a wrong-EA memclr elsewhere" was a misdiagnosis. The
   correct reading: an **entire live 8 KB tiny span is freed** (its 512 noscan
   string backings, still reachable via the `keys []string` backing array, lose
   liveness), then legitimately reused+zeroed.
4. **Inference (this angle):** the fault is **upstream in GC mark/sweep** — a
   whole live span goes unmarked → swept → freed. Suggested next probe: watch the
   victim span's `gcmarkBits` (span+0x48) across the pre-corruption GC to
   distinguish mark-miss vs sweep-error, then audit `scanobject` /
   `typePointers.next` of the `[]string` backing (HANDOFF.md's original converged
   suspect) — NOT any store lowering.

---

## 6. Proof the instrumentation is inert by default

With `DDSTRAP` unset, `ddjit-x86-strap` behaves as baseline:

```
strap-engine (DDSTRAP off): corrupt=4 done=7 other=4   (of 15, ~27% — baseline range)
leaked STRAP/VICWIN/LIVE-SLOT lines: 0
```

`emit_bug201_trap()` returns before emitting any bytes when the env is unset, so
translated code is identical to baseline. Corrupt-rate baseline ≈ 20–40%;
victim-window instrumentation still reproduces (it only `brk`s on genuine victim
writes, no per-store kernel round-trip). Early handler-side filtering (brk on all
large zeroing, filter in handler) DID perturb repro to 0/12 — fixed by moving the
elem filter INLINE so large-object zeroing never brks.

---

## 7. Status / what superseded this

This angle's **method and primary data stand** and are reusable:
- The victim is a deterministic, reproducible target: span `0xc000102000`
  (8 KB, 512×16 B), firsti=1014, fails=410, under DDFIXHEAP.
- The corrupting write is a **zero-store into memory that allocBits marks free**
  — captured with exact guest PCs. The wrong-EA-memclr hypothesis is dead.

My **"prematurely-freed live span (mark/sweep bug)" inference was later
refuted.** A subsequent hardware-watchpoint / mark-sweep agent found mark and
sweep are **healthy** — the live span is NOT wrongly freed by GC. The true
mechanism is a **wrong-DESTINATION bulk zero**: a bulk/zeroing operation whose
destination is miscomputed writes zeros across the (legitimately live) victim
region. So:
- Correct: block-zero `@0x40cff3` and `memclrNoHeapPointers` are not the *root*
  miscompile; the allocBit==1 signal does not catch the corruption.
- Superseded: "GC freed the span." The span is live; something zeroes it with a
  wrong destination address. Re-examine the victim-window writers (§4.3) as the
  *wrong-dest bulk-zero* rather than *legit reuse of freed memory*, and the
  hardware-watchpoint trace for the faulting bulk-zero's address computation.

The victim-window watch (`DDVICLO`/`DDVICHI` + `bug201_vicwin_dump`) remains the
right tool for the hardware-watchpoint agent: it already names every guest PC
that writes `0xc000102000–0xc000104000` in program order. The open question it
should answer: which of those writers has a **miscomputed destination** (vs.
which are legitimate post-free reuse) — distinguished by whether the region is
actually live/reachable at write time, which the mark-sweep agent's watchpoint
data now settles.
