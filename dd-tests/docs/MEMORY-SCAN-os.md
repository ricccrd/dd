# Memory-leak audit — dd Linux syscall-emulation + daemon

READ-ONLY audit. No engine source was modified. Scope: `dd-jit/src/runtime/os/linux/`
(service.c + service/, fscache.c, elf.c, thread.c, signal.c, sentry.c, container/) and
the Rust daemon `dd-daemon/`.

Goal: per-operation host-memory allocations (per syscall / fork / exec / open fd / container /
connection) that are NOT freed or NOT bounded, so a long-lived engine's RSS grows.

Legend:
- **GUEST-VISIBLE** = shows up in the guest's own RSS / getrusage (guest-charged memory).
- **ENGINE-INTERNAL** = only the host engine/daemon process RSS or host fd table grows; invisible to the guest.

---

## Verdict on the two named leads

### LEAD 1 — fscache.c `mc_` stat cache: **REFUTED (bounded)**

`fscache.c:111-117` — the metadata cache is a **fixed-size, statically-allocated, direct-mapped
array**, not a growing structure:

```c
#define MCACHE_N 8192
static struct mcent { uint64_t hash; char path[192]; int rc; struct stat st; } g_mc[MCACHE_N];
```

- `mc_store()` (`fscache.c:142-154`) writes into slot `mc_hash(p) & (MCACHE_N-1)`. A colliding
  path **overwrites** the existing slot (`strcpy(e->path, p)` into the same fixed `char path[192]`).
  There is no `malloc` per entry and no linked overflow chain.
- Total footprint is constant: `8192 * sizeof(mcent)` ≈ 8192 × ~360 B ≈ **~2.9 MB, allocated once
  in BSS**. It cannot grow with the number of distinct paths stat'd.
- The lead's premise ("no TTL, evicts only by exact path → accumulates forever") is **half true and
  harmless for memory**: yes there is no TTL and `mc_evict()` is path-precise, but a brand-new
  distinct path does not *add* an entry — it *replaces* whichever path currently hashes to its slot.
  So distinct-path pressure causes cache **thrashing / lower hit-rate**, never RSS growth.
- Same conclusion for every sibling cache, all fixed BSS arrays: `g_rl[2048]` (readlink,
  `fscache.c:169`), `g_ac[2048]` (access, `:209`), `g_rc[RCACHE_N=8192]` (path-resolution, `:255-260`),
  `g_oc[OCACHE_N=8192]` (openat-resolution, `:336-341`), `g_fdpath[1024][192]` (fd→path).
- The functional staleness risk in `g_rc`/`g_oc` is handled by `g_res_epoch` invalidation
  (`fscache.c:275`) and `rc_reset()` on fork (`:282`).

**Memory verdict: not a leak. Bounded at a constant ~2.9 MB regardless of workload.**

### LEAD 2 — execve teardown (elf.c / service.c case 221): **REFUTED (teardown present)**

execve **does** tear down the old guest address space before loading the new image:

- Every guest mapping is registered in a tracking registry as it is created:
  `gmap_add()` is called for the ELF image+interp (`elf.c:463`), the stack (`elf.c:508`),
  the post-exec heap (`service.c:2103`), and every guest `mmap` (`service/mem.c:128, :207`).
- `case 221` (execve, `service.c:2016`) calls **`gmap_reset_all()` at `service.c:2076`**, which
  `munmap`s **every** tracked mapping (`container/vfs/gmap.c:28-31`) *before* `load_elf` runs.
- The temporary host-heap copies execve makes of the path/argv (it must copy them out of the
  guest memory it is about to munmap) are freed on the success path:
  `strdup` at `service.c:2071,2074` → `free` at `service.c:2107-2109`.

So there is no per-exec leak of the previous guest image/heap/stack. Per-exec leaked size = **0**
under normal operation.

**Caveat (bounded, pre-existing, acknowledged in-code):** the registry is a fixed array
`g_gmap[GMAP_N=8192]` (`container/vfs/gmap.c:9-10`). `gmap_add` silently drops adds past 8192
(`:13`). A guest that holds >8192 live mappings would leave the overflow ones **untracked**, so
`gmap_reset_all` at the next execve would NOT munmap them → they leak across that exec. The comment
at `gmap.c:9` documents exactly this (the cap was already raised 1024→8192 for "a heavy guest"). This
is GUEST-VISIBLE address space and requires >8192 simultaneous mappings to trigger; low likelihood.

**Memory verdict: not a per-exec leak. Teardown is correct; only the >8192-mapping overflow corner leaks.**

---

## Confirmed candidates (ranked by likelihood × impact)

