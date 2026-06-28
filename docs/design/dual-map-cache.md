# DESIGN — dual-mapped RX/RW code cache (the robust fork+exec fix)

Status: **design, ready to implement** (no source edited). Date 2026-06-27.
Companion to `docs/design/research-busybox-crash.md` (the root-cause) — this is **fix #3** from
that doc's ranked list: eliminate the W^X/`MAP_JIT` execute-permission bug *class* by putting
execute permission in the page tables instead of in per-thread APRR state.

Scope: the **shared** engine cache `dd-jit/src/runtime/jit/cache.c` and the two emitters/dispatchers
that drive it. Engine-dedup PR1 has landed, so **both** the aarch64 engine
(`targets/linux_aarch64.c` → `jit/dispatch.c`) and the x86 engine
(`targets/linux_x86_64.c` → `frontend/x86_64/dispatch.c`) `#include "../jit/cache.c"`. This change
affects both; design it once in the shared file.

---

## 0. The bug in one paragraph (recap)

`research-busybox-crash.md` captured an **instruction-abort permission fault**
(`esr=0x8200000f`, EC 0x20 / IFSC 0x0F) at the host PC, inside the 64 MB `MAP_JIT` cache, in a
`fork()` child resuming `run_block()` (guest context `ld-musl _Fork+0x30`). The cache's *execute*
permission on Apple Silicon is gated **per-thread** by APRR, toggled via
`pthread_jit_write_protect_np()` (`jit/dispatch.c:67,75,77,86`, the IBTC per-site fill in
`frontend/aarch64/dispatch_hooks.h:37,40`, and the x86 mirror `frontend/x86_64/dispatch.c:48,52,55,58`).
That per-thread bit is **not reliable across `fork()`**, so the child's first instruction fetch from
the cache faults. The one-line re-assert mitigation (candidate #1) is in but insufficient under load.

**The fix:** stop relying on per-thread APRR. Map the cache's physical pages **twice** — an **RX**
view we execute from and a **RW** alias we write through during translation. Execute permission then
lives in the page tables (the RX mapping), which `fork()` inherits like any other mapping, for every
thread, with no APRR dependency. The `pthread_jit_write_protect_np` toggles are deleted.

---

## 1. The macOS mechanism — two VAs over the same physical pages

### 1.1 What we need

Two virtual mappings of the **same** physical pages:

* **RX view** (`g_cache`): `VM_PROT_READ | VM_PROT_EXECUTE`. Executed from. Never written.
* **RW alias** (`g_cache_rw`): `VM_PROT_READ | VM_PROT_WRITE`. Written through during translate /
  chain / IBTC fill. Never executed.

Per-process, established **once at startup before any guest thread spawns**. Inherited by `fork()`
through the page tables.

### 1.2 Primary mechanism: `mach_vm_remap()` of the existing `MAP_JIT` region

This is the minimal-delta path — it keeps the current `MAP_JIT` allocation as the RX view and adds a
second VA aliasing the same pages, set to RW. It is the production-proven recipe on Apple Silicon
(used by Dolphin, .NET/CoreCLR, Unicorn, ChakraCore for exactly this purpose).

Replace the current single `mmap` (`targets/linux_aarch64.c:293`, `targets/linux_x86_64.c:140`) with
a shared `cache_alloc()` (new, in `jit/cache.c`) doing:

```c
#include <mach/mach.h>        // already in linux_aarch64.c:28; ADD to linux_x86_64.c (or to cache.c)
#include <mach/mach_vm.h>     // mach_vm_remap / mach_vm_protect prototypes

// 1. RX view: the MAP_JIT region, executable, NO write bit (we never write through it).
g_cache = mmap(NULL, CACHE_SZ, PROT_READ | PROT_EXEC,
               MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
if (g_cache == MAP_FAILED) { perror("mmap jit"); return 1; }

// 2. RW alias: a second VA mapping the SAME physical pages (copy=FALSE => share, not copy).
mach_vm_address_t rw = 0;
vm_prot_t cur = 0, max = 0;
kern_return_t kr = mach_vm_remap(
    mach_task_self(), &rw, CACHE_SZ, /*mask=*/0,
    VM_FLAGS_ANYWHERE,
    mach_task_self(), (mach_vm_address_t)g_cache,
    /*copy=*/FALSE,
    &cur, &max,
    /*inheritance=*/VM_INHERIT_COPY);     // child gets its own COW pair; see §3
if (kr != KERN_SUCCESS) { /* fallback: g_cache_rw = g_cache; see §4 rollout PR-A */ }

// 3. Drop the alias to RW (the MAP_JIT source carries max_protection RWX, so this is permitted).
kr = mach_vm_protect(mach_task_self(), rw, CACHE_SZ, /*set_maximum=*/FALSE,
                     VM_PROT_READ | VM_PROT_WRITE);

g_cache_rw  = (uint8_t *)rw;
g_rw_delta  = g_cache_rw - g_cache;       // signed byte offset RX->RW; the whole trick (§2)
g_cp        = g_cache;                     // bump pointer stays RX-canonical (§2)
```

Notes that make this *actually work* on Apple Silicon:

* The source region is `MAP_JIT` created under `com.apple.security.cs.allow-jit` (already our only
  entitlement — `dd-jit/jit.entitlements`), so its `max_protection` is RWX. `mach_vm_remap` inherits
  that max, which is why step 3's `mach_vm_protect(... RW)` succeeds. **No new entitlement is
  required.**
* The RX view is mapped **without** `PROT_WRITE`. We never call `pthread_jit_write_protect_np` on it
  again, so its thread's APRR stays in the default execute-allow state — and, crucially, *execute
  permission is page-table state, not APRR state*, so the child inherits it.
* Writes go through `g_cache_rw`, a **plain** RW region (not JIT-attributed) — no APRR gate, so a
  `fork()` child can write the cache during translation without any per-thread toggle.
* `sys_icache_invalidate` after writes is still mandatory — see §4 (Apple cores are PIPT, so cleaning
  by the RX VA reaches the same physical lines we wrote via the RW VA).

### 1.3 Alternative mechanism: named memory entry + double `vm_map`

Equivalent result, more code, and drops `MAP_JIT` entirely:

```c
mach_vm_size_t sz = CACHE_SZ;
mem_entry_name_port_t obj = MACH_PORT_NULL;
mach_make_memory_entry_64(mach_task_self(), &sz, 0,
    MAP_MEM_NAMED_CREATE | VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXECUTE,
    &obj, MACH_PORT_NULL);
// map RX
mach_vm_map(mach_task_self(), &rx, sz, 0, VM_FLAGS_ANYWHERE, obj, 0, FALSE,
            VM_PROT_READ|VM_PROT_EXECUTE, VM_PROT_READ|VM_PROT_EXECUTE, VM_INHERIT_COPY);
// map RW (same object)
mach_vm_map(mach_task_self(), &rw, sz, 0, VM_FLAGS_ANYWHERE, obj, 0, FALSE,
            VM_PROT_READ|VM_PROT_WRITE,  VM_PROT_READ|VM_PROT_WRITE,  VM_INHERIT_COPY);
```

This needs `com.apple.security.cs.allow-unsigned-executable-memory` (because the executable mapping
is **not** `MAP_JIT`) — a new entitlement in `jit.entitlements`. **Recommendation: use 1.2
(`mach_vm_remap`)**, not this — it keeps the existing `MAP_JIT`/entitlement and is a smaller, more
reversible diff. Document 1.3 only as the fallback if `mach_vm_remap` of `MAP_JIT` ever proves
unreliable on a future OS.

---

## 2. The `cache.c` changes — route writes through the RW alias

### 2.1 New state + the one helper (in `jit/cache.c`, next to `g_cache`/`g_cp` at line 7)

```c
static uint8_t *g_cache, *g_cp;     // RX view (executed) + RX-canonical bump pointer  [unchanged]
static uint8_t *g_cache_rw;         // RW alias base (same physical pages, writable)
static ptrdiff_t g_rw_delta;        // g_cache_rw - g_cache; 0 => fallback, writes hit RX (rollout PR-A)

// Map an RX cache address to its writable alias. ALL cache stores go through this.
static inline void *cw(const void *rx) { return (uint8_t *)rx + g_rw_delta; }
// Convenience for the common 4-byte / 8-byte stores:
static inline void cw32(void *rx, uint32_t v) { *(uint32_t *)cw(rx) = v; }
static inline void cw64(void *rx, uint64_t v) { *(uint64_t *)cw(rx) = v; }
```

**Design invariant (Strategy A — "RX-canonical"):** `g_cp`, `g_cache`, `map_put` `host`/`body`, every
`uint32_t *patch`, every `Lxxx` label, the IBTC `body`, and the dispatcher's `code` **all stay RX
addresses, exactly as today.** Nothing about *addressing or branch-displacement math changes* — those
are correct for execution from the RX view. The **only** change is that the physical **store**
targets `cw(addr)` instead of `addr`. This keeps the diff mechanical and keeps §B (below) correct for
free, because all branch displacements are self-relative deltas that are invariant under the constant
`g_rw_delta` offset.

`cache_alloc()` (the §1.2 body) also moves into `jit/cache.c` and is called from both targets,
replacing the two duplicated `mmap` blocks.

### 2.2 Every write site that must switch to the RW alias

**A. Block emission — the emitters.** Both `emit32`s write `*(uint32_t*)g_cp`:

* `jit/emit_arm64.c:5-8` (`emit32`) — aarch64 host emitter.
* `frontend/x86_64/emit.c:7-10` (`emit32`) — x86 engine's arm64 host emitter.

```c
static void emit32(uint32_t in) { cw32(g_cp, in); g_cp += 4; }   // was: *(uint32_t*)g_cp = in;
```
`g_cp` stays RX; only the store is redirected. Every `e_*` emitter funnels through `emit32`, so this
one line per file covers the bulk of translation.

**B. In-block backpatches in aarch64 `frontend/aarch64/translate.c`** — direct `*p =` stores into the
cache for forward labels, IC/IBTC stubs, and literal slots. Each becomes `cw32`/`cw64`:

| line | current | change |
|------|---------|--------|
| 164  | `*p_full = …`     | `cw32(p_full, …)` |
| 264  | `*p_adr = …`      | `cw32(p_adr, …)`  |
| 314,316,318 | `*p_cbz/*p_cb1/*p_cb2 = …` | `cw32(p_cbz, …)` etc. |
| 364,367 | `*(uint64_t*)g_cp = 0` (IC literal slots) | `cw64((uint64_t*)g_cp, 0)` |
| 369,370,371,373 | `*p_ldrt/*p_cbnz/*p_ldrb/*p_adr = …` | `cw32(…)` |
| 694,724,735,766,778 | `*patch = …` (b.cond/cbz/tbz fixups) | `cw32(patch, …)` |
| 894  | `*defer[i].patch = …` (exclusive-region exit fixups) | `cw32(defer[i].patch, …)` |

The displacement math on each of these (`(g_cp - patch)/4`, `(Lxxx - p)/4`) is **unchanged** — both
operands stay RX, the delta is correct for execution.

**C. In-block backpatches in x86 `frontend/x86_64/translate.c`:**

| line | current | change |
|------|---------|--------|
| 695  | `*patch = … cbz …`   | `cw32(patch, …)` |
| 723  | `*patch = … b.cond …`| `cw32(patch, …)` |
| 1844 | `*patch = … b.cond …`| `cw32(patch, …)` |

**D. Inter-block chaining backpatch — `patch_links_to` (shared `jit/cache.c:95-108`).** This is the
fork-relevant one (chaining a peer block to a freshly translated target):

```c
// was: *g_pend[i].slot = (is_bl ? 0x94000000u : 0x14000000u) | ((uint32_t)d & 0x3FFFFFFu);
cw32(g_pend[i].slot, (g_pend[i].is_bl ? 0x94000000u : 0x14000000u) | ((uint32_t)d & 0x3FFFFFFu));
sys_icache_invalidate(g_pend[i].slot, 4);   // RX address — unchanged, still required (§4)
```
`d = (body - slot)/4` with both RX — unchanged. `g_pend[i].slot` and `body` remain RX. (The x86 engine
calls the **same** shared `patch_links_to`; `translate.c:1907` `if (!g_threaded) patch_links_to(...)`.)

**E. IBTC per-site monomorphic literal fill — `frontend/aarch64/dispatch_hooks.h:36-40`.** This writes
two 64-bit literals *embedded in emitted code* (`ic_site` points into the cache):

```c
if ((c)->ic_site != 1) {                          // per-site monomorphic IC (literals in JIT cache)
    cw64(&((uint64_t *)(c)->ic_site)[1], (uint64_t)bd - 8);  // Lsite_body
    cw64(&((uint64_t *)(c)->ic_site)[0], (c)->pc);           // Lsite_tgt
}
```
The two `pthread_jit_write_protect_np(0/1)` calls around it (`:37,40`) are **deleted**. Note the stored
`bd - 8` (the IBTC `body_ind`) stays an **RX** address — it is branched to at runtime and must be
executable. The shared `g_ibtc[h].body`/`.target` stores (`:34-35`) are plain data, unchanged.
The x86 IBTC fill (`frontend/x86_64/dispatch.c:61-73`) writes only the plain-data `g_ibtc` table (no
in-cache literals), so it needs no `cw` change — just its W^X toggles removed (§2.3).

**F. The cache flush.** On wholesale flush the dispatcher resets `g_cp = g_cache` and `memset`s the
*side tables* (`g_map`, `g_ibtc`) — those are plain globals, **not** the cache, so they take no `cw`.
There is currently **no** memset of the cache *contents* (a flush just rewinds the bump pointer and
lets new blocks overwrite). If we ever choose to scrub stale code on flush (defensive), that memset
must target `cw(g_cache)`:  `memset(cw(g_cache), 0, CACHE_SZ)`. The flush's
`pthread_jit_write_protect_np(0/1)` pair (`jit/dispatch.c:67,75` and x86 `dispatch.c:48,52`) is
**deleted** — the bump-pointer reset and side-table memsets never touch JIT-attributed memory.

### 2.3 Remove the W^X toggles (dispatchers)

With every write going through the RW alias, the cache is never executed in write mode and never
written in execute mode by construction. Delete all `pthread_jit_write_protect_np` calls:

* `jit/dispatch.c:67,75` (flush) and `:77,86` (translate + `patch_links_to`).
* `frontend/aarch64/dispatch_hooks.h:37,40` (IBTC per-site fill).
* `frontend/x86_64/dispatch.c:48,52,55,58` (the x86 mirror).

The existing `sys_icache_invalidate(g_emit_start, g_cp - g_emit_start)` (`jit/dispatch.c:82`,
`frontend/x86_64/dispatch.c:59`) **stays** — see §4.

---

## 3. Why this fixes `fork()` (and unlocks `soak/smc`)

* **Execute permission is in the page tables, not APRR.** The RX view is mapped `PROT_EXEC` once at
  startup. `fork()` duplicates the address space — the child inherits the RX mapping (and the RW
  alias) with their protections via COW page tables. The child's first post-`fork` `run_block()`
  fetches from an `EXEC` page **regardless of any per-thread state**, so the EC 0x20 / IFSC 0x0F
  instruction-abort observed in `research-busybox-crash.md` cannot occur. The bug *class* — every
  path that "assumes but never asserts" execute mode (`run_guest()` cache hit, the `service.c:220`
  fork child) — is gone, not just the one observed window.
* **No per-thread toggle to get wrong.** There is no `pthread_jit_write_protect_np` left to be in the
  wrong state across `fork`, `pthread_create`, signal entry, or a cache *hit*. The load-dependent race
  has no surface left.
* **`soak/smc` (self-modifying guest JIT pages) becomes serviceable.** PLAN.md's `soak/smc` gap was
  that the engine needed guest pages to be RWX. The dual-map removes the need for *host* RWX, and the
  same RX/RW-alias technique is the principled way to host guest-writable+executable regions: writes
  land via the RW alias, execution via RX, with `sys_icache_invalidate` between — no RWX page ever
  exists. (Wiring guest SMC pages through this is follow-up work, but the mechanism is now present.)

---

## 4. Risks + safe rollout

### 4.1 Risks and how each is handled

* **icache/dcache coherence — still required.** Writing via the RW alias does **not** make the new
  bytes visible to the RX instruction stream by itself. Apple Silicon caches are PIPT, so the existing
  `sys_icache_invalidate(g_emit_start, …)` (after translate, `jit/dispatch.c:82`) and the per-slot
  `sys_icache_invalidate(g_pend[i].slot, 4)` (in `patch_links_to`) **must stay**, addressed by the
  **RX** VA (they clean D-cache to PoU + invalidate I-cache for the same physical lines we wrote via
  RW). This is unchanged from today; do **not** remove it when removing the W^X toggles. The ordering
  contract in `run_guest()` (`jit/dispatch.c:81-86`: icache-invalidate the new block *before*
  `patch_links_to` chains peers to it) is preserved verbatim.

* **§B host-`bl` shadow stack — correct for free under Strategy A.** `emit_shadow_push`
  (`frontend/aarch64/translate.c:135`) emits `adr x3, Lcont` so the *runtime* host_ret is computed
  PC-relative **from the executing RX view**; it is stored into `cpu->sstk` as an **RX** address and
  later branched to. Because Strategy A keeps `g_cp`/labels RX and only redirects the store, the
  emitted `adr` and its backpatched offset are unchanged, so the shadow stack keeps holding executable
  RX addresses. `G_SHADOW_CLEAR`/`G_SHADOW_RESET` (`dispatch_hooks.h:22`, `abi.h:39`) are plain `ssp`
  resets — untouched. No host_ret is ever computed from the RW alias.

* **Thread-safety — strictly improved.** `g_cache_rw`/`g_rw_delta` are set once before threads spawn
  and never mutated, so they need no lock. Cache mutation stays serialized under `g_jit_lock`
  (`run_guest` `jit/dispatch.c:58`). Removing the process-wide `pthread_jit_write_protect_np` toggle
  removes a real hazard the code already flagged: `dispatch_hooks.h:30` notes the per-site toggle
  *"toggles W^X process-wide -> both race peers"*. The dual-map deletes that cross-thread window.

* **Engine-dedup interaction (both engines).** The alloc + helper + `patch_links_to` change live in
  the shared `jit/cache.c` and are picked up by both targets automatically. The per-engine edits are
  symmetric and must land together: `emit32` (×2: `jit/emit_arm64.c`, `frontend/x86_64/emit.c`),
  in-block backpatches (`frontend/aarch64/translate.c`, `frontend/x86_64/translate.c`), IBTC fill
  (`dispatch_hooks.h` aarch64; `frontend/x86_64/dispatch.c` x86), and the W^X-toggle removal in **both**
  dispatchers (`jit/dispatch.c`, `frontend/x86_64/dispatch.c`). Add `#include <mach/mach.h>` +
  `<mach/mach_vm.h>` where `cache_alloc()` ends up (aarch64 target already has `mach/mach.h:28`; the
  x86 target does not — easiest is to include them in `jit/cache.c` itself so both unity TUs get them).

* **`mach_vm_remap` fork-coherence is the one empirical unknown.** The remaining question is whether,
  after `fork()`, the child's two VAs still alias *each other* over the child's COW-private copy
  (so a write via the child's RW alias is seen by the child's RX view). This is exactly what the gate
  repro validates — and the phased rollout keeps the old W^X path alive until it's proven.

### 4.2 Phased rollout (de-risked, revertible)

**PR-A — add the dual map + route writes, keep W^X as fallback (this PR).**

1. Add `g_cache_rw`/`g_rw_delta`/`cw`/`cw32`/`cw64` and `cache_alloc()` to `jit/cache.c`; call it from
   both targets in place of the raw `mmap`.
2. In PR-A, keep the RX view mapped **RWX** (as today) and **keep** every
   `pthread_jit_write_protect_np` call. On `mach_vm_remap` **success**, `g_rw_delta != 0` and writes
   land via the RW alias (the toggles still fire but are now redundant/harmless). On **failure**, set
   `g_cache_rw = g_cache`, `g_rw_delta = 0` — `cw` is identity and behavior is **bit-identical to
   today**. This makes PR-A safe to land even if the remap is rejected on some host.
3. Route every write site (§2.2 A–F) through `cw`. This is mechanical and independently reviewable.
4. **Gate:** the full cross-engine matrix stays green (~236), **and** the concurrent fork+exec busybox
   repro from `research-busybox-crash.md` (`target/rb-scratch/`, 8–16 procs × several rounds under
   `CRASHDBG=1`) shows the `[CRASH] sig=…` / `[REGION]` instruction-abort **gone** — verified with the
   W^X toggle **forced off** (e.g. via a debug env knob, or temporarily stubbing
   `pthread_jit_write_protect_np` to no-op) so the test isolates the alias as the load-bearing fix.

**PR-B — drop to RX, delete the toggles (follow-up, after PR-A bakes).**

5. Flip the RX view's `mmap` from `PROT_READ|WRITE|EXEC` to `PROT_READ|EXEC` (so the cache is never
   writable through the executed VA), and **delete** all `pthread_jit_write_protect_np` calls (§2.3).
