# dd — engineering reference

`dd` JIT-translates Linux/macOS containers on an arm64 macOS host — no VM. Workspace:
`dd-jit` (engine + bindings), `dd-daemon` (Docker Engine API), `ddcli`/`dd-gui`.

Open work: **[TODO.md](TODO.md)**. Tests: `make test` (engine×case matrix), `make scenarios`
(real software via the daemon), `make coverage` (syscall/opcode gaps). Pending SQLite-parity
patches: `arm-a{1,3,b1}.diff`. Not-yet-built ideas: `ideas/`.


---

# dd-jit ARCHITECTURE — the mental model + interface contracts

dd-jit JIT-translates guest machine code (linux/x86_64, linux/aarch64, darwin/aarch64) to host ARM64
macOS and emulates the guest OS in userspace — no VM. One C codebase, three Mach-O executables (one per
`(guest OS, guest ISA)` target). Each target is **one unity translation unit** (`targets/<target>.c`
`#include`s the slice it needs) compiled by `build.rs` to `ddjit-<target>` and codesigned with the
`allow-jit` entitlement — no link step between modules; a "module" is a `.c` pulled into the TU.
(`build.rs` drives clang/codesign directly on a macOS host, or through the `mac` bridge on a Linux dev
host; on a real mac a missing target is a hard build failure, never a silent degrade.)

## Domains (the real tree, `src/runtime/`)

- `include/cpu_{aarch64,x86_64}.h` — the guest CPU register file + engine scratch. **Offsets are baked
  into emitted machine code**; `_Static_assert(offsetof…==OFF_*)` guards each so a struct edit fails the
  build, not a guest.
- `translate/<isa>/` — guest ISA → host ARM64. **aarch64** is a near-1:1 transliterator (copies
  instructions, mangles only those naming a stolen host reg). **x86_64** (jit86) is a real
  decoder→emitter: `decode.c`, `translate.c` + `translate/{alu,mov,shift,repstr,x87,trace}.c`, `emit.c`,
  `avx.c` (VEX/EVEX), `x86_ops.c`, plus its own `elf.c`/`legacy.c`/`sysmap.h`/`pcache.c`/`forkserver.c`.
  Each frontend exposes `abi.h` (the G_* seam) + `dispatch_hooks.h` (the dispatch seam) + `fill_stat.c`.
- `engine/` — the host ARM64 execution core shared by the linux targets: `cache.c` (64 MB dual-mapped
  W^X code cache, gpc→host map, chaining, STW flush, IBTC), `dispatch.c` (`run_guest()` loop + the
  aarch64 trampolines), `stubs.c`.
- `host/arm64/asm.c` — the shared low-level ARM64 encoder (`e_*`); the host-assembler dedup landed.
- `os/linux/` — the guest-OS personality: `elf.c` (loader), `signal.c`, `thread.c`, `fscache.c`,
  `sentry.c` (untrusted-guest split), `syscall/` (`dispatch.c` defines `service()` + the split-module
  dispatch; one file per family: `io fs mem net proc signal time sysv event misc rare helpers`),
  `container/` (`vfs.c`+`vfs/{resolve,overlay,gmap}.c`, `netns.c`, `state.c`).
- `os/darwin/` — `jitdarwin.c` (same-ISA macOS DBT) + `jail/jail.c` (a DYLD-interpose jail, a separate
  mechanism, NOT a JIT). `os/container_parse.h` — the shared strict numeric config validator.

`linux_x86_64` shares the whole `os/linux/` personality and `engine/cache.c`+`dispatch.c`; it keeps its
own emitter and its own `run_block`/`block_return` trampolines (16-GPR model, cpu pinned in x28) and
defines `G_OWN_TRAMPOLINES`. `darwin_aarch64` imports `os/darwin/jitdarwin.c` whole.

## THE REAL INTERFACES (contracts a change must not break)

