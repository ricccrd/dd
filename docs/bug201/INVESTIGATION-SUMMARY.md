# BUG #201 — Investigation Summary (chronological, one probe at a time)

The consolidated narrative of the multi-agent hunt. Each section: **symptom/what it tested → how it went down (method) → why (finding)**.
Per-probe deep detail lives in the sibling files (`angle-a-peralloc-trace.md`, `angle-b-bitscan-audit.md`,
`angle-c-minimize-bisect.md`, `emitter-trap.md`, `marksweep-probe.md`). The live conclusion is in `../BUG201.md`.

## The bug (symptom)
x86_64-guest only. A GC'd Go runtime under dd (x86→ARM64) **silently corrupts live heap data**, ~30–40% of runs,
ASLR/seed-dependent. Canonical deterministic repro `target/heap201/mcount` under `DDFIXHEAP` (firsti=1014,
fails=410): one **8 KB tiny span `0xc000102000`–`0xc000104000`** (512×16 B slots) holding **410 live 13-byte
`noscan` string backings** for `keys[1014..]` reads back **all zeros** while the string headers stay intact →
map lookups / `keys[i]` compares fail. Cascades in the wild to **go build/`go mod download`, gcc, .NET, clang -O2,
rustc, CPython** — any GC'd/managed x86 runtime. Plain C/C++ is unaffected. **NOT** an arm64 bug.

---

## Probe 1 — Angle A: differential per-allocation trace  → **ALLOCATOR EXONERATED**
- **Tested:** hypothesis #1 "the tiny allocator hands out a block that is actually a live slot, and the block-zero
  clears it" — find the first allocation whose returned block is a live slot.
- **How:** instrumented every tiny alloc (dual `brk` probes 0x5202/0x5203 in the mallocgc tiny path + an
  `alloc_probe` arena walk reading the **persistent `allocBits` at span+0x40**, not the transient gcmarkBits).
  Disassembled the guest tiny-allocator PC map (mallocgc 0x40cd00, tiny-bump ret 0x40ced6, nextFreeFast
  0x40cf1d–0x40cfa6, block-zero 0x40cff3, memclr 0x463440). Ran the deterministic DDFIXHEAP trigger.
- **Why (result):** across **~80,000 allocations per corrupting run** (40,801 refill + 39,199 tiny-bump), **every
  returned block was genuinely free** — allocBit=0, markBit=0, overflowLive=0, LIVE-HANDOUT=0. The allocator NEVER
  hands out a live block; there is no "first wrong block." → The corrupting store's destination is computed
  **outside** the allocator handout. (Caveat: gcphase read returned 0 = inconclusive; the brk fired *before* the
  store so x17-vs-r11 was not yet verified at the store itself — handed to later probes.)

## Probe 2 — Angle B: x86 bit-scan / allocCache / address-arithmetic differential audit  → **VALUE-CORRECT**
- **Tested:** does `nextFreeFast`'s bit-scan (find lowest free slot) or its `base + index*elemsize` arithmetic
  miscompile, yielding a wrong block address?
- **How:** (a) a per-instruction audit of dd's lowering of BSF/BSR/TZCNT/LZCNT/POPCNT vs the Intel SDM — value
  (RBIT+CLZ etc.) AND the ZF/CF flags, respecting dd's `cpu->nzcv = ARM borrow-C = NOT(x86 CF)` convention; (b)
  self-checking guest binaries (`flagchk`, `addrchk`, `bitchk`, `shiftchk2`) run natively (oracle) vs under dd
  over every bit pattern (zero / single-bit at all 64 positions / high-bit / full / 0x5555/0xAAAA / the
  clear-lowest-bit walk) and the full `IMUL/ADD/SHL $4/LEA` address math over all size classes + high heap bases
  incl. the 5 TB DDFIXHEAP region.
- **Why (result):** **`bad=0` everywhere** — bit-scans (value+flags), the allocCache `SHRQ %cl` + shift-by-≥64
  guard, and the address arithmetic are **all correct in isolation**. → The fault is a **context/ordering
  interaction on an individually-correct instruction**, not a value-wrong opcode.

## Probe 3 — Angle C: minimize + translation-toggle bisect  → **BASELINE lowering; 2 methodology bugs**
- **Tested:** does any dd optimization cause it? (translation-toggle bisect) + shrink the repro.
- **How:** toggled every optimization at **N≥40** (small-N is pure noise — proven: NOLSE looked like 0/13 but was
  4/23 at N=30 and is actually a no-op for x86 guests). Toggles: NOSTITCH (timing control), NOTIER2/NOTIER2X,
  NOFLAGELIDE, NOSSEOPT, NOREPCMP, NOLSE, plus the already-ruled-out NOEAOPT/NOGUESTFOLD/NOREP.
- **Why (result):** **NO toggle flips it to clean** → the miscompile is in an **always-on BASELINE lowering**, not
  any optimization. Consistent with A (allocator ok) and B (individually-correct instruction). **Two methodology
  bugs found that invalidated earlier controls:** (1) `env GOGC=off <engine>` **never reaches the guest** — the x86
  engine builds guest env from `DD_GUEST_ENV` + a fixed {PATH,HOME,TERM,LANG} only (translate/x86_64/elf.c:355);
  you must use `DD_GUEST_ENV=$'GODEBUG=gctrace=1\n'`. (2) Even forwarded, `GOGC=off` does NOT fix mcount (it calls
  `runtime.GC()` every 100 passes = forced GCs); the **real trigger invariant is "no explicit `runtime.GC()` ⇒ no
  corruption"** (mnogc = mcount minus the GC line = 0/30). DDFIXHEAP determinism is per-binary + layout-fragile —
  recompiling the repro moves/kills the victim, so **`mcount` is the canonical repro; do not reimplement it.**

