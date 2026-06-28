# PLAN — remaining work

`dd` is a Cargo workspace: `dd-jit` (the JIT runtime + Rust bindings), `dd-daemon` (the Docker Engine
API), and the desktop surface (`dd-client`/`dd-gui`/`dd-cli`). **This file is the work list only — what
is missing or not yet implemented.** Validate via the Makefile: `make test` (cross-engine matrix, **~240
green** / 3 engines), `make test-docker[-full|-net]`, `make test-macos` (23/23), `make test-realsw`,
`make coverage` (syscall/opcode gap report).

## Large subsystems (priority order) — each has an executable design + first-PR roadmap in `docs/design/`

1. **jit86 engine dedup** → `docs/design/engine-dedup.md`. Lift the host `jit/` engine so the code cache +
   dispatcher are cpu-agnostic (the cpu struct + x86 decoder stay per-arch). **PR1 ✅** (x86 on shared
   `jit/cache.c`) + **PR2 ✅** (dispatch.c frontend-hook seam, aarch64 bit-identical). **Remaining: PR3/PR4**
   — x86 hook definitions + swap the x86 target onto `jit/dispatch.c`; also hook the 5th divergence (the
   per-block `g_trace` dump). Gate: matrix green both engines.
2. **Networking Phase-2b — netstack** → `docs/design/netstack.md`. External egress already works; the gap is
   L3 identity + reachability. **PR1 ✅** (daemon IPAM `172.18/12`, per-container IP, `--network` join →
   docker-net 5/7). **PR2 ✅** (br_* AF_UNIX switch + `/etc/hosts` reach-by-name DNS + fd-dup socket-metadata
   carry → **docker-net 7/7**, reach-by-name/ip + cross-network isolation). **Remaining:** only the optional
   in-process `smoltcp` stack behind `DD_NETSTACK`.
3. **Untrusted-guest isolation — sentry process-split** → `docs/design/sentry-split.md`. **No PR yet.** The
   trust boundary is one line (`run_guest`→`service(c)`); route it through `syscall_route(c)` on
   `g_untrusted`, split `service()` by authority (compute/mem local, fs/net/proc → sentry over an SPSC
   ring), deny-default Seatbelt. First PR: ring + read/write/open family behind the flag.
4. **x86 translator → native** → `docs/design/x86-perf.md`. **PR1 ✅** (lazy NZCV `sub/cmp→Jcc`), **extended ✅**
   (Opt3: add/and/or/xor/test producers + dead-flag elim), **`pmovmskb` ✅** (W3-B, 48→8 NEON). The tier-2
   substrate **landed** — arm adaptive tier-up (W4-E), stolen-register mangle elimination via trace-local
   allocation (W6-C, the former "tier-2 trace optimizer" subsystem), and the x86 tier-2 optimizer (W5-B).
   **Remaining:** carry-value consumers (adc/sbb deferral) + x87 `fptop` tracking.

## Deep bugs (root-caused; fixes scoped)

- **fork()+execve() non-PIE crash (victims: gcc toolchain; postgres' `gosu`) — PARTIAL fix landed (`NONPIE_NOFIXUP=1`).**
  `execve` biases a non-PIE `ET_EXEC` off its fixed low vaddr (`__PAGEZERO` forces it); its un-relocated
  absolute refs then fault. Landed (W6A-1): record the non-PIE `[lo,hi)+bias`, redirect absolute code jumps
  `+bias`, and a SIGSEGV fault-fixup of faulting integer ld/st at `si_addr+bias` (PIE untouched —
  `g_nonpie_lo==0`). **Remaining:** SIMD-Q/`ldr q` + LSE-atomic-RMW absolute forms, and syscall pointer-args
  into low `.rodata`/`.data`, aren't redirected (they take a clean abort, not silent corruption). The
  host-`__PAGEZERO`-shrink path stays a confirmed dead end (breaks PIE / the aarch64 host). `docs/design/fix-nonpie-crash.md`.

## Coverage gaps — syscalls

*Source: `make coverage`. Static **178/323 handled** at last snapshot (many since landed).*
- **Edge corners** (`edge` group; xfail-tracked): `mprotect` PROT_NONE is a no-op (no fault → RELRO/guard
  pages unenforced), `pipe2(O_DIRECT)` packet mode (host-limited — macOS pipes can't frame writes).
- **Host-limited (emulate or leave ENOSYS):** POSIX mqueue `mq_*`(180-185), `timer_create`/`timer_*`(107-111)
  *(could ride kqueue)*; `pidfd`/`io_uring`/NUMA/keyring/module/`ptrace` out of scope.

## Docker API — remaining field/behaviour fidelity (Engine API v1.43, single-node; Swarm out of scope)

| Area | What's left | Pri |
|------|-------------|:---:|
| `docker exec` | `--privileged` (rare; `-e`/`-w`/`-d`/`-u` done) | P3 |
| `docker run` opts | wider `HostConfig`: restart policy, `--cap-add`, `--device`, `--mount`, `--privileged` (`--user`/`--label` done) | P2 |
| `docker build` | BuildKit/layer cache (every build re-runs from base) — args/target/nocache/labels/digest-IDs done | P2 |
| `docker cp` | non-default-driver named volumes (default `<volumes_dir>/<name>` handled) | P3 |

## Platform limitations (macOS host — can't provide the Linux primitive; off the work-list)
Non-PIE `ET_EXEC` fixed-vaddr (the `__PAGEZERO` low-4GB reservation — see the deep bug above for the
achievable workaround), cpu/io throttling (no cpu/io cgroup; mem+pids *are* enforced via rlimit), `pidfd`,
`io_uring`. These would come for free on a linux→linux build.

## Portability matrix (seams exist; only darwin-host / both-guests built)
`hal/<os>` (darwin built; linux = mprotect/SIGSEGV-ucontext/seccomp), `jit/emit_<isa>` (arm64 built),
`frontend/<arch>` (aarch64 + x86_64 built), `os/<os>` (linux built). Eventual targets: linux→darwin,
darwin→linux, linux→linux, darwin→darwin.
