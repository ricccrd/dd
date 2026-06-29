# dd-jit — debugging & profiling (the consolidated guide)

How to turn a vague external bug report ("image X, command Y, it hangs/crashes/is wrong/slow")
into a reproducible diagnosis. This is the **target** design plus the **runbook** for what exists
today. Pending work is one PLAN PART B phase (*Observability/debug-build consolidation*); this doc
is its spec. Read [`architecture/LAUNCH.md`](architecture/LAUNCH.md) first — the env-forwarding
reality below is the core thing to fix.

---

## 0. The core problem (read this first)

Two structural gaps make the engine hard to debug today:

1. **No debug build.** `build.rs` compiles **one** flavor: `clang -O2`, no `-g`, no
   `_Static_assert`, no `assert()`, no sanitizer. There is **zero** compile-time gating — every
   diagnostic is a runtime `getenv()` check compiled into the hot path (e.g. `if (g_trace)` per
   dispatch). So you cannot ship a "production" binary with diagnostics compiled OUT, nor a "debug"
   binary with assertions/symbols/sanitizers compiled IN. One binary, all compromises.
2. **The knobs don't reach the engine.** Launch is **not** an FFI call — the binding does
   `exec env <K=V …> ddjit-<target> <argv>` (LAUNCH.md). Only vars **baked into that prefix** reach
   the engine; ambient env is dropped. `SpawnConfig::script()` emits only `DD_*` (container
   contract), so `JT`/`PROF`/`NO*`/`DDJIT_*` are invisible on the normal `cargo run`/daemon path.
   Today you must run the engine binary directly, or hand-edit the launch line, to use any knob.
   (See the memory note "mac bridge drops env": `mac bash -lc` does not forward ambient env either.)

Everything below fixes both: **a one-switch debug build** + **a `DDJIT_*` namespace that the binding
forwards**.

---

## 1. Inventory — what exists today (grep, not guess)

All gated at **runtime via `getenv`**; none compile-time. (`getenv` results are mostly cached into a
`g_*` flag once, but the trace/PROF/CRASHDBG print sites are always compiled in.)

| knob | g_var / effect | gating | class today → target |
|---|---|---|---|
| `JT` | `g_trace` — per-block/dispatch trace, `[jit86]`/`[sig]` lines, bounded by `g_tracecap` | runtime | debug → `DDJIT_TRACE=block` |
| `CRASHDBG` | guest crash dump: `[FAULT]`/`[CRASH] sig= fault= pc=`, bytes@rip, GPRs; installs `diag_crash` + alt-stack + Mach exc port | runtime | debug → `DDJIT_CRASHDUMP` |
| `PROF` | `g_prof` — counter bundle at `exit_group`: crossings, syscalls, IBTC fills/miss, translations, xlate_ms, shadow-return hit-rate, futex, tier-2 | runtime | profile → `DDJIT_PROF` |
| `IBPROF` | `g_ibprof` — indirect-branch / IBTC resolve traffic + stability log | runtime | profile → `DDJIT_PROF=ibranch` |
| `COLDPROF` | `g_coldprof` — cold-translation profiling | runtime | profile |
| `T2DUMP` | dump tier-2 block host bytes (`[t2dump]`) | runtime | debug |
| `AVXTRACE` | VEX/EVEX decode trace | runtime | debug |
| `LAZYDIAG`/`LAZYBUDGET`/`NOLAZYFIX` | lazy zero-page fixup diag / budget / disable | runtime | debug+tune |
| `DD_FAULTCOUNT` | non-PIE per-access fault counter (`nonpie_guard_count`) | runtime | profile |
| `DDFINIDBG`,`DDJITD_DIAG`,`DDEPOLLPROF`,`JIT86_FASTSTAT` | fini / forkserver / epoll / fastsys stats | runtime | profile |
| `UNIMPL` sites | `report_unimpl` (x86, exit 70), `[avx]/[sse3b] UNIMPLEMENTED map/op/rip`, `[jit86] aborting at rip marker` | always on | dump (standardize) |
| **kill-switches** `NOSSEOPT NOREP NOREPCMP NOLAZY NOSTITCH NOTIER2 NOTIER2X NOIBTC/IBTC1WAY NOGUESTFOLD NOFUTEXQ NODUALMAP NOSMC NOLSE NOSHADOWTUNE NOEAOPT NOX87OPT NOMTIBTC NOSTEAL1617 NOTMPFS NORWXFIX NOSOCKADDR NOEPOLLOPT NOFLAGELIDE NOGOREBASE NONPIE_NOFIXUP` | A/B-disable one optimization to bisect a miscompile | runtime | tune → `DDJIT_NO=<opt,…>` |
| **tuning** `TIER2_THRESHOLD TIER2X_THRESHOLD SHADOWGATE S3DB_DURABILITY W4_NOOPENCACHE DD_NOPATHCACHE TIER2_SELFTEST` | thresholds / self-test | runtime | tune |
| **already `DDJIT_*`** `DDJIT_UNTRUSTED DDJIT_SANDBOX DDJIT_PCACHE DDJIT_PCACHE_DIR` | sentry / persistent code cache | runtime | keep |
| **legacy `JIT86_*`** `JIT86_NOFASTSYS JIT86_NOSIGINLINE JIT86_NETNS JIT86_VOL …` | x86 dup of `DD_*`/knobs | runtime | retire (LAUNCH.md) |

