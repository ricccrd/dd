# dd performance — JIT tuning for the everyday developer

`dd` runs containers by **JIT-translating** the guest (x86-64 / arm64 Linux) to the arm64 macOS host —
there is **no VM**. The defaults are chosen so the everyday developer gets the fast path with **zero
configuration**: just run `docker run` / `docker build` against `dd-daemon` and it's fast.

This file documents what's on, what the daemon manages for you, and the few advanced knobs.

---

## On by default — transparent, bit-exact (you get these automatically)

The daemon's JIT applies all of these to **every** container, with **byte-identical** output to a plain
run. No flags, no opt-in. They are measured wins from the optimization sweep, each matrix-gated:

| Area | Win |
|------|-----|
| String/SIMD (musl libc) | SSE2 `pmovmskb`→NEON (strcmp 3.2×, strlen 2.7×); `rep movs/stos`→`memcpy`/`memset` (2.5×) |
| Codegen | memory-addressing fast path (grep −13%); lazy flags (b_int −22%); trace/superblock (x86 + arm); adaptive tier-up + **stolen-register mangle elimination** (arm hot loops up to 3.3×); x86 tier-2 |
| Syscalls | inline `clock_gettime`/`gettimeofday` via `CNTVCT` (7.6×); inline `rt_sigprocmask` (4.3×); `epoll`→`kqueue` batching (14–15× fewer kqueue calls) |
| Filesystem | path-resolution cache (`stat` 40×); `openat` cache (4.3×) |

Each can be individually disabled with its `NO*` env (`NOSSEOPT`, `NOLAZY`, `NOSTITCH`, `NOEAOPT`,
`NOEPOLLOPT`, `DD_NOPATHCACHE`, `W4_NOOPENCACHE`, `JIT86_NOFASTSYS`, `JIT86_NOSIGINLINE`, `NOTIER2`,
`NOTIER2X`) — **for debugging/A-B only**; you never need to.

---

## Managed for you by the daemon — the container cold-start wins

### Persistent code cache (on by default for containers)
The 2nd+ run of an image **skips translation entirely** (~−40% cold start). The daemon enables it for
every container (`spawn_cfg` sets `DDJIT_PCACHE=1` + a cache dir under the dd home, `~/.dd/pcache`).

- **Safe by construction:** the cache key is `engine-version + cpu-struct-size + image/interp bases +
  hash(dev/ino/size/mtime of the binary)`. So it auto-invalidates when you rebuild an image or update dd,
  and a corrupt/truncated cache gracefully misses + rebuilds. **It can never serve stale or wrong code**,
  so you never have to clear it for correctness.
- **Disk management:** it shows up in `docker system df` and is cleared by `docker system prune` — the
  same commands you already use. (Disable daemon-wide with `DD_PCACHE=0` when starting the daemon.)

### Warm fork-server (opt-in)
A resident `ddjitd` can prewarm once and `fork()` an exec-less worker per launch, pushing repeated
short-lived launches toward **sub-millisecond**. This is a runtime mode (not per-run); the daemon can
drive it for hot images.

---

## Advanced knobs — genuine tradeoffs (opt-in only)

These are **off / safe by default** because they trade correctness or behavior for speed — the default is
the right choice for everyday work:

- **fsync durability** — `S3DB_DURABILITY=fast|none|strict` (default **`fast`** = today's plain `fsync`,
  crash-safe). `none` = no fsync (ephemeral/CI throwaway containers, sqlite 2.9×, **not** host-crash
  durable); `strict` = `F_FULLFSYNC` (real power-loss durability, ~160× slower per fsync). Leave it on
  `fast` unless you specifically want one of the tradeoffs.
- **threading** — the dual-mapped code cache + threaded IBTC fill are single-thread byte-identical and on
  by default; `NODUALMAP=1` reverts to the legacy per-thread W^X path if ever needed.

---

## TL;DR for the everyday developer

You don't configure anything. Containers run fast: every transparent codegen/syscall win is on, repeated
runs of an image skip translation via a self-managing cache, and `docker system df`/`prune` show and clear
that cache like any other. The only knob you might ever reach for is `S3DB_DURABILITY=none` if you want a
throwaway/CI container to skip fsync for speed.
