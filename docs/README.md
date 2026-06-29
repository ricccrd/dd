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

## Design subplans

- [design/](design/) — per-topic subplans + gated bit-exact diffs (arm-a1/a3/b1 SQLite levers, sentry-security, netstack, x86-perf, engine-dedup, fix-postgres, test-coverage).
- [ideas/](ideas/) — forward-looking, not-yet-built: [RENDERING.md](ideas/RENDERING.md) + [RENDERING_PLAN.md](ideas/RENDERING_PLAN.md) (GUI display layer).

## Testing

- [testing/CHARTER.md](testing/CHARTER.md) — coverage charter (the goal/contract).
- [testing/TESTING.md](testing/TESTING.md) — how to run the two surfaces × two run-classes × three targets.
- [testing/IMAGE-MANIFEST.md](testing/IMAGE-MANIFEST.md) — curated real-software Docker image recipes + deterministic markers.