**Bottom line:** the mechanisms mostly exist but are (a) ad-hoc-named across three schemes
(bare / `JIT86_*` / `DDJIT_*`), (b) all runtime-gated with no compiled-out prod path, and (c)
**not forwarded** by the binding. Two gaps the link-hang & go-copystack bugs exposed: there is **no
periodic guest-PC sampler** (to localize a 100%-CPU spin without hand-instrumenting) and the
indirect-branch log (`IBPROF`) is undocumented and unreachable via the normal path.

---

## 2. Build profiles — one switch (`DD_DEBUG`)

Make `build.rs` honor a single cflag flavor selected by a cargo feature / env:

```
ddjit-release  (default)   clang -O2 -DNDEBUG            trace/dump points COMPILED OUT (zero cost)
ddjit-debug    DD_DEBUG=1  clang -O1 -g -DDD_DEBUG       asserts + offset _Static_asserts + trace IN, symbols
ddjit-asan     DD_SAN=addr clang -g -fsanitize=address,undefined        engine-C memory/UB bugs
ddjit-tsan     DD_SAN=thread clang -g -fsanitize=thread                 threaded-JIT races (run under test-realsw)
```

Rules:
- **Hot path is zero-cost in release.** Wrap trace/sampler/PROF print sites in
  `#ifdef DD_DEBUG` (or a `DDBG(...)` macro that expands to nothing in release) — *compiled out*,
  not merely `if (g_trace)`-gated. The `getenv`-cached `g_*` flags stay for runtime on/off **within**
  the debug build.
- **Asserts + the cpu-offset `_Static_assert`s** (PLAN PHASE 2) live behind `DD_DEBUG`/no-`NDEBUG`
  so a wrong `OFF_*` fails the **debug build**, never a guest. Sanitizers see only engine C, never
  guest memory (note in the matrix); valgrind is out (MAP_JIT SMC).
- **Expressed as ONE switch:** `DD_DEBUG=1 cargo build` (build.rs reads the env, picks the cflag
  set, codesigns the same way). `make engine-debug` / `make engine-asan` / `make engine-tsan`
  wrappers. The Rust binding picks `ddjit-<target>-debug` when `DDJIT_DEBUG=1` is set at run time.

---

## 3. Standard dumps — one format for "what we need"

Every diagnostic emits a **single tagged line** `[<kind>] key=val …` to stderr (machine-greppable),
guest addresses in **guest space**. The bundle (DDJIT_DEBUG=1) turns the relevant set on.

