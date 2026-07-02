# BUG #201 — Hardware-watchpoint probe: ROOT CAUSE FOUND + FIX

Author: the ARM64 hardware-watchpoint agent (worktree `agent-a45f2dcfb81db5f5b`).
Scope: x86_64 guest → ARM64 macOS, dd JIT. Canonical repro `target/heap201/mcount` under `DDFIXHEAP=1`.

--------------------------------------------------------------------------------------------------------
## 0. TL;DR — ROOT CAUSE (definitive, caught red-handed)
--------------------------------------------------------------------------------------------------------
BUG #201 is **NOT a wrong-destination CPU store** (the converged prime suspect "duffzero / str q15 wrong
EA" is REFUTED). It is a **guest-4 KB-page vs host-16 KB-page mismatch in dd's `MADV_DONTNEED` emulation.**

The guest (Go's scavenger) legitimately calls `madvise(0xc000100000, 0x2000, MADV_DONTNEED)` on a **free**
8 KB span. That range ends exactly at `0xc000102000` and does **not** include the live victim span. But on
Apple Silicon the host page is **16 KB**, and dd emulated Linux `MADV_DONTNEED` by
`mmap(a0, a1, MAP_FIXED|MAP_ANON|MAP_PRIVATE)`. `MAP_FIXED` with a sub-16 KB length rounds the partial host
page OUT to the full 16 KB `[0xc000100000, 0xc000104000)`, which also contains the **live** tiny string
span `0xc000102000` — so the live span's 410 strings are unmapped and replaced with fresh zero pages.
The mspan metadata (separate `0x5000..` gcBits region) stays healthy → "live data silently zeroed."

- File/line: `dd-jit/src/runtime/os/linux/syscall/mem.c`, the `adv == 4` (MADV_DONTNEED) block in `svc_mem`.
- Why x86-only: the x86-Linux guest (Go) uses 4 KB pages and scavenges at 4/8 KB granularity, so it routinely
  DONTNEEDs ranges smaller than / unaligned to the 16 KB host page. An arm64-Linux guest uses matching pages.
- Why GC-triggered / ~22–40% / ASLR-dependent: it needs a free span to sit in the *same 16 KB host page* as a
  live span and the scavenger to DONTNEED the free one — a layout+timing coincidence.

### The fix (mem.c, `MADV_DONTNEED`)
Only `MAP_FIXED`-remap the host-page-aligned INTERIOR of `[a0, a0+a1)` (safe physical release + zero); zero
the partial edge host pages with `memset` over EXACTLY the requested bytes, never remapping a host page
shared with a neighbour. For host-page-aligned ranges this is byte-identical to the old path; for
sub-page/unaligned ranges it stops the neighbour clobber while preserving Linux "reads back zero" semantics.

### Verification (same harness, `env DDFIXHEAP=1 <engine> ./mcount`, no other instrumentation)
- **Before fix: 11/50 corrupt (22%)** (and 9/40 in an earlier baseline).
- **After fix: 0/50 corrupt.**

--------------------------------------------------------------------------------------------------------
## 1. Did the hardware watchpoint work on macOS? YES.
--------------------------------------------------------------------------------------------------------
An ARM64 single-address hardware watchpoint via `thread_set_state(thread, ARM_DEBUG_STATE64, …)` **works
self-hosted on Apple Silicon**, with these findings (verified with standalone tests + in-engine):

- **Entitlement:** the process must be codesigned with `com.apple.security.get-task-allow` for a thread to
  set its OWN (or another of its threads') debug registers. Added it to a scratch entitlements file
  (`jit_hw.entitlements` = jit.entitlements + get-task-allow). Without it the set is rejected;
  `allow-jit`/`unsigned-executable-memory` alone are not enough.
- **DBGWCR0/DBGWVR0 encoding used:** `wvr0 = addr & ~7`; `wcr0 = enable(bit0) | PAC=EL0(0b10<<1) |
  LSC(bits[4:3]) | BAS=8 bytes(0xff<<5)`. LSC=0b10 store-only (=`0x1ff5`), 0b11 load+store, 0b01 load.
- **Delivery:** a watchpoint hit surfaces as a **BSD `SIGTRAP`** (no mach exception port needed) with ESR
  `EC=0x34` (watchpoint, same EL) in `uc->uc_mcontext->__es.__esr`, and the faulting data address in
  `__es.__far`. `EC=0x3C` distinguishes a software `brk` (the existing DDUMP path), so the two coexist.
  Handled at the top of `jit86_syncguard` (elf.c): read pc + x17 + far, map host-PC→guest block via
  `resolve_guest_block`, disarm self, return (the access re-executes and completes; single-shot).
- **Catches every store form:** verified a plain `str`, a NEON 128-bit `str q`, AND `dc zva` all trigger the
  8-byte store watchpoint (so the suspected `str q15` / a `DC ZVA` bulk-zero would have been caught).
- **Self-arm is reliable; cross-thread arming needs suspend; continuous suspend/resume is FATAL.**
  - `thread_set_state(ARM_DEBUG_STATE64)` on a *running* peer thread is silently dropped; you must
    `thread_suspend` → set → `thread_resume` (like debugserver). Verified working in isolation.
  - BUT repeatedly suspend/resume-ing Go runtime threads **corrupts the Go scheduler** (`fatal error:
    runtimer: bad p`). Do not do it.
  - Winning design: a **read-only monitor thread** (never runs guest code) polls the Go arena for the victim
    span; once populated it sets a global `g_hw_want`; every **guest thread SELF-arms** its own debug regs
    from the top of the `run_guest` dispatch loop (`engine/dispatch.c`, guarded by `G_OWN_TRAMPOLINES` so
    only the x86 target compiles it). Self-arm is safe, needs no suspend, and new M's arm on their first
    dispatch iteration. Readback confirmed `wvr0=c000102000 wcr0=1ff5` sticking on many threads, and a
    **load+store watch (HWLSC=3) trapped a `ldr q1,[x17]` read of the victim** — proving the in-engine
    watchpoint is fully live.

--------------------------------------------------------------------------------------------------------
## 2. How the watchpoint cracked it (the decisive negative result)
--------------------------------------------------------------------------------------------------------
The store-only watchpoint on `0xc000102000`, armed on every guest thread and verified live, **NEVER fired**
across 30+ corrupting runs — yet those runs zeroed the victim (`CORRUPT firsti=1014`). Since watchpoints
provably catch `str`/`str q`/`dc zva`, this **excluded a guest CPU store entirely** and pointed at a
page-level operation. A monitor that polls the victim's first 16 bytes and, on the non-zero→zero
transition, dumps `mach_vm_region_recurse` showed:

```
[HWZERO] victim c000102000 NON-ZERO("key-1014-tail") -> ZERO while allocCount=410 (span still live) -- NO store trapped => PAGE-LEVEL zeroing
  [region victim  c000102000] base=c000100000 size=4000 share_mode=2(PRIVATE) object_id=2570418497 ref=1
  [region arena   c000000000] base=c000000000 size=..   share_mode=6            object_id=3000851708 ref=5
```

The victim's **16 KB host page became a distinct, freshly-remapped private-anon VM object** (its own
object_id, ref=1), carved out of the shared arena object → a MAP_FIXED remap, not a write. Broadening the
mem-syscall probe from "covers the victim" to "**overlaps the victim's 16 KB host page**" then caught the
exact call (a probe bug that hid it before — the guest range ends at the victim, so `a0<=vic<a0+a1` was
false):

```
[MSYS-HIT#2] nr=233(madvise) addr=c000100000 len=2000 end=c000102000 a2=4(DONTNEED)
   OVERLAPS victim_pg[c000100000,c000104000) vic=c000102000 rip=464737 slot0='key-1014-tail'
```

--------------------------------------------------------------------------------------------------------
## 3. Why every prior probe missed it (all consistent, no contradictions)
--------------------------------------------------------------------------------------------------------
- **Allocator / bit-scan / GC mark / GC sweep exonerations were all correct** — none of them is the cause;
  the span genuinely stays marked+allocated (the metadata region is a different host page, untouched).
- **"Wrong-destination str q15" was an inference, never a caught store** — the hardware watchpoint (which
  *would* have caught any such store) proves no store occurs. The zeroing is `mmap(MAP_FIXED)` collateral.
- **Prior `DDMADV`/`DDMSYS` "0 hits"** came from (a) the probe checking `a0<=vic<a0+a1` (misses the adjacent
  range whose host page overlaps the victim), and (b) the DDUMP-brk perturbation suppressing corruption to
  ~0 in instrumented runs. The zero-perturbation monitor + the 16 KB-page-overlap check fixed both.

--------------------------------------------------------------------------------------------------------
## 4. Reusable machinery added (x86 target only; all env-gated, INERT by default)
--------------------------------------------------------------------------------------------------------
In `translate/x86_64/elf.c` (+ a 3-line self-arm hook in `engine/dispatch.c`, `#ifdef G_OWN_TRAMPOLINES`):
- `DDHWWATCH=1` — start the zero-perturbation HW-watchpoint monitor (polls arena via `DDARENAS`, watches
  `DDSPAN` default 0xc000102000). Guest threads self-arm in the dispatch loop.
- `HWLSC=1|2|3` — load / store / load+store watch (diagnostic).
- `HWACNT=<n>` — arm once the victim span's allocCount ≥ n (default 300).
- `DDHWDIAG=1` — read back debug regs after each self-arm (proves the set stuck).
- `DDHWZERO=1` — poll the victim; on non-zero→zero transition dump `mach_vm_region_recurse` for the victim +
  controls (reveals a remap vs in-place write).
- `DDMSYS=1` (mem.c) — log any mmap/munmap/mremap/mprotect/madvise **overlapping the victim's 16 KB host
  page**, with guest rip + advice.

Build (mac bridge): `clang -O2 <target>/linux_x86_64.c -o <bin>` then
`codesign -s - --entitlements jit_hw.entitlements -f <bin>` (jit.entitlements **plus** get-task-allow).

--------------------------------------------------------------------------------------------------------
## 5. Fix status / caveats
--------------------------------------------------------------------------------------------------------
- Fix is a self-contained change to the `MADV_DONTNEED` (adv==4) block in `mem.c`. UNCOMMITTED, in worktree
  `agent-a45f2dcfb81db5f5b`. Standalone repro: **11/50 → 0/50**.
- The same class of bug (host-page rounding of a partial `MAP_FIXED`/unmap over a live neighbour) is worth a
  quick audit elsewhere in mem.c: the `MAP_FIXED` mmap path and `anon_split_unmap`/munmap handling of
  sub-16 KB-page guest ranges. The DONTNEED path is the one BUG #201 hit; others may lurk for other guests.
- Basics-matrix regression run not executed here (needs the full dd build/test infra); the change is
  conservative — byte-identical for host-page-aligned ranges, strictly more correct for unaligned ones, and
  preserves Linux "DONTNEED reads back zero" semantics (interior remap + edge memset).