### C1 — getdents `g_dirs[64]` DIR* cache leaks a `DIR*` + dup'd host fd per call once full · MEDIUM-HIGH

- **file:line:** `service.c:1362-1379` (cache lookup/insert), cache decl `service/helpers.c:95-96`,
  drop on close `service/helpers.c:97-99` + `service.c:1306`.
- **allocated:** `fdopendir(dup(fd))` — a libc `DIR` stream (internal buffer, typically 32 KB+) **and**
  a duplicated host fd, on every getdents64 that misses the cache.
- **hot path:** `getdents64` (syscall 61) on a directory fd that is not already cached.
- **why leaked/unbounded:** the open `DIR` is only stored (and thus only later closed by
  `dirs_drop` on `close()`) **if `g_ndirs < 64`** (`service.c:1374`). Once 64 directory fds are
  cached simultaneously, the `if` is false: the freshly `fdopendir(dup(fd))`'d stream is used for
  this one call and then **neither stored nor closed**. Because it was never stored, the *next*
  getdents on the same fd misses again and leaks *another* `DIR*`+fd. A single large directory read
  (many getdents calls until EOF) under a full cache leaks one `DIR*`+fd *per call*. Also a latent
  correctness bug: the uncached stream restarts from offset 0 each call.
- **classification:** ENGINE-INTERNAL (host RSS + host fd table; leads to EMFILE before OOM).
- **workload:** a guest that keeps **>64 directory streams open simultaneously** and reads them
  (e.g. a recursive `nftw`/`find`-style traversal that `opendir`s many directories before closing
  them, or a server holding many `DIR*`), then issues `getdents` on a 65th+ directory in a loop. Each
  getdents call past the cap leaks ~32 KB + 1 fd. Reproduce: `for d in $(seq 1 200); do ...keep 200
  opendir fds, then ls a large dir...`.
- **confidence:** High that the code path leaks; Medium that real workloads hit >64 concurrent dir fds.

### C2 — inotify directory snapshot string not freed on `close()` / `inotify_rm_watch` · MEDIUM

- **file:line:** alloc `service.c:3015` (`g_inotify_snap[wfd] = dir_snapshot(p)`), refresh/free
  `service.c:3014` + `service/io.c:103`. Decl `container/vfs.c:315` (`char *g_inotify_snap[1024]`).
  Cleanup gap: `inotify_rm_watch` `service.c:3021-3029` and generic `close()` `service.c:1286-1311`.
- **allocated:** a `malloc`'d, newline-joined directory-listing snapshot string (`dir_snapshot`,
  `service/helpers.c:64-80`), one per **directory** inotify watch. Size ∝ number of entries in the
  watched directory.
- **hot path:** `inotify_add_watch` on a directory (syscall 27).
- **why leaked:** `g_inotify_snap[wd]` is freed only when a *new* `inotify_add_watch` reuses the
  same fd number (`service.c:3014`) or when a subsequent read refreshes it (`io.c:103`). Neither
  `inotify_rm_watch` (case 28, which just `close()`s the fd, `service.c:3026`) **nor** the generic
  `close()` handler (case 57, `service.c:1305-1308` frees memf/dirs but **not** `g_inotify_snap` and
  does not clear `g_inotify[]`/`g_inotify_wpath[]`) frees the snapshot. So a watch that is added,
  used, then removed/closed leaks its snapshot string until that exact fd number is later re-watched
  on a directory.
- **classification:** ENGINE-INTERNAL. Bounded by the 1024-slot array × directory-listing size, but
  unbounded in *time* for a churn of distinct fd numbers (fd numbers cycle, so over a long run the
  resident set of leaked strings is at most ~1024 entries — bounded but never reclaimed).
- **workload:** a guest that repeatedly `inotify_add_watch(dir)` then `inotify_rm_watch`/`close` in a
  loop over many large directories (e.g. a file-watcher like `watchman`/webpack watching and
  unwatching directories). Each cycle on a fresh fd number strands one listing string.
- **confidence:** High that the free is missing on the rm/close paths; Medium impact (bounded by 1024 slots).

### C3 — POSIX mq messages not freed when a queue is abandoned · LOW

- **file:line:** alloc `service.c:3331` (`malloc(len)` in `mq_timedsend`), freed on receive
  `service.c:3371`. Queue array is fixed (`g_mqq`).
- **allocated:** one `malloc`'d copy per enqueued POSIX message; stored in `q->msg[pos].data`.
- **hot path:** `mq_timedsend` (syscall 182).
- **why possibly leaked:** the per-message buffer is freed only on `mq_timedreceive`
  (`service.c:3371`). Messages still queued when the queue is closed/unlinked (no drain) are not
  freed. Bounded by `q->maxmsg` per queue × number of queues (fixed `g_mqq`), so capped, not
  unbounded — but never reclaimed for the life of the engine if queues are abandoned full.
