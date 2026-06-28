# RESEARCH — the intermittent busybox fork+exec SIGSEGV ("the flaky matrix tail")

Status: **root-caused** (read-only investigation; no fix applied — engine refactor in progress).
Date: 2026-06-27. Captured live against the already-built engine
(`target/release/build/ddjit-*/out/ddjit-linux_aarch64`, ad-hoc-signed with
`com.apple.security.cs.allow-jit`) driven through the `mac` bridge.

## TL;DR

The flaky tail is **not** a guest fault and **not** the non-PIE bias bug. It is a
**W^X / `MAP_JIT` execute-permission fault in the `fork()` child**: the child resumes
`run_block()` and the **instruction fetch from the 64 MB `MAP_JIT` code cache is denied**
because the cache is not in execute mode for the child's thread (the per-thread
`pthread_jit_write_protect_np` / APRR state). Ground truth from the faulting thread's
`ESR_EL1`:

```
[REGION] sig=10(SIGBUS) fault=0x10669d794 hpc=0x10669d794 far=0x10669d794
         esr=0x8200000f  region=[0x1065c4000 .. +0x4000000) prot=RWX(7) share=private in=Y
```

* `hpc == far == fault` → the **host PC is the fault address** ⇒ an **instruction-fetch** abort.
* `esr = 0x8200000f` → **EC = 0x20** (Instruction Abort from a lower EL) + **IFSC = 0x0F**
  (**permission** fault, level 3). The page is *mapped* (prot = RWX) — it is a *permission*
  fault, i.e. **no-execute for this thread**, the W^X/APRR signature.
* The fault address is inside the **64 MB (`0x4000000`) `MAP_JIT` code cache** (`CACHE_SZ`,
  `jit/cache.c:5`; mapped `targets/linux_aarch64.c:293`), not the guest image/heap/stack.

It is intermittent + load-dependent because it depends on the child's per-thread W^X state at
the first post-`fork()` `run_block()`, which the dispatcher **assumes but never asserts**.

## How it was captured

Repro driver (concurrent, fork+exec-heavy busybox load through the `mac` bridge, unique to this
investigation — see `target/rb-scratch/`):

```
env CRASHDBG=1 <jit> --rootfs <alpine> /bin/sh -c \
  'i=0; while [ $i -lt 60 ]; do find /etc|head -2>/dev/null; seq 1 5|tr "\n" " "; \
       sh -c "seq 1 3|head -1"; i=$((i+1)); done'
```

Run 8–16 of these concurrently; a handful of forked **children** crash per round while the
parent shells still exit `rc=0` (the shell tolerates a dead `find`/`seq` child — which is exactly
why the tail only *flakes* and is hard to see). The diagnostics fire because the `CRASHDBG` child
clears its inherited Mach exception port (`service.c:1669`) so the inherited POSIX `diag_crash`
(`targets/linux_aarch64.c:80`) reports it.

### Captured faults (multiple samples)

`diag_crash` output (sig digit `0` ⇒ host signal `10` = **SIGBUS**; `tid=0` = a fork child):

| guest pc (`c->pc`)   | fault (`si_addr`)   | musl base (same proc) | pc − base |
|----------------------|---------------------|-----------------------|-----------|
| `0x1011d039c`        | `0x105a35794`       | `0x101188000`         | **0x4839c** |
| `0x101b0039c`        | `0x105d2d794`       | `0x101ab8000`         | **0x4839c** |
| `0x109e8039c`        | `0x105d83edc`       | `0x109e38000`         | **0x4839c** |
| `0x103ba039c`        | `0x108601794`       | `0x103b58000`         | **0x4839c** |