| # | interface | where | invariant (break = silent corruption) |
|---|---|---|---|
| 1 | **`struct cpu` layout** | include/cpu_*.h | Offsets are baked into emitted code AND into the `run_block`/`block_return` asm (host saves at `[x0,#288..#376]`, `host_v`@896, etc.). **Append-only past the baked region; update every `OFF_*`** (the `_Static_assert`s enforce it). |
| 2 | **`abi.h` G_* seam** | translate/*/abi.h → os/linux | os/linux switches on the **canonical (aarch64) syscall number**; x86 maps via `canon_x86()` in `G_NR`. The high `O_*` group differs by arch (`G_O_DIRECTORY/NOFOLLOW` — aarch64 `O_LARGEFILE` 0x20000 == x86 `O_NOFOLLOW`, so a hardcoded bit turned every symlink open into ELOOP), as do `G_UNAME_MACHINE` and `G_BRK_GROWABLE` (aarch64 grows a real brk; x86 reports a fixed break so glibc uses its mmap allocator). Never hardcode a per-ISA value in os/linux. |
| 3 | **dispatch seam** | dispatch_hooks.h + engine/dispatch.c `#ifndef` defaults | ~10 hooks (`G_DISPATCH_ENTER/DEBUG/CHAIN/AFTER_TRANSLATE/SHADOW_CLEAR/IBTC_FILL/DISPATCH_REASON/TRACE_DUMP`, `G_BLOCK_ALIGN`, VDBE/IBPROF) expand INSIDE `run_guest()` (so `break`/`continue` reach the loop). The `#ifndef` defaults reproduce the aarch64-inline code verbatim; x86 overrides each. |
| 4 | **block-dispatch reason** | `reason`+`redirect`+`pc/rip` in cpu | Every `R_*` an emitter produces MUST be handled in that frontend's `G_DISPATCH_REASON`. aarch64: `R_BRANCH/R_SYSCALL/R_TIER2/R_ICFLUSH`; x86 adds `R_CPUID/R_AVX/R_SSE3B/R_REPSTR/R_X87{FLD,FSTP,FUNC}/R_DIV/R_IDIV`. **Syscall pc-advance is per-ISA**: aarch64 does `pc+=4` after `service()`; x86 pre-advances `rip` in the emitter. `redirect` (execve/sigreturn set pc directly) suppresses the advance. |
| 5 | **service split-module dispatch** | os/linux/syscall/dispatch.c | `service_local()` calls `svc_{sysv,mem,signal,time,io,fs,proc,net,event,misc,rare}()` in order; each returns 1 if it handled `nr`, else fall to the main switch (ENOSYS if unhandled). Each family tail-calls `svc_done()` (which applies the BSD→Linux `m2l_errno` translation), so a syscall is owned by **exactly one** place — two handlers = first wins, silently. |
| 6 | **cache key/hash** | engine/cache.c + `G_GPC_HASH_SHIFT` | Cache keyed purely by guest PC (aarch64 `>>2`, x86 `>>0`). `JIT_MAP_N=2^19` is sized so the 64 MB arena fills (→ wholesale flush) before the open-addressed map does; **a full map silently no-ops `map_put`** → `map_body` returns NULL → `patch_links_to` bakes a wild `b` (mongod SIGILL). SMC must invalidate (`smc_icflush`/`smc_on_write`). |
| 7 | **ELF/loader handoff** | os/linux/elf.c (arm) · translate/x86_64/elf.c | Per-ISA: refuses a foreign `e_machine`, maps PT_LOAD HIGH, sets entry/base/auxv + `g_nonpie_{lo,hi,bias}`. auxv `machine`/`platform` and **AT_PAGESZ = host 16K** (not guest 4K — ld.so segment alignment) are per-frontend. |
| 8 | **per-arch struct fill** | translate/*/fill_stat.c | os/linux calls the frontend's `fill_linux_stat` (stat/sigaction/epoll_event differ by ISA — x86 packs `epoll_event` to 12 B, aarch64 to 16 B); never lay out a guest struct in shared code. |
| 9 | **trampoline ownership** | `G_OWN_TRAMPOLINES` | The register model (31 vs 16 guest GPRs, cpu-ptr pinning) is the ONE irreducible per-ISA divergence; the shared loop only CALLS `run_block`, never bakes its offsets. |