- **classification:** ENGINE-INTERNAL, bounded.
- **workload:** create POSIX mqueues, fill each to `maxmsg`, close without draining, repeat across
  many queue names. Capped by the fixed queue table.
- **confidence:** Medium that the close/unlink path lacks a drain-free; Low impact (hard cap).

---

## Alloc sites CONFIRMED bounded / correctly freed

- **thread.c — per-thread `struct cpu` (`thread.c:235`):** freed on thread exit
  (`thread_trampoline`, `thread.c:227`) and on `pthread_create` failure (`thread.c:261`). Detached
  thread, no join leak. Bounded by live thread count. **OK.**
- **fork():** dd's fork is a real host `fork()`; the child inherits the address space and the futex
  bucket table is a process-shared fixed region (`thread.c:45-68`). No per-fork host struct in a
  pid→struct map that needs reaping was found in os/linux (children reaped by `wait4`, case 260,
  `service.c:2121`). `sentry_fork_child` (`sentry.c:230-238`) just re-mints the lazy ring identity —
  no allocation. **OK.**
- **sentry ring pool (`sentry.c`):** a SINGLE fixed shared-memory region mapped once at startup
  (`sentry.c:1109`); the ring pool is a fixed `ring[SENTRY_NRINGS]` array. **Zero `free()` and zero
  per-request `malloc` in the whole file** — no per-syscall host allocation. Lanes are claimed lazily
  and released by CAS on thread/process exit (`ring_release`, `sentry.c:220-227`). A thread that dies
  without releasing strands a *lane* (functional pool-exhaustion, overflow threads then share lanes) —
  not memory growth. **OK / bounded.**
- **netns.c port forwarders:** `fwd_relay` (`netns.c:439`, per accepted TCP connection) freed at
  relay-thread exit (`netns.c:389`) and on create failure (`:444`); `fwd_listen` (`:463`, per
  published port, capped 32 via `g_nfwd`) freed on bind-fail (`:469`); `udp_fwd` (`:645`, per
  published UDP port, capped 32) freed at `:568/:579/:627`; UDP peers are a fixed
  `peers[UDP_FWD_MAXPEERS=64]` ring with eviction. All per-connection allocs freed at thread exit;
  per-port allocs capped at 32. **OK / bounded.**
- **vfs.c memfd (`memf`) (`container/vfs.c:140`, buf `realloc` `:127`):** fixed index array
  `g_memf[1024]` keyed by fd; freed on `close` (`memf_close`, `vfs.c:186-193`), on materialize
  (`vfs.c:167-181`), and on adoption failure (`vfs.c:152-153`). RAM hard-capped per-file
  (`MEMF_CAP` 256 MB) and process-wide (`MEMF_TOTAL_CAP` 1 GB, `g_memf_total` accounting). This is
  GUEST-VISIBLE scratch RAM with an explicit ceiling. **OK / capped.**
- **mem.c mremap save buffer (`service/mem.c:191`):** transient `malloc(head)` freed unconditionally
  a few lines later (`service/mem.c` after the fixed map). **OK.**
- **helpers.c `dir_snapshot` string builder (`service/helpers.c:68,75`):** the returned string's
  lifetime is the caller's responsibility — handled in the inotify diff (freed at `io.c:103`); the
  un-freed case is C2 above. The `realloc` growth loop itself is bounded by directory size. **OK
  (modulo C2).**
- **helpers.c kevent array `realloc` (`service/helpers.c:246`):** grows a local kevent array within a
  single call; not retained across calls. **OK.**
- **sysv.c System V IPC (`service/sysv.c`):** `g_shm_segsz[SHM_SEGSZ_MAX]` is a fixed slot array with
  `used` flags and reuse, cleared on `IPC_RMID` (`sysv.c:64-70`). No per-op `malloc`. **OK / bounded.**
- **state.c (`container/state.c`):** all fixed static globals (hostname, portmap[ ], fd_cport[1024],
  counters). No allocation. **OK.**
- **signal.c / service/signal.c / service/time.c / overlay.c / resolve.c:** no heap allocation
  (fixed static arrays only). **OK.**
- **elf.c `elf_interp` / `load_elf` file maps (`elf.c:22, :426`):** the `mmap`'d ELF file is
  `munmap`'d before return (`elf.c:40, :496`); `strdup(ge)` in `build_stack` freed at `elf.c:531`.
  The image/stack/heap maps are tracked via `gmap_add` and reclaimed at execve. **OK.**

---

## Daemon (dd-daemon/) findings