## Probe 4 — Emitter-level allocBits trap  → thought "mark/sweep"; **later superseded**
- **Tested:** trap ANY 16-byte zeroing store whose destination lands in a live (allocBit=1) heap slot, to catch a
  wrong-EA memclr directly.
- **How:** `emit_bug201_trap()` in emit.c, emitted after every `str q`/`stp q`/`e_store` with rn==17: an inline
  ARM64 arena-walk guard reading allocBit for EA=x17 and `brk #0x5211` on a hit (NZCV saved/restored; env-gated
  `DDSTRAP`; inert when off). Plus a victim-window ring buffer.
- **Why (result):** the only allocBit==1 zeroing stores were **legit** (guestPC 0x251da8, elem=8, identical on
  clean+corrupt runs). The victim slots are **allocBit=0 at zero-time**, and the block-zero @0x40cff3 only ever
  zeroes genuinely-free slots. The agent inferred "the span was prematurely freed (mark-miss/sweep) then correctly
  reused+zeroed" — **this mark/sweep inference was REFUTED by Probe 5.**

## Probe 5 — GC mark/sweep span-metadata watch  → **GC HEALTHY; it's a wrong-DEST bulk-zero**
- **Tested:** is the span freed via a mark-miss (never marked) or a sweep bug (marked but swept)?
- **How:** `DDSPAN` watch at `runtime.(*sweepLocked).sweep` entry (guest 0x422e0d, rdx=span) sampling the victim
  span's `gcmarkBits`(span+0x48) and `allocBits`(span+0x40) once per forced GC; `DDVICDUMP` arena-walk at the
  moment of corruption; DDBZ/DDBZ2 (block-zero, both guest r11 AND ARM EA x17), DDMCLR/DDMADV/DDMSYS (syscall
  covers), DDRAC (refillAllocCache); and `resolve_guest_block` (a `g_map{gpc,host,body}` reverse lookup host-PC→
  guest-block, for the next probe to reuse).
- **Why (result):** gcmarkBits=410 AND allocBits=410 at **every** GC including the corrupting one (pass 600) — the
  span is **never unmarked, never loses allocBits, never freed** (same struct pointer throughout). Bytes intact at
  the pass-600 sweep, **all-zero by pass ~619** while allocBits is still 410 → a **MUTATOR-phase (post-sweep)
  wrong-destination 16-byte SIMD zero-store** bulk-zeroes the live span during a `make([]*int,0,128)` allocation.
  **NEITHER mark-miss NOR sweep** — GC metadata is healthy. Ruled out non-perturbingly: block-zero (both r11+x17),
  memclr 0x463440, madvise/mmap/munmap/mremap/mprotect, refillAllocCache (`allocCache==^allocBits`, 0 mismatch —
  the earlier "42 mismatches" was a stale-word-index probe bug).

---

## Converged conclusion (what all five agree on)
A **wrong-DESTINATION 16-byte SIMD zero-store (`str q15`)** — NOT the tiny block-zero, NOT `memclrNoHeapPointers`,
NOT the allocator, NOT GC mark/sweep — bulk-zeroes the whole live victim span `0xc000102000` during a *later*
mutator allocation (pass ~619 `make([]*int,0,128)`). It is a **codegen bug at one specific store site**: the
destination EA scratch register (x16/x17) is left **clobbered/stale across an EA-fold or translation-block
boundary**, so a zero-init of a NEW allocation writes to the live span's address instead of the new object's.
It survives every optimization toggle (baseline lowering) and every store observed so far is individually correct
— the wrong VALUE is a stale EA, not a wrong opcode. **Prime suspect:** dd's lowering of `runtime.duffzero`
@guest 0x462f20 (64× `MOVUPS X15,(DI)`, base=rdi) or an inline `MOVUPS X15` zero-loop.

## The obstacle & the definitive next probe
Every SOFTWARE store-watch (RO `mprotect`) perturbs it away — it faults on the millions of legit stores → timeout
or shifts the manifestation. **A single-address ARM64 hardware watchpoint (`DBGWCR`/`DBGWVR` via mach
`thread_set_state`) on 8 bytes at `0xc000102000`** faults ONLY on a store to that one address — zero perturbation —
so it traps the culprit and, via `resolve_guest_block`, yields the exact guest PC + the mistranslated instruction.
Arm it AFTER the 410 live strings are in place (post pass-600 sweep). That is the current probe in flight.

## Reusable assets for the current agent
- Canonical deterministic repro: `target/heap201/mcount` (DDFIXHEAP firsti=1014 fails=410, ~30% corrupt). **Do not
  recompile it** (layout-fragile).
- `resolve_guest_block` (g_map reverse lookup) + span/arena math — in `marksweep-probe.md` + the ad2b475d worktree.
- Guest env MUST go through `DD_GUEST_ENV` (host env is dropped) — Angle C.
- N≥40 for any rate comparison (small-N is noise) — Angle C.