6. Re-run the same gate (matrix + concurrent busybox + `soak/forkchurn`). With the toggles gone, a
   regression now fails loudly rather than silently falling back — which is why it is a separate PR.

This sequencing means a defect in the alias mechanism in PR-A still has the per-thread W^X path live
underneath it, and PR-B only removes that safety net once the alias is proven under the exact load
that produced the original crash.

---

## 5. File/function index (for the implementer)

* `jit/cache.c:5-9` — `CACHE_SZ`, `g_cache`, `g_cp`, `g_emit_start`. Add `g_cache_rw`, `g_rw_delta`,
  `cw`/`cw32`/`cw64`, `cache_alloc()`.
* `jit/cache.c:95-108` — `patch_links_to` (chaining backpatch) → `cw32` (§2.2-D).
* `jit/dispatch.c:67,75,77,86` — flush + translate W^X toggles → **delete** (PR-B); `:82`
  `sys_icache_invalidate` → **keep**.
* `jit/emit_arm64.c:5-8` — `emit32` → `cw32` (§2.2-A).
* `frontend/aarch64/translate.c` — in-block backpatches at 164, 264, 314/316/318, 364/367, 369-373,
  694, 724, 735, 766, 778, 894 → `cw32`/`cw64` (§2.2-B).
* `frontend/aarch64/dispatch_hooks.h:36-40` — IBTC per-site literal fill → `cw64`, delete toggles
  (§2.2-E).
* `frontend/x86_64/emit.c:7-10` — `emit32` → `cw32` (§2.2-A).
* `frontend/x86_64/translate.c:695,723,1844` — in-block backpatches → `cw32` (§2.2-C).
* `frontend/x86_64/dispatch.c:48-59` — flush/translate W^X toggles → **delete**;
  `:59` `sys_icache_invalidate` → **keep**.
* `targets/linux_aarch64.c:293` and `targets/linux_x86_64.c:140` — the two `mmap(... MAP_JIT)` calls →
  replace with `cache_alloc()`. Add `#include <mach/mach.h>`+`<mach/mach_vm.h>` (aarch64 already has
  `mach/mach.h:28`; x86 lacks both) — preferably in `jit/cache.c`.
* `dd-jit/jit.entitlements` — **unchanged** for the `mach_vm_remap` path (§1.2); only the §1.3
  fallback would need `com.apple.security.cs.allow-unsigned-executable-memory`.
* Gate repro + diagnostics: `target/rb-scratch/` (driver, `[REGION]` ESR probe, `CRASHDBG`).
```