Global mutable state lives in `model.rs:250-258` (`Inner`): `containers: HashMap`, `images: Vec`,
`volumes: Vec`, `networks: Vec`, `live: HashMap`, `execs: HashMap`, all behind `Arc<Mutex<Inner>>`.

### D1 — `execs` HashMap never removed: UNBOUNDED · HIGH (the primary daemon leak)

- **file:line:** insert `dd-daemon/src/containers/exec.rs:106` (`g.execs.insert(...)`); decl
  `model.rs:257`. There is **no** `execs.remove` / `.retain` / `.clear` anywhere in the crate.
- **stored:** one `Exec` record (container_id, cmd `Vec<String>`, env, working_dir, user) per
  `docker exec` create.
- **hot path:** every `docker exec`. The reaper (`runtime.rs:482-486`) records the exit code and
  removes only the `Live`, leaving the `Exec` forever. Container teardown does **not** purge execs:
  `containers_delete` (`lifecycle.rs:323-332`), `containers_prune` (`inspect.rs:529`), and AutoRemove
  (`runtime.rs:494-500`) clean `containers`+`live` but never `execs` — so exec records outlive their
  parent container.
- **why unbounded:** strictly monotonic growth, no cap, no eviction, intentional retention for
  post-exit `exec inspect`.
- **classification:** ENGINE-INTERNAL (daemon RSS). Small per entry (a few strings) → slow but
  unbounded leak; classic long-lived-daemon creep.
- **workload:** `docker exec <c> true` in a loop (CI health-check / liveness probes against a
  long-lived container) → one `Exec` per call, forever. Also `create; exec; rm` loops.
- **confidence:** High.

### D2 — failed exec spawn orphans its `Live`: MEDIUM (lower volume, larger objects)

- **file:line:** insert `exec.rs:150` (`g.live.insert(exec_id, live)`); removed only by the reaper at
  `runtime.rs:484`. `spawn_live` can bail via `live_fail` at `runtime.rs:304/333/346/353/398`
  **before** the reaper task is spawned (`:428`). `live_fail` (`runtime.rs:567-573`) does not call
  `g.live.remove`, and its `containers.get_mut(cid)` is a no-op for an exec id.
- **stored:** an `Arc<Live>` (broadcast channels + up to 8 MiB `log_chunks` buffer) keyed by exec id.
- **why leaked:** an exec that fails to spawn never reaches the reaper, so its `Live` entry is never
  removed (no container-rm path covers an exec-keyed entry).
- **classification:** ENGINE-INTERNAL. Each leaked object is large (channels + buffer).
- **workload:** repeatedly start execs that fail to spawn (bad binary / openpty / async-fd error).
- **confidence:** Medium.

### D3 — `image_size` cache never evicted on `rmi`: LOW

- **file:line:** `util.rs:423-429` (`OnceLock<Mutex<HashMap<String,i64>>>` keyed by image rootfs
  path). No eviction on `docker rmi`.
- **why unbounded:** grows one `(String,i64)` per distinct image-rootfs path ever seen; tiny entries,
  bounded in practice by churn of distinct images but technically unbounded over daemon lifetime.
- **classification:** ENGINE-INTERNAL, near-negligible.
- **workload:** pull/`rmi` distinct images in a loop.
- **confidence:** Low impact.

### Daemon collections CONFIRMED bounded (insert ↔ remove verified)

- **`containers`** — insert `lifecycle.rs:156`; removed on rm `lifecycle.rs:323`, prune
  `inspect.rs:529`, AutoRemove `runtime.rs:494`. OK.
- **`live` (container-keyed)** — insert `lifecycle.rs:225` + restart `runtime.rs:553`; removed on rm
  `lifecycle.rs:331`, prune `inspect.rs:529`, reaper `runtime.rs:484/500`. OK (exec-keyed entries are
  the D2 exception).
- **`images`** (`Vec`) — `retain` on `rmi`/replace (`images.rs:189/414/687`, `build.rs:611/735`). OK.
- **`volumes`** (`Vec`) — `retain` on rm/prune (`volumes.rs:94/115`). OK.
- **`networks` + per-net `endpoints`** — `networks.retain` on rm/prune (`networks.rs:143/220`);
  `leave_network` clears endpoints (`networks.rs:87-88`) from rm/AutoRemove. OK.
- **Event bus** — `broadcast::channel(256)` (`events.rs:31`), fixed ring; slow clients lag rather
  than retain history. OK.
- **`live.log_chunks`** — capped at 8 MiB via front-drain in `push_log` (`runtime.rs:36-53`). OK.
- **Per-connection spawns** (attach/exec hijack `exec.rs:18`, events stream, pumps
  `runtime.rs:417/419`) — terminate on guest exit / client disconnect; no retained registry. OK.
