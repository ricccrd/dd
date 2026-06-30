# dd — docs

`dd` runs Linux/macOS containers on arm64 macOS by JIT-translating the guest — no VM.
Workspace: `dd-jit` (engine + bindings), `dd-daemon` (Docker API), `ddcli`/`dd-gui`.

- **[BUGS.md](BUGS.md)** — open bugs + next-work plan. Start here.
- [STATUS.md](STATUS.md) — live test-lane gaps / harness facts.

**Architecture / ops**
- [architecture/ARCHITECTURE.md](architecture/ARCHITECTURE.md) — mental model + `G_*`/dispatch contract.
- [architecture/LAUNCH.md](architecture/LAUNCH.md) — launch/env/bindings contract.
- [architecture/OPTIMIZATIONS.md](architecture/OPTIMIZATIONS.md) · [PERFORMANCE.md](PERFORMANCE.md) — engine opt design + knobs.
- [DEBUGGING.md](DEBUGGING.md) — `DDJIT_*` knobs, dumps, bug-report runbook.

**Design references** (how SHIPPED mechanisms work — read before touching that area; not TODOs)
- [design/nonpie-pagezero.md](design/nonpie-pagezero.md) — `__PAGEZERO`/non-PIE bias-fold + the permanent platform limitation. **Highest future-conflict area.**
- [design/DIAGNOSIS.md](design/DIAGNOSIS.md) — jit86 codegen invariants.
- [design/sentry-split.md](design/sentry-split.md) — untrusted-guest trust boundary.
- [design/fix-nonpie-crash.md](design/fix-nonpie-crash.md) — a rejected non-PIE approach (don't retry).
- [design/netstack.md](design/netstack.md) — forward smoltcp plan (not built).

**Testing** — [testing/TESTING.md](testing/TESTING.md) (how to run) · [testing/SYSCALL-COVERAGE.md](testing/SYSCALL-COVERAGE.md) · [testing/IMAGE-MANIFEST.md](testing/IMAGE-MANIFEST.md) · [testing/CHARTER.md](testing/CHARTER.md)

**[ideas/](ideas/)** — not yet built (RENDERING).