**Two-frontend rule:** a guest-ISA task is x86 OR aarch64, never both files. An os/linux task is shared
— it must work for BOTH frontends via the G_* seam (#2), never by special-casing a guest ISA.

## Engine invariants (the host execution core)

- **Dual-mapped W^X (Apple-Silicon JIT).** `engine/cache.c` `vm_remap`s a second RX alias of the same
  physical pages; the writer uses the RW alias, execution uses RX (`J_RX`/`J_RW` = `±g_rw2rx`). All
  PC-relative emission/backpatch is a difference of two cache addresses → **alias-invariant** (only the
  few absolute handoffs convert). `jit_wprot` is then a no-op (no per-region permission flip, no
  peer-thread W^X race). `NODUALMAP=1` falls back to one MAP_JIT region toggled per translation.
- **fork rebuilds the dual map.** `fork()` COWs the RW and RX aliases independently, so a child's two
  views silently diverge (writes via RW never reach the COW'd RX → executes stale/zero pages).
  `jit_after_fork` builds a FRESH dual map in the child and drops inherited translations. (No-op for the
  single-MAP_JIT fallback, where RWX is inherited correctly.)
- **Stop-the-world flush (threaded).** When the cache fills and a peer thread is live, in-place reuse
  would free code under a peer mid-block. Instead: park every peer at a safepoint (a process-wide
  `SIGEMT` handler — a signal the guest map never targets), switch to a **fresh** cache, release. The
  old cache is retired (kept mapped, unmodified) and reclaimed only once no thread's published
  `exec_gen` still names its generation — bounds retained VA, no per-flush leak. `stw_unregister` is
  held under the reg lock for the whole flush to close a lost-signal hang (the rustc/Go "blocks at exit,
  0% CPU"). Single-threaded never reaches this.
- **Lock-free IBTC.** Indirect branches probe a shared `{target,body}` hash inline. Entries are
  16-byte-aligned so a 128-bit `stp`/`ldp` is single-copy atomic under FEAT_LSE2 (all Apple Silicon);
  the writer publishes with a `dmb ish` + atomic 128-bit store so a lock-free reader never sees a torn
  new-target/old-body pair. (`NOMTIBTC=1` reverts to the locked path.)

## Two data flows (the whole engine in 12 lines)

```
TRANSLATE (cold path)                    SYSCALL (warm path)
  run_guest loop (engine/dispatch.c)       block exits, reason=R_SYSCALL
   └ map_host(pc) miss                       └ G_DISPATCH_REASON → service(c)
      └ translate_block(pc)   [translate]        └ svc_*() family modules  (#5)
         └ decode → emit ARM64 [emit/asm.c]      └ else main switch(G_NR)  (#2)
         └ map_put(pc → host)  [cache.c #6]      └ G_A0..A5 in, G_RET out
   └ run_block(c, J_RX(code))  [trampoline #9]   └ errno xlate macOS→Linux (svc_done)
      └ block sets reason, returns (#4)        (aarch64 pc+=4 / x86 pre-advanced)
```

## Platform limitations (the macOS primitive does not exist) — detail in [`../DESIGN.md`](../DESIGN.md)

non-PIE `ET_EXEC` low vaddr (mandatory ~4 GiB `__PAGEZERO`; §1) · no cgroup cpu/io throttling (mem+pids
via rlimit; cgroup is synthetic `/sys` files only) · no seccomp on macOS, no kernel namespaces (Seatbelt
gates operation classes; isolation is a userspace path-jail + the sentry split; §3) · no TUN / raw
sockets / root for networking — L3 is synthesized over AF_UNIX rendezvous (§4) · `pidfd`/`io_uring`/
`mq_*` absent · plain `mmap(PROT_EXEC)` without MAP_JIT unsupported · AT_PAGESZ must equal the host 16K
granule (host 16K vs guest 4K also forces partial-`munmap` to release only whole host pages).


---

# dd — design & mechanism reference

The non-obvious "why" and the hard invariants behind dd-jit's shipped mechanisms, grounded in the
actual source (`dd-jit/src/runtime/`). **Read the relevant section before touching that area** — each
invariant here is the root of a fixed bug. Live open bugs are in [`BUGS.md`](BUGS.md); the interface
contracts + engine-core invariants are in [`architecture/ARCHITECTURE.md`](architecture/ARCHITECTURE.md).

---

## 1. Non-PIE low-address hosting — the `__PAGEZERO` platform limitation

**Read before touching `os/linux/elf.c`, the aarch64 fold in `translate/aarch64/translate.c`, or the
per-syscall pointer rebasing in `syscall/dispatch.c`.** Highest future-conflict area.

### The limitation (macOS, immovable)

A GNU-toolchain non-PIE `ET_EXEC` is linked at a fixed low vaddr (`0x400000`) with un-relocated
absolute code *and data* refs. On arm64 macOS the engine's main Mach-O carries a **mandatory ~4 GiB
`__PAGEZERO`** (`0x0…0x1_0000_0000` unmapped), so those low addresses are unmappable in-process and
unrecoverable (a `-pagezero_size` shrink won't load on arm64; a helper has its own pagezero;
`mmap(MAP_FIXED)`/`mach_vm_*`/`MAP_32BIT` can't return `0x4xxxxx`; macOS has no `mmap_min_addr` knob).
**The guest stays mapped HIGH; correctness comes from address folding, never from low memory.** Native
low-vaddr load is wontfix.

### How it actually works (three layers, all gated on `g_nonpie_lo != 0` — inert for PIE)

`load_elf` (`elf.c`) maps PT_LOAD HIGH at `+bias` and records `[g_nonpie_lo,g_nonpie_hi)` + `g_nonpie_bias`.

1. **Dispatcher code-redirect** (`engine/dispatch.c`): a guest PC landing in the low link span is
   bumped `+bias` so real code is translated, not faulted.
2. **Translate-time bias-fold** (`emit_fold_mem` in `translate/aarch64/translate.c`, default on,
   `NOGUESTFOLD=1` off): the common load/store forms get the bias folded into the effective address
   inline. **No host register is stolen for the bias** — stealing a 5th GPR is unsafe (Go reserves
   R27/R28; any instruction form not flagged by `gpr_field_mask()` would read the engine value).
3. **`nonpie_fixup` SIGSEGV fallback** (`elf.c`): forms the fold skips (ldr-literal, writeback
   exclusive-monitor pairs, AdvSIMD multi-structure, DC ZVA) fault at the low addr and are served at
   `+bias` by an instruction decoder covering LDP/STP, LSE RMW/SWP/CAS, a **software LL/SC monitor**
   (the two halves of `ldxr/stxr` arrive as separate faults so the hardware monitor can't carry — a
   per-thread reservation + atomic CAS ensures two threads racing a low image lock can't both succeed;
   a naive memcpy deadlocked musl's `a_cas`), and LD1-4/ST1-4. An un-decodable form re-raises → clean
   abort, never wrong data.

### The invariants you must not break

- **Pointer *values* stay LOW.** `g2h(p)=p+bias` at every guest memory access AND every syscall that
  dereferences a guest pointer; `h2g(p)=p-bias` on every host addr returned to the guest. `adr/adrp`
  and baked pointers materialize against the **un-biased (low) PC** via `pcrel_base()` — gcc/Go compare
  a computed pointer against a stored low pointer; a high result mismatches → gcc ICE / invalid free.
- **Fold the FULL effective address before the range test**, not the base register alone: for
  `[Xn,Xm{,ext}]`, if Xm is the high pointer and Xn a small index, biasing Xn alone corrupts ld.so.
  The fold computes `Xn+extend(Xm)`, tests *that* against `[lo,hi)`, then biases.
- **Never regress the PIE matrix:** every fold + the `nonpie_fixup` install is gated on `g_nonpie_lo`,
  which is 0 for PIE/static-PIE (all the matrix ever sees) → codegen byte-identical.
- A guest fault's reported address must be in **guest (low)** space.

---

## 2. aarch64 transliterator (`translate/aarch64/`)

A near-1:1 copy that mangles only the three non-trivial paths below; everything else is verbatim.

- **Stolen-register mangling.** Stolen host regs: **x28**=cpu ptr, **x18**=scratch (macOS async-zeroes
  it), **x30**=host link; plus **x16/x17** when `g_steal1617` (default; `NOSTEAL1617=1` reverts). Guest
  values live in `cpu->x[]`. An instruction naming a stolen reg is rewritten (`emit_mangled_x18`):
  spill scratch to **`cpu->mscratch[]`** (NOT the stack — collides with the guest frame), load guest
  values, emit with substituted fields, store back. **Invariant: the scratch arrays must hold 4 distinct
  stolen regs** (e.g. `madd x16,x17,x18,x28`); sizing 2 overflowed and `__stack_chk_fail`'d cc1.
- **LSE atomic upgrade** (`try_lse_atomic`): recognizes the `ldxr/…/stxr/cbnz` retry loop and emits a
  single LSE op (`swpal`/`ldaddal`/`casal`, ~2.3×, livelock-free). **LDAR≠LDXR**: bit23 distinguishes
  the ordered `LDAR` (does NOT open an exclusive region) from `LDXR` (does). Inside an exclusive
  monitor region no spill may be injected (it clears the monitor → `stxr` retries forever), so
  conditional exits are deferred to a fixed-size table and emitted after the `stxr`.
- **§B shadow-return prediction is PRESENT but DEFAULT-OFF** (`shadowgate()` default `-2`). It pushes a
  `(guest_ret, host_ret, guest_sp)` shadow frame at each `bl` and predicts `ret` via the host RAS,
  validating **both** guest_ret and guest_sp (so it never mislands — a mismatch falls back to IBTC).
  Measured on real workloads (sqlite/qsort/deep recursion) the ~40-insn push+validate costs **more than
  the ~1-cycle IBTC per-site IC it replaces → net-negative**, so it is disabled and every `ret` goes
  through the IBTC. Do not "turn §B on" as an optimization without re-measuring.
- **SMC**: a guest `ic ivau` exits `R_ICFLUSH` → `smc_icflush` drops `g_map`+`g_ibtc`+pending links
  (old block code stays valid; shadow host_rets still point at it). `NOSMC=1` disables the intercept.
- **tier-2** folds a hot single-block self-loop's two-branch back-edge into one `b.cond body`, but
  **skips a loop with a store→later-load to the same address** (`loop_has_rmw_hazard`) — Apple Silicon
  replays stale store-forwarding there (~3.7× slowdown).

---

## 3. x86_64 → ARM64 codegen invariants (`translate/x86_64/`)

jit86 is a real decoder→emitter. Load-bearing flag/codegen facts (each is a fixed miscompile):

- **Flag substrate = ARM NZCV, with the C bit storing the INVERTED x86 CF** (borrow convention: stored
  C = NOT x86 CF). N=SF, Z=ZF, V=OF. Helpers `e_nzcv_save{,_ci,_c1}` spill with the right correction
  for sub/cmp (FL_SUB), add (FL_ADD, flip C), and logic (FL_LOGIC, CF=OF=0). Flags are **lazy**: live
  NZCV holds the result, spilled to `cpu->nzcv` only at a block boundary or when a non-Jcc reads them;
  a producer fully overwritten before any read is dropped (dead-flag elim).
- **PF and AF are DEDICATED substrate lanes now** (`cpu->pf`, `cpu->af`) — NOT aliased to the V flag (a
  former bug). Integer ALU ops stash the low result byte in `cpu->pf`; `e_pf_compute` computes
  even-parity from it for `setp/setnp/cmovp`. `cpu->af` holds `(a^b^result)` (AF = bit 4); logical ops
  store 0 (matches qemu CC_OP_LOGIC). `lahf/pushfq` read bit 4 / bit 2; `popfq/sahf` write them back.
- **Unordered FP compare** (`comisd/ucomisd`): ARM `FCMP` sets unordered as `Z=0,C=1,V=1`, but x86 needs
  `ZF=PF=CF=1,SF=0`. `e_nzcv_save_fcmp` applies the fixup **inline** after every FCMP: `Z|=V`, `C&=~V`
  (→ borrow=1 → CF=1), `N=0`, and the PF byte = `NOT V` (→ PF=1 when unordered).
- **Register-direct byte MOV** (0x88/0x8A mod=3) honors 8-bit width and remaps no-REX reg indices 4–7
  to **AH/CH/DH/BH** via `byte_val`/`byte_wb` (`is_hi8`); a full-width `e_mov_rr` here silently wrote
  the WRONG 64-bit register (`mov %al,%ch`→RBP) and clobbered upper bits. Fixed.
- **rep movs/stos** lower to a single host memcpy/memset, but `dd_rep_movs` keeps **element-granular
  forward-overlap smear** (when `dst<src<dst+n`) per x86 semantics; **cmps/scas** exit `R_REPSTR` to a C
  helper that writes the exact RCX/RSI/RDI + ZF/SF/CF/OF end-state (`NOREP=1`/`NOREPCMP=1` fall back).
- **x87** is an 8-slot ST stack at **double precision** (`cpu->st[]`; loses the 80-bit tail). A
  static-top optimization resolves `ST(i)` at translate time after `finit`. m80 `fld/fstp`
  (`R_X87FLD/FSTP`) and transcendentals (`R_X87FUNC` → host libm) exit to C. `fnstsw` reads C0–C3 set in
  `cpu->fpsw` by the FP-compare path.
- **AVX/AVX2/AVX-512 (`R_AVX`) and legacy 0F38/0F3A (`R_SSE3B`) are emulated in C**, correctness-first:
  each insn exits the block, `do_avx`/`do_sse3b` re-decode at `rip` and operate on the
  `v/vhi/vz/vx/kreg` register file, then advance `rip`. SSE/scalar fast paths stay inline NEON.
- **64-bit div/idiv** exit `R_DIV/R_IDIV` (ARM has no 128/64 divide), divisor in `cpu->divop`; `/0`
  raises a guest `#DE`→SIGFPE. **CPUID** exits `R_CPUID`. **MXCSR↔FPCR** rounding uses different bit
  encodings; `ldmxcsr/fldcw` translate RC→RMode, `cvttsd2si` stays truncate.

---

## 4. Untrusted-guest isolation — the sentry split (`os/linux/sentry.c`)

**Shipped, not stubbed**, but **opt-in and dormant by default**: everything is gated on `g_untrusted`
(`DDJIT_UNTRUSTED`). With the gate off (the entire test matrix) `service()` never calls
`syscall_route()` — byte-identical to baseline.

- **Trust boundary = process boundary.** The engine `fork()`s **after** loading the image: the parent
  becomes the **worker** (runs the JIT + guest code; holds only compute/memory authority) and the child
  becomes the **sentry** (owns the real fd table, rootfs/jail, network, `execve`; runs `service_local`).
  Under `DDJIT_SANDBOX` the worker is wrapped in a **deny-default macOS Seatbelt** profile
  (`deny default` + explicit `deny file-read-data/file-write*/network*/mach-lookup` — the last blocks
  the bootstrap/WindowServer escape). It **fails closed** (a worker that can't be confined must not run).
- **The RING** (`struct sentry_shm` → `sentry_ring[SENTRY_NRINGS=8]`): each ring is a mailbox with a
  strict atomic **turn ping-pong** (0=worker owns, 1=sentry owns; release/acquire), `a[6]` args, a
  `redir[6]` pointer-offset table, and a **1 MiB bounce `buf`**. Threads claim a private lane via a
  CAS free-list (≤8 threads → zero contention; beyond, overflow lanes serialize on a `busy` lock).
  Wakeup is **spin + `sched_yield`** (a futex/`__ulock` wake is a deferred perf item, not correctness).
- **The sentry never dereferences a guest pointer.** Only marshalled bytes cross. Pointer args carry a
  `redir` byte-offset into `buf`; the sentry **snapshots the redirect metadata into private locals**
  before validating (closes the validate-in-place TOCTOU — a racing worker can't rewrite a field
  between check and use), bounds-checks every offset, and rebases the register to a ring pointer. iovecs
  are copied into a private `iovec[]` the kernel reads.
- **Guest fds are virtual** (`sentry_proc` per-process tables, `vfd_alloc/real/drop`): a guest can only
  name an fd the sentry handed *it* — a raw integer matching a sentry-internal fd resolves to `-EBADF`.
  `fork` dup()s each inherited real fd into the child's table. `SCM_RIGHTS` translates guest↔real fds in
  the cmsg in place.
- **Local vs remote:** compute/memory/futex/clocks/TLS, plus `clone`/`execve`/`wait4`/anon+file `mmap`,
  stay local; the **fs/net/proc** family (read/write/open/stat/socket/poll/dup/fcntl/ioctl) is
  forwarded. **Limitations (roadmap, not correctness):** spin wakeup; eventfd2/timerfd/signalfd4/select/
  sendmmsg not yet forwarded (they'd make worker-local fds a forwarded read can't see); no Linux
  seccomp; no sentry-side policy enforcement (path allowlist / net egress) yet; cross-process fd
  isolation is best-effort (the `wpid` stamp is worker-supplied).

---

## 5. Container jail + networking (`os/linux/container/`)

It is a **userspace path-jail + overlay + virtual switch**, not kernel namespaces or cgroups.

- **Path confinement** is three-layered: `confine()` lexically clamps `..` at root; `confine_in()`
  realpaths the deepest existing parent and verifies the canonical result stays inside the jail (escape
  → `/.jail-escape-denied`); `resolve_at()` does a **TOCTOU-free** component walk on pinned dir-fds with
  a 40-hop symlink budget and a final `openat(O_NOFOLLOW)`. Symlinks resolve **rootfs-relative**, never
  host-relative.
- **Overlay**: a writable upper + up to 8 read-only lowers, with `.wh.` whiteouts and copy-up. Resolve
  searches upper then lowers top-down — a fresh container's upper is empty and the program lives only in
  a lower, so the ELF loader must search the FULL overlay (a bare-exec resolve bug).
- **`gmap`** (`vfs/gmap.c`, 8192 entries) tracks every guest mapping so `execve` (`gmap_reset_all`) can
  tear the inherited address space down before loading the new image — otherwise the new (non-PIE)
  image's baked low refs collide with the dense post-fork layout → SIGSEGV.
- **Networking (no TUN, no raw sockets, no root):**
  - **Egress works as host passthrough** — a guest `AF_INET` socket to a non-local dest IS a host
    socket (shares the host's IP/routes/DNS).
  - **Loopback isolation**: `127/8` `SOCK_STREAM` is swapped (`lo_swap`) to a per-container `AF_UNIX`
    socket at `$DD_NETNS/p<port>`; `getsockname` reports the virtual `127.0.0.1:port`, not the path.
  - **Inter-container L3 (a virtual switch, partially built):** with `DD_NETBR`+`DD_IP`, a guest
    `AF_INET` peer on the same `/16` is swapped to an `AF_UNIX` rendezvous at
    `/tmp/.ddbr-<netid>/<ip>:<port>`; `getsockname` reports the virtual `<ip>:port`. Egress to outside,
    and to the host, remains passthrough; the full userspace TCP/IP stack (`smoltcp`, real `eth0`/
    netlink/ICMP) is the unbuilt next arc (and `smoltcp` is unfetchable offline).
  - **Published ports** (`-p H:C`): a host-side forwarder thread (`fwd_listen_thread` TCP /
    `udp_fwd_thread` UDP) binds `0.0.0.0:H` and relays to the guest's `AF_UNIX` switch socket.
  - Translations: Linux↔macOS `sockaddr` (`sa_l2m`/`sa_m2l`, `NOSOCKADDR=1`), `termios`, and signal
    numbers all differ and are mapped. `TCP_CORK→TCP_NOPUSH` carries a warning (NOPUSH corks until close
    → breaks keepalive servers). **Limitations:** fixed 1024-fd arrays, 104-byte `sun_path` (hashed).

---

## 6. Syscall layer + signals/threads (`os/linux/`)

- **Errno boundary.** Each `svc_*` family tail-calls `svc_done()`, which applies the BSD→Linux
  `m2l_errno` map — so a sub-dispatched syscall is translated exactly once (the early-`return` errno
  bypass is avoided; `redirect` cases skip it entirely). EAGAIN 35↔11, ENOSYS 78↔38, etc.
- **macOS/BSD emulations:** `epoll`→kqueue (with a default deferred-changelist coalescing opt,
  `NOEPOLLOPT=1`); `eventfd`→pipe; `inotify`→kqueue + directory-snapshot diff; `signalfd`→self-pipe +
  `g_pending` scan. The **fscache** memoizes path-resolution + `openat`-resolution + stat/readlink/access
  by path, **epoch-invalidated** (`res_bump` on any FS mutation; no TTL) and fd-evicted on write; it is
  hard-reset under lock on fork to avoid COW-stale entries.
- **futex** uses 256 per-address hash buckets of `PTHREAD_PROCESS_SHARED` mutex+condvar (so a
  MAP_SHARED table lets a forked child wake parent threads). WAKE always takes the bucket mutex +
  broadcast — a lock-free skip lost wakeups on ARM's weak memory. CLEARTID-wake first checks the addr is
  still mapped (`mach_vm_region`) because a detached thread munmaps its own stack before exit.
- **Signals:** sync faults (ILL/TRAP/BUS/FPE/SEGV) are served by the engine's own guards and **never
  forwarded to the host** — and `rt_sigaction` must keep them installed (a parent restoring
  `SIGILL→SIG_DFL` left an exec'd child's CPU-feature-probe trap fatal). A sync fault does NOT build the
  guest frame in the host handler (on aarch64 host SP == guest SP → would clobber it); it marks pending
  for `maybe_deliver_signal` after the dispatcher's pc-advance. Async signals use `g_pending`
  (process-wide) + `cpu->tpending` (thread-directed via tkill/tgkill). Only the 3 stop-signals
  (TSTP/TTIN/TTOU) are mirrored to the real host mask (job control needs the kernel to see them).
- **Threads/identity:** `clone(CLONE_THREAD)`→host pthread on a copied `cpu`; **gettid** returns
  `container_pid()==1` for the init thread (`tid==0`) and a unique id per worker — load-bearing for Go's
  STW preemption (`tgkill` to a worker). Guest 4K vs host 16K pages: a partial `munmap` releases only
  whole host pages, keeping unaligned edges mapped.

---

## 7. Darwin targets (`os/darwin/`)

- **`jitdarwin.c`** — a same-ISA aarch64→aarch64 DBT (POC, single-threaded, 32 MB cache): translate
  blocks (synth exits at branches, copy other insns), intercept `svc #0x80` → `darwin_service()` (PID-1
  identity + a path-jail over volumes+rootfs on the BSD path syscalls). No VM.
- **`jail/jail.c`** — NOT a JIT: a DYLD-interpose jail for **native** arm64 binaries. Interposes the
  libc path/stat/exec family onto the rootfs+lowers+volumes, maps `getpid→1`/`gethostname→DD_HOSTNAME`,
  publishes ports, and uses the shared `container_parse.h` validator. It **strips
  `DYLD_INSERT_LIBRARIES` for arm64e** binaries (dyld refuses a non-arm64e interpose dylib).

---

## 8. Pending performance levers (designed, not shipped)

Core engine opts are landed (block chaining, IBTC, LSE atomic upgrade, fs-metadata cache, tier-2,
inline `clock_gettime`/`rt_sigprocmask`, path/openat caches, persistent pcache, fork-server — all in the
code, each with a `NO*` kill-switch; see [`DEBUGGING.md`](DEBUGGING.md) for the knob inventory).
Remaining designed-but-unshipped: **aarch64 SQLite parity** — bit-exact gated diffs, apply **in order
A3 → A1 → B1 → B2** (`arm-a3.diff`, `arm-a1.diff`, `arm-b1.diff` in this folder; rebuild + `make
test`/`make test-diff` after each; out-of-order breaks the bit-exactness gate); redis command-dispatch
hot path (~18× off native, CPU-bound); `rep cmps`/`scas` inline fast path; sub-ms startup (fork of the
large VM reservation). Iterative, not one-shot.


---

# dd-jit LAUNCH — the launch/env/bindings contract

## Launch is NOT an FFI call

The Rust binding does **not** dlopen/call the engine. `SpawnConfig::script()` (src/lib.rs) builds a
shell line and spawns `bash -lc`:

```
exec env  <K=V …>   <engine-binary>  <--flags …>   <guest argv …>
          └ config   └ ddjit-<target>  └ config       └ what the container runs
```

So the launch contract = **(env vars) + (CLI flags) + (argv)**. **Env is the only universal channel:**
the darwin jail is a DYLD-interposed dylib in the *native* guest process — there is no translator argv
to receive flags. So the container contract standardizes on env; flags are at most a thin CLI
convenience mapping onto the same env. One entry symbol per target (`dd_run`), one binding template.

## The two env classes — never mix them

| class | namespace | stability | consumed by |
|---|---|---|---|
| **container contract** | `DD_*` | public, stable | the engine (must NOT leak into the guest's env) |
| **engine tuning / debug** | `DDJIT_*` | internal, unstable, A/B kill-switches | the engine (see DEBUGGING.md) |

A front-end (daemon/CLI) sets only `DD_*`. The prefix tells a reader the class: `DD_` = container
config, `DDJIT_` = engine knob.

### `DD_*` — the container contract

| var | meaning | format |
|---|---|---|
| `DD_ROOTFS` | writable rootfs (overlay upper or plain) | path |
| `DD_LOWERS` | overlay read-only layers, highest first | `p1,p2,…` |
| `DD_VOLUMES` | bind mounts | `[ro:]guest:host,…` |
| `DD_PUBLISH` | port maps | `host:container,…` |
| `DD_NETNS` | private-loopback ns id (unset = shared host stack) | id |
| `DD_HOSTNAME` | UTS hostname | string |
| `DD_MEM_MAX` / `DD_PIDS_MAX` | limits (0 = unlimited; mem+pids via rlimit) | int |
| `DD_UID` / `DD_GID` | container uid/gid (default 0) | int |
| `DD_CWD` | initial working dir inside container | path |
| `DD_SANDBOX` | untrusted-guest isolation on | 0/1 |
| `DD_GUEST_ENV` | the guest process's OWN environment — the ONLY thing copied onto the guest stack | `K=V\nK=V\n…` |

### The guest-env boundary (prevents config leaks)

`DD_*` is engine config and must **not** appear in the guest's environment. The guest's own env arrives
as the single opaque block `DD_GUEST_ENV` and is the only thing placed on the guest stack
(`os/linux/elf.c`). Use one shared env-construction path for both frontends — never parse
`DD_GUEST_ENV`.

## Input validation — the C is the security boundary

The engine runs **untrusted guest images** and is invoked **directly** (human/docker `main()` CLI), not
only via the typed Rust binding — so it **must not trust its input** even if the binding is correct.
Every `DD_*` input:

1. **Parse with `strtol`+`errno`** (not `atoi`) → reject non-numeric / overflow.
2. **Range-check** (port ≤ 65535, uid/gid in range, sizes sane).
3. **Bound-check collections** → on cap overflow, **error out**, never silently drop.
4. **Fail fast:** print `dd: invalid DD_<X>=<v>` and exit nonzero — never coerce a bad value to a
   privileged default. (The footgun: `DD_UID=oops` → `atoi` → `0` = **root**.)

One shared validating parser for all targets. Classes: `DD_*` validated strictly; `DDJIT_*` dev knobs
(lax fine); `DD_GUEST_ENV` opaque pass-through, never parsed.


---

# dd-jit — debugging & profiling

Turn a vague report ("image X, command Y, hangs/crashes/wrong/slow") into a diagnosis.

## Two structural gaps to know

1. **One build flavor.** `build.rs` compiles only `clang -O2` (no `-g`/asserts/sanitizer); every
   diagnostic is a runtime `getenv` in the hot path. Target: a one-switch `DD_DEBUG` build
   (`-g`+asserts+offset `_Static_assert`s) with trace/dump points **compiled out** in release.
2. **Knobs don't reach the engine.** Launch is `exec env <K=V…> ddjit-<target> <argv>` (see
   [`architecture/LAUNCH.md`](architecture/LAUNCH.md)) — only vars baked into that prefix reach the
   engine; ambient env is dropped. `SpawnConfig::script()` emits only `DD_*`, so `DDJIT_*`/legacy
   knobs are invisible on the normal `dd run`/daemon path. Fix: forward allow-listed `DDJIT_*` into the
   launch prefix like `DD_*`. (Until then, legacy bare names — `JT`/`PROF`/`CRASHDBG`/`NO*` — work
   **only** when run against the engine binary directly.)

## Knob inventory (all runtime `getenv` today; → target `DDJIT_*` name)

| knob | effect | target |
|---|---|---|
| `JT` | `g_trace` per-block/dispatch trace, bounded by `g_tracecap` | `DDJIT_TRACE=block` |
| `CRASHDBG` | guest crash dump `[CRASH] sig= fault= pc=`, bytes@rip, GPRs; alt-stack + Mach exc port | `DDJIT_CRASHDUMP` |
| `PROF` | counter bundle at `exit_group`: crossings, syscalls, IBTC fills/miss, xlate_ms, shadow hit-rate, futex, tier-2 | `DDJIT_PROF` |
| `IBPROF` | indirect-branch / IBTC resolve traffic + stability | `DDJIT_PROF=ibranch` / `DDJIT_TRACE=ibranch` |
| `COLDPROF` `T2DUMP` `AVXTRACE` `LAZYDIAG/LAZYBUDGET/NOLAZYFIX` `DD_FAULTCOUNT` `DDFINIDBG` `DDJITD_DIAG` `DDEPOLLPROF` `JIT86_FASTSTAT` | cold-xlate / tier-2 bytes / AVX / lazy-fixup / non-PIE fault count / fini / forkserver / epoll / fastsys stats | (profile/debug) |
| **kill-switches** `NOSSEOPT NOREP NOREPCMP NOLAZY NOSTITCH NOTIER2 NOTIER2X NOIBTC NOGUESTFOLD NOFUTEXQ NODUALMAP NOSMC NOLSE NOSHADOWTUNE NOEAOPT NOX87OPT NOMTIBTC NOSTEAL1617 NOTMPFS NORWXFIX NOSOCKADDR NOEPOLLOPT NOFLAGELIDE NOGOREBASE NONPIE_NOFIXUP` | A/B-disable one optimization to bisect a miscompile | `DDJIT_NO=<opt,…>` |
| **tuning** `TIER2_THRESHOLD TIER2X_THRESHOLD SHADOWGATE S3DB_DURABILITY W4_NOOPENCACHE DD_NOPATHCACHE TIER2_SELFTEST` | thresholds / self-test | `DDJIT_*` |
| already-`DDJIT_*` `DDJIT_UNTRUSTED DDJIT_SANDBOX DDJIT_PCACHE DDJIT_PCACHE_DIR` | sentry / persistent code cache | keep |

Missing tools the link-hang/go-copystack bugs exposed: a **periodic guest-PC sampler** (localize a
100%-CPU spin) and a reachable indirect-branch log.

## Standard dumps — one tagged line `[<kind>] key=val …` to stderr, addrs in **guest** space

`[syscall-unimpl] nr= a0..a5= pc= tid=` · `[op-unimpl] arch= bytes= gpc= map/op/pp` (exit 70) ·
`[crash] sig= code= fault= pc=` + GPRs + faulting-block disasm + the guest map line covering pc/fault ·
`[syscall] nr= name= (…)= -> ret <us>` · `[block] gpc= reason= n=` · `[ibranch] src= tgt= hit/miss
fill=` (runaway indirect loop = link-spin signature) · `[sample] pc= block= n=` every N ms + an exit
top-N histogram (the missing tool for hang/spin) · `[dd] target= build= ver= flags=` banner.

## RUNBOOK — an external report came in

Inputs: image ref, exact argv, symptom, the `[dd]` banner if any.

```
DD_DEBUG=1 make engine-debug          # -g + asserts engine (once)
DDJIT_DEBUG=1 dd run <image> <cmd…>   # debug build + bundle (banner+crashdump+unimpl)
```

| symptom | knobs | look for |
|---|---|---|
| crash (SIGSEGV/139/255) | `DDJIT_CRASHDUMP=1` | `[crash]` pc + bytes@pc; is fault addr in a guest map? |
| hang / 100% CPU | `DDJIT_SAMPLE=10 DDJIT_TRACE=ibranch` | `[sample]` top block + `[ibranch]` runaway src→tgt loop |
| hang / 0% CPU | `DDJIT_TRACE=syscall` | last `[syscall]` with no return = stuck in futex/wait4/poll |
| wrong output | `DDJIT_NO=stitch` then `tier2,ibtc,lse,smc,guestfold` one at a time | which disabled opt fixes it = the miscompiler; cross-check `make test-diff` |
| missing syscall/opcode | (bundle on) | `[syscall-unimpl]`/`[op-unimpl]` gives nr/bytes + guest pc |
| perf | `DDJIT_PROF=1 DDJIT_SAMPLE=5` | `[prof]` area + `[sample]` histogram → dominant cost |

Collect one bundle to attach:
```
DDJIT_DEBUG=1 DDJIT_TRACE=block,syscall,ibranch DDJIT_SAMPLE=10 DDJIT_PROF=1 \
  dd run <image> <cmd…> 2> dd-diag.log
```
For a suspected race, rebuild `DD_SAN=thread make engine-tsan` and re-run under `make test-realsw`.