`c->pc` is **always `ld-musl + 0x4839c`** — perfectly deterministic across ASLR slides. The
`[REGION]`/ESR probe (a `DYLD_INSERT_LIBRARIES` shim, no `CRASHDBG` so the engine installs no
competing handler) then showed `fault` lands inside the **`MAP_JIT` cache** with the
instruction-abort permission ESR above. Three independent region samples, all 64 MB RWX private:
`fault=0x107b59794 region=0x107a80000`, `fault=0x106615794 region=0x10653c000`,
`fault=0x1057e5794 region=0x10570c000`.

## What the addresses mean

`c->pc = musl+0x4839c` is **not** the faulting instruction — it is the **chain-entry** guest PC
the dispatcher last set; with block-chaining/IBTC, `c->pc` is not advanced while emitted code
chains forward, and the real host fault is in the cache (per the ESR). The guest PC nails the
*context*. Disassembling `ld-musl-aarch64.so.1`:

```
0004836c <_Fork>:                       ; musl's fork() primitive
   ...
   4838c:  mov  x8, #0xdc   ; 220 = clone
   48390:  mov  x0, #0x11   ; 17  = SIGCHLD  ⇒ this is a plain fork()
   48394:  mov  x1, #0x0
   48398:  svc  #0x0        ; the fork syscall → service.c case 220 → host fork()
   4839c:  mov  x19, x0     ; <-- c->pc reported here (chain entry, just after the svc returns)
   483a0:  bl   482e0       ; __post_Fork: child branch re-inits pthread self / set_tid_address
```

So the crash is **inside the child returning from `fork()`** (musl `_Fork` → `__post_Fork`),
at the moment the child's dispatcher does its first `run_block()` back into the `MAP_JIT` cache.

Confirms it is the **distinct PIE bug** PLAN.md flagged: busybox/musl are PIE, loaded high
(`base≈0x101xxxxxx`); the fault is in the cache, not at a low non-PIE link vaddr; ESR is a
*permission* fault, not a translation (unmapped) fault — so it is categorically **not** the
non-PIE absolute-ref crash.

## Root cause

On Apple Silicon, a `MAP_JIT` region's **execute permission is gated per-thread** by the APRR
state toggled via `pthread_jit_write_protect_np()` (effective only because the engine carries
`com.apple.security.cs.allow-jit` — verified on the binary). The engine's contract is "the cache
is in **execute** mode whenever `run_block()` runs." Every `pthread_jit_write_protect_np(1)`
(re-assert execute) call lives **only inside the dispatcher's translate / cache-flush / IBTC-fill
paths** (`jit/dispatch.c:73,84,105`). **`run_block()` on a cache *hit* never re-asserts execute,
and the `fork()`-child resume path (`service.c:220`) never asserts it either.** It relies on the
thread's ambient W^X state.

Across `fork()` that ambient state is **not reliable**: the child resumes with the cache **not
executable for its thread**, so the very first instruction fetch from the cache raises the
instruction-abort permission fault (EC 0x20 / IFSC 0x0F) → SIGBUS.

Why **intermittent / load-dependent**: the child only faults when its first post-fork
`run_block()` executes with the cache in non-execute mode. Whether that happens depends on the
child's APRR state at that instant and whether a translate (which *does* re-assert execute)
intervenes first — a per-fork race. `soak/forkchurn` survives because its post-fork blocks differ;
high matrix load multiplies fork frequency and the odds of landing in the bad window.
(`diag_crash` "never fired" historically because, pre-fix, the child kept the inherited Mach
exception port and the POSIX handler was bypassed — now corrected by the port-clear at
`service.c:1669`, which is how these samples were captured.)

### Why it could not be reproduced standalone (and why that's consistent)

Five isolated C reproducers (`target/rb-scratch/forkcow*.c`: fork + large COW `MAP_ANON` write;
child store from `MAP_JIT` code; child *patching* a dirty `MAP_JIT` page; second live thread at
fork; execute-in-write-mode) **never faulted** — because an ad-hoc-signed test binary **without**
`com.apple.security.cs.allow-jit` has W^X as a *no-op*, so APRR isn't enforced. Re-signing with
the entitlement still didn't trip the exact race synthetically, which is expected: the trigger is
the specific musl `_Fork`→cache-hit timing, not a generic COW write. The dispositive evidence is
the ESR/region capture from the **real** engine, not a synthetic positive.

