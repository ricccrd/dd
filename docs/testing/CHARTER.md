# dd-tests Coverage Charter

The single source of truth for the test-coverage expansion. Every builder/category agent reads this
before writing a case.

## Mission

Build test coverage **broad and real enough to catch any compatibility bug in dd**, where the thing
under test is the **JIT engine and everything it does** (instruction translation, syscall emulation,
fork/exec, threads, memory, edge corners). The Docker daemon is **not** the primary target — it is the
**vehicle** that pushes real, popular software through the JIT under realistic conditions.

## Targets (the 3 arches)

| Target     | Engine          | Coverage level |
|------------|-----------------|----------------|
| arm-linux  | `LinuxAarch64`  | **full**       |
| amd64-linux| `LinuxX86_64`   | **full**       |
| arm-mac    | `DarwinAarch64` | lighter-touch  |

**Linux parity is mandatory:** every Linux test runs on **both amd64 and arm64** (same case, both
arches) so any architectural divergence is caught. macOS does **not** need the same breadth.

## Requirements (all of them)

1. **≥1000 distinct test cases** (each runs on every target it applies to → ~2–3× executions).
2. **Two surfaces:**
   - **Basics** — harness sanity, compiling, libc, syscalls, compiled C, codegen/ABI: prove things work
     at the fundamental level. (Rust harness `dd-tests`.)
   - **Real software via the real daemon** — boot `dd-daemon`, pull the **most popular images**, and run
     **real developer workflows** against them: distros (ubuntu/debian/alpine/fedora/rocky/arch, many
     versions), language runtimes (python, node, ruby, go, java, php, rust, perl), databases (postgres,
     mysql/mariadb, redis/valkey, mongo, nats, etcd, memcached, sqlite), web servers (nginx/httpd/caddy),
     toolchains (gcc/clang), utilities (busybox/curl/git/…). Prove parity is on par across arches.
3. **User perspective is first-class:** drive containers the way a developer does —
   `docker exec -it <c> /bin/bash` — and build coverage on top of that interactive-shell path.
4. **Categories.** Tests are organised into manageable categories, one owner (agent) per category.
5. **Two run-classes, documented:**
   - **quick** — fast, cache-only, for **development** iteration (no big pulls).
   - **long** — full **compatibility** confrontation (pulls many images, heavy workloads); CI/nightly.
   - `TESTING.md` documents exactly how to run each, per target.
6. **Resource-sensitive.** Running everything must stay light: **one daemon per run**, **lazy/cached
   image pulls**, **offline-skip** in quick mode, **per-case timeouts**, **always clean up**. A dev must
   be able to run a single category/target fast.
7. **Universal API.** One clean, uniform way to run a command against any target (`run_on`/`exec_it`/
   `both` + `want_*`), so cases are consistent, **replicable**, and ideally **deterministic** (pin
   output; avoid clocks/random).
8. **Well structured & countable.** A `--count`/list mode enumerates every case without running, to
   prove the ≥1000 target cheaply; filtering by category/class/target.
9. **xfail-tracked gaps, green gate.** Known engine bugs are written as real tests but marked `xfail`
   on the affected target — the gate stays green, the gap is documented, and **XPASS auto-alerts** the
   moment an engine fix lands.

## Division of labour (strict)

- **This orchestrator only WRITES TESTS and DOCUMENTS** what is missing / crashing / not-ok.
- **It does NOT fix the engine.** Project memory is explicit: do not edit dd-jit engine C in this lane.
- **Every time a test surfaces a bug → spawn a dedicated diagnostic agent** to root-cause it and record
  the finding in `GAPS.md`. Builder agents never stop to debug the engine themselves.
- **Keep delegating; do not stop** until coverage is broad. Idle capacity = launch the next category or
  diagnostic agent.

## Definition of done

`--list`/count (basics) + scenario count ≥ **1000** distinct cases · full run **green** (no *unexpected*
failure; known engine bugs xfail-tracked) · **all 3 targets** exercised · real images **actually pulled
and executed** (never mocked) · `quick` and `long` both documented and runnable · `GAPS.md` enumerates
every crash/missing/divergence with an owning diagnostic agent.
