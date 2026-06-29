# dd — documentation

`dd` runs Linux (and macOS) containers on an arm64 macOS host by **JIT-translating** the guest
binary — **no VM**. A Cargo workspace: `dd-jit` (runtime + Rust bindings), `dd-daemon` (Docker
Engine API), and the desktop surface (`ddcli` / `dd-gui`). This folder is the single home for all
authored design/plan docs (component `README.md`s stay with their crates; the published
website lives under `website/docs/`).

## Start here

- **[PLAN.md](PLAN.md)** — THE plan. PART A = open bugs / what's missing or failing; PART B = the phased refactor. Read this first.
- **[STATUS.md](STATUS.md)** — live test-lane gap inventory: coverage gaps, xfail rechecks/clears, harness facts, bug protocol.

## Architecture & refactor

- [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) — the mental model + interface (`G_*`/dispatch) contracts.
- [architecture/REFACTOR.md](architecture/REFACTOR.md) — boundaries, ownership, the incremental move order (rationale behind PLAN PART B).
- [architecture/TREE.md](architecture/TREE.md) — the final target folder tree + per-step move-map.
- [architecture/LAUNCH.md](architecture/LAUNCH.md) — the one launch/env/bindings contract.
- [architecture/OPTIMIZATIONS.md](architecture/OPTIMIZATIONS.md) — engine-internal optimization design (chaining, IBTC, §B, register-stealing).
- [PERFORMANCE.md](PERFORMANCE.md) — developer-facing perf defaults & knobs.

## Design & mechanism references

`design/` documents **how implemented mechanisms work** — read the relevant one *before*
touching that area so you don't reintroduce a fixed bug or break an invariant. These are
**reference docs for SHIPPED behavior, not pending work** — pending work lives only in
[PLAN.md](PLAN.md). (Don't treat a "we should…" sentence here as a TODO; it's history.)

- [design/nonpie-pagezero.md](design/nonpie-pagezero.md) — **`__PAGEZERO` / non-PIE guest_base bias-fold** (SHIPPED). Why non-PIE `ET_EXEC` is mapped high + per-access biased, the strict `[lo,hi)` span-gate, and the permanent platform limitation. **Read before touching the non-PIE path in `translate.c`/`elf.c`** — this is the highest future-conflict area.
- [design/DIAGNOSIS.md](design/DIAGNOSIS.md) — root-cause + invariants of the original jit86 (x86_64) codegen bugs (SHIPPED reference).
- [design/sentry-split.md](design/sentry-split.md) — the untrusted-guest sentry split design (SHIPPED).
- [design/fix-nonpie-crash.md](design/fix-nonpie-crash.md) — a **rejected** non-PIE approach + why it fails (so it isn't retried).
- [design/w6a-deepbugs.md](design/w6a-deepbugs.md), [design/w6b-sentry.md](design/w6b-sentry.md) — deep-bug cluster + sentry PoC notes (SHIPPED).
- other subplans (some still forward-looking): arm-a1/a3/b1 SQLite levers, sentry-security, netstack, x86-perf, engine-dedup, fix-postgres, test-coverage.

## Ideas (not yet built)

- [ideas/](ideas/) — [RENDERING.md](ideas/RENDERING.md) + [RENDERING_PLAN.md](ideas/RENDERING_PLAN.md) (GUI display layer).

## Testing

- [testing/CHARTER.md](testing/CHARTER.md) — coverage charter (the goal/contract).
- [testing/TESTING.md](testing/TESTING.md) — how to run the two surfaces × two run-classes × three targets.
- [testing/IMAGE-MANIFEST.md](testing/IMAGE-MANIFEST.md) — curated real-software Docker image recipes + deterministic markers.
