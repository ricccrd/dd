# How to test dd

dd is tested on **two surfaces**, in **two run-classes**, across **three targets**. This document is the
operator's guide: what to run for daily development vs. full compatibility, and how the pieces fit.
See `CHARTER.md` for *why* (the goal/requirements) and `GAPS.md` for the live list of known breakage.

## The two surfaces

| Surface         | What it proves                                   | Lives in                         | Driver |
|-----------------|--------------------------------------------------|----------------------------------|--------|
| **Basics**      | harness, compiling, libc, syscalls, codegen/ABI  | `dd-tests/src/cases/` (Rust)     | `cargo run -p dd-tests` |
| **Real software** | popular images run real workloads through the JIT via the real daemon | `dd-tests/src/scenarios/` (Rust) | `cargo run -p dd-tests --bin scenarios` |

Both surfaces are **Rust** — one harness, uniform reporting, countable and filterable. No bash.

## The three targets

`arm` = linux/arm64 · `amd` = linux/amd64 · `mac` = darwin/arm64. **Linux parity is mandatory** — every
Linux case runs on **both** `arm` and `amd`. `mac` is lighter-touch.

## The two run-classes

| Class   | For            | Cost           | Images                       |
|---------|----------------|----------------|------------------------------|
| **quick** | development  | seconds–minutes | cache-only (offline-skip; never pulls) |
| **long**  | compatibility | minutes–hours  | pulls the full popular-image set       |

---

## Quick start (development loop)

```bash
make jit                       # build/codesign the 3 JIT engines + crates (once)

# Basics — fast, every engine, golden/oracle checked:
cargo run -p dd-tests                    # whole basics matrix
cargo run -p dd-tests -- libc            # one group (name filter)
cargo run -p dd-tests -- -e arm64        # one target
cargo run -p dd-tests -- -e x86_64 net   # one group on one target

# Real software — QUICK class (cache-only, both Linux arches, no big pulls):
cargo run -p dd-tests --bin scenarios                    # all categories, quick
cargo run -p dd-tests --bin scenarios -- -c databases -t arm   # one category, one arch — fastest signal
cargo run -p dd-tests --bin scenarios -- --count         # list every case + total (runs nothing)
```

`quick` reuses whatever images are already in `DD_IMAGES` and **skips** anything not present, so it never
blocks on the network and stays light on a resource-constrained box.

## Full compatibility run (CI / nightly)

```bash
cargo run -p dd-tests --bin scenarios -- --long              # pull the popular-image set, heavy workloads, both arches
cargo run -p dd-tests --bin scenarios -- --long -c languages # one category, full
make test                                                    # basics matrix (Makefile wrapper)
```

The `long` class is the real compatibility confrontation: it pulls postgres/redis/node/gcc/distros/… and
runs developer workflows (including `docker exec -it /bin/bash` shell sessions) on `arm` **and** `amd`.

---

## Reading results

Each case reports one of: `✓ pass` · `✗ fail` (a real, unexpected break — **fails the gate**) ·
`x xfail` (a **known** engine gap, tracked, does not fail the gate) · `✓! XPASS` (an xfail that now
passes — the engine was fixed; drop the marker) · `· skip` (image absent / target n/a).

**Known engine bugs are written as real tests and marked `xfail`** on the affected target. The gate
stays green; the gap is documented in `GAPS.md`; XPASS auto-alerts the moment the engine lane fixes it.

## When a test finds a bug

Do **not** debug the engine inside a test. Instead:
1. Mark the case `xfail <id> <target> "<one-line reason>"` so the gate stays green.
2. Add a row to `GAPS.md` (id, target, symptom, suspected area).
3. A dedicated **diagnostic agent** is spawned to root-cause it. Tests only document; they never fix the
   dd-jit engine C (see `CHARTER.md`).

---

## Layout

```
dd-tests/
  src/
    lib.rs                  # Basics surface: the Engine × Case runner (universal API for basics)
    cases/                  # Basics: one module per category; all() aggregates them
    scenario.rs             # Real-software framework: Target/Scenario/Daemon/run_one (universal API)
    scenarios/              # Real-software: one module per category (distros, languages, databases, …)
    bin/scenarios.rs        # the real-software runner (boots ONE daemon, the entry point)
  docs/
    CHARTER.md              # goal + all requirements (the contract)
    TESTING.md              # this file
    GAPS.md                 # live inventory of crashes / missing / divergences + owning agent
```

## Authoring a real-software case (the universal API, in Rust)

```rust
// src/scenarios/databases.rs  → registered in src/scenarios/mod.rs::all()
use crate::scenario::{scen, sgroup, ScenGroup, Target};

pub fn group() -> ScenGroup {
    sgroup("databases", vec![
        // Runs on BOTH Linux arches by default; `exec` is the `docker exec -it /bin/sh` user path.
        scen("databases/redis-ping", "redis:alpine")
            .exec("redis-server --save '' --daemonize yes; sleep 1; redis-cli ping")
            .has("PONG")
            .xfail(&[Target::ArmLinux]),   // known fork+exec engine gap — see GAPS.md
    ])
}
```

Rules: stable `id` = `"category/name"`; pin output (no clocks/random); deterministic; `.exec(...)` for the
developer-at-a-shell path, `.run(&[...])` for one-shot; `.xfail(&[Target::…])` for a known engine gap;
`.long()` for heavy/large-pull cases; `.only(&[...])`/`.plus_mac()` to adjust targets.