| dump | trigger | emits |
|---|---|---|
| **missing syscall** | unhandled nr | `[syscall-unimpl] nr=<n> a0..a5=<…> pc=<guest> tid=<n>` (then the existing ENOSYS) |
| **UNIMPL opcode** | `report_unimpl`/avx | `[op-unimpl] arch=<x86/arm> bytes=<hex…> gpc=<guest> map/op/pp` (replaces the 3 ad-hoc spellings; exit 70) |
| **crash dump** | SIGSEGV/BUS/ILL/FPE | `[crash] sig=<n> code=<n> fault=<addr> pc=<guest>` + GPRs/flags + faulting-block disasm + the guest map line covering pc & fault (extend today's `CRASHDBG`/`diag_crash`) |
| **syscall trace** (strace-like) | `DDJIT_TRACE=syscall` | `[syscall] nr=<n> name=<…> (a0,…)=<…> -> <ret> <us>` per call |
| **block/dispatch trace** | `DDJIT_TRACE=block` | `[block] gpc=<guest> reason=<…> n=<seq>` (today's `JT`; keep `g_tracecap`) |
| **indirect-branch / IBTC log** | `DDJIT_TRACE=ibranch` | `[ibranch] src=<gpc> tgt=<gpc> hit/miss fill=<n>` (promote today's `IBPROF`) — **localizes link-spin / runaway indirect loops** |
| **guest-PC sampler** | `DDJIT_SAMPLE=<ms>` | a SIGALRM/timer thread prints `[sample] pc=<guest> block=<gpc> n=<count>` every N ms; on exit a top-N histogram — **the missing tool for 100%-CPU hang/spin bugs** (link-hang, go-copystack) |
| **engine banner** | always (1 line) | `[dd] target=<…> build=<release/debug/asan> ver=<git> flags=<DDJIT_* set>` — goes in every bundle |

---

## 4. One knob namespace that reaches the engine

Per LAUNCH.md: **`DD_*` = container contract (public)**, **`DDJIT_*` = engine dev knobs
(unstable)**. Fold the bare/`JIT86_*` names under `DDJIT_*`:

| use | knob |
|---|---|
| umbrella debug bundle | `DDJIT_DEBUG=1` → crashdump + op/syscall-unimpl + banner + (debug build) asserts |
| tracing (comma list) | `DDJIT_TRACE=block,syscall,ibranch,avx` (was `JT`,`AVXTRACE`,`IBPROF`) |
| sampler | `DDJIT_SAMPLE=10` (ms) |
| crash dump only | `DDJIT_CRASHDUMP=1` (was `CRASHDBG`) |
| profiler | `DDJIT_PROF=1` / `DDJIT_PROF=ibranch,sys,tier2` (was `PROF`,`COLDPROF`,`DD_FAULTCOUNT`) |
| disable an opt (bisect) | `DDJIT_NO=stitch,tier2,ibtc,lse,smc,guestfold,…` (one parser, was the `NO*` zoo) |
| tuning | `DDJIT_TIER2_THRESHOLD=`, `DDJIT_LAZYBUDGET=`, … |

**Forwarding (the fix that makes them usable):** extend `SpawnConfig` so any `DDJIT_*` set in the
caller's environment (or a `--debug`/`DDJIT_DEBUG`) is copied into the `exec env …` launch prefix —
exactly like `DD_*`. One allow-listed prefix copy in `script()`. Then a normal
`DDJIT_TRACE=block,syscall dd run <image> <cmd>` reproduces with full diagnostics; the daemon path
honors the same env. Keep back-compat readers for the old bare names during migration (PLAN step).

---

## 5. Profiling — the consolidated profiler

`DDJIT_PROF=<areas>` (default = all) prints one `[prof] …` block at `exit_group`; areas map to the
existing `g_prof_*` counters:

| area | reports (already collected) |
|---|---|
| `dispatch` | dispatcher round-trips/crossings, IBTC fills/miss (on/off) |
| `ibranch` | indirect-branch traffic + stability (`g_ibprof`) |
| `sys` | syscall counts + timing (extend the `g_prof_sys` path with per-nr count/ns) |
| `xlate` | translations, tier-2 promotions, fold-elide, xlate_ms (cold via `COLDPROF`) |
| `cache` | code-cache occupancy / flushes / W^X toggles / dualmap |
| `shadow` | §B shadow-return hit-rate, bl_shadow/bl_leaf split |
| `futex` | fast/slow wakeups, waits, queue depth |
| `sample` | guest-PC histogram top-N (from §3 sampler) — the hot-block report |

How to read: hit-rate low + high IBTC miss ⇒ indirect-branch-bound (redis dispatch); high xlate_ms +
fault count ⇒ translation/non-PIE bound; the `sample` histogram names the hot guest block directly.

---

## 6. RUNBOOK — an external report came in

**Inputs you need from the reporter:** image ref, exact command/argv, symptom, and the engine banner
(`[dd] …`) if they have any stderr.

**Step 0 — reproduce on a debug engine:**
```
DD_DEBUG=1 make engine-debug              # build the -g + asserts engine once
DDJIT_DEBUG=1 dd run <image> <cmd…>       # debug build + standard bundle (banner+crashdump+unimpl)
```

**Step 1 — pick knobs by symptom class:**

| symptom | knobs | what to look for |
|---|---|---|
| **crash** (SIGSEGV/exit 139/255) | `DDJIT_CRASHDUMP=1` | `[crash]` pc + bytes@pc + faulting block; is fault addr in a guest map? |
| **hang / 100% CPU spin** | `DDJIT_SAMPLE=10 DDJIT_TRACE=ibranch` | `[sample]` top block + `[ibranch]` runaway src→tgt loop (the link-hang/go-copystack signature) |
| **hang / 0% CPU (blocked)** | `DDJIT_TRACE=syscall` | last `[syscall]` with no return = stuck in futex/wait4/poll |
| **wrong output** | `DDJIT_NO=stitch` then `tier2,ibtc,lse,smc,guestfold` one at a time | which disabled opt makes output correct = the miscompiling opt; cross-check `make test-diff` |
| **missing syscall / opcode** | (bundle on) | `[syscall-unimpl]`/`[op-unimpl]` line gives nr/bytes + guest pc to implement |
| **perf** | `DDJIT_PROF=1 DDJIT_SAMPLE=5` | `[prof]` area + `[sample]` histogram → the dominant cost |

**Step 2 — collect ONE diagnostic bundle to attach:**
```
DDJIT_DEBUG=1 DDJIT_TRACE=block,syscall,ibranch DDJIT_SAMPLE=10 DDJIT_PROF=1 \
  dd run <image> <cmd…> 2> dd-diag.log
# dd-diag.log now has: [dd] banner (target+build+ver+flags), the trace, any [crash]/[*-unimpl],
# the [sample] histogram, and the [prof] block.
```
Attach `dd-diag.log` + the image ref + exact argv. The banner line pins engine version/build flags
so the repro is unambiguous. For a suspected race, rebuild `DD_SAN=thread make engine-tsan` and
re-run under `make test-realsw`.

---

## 7. Open work

Tracked as PLAN PART B → *Observability/debug-build consolidation*. Until it lands, the legacy bare
names (`JT`/`PROF`/`CRASHDBG`/`NO*`) still work **only when run against the engine binary directly**
(they are not forwarded by `dd run`/the daemon).
</content>
</invoke>