## Candidate fixes (ranked)

1. **Re-assert execute in the child + before `run_block` (cheapest, targeted).**
   In `service.c` case 220 (and case 435) child branch, after `G_SHADOW_RESET(c)`, call
   `pthread_jit_write_protect_np(1)`. Belt-and-suspenders: also re-assert it immediately before
   `run_block(c, code)` in `run_guest()` (`jit/dispatch.c:119`) so a cache *hit* never executes in
   write mode regardless of how the thread got there. Cost: one register write per block on the hot
   path for the unconditional variant (cheap; it's a `MSR` to a system reg). **Recommended first
   step** — directly closes the observed fault.

2. **Assert execute once per `run_guest()` entry / per thread-resume.** Call
   `pthread_jit_write_protect_np(1)` at the top of `run_guest()` and after any `fork()` return,
   amortizing the per-block cost while still covering the child. Lighter than (1)'s per-block
   variant; relies on nothing else ever leaving the thread in write mode between blocks (true today
   — every translate/IBTC path already pairs `0`→`1`).

3. **Dual-mapping the code cache (RX view + RW alias) instead of per-thread W^X.** Map the cache
   once `PROT_EXEC` (RX) and a second alias `PROT_WRITE` (RW) over the same pages (`MAP_SHARED`
   memfd / `vm_remap`); emit through the RW alias, execute through the RX view. Execute permission
   then lives in the **page tables** (survives `fork()` for all threads) and the APRR/W^X toggle —
   and this entire bug class — disappears. Heaviest, but principled, and it also addresses the
   PLAN's `soak/smc` RWX gap. Larger change; sequence after (1) proves the diagnosis.

4. **Don't run JIT in a bare-fork child** — insufficient alone: `soak/forkchurn` (fork *without*
   exec) legitimately executes translated code in the child, so the child must run the cache.

## Cheapest next experiment to confirm

Apply fix (1) — the one-line `pthread_jit_write_protect_np(1)` in the `service.c:220` child branch
(plus, if desired, the pre-`run_block` re-assert) — and re-run the concurrent driver above
(`CRASHDBG=1`, 8–16 procs × several rounds). Expectation: the `[CRASH] sig=…` lines and the
`[REGION]` instruction-abort vanish entirely. (Blocked right now only by the in-progress engine
refactor / read-only constraint — the diagnostic plumbing and driver are ready in
`target/rb-scratch/`.)

If a positive standalone repro is wanted for a regression test: instrument `run_guest()` to log
`pthread_jit_write_protect_np`'s effective state on the first post-fork `run_block` (or read APRR
`S3_6_C15_C1_5`) and assert it is execute — it will show write mode on the flaking iterations.

## Key references

* `dd-jit/src/runtime/jit/dispatch.c:40` `run_guest()`; `:56-110` translate/IBTC W^X toggles;
  `:119` `run_block(c, code)` (the unguarded execute on a cache hit).
* `dd-jit/src/runtime/os/linux/service.c:1653` case 220 fork; `:1662` `G_SHADOW_RESET`; child
  branch is where the execute re-assert belongs.
* `dd-jit/src/runtime/targets/linux_aarch64.c:293` `g_cache = mmap(… MAP_JIT)`; `:80` `diag_crash`;
  `:115` `exc_thread` (`[MACH]`). `jit/cache.c:5` `CACHE_SZ = 64 MB`.
* Captured ESR `0x8200000f` ⇒ EC 0x20 (instruction abort) / IFSC 0x0F (permission L3); fault inside
  the 64 MB RWX `MAP_JIT` region; guest context = `ld-musl` `_Fork+0x30` (`+0x4839c`).
* Investigation scratch (drivers, probes, reproducers): `target/rb-scratch/`.
