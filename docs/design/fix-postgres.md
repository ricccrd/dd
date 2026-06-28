# Fix — postgres:alpine startup (post-shebang)

Status: **root-caused with live capture; fixes scoped (no source changed by this doc).** Capture against the
already-built `target/release/dd-daemon` (Mach-O, run through the `mac` bridge — it **drops ambient env**, so
the daemon must be launched as `mac bash -lc "env DD_IMAGES=… DDOCKERD_SOCK=… DDJIT_DIR=… dd-daemon"`; the
build-time JIT path baked into the binary was stale, hence the explicit `DDJIT_DIR=…/ddjit-f5c3afad57f04f24/out`)
and the prebuilt `ddjit-linux_aarch64`. `CRASHDBG=1` forwards to the JIT (`lib.rs:168`); `JT=1` only by
invoking the JIT directly (`mac bash -lc "exec env CRASHDBG=1 JT=1 <jit> --rootfs <rootfs> <prog> …"`).
Image: postgres **18.4** (alpine), gosu **1.17** (Go 1.24.6).

The shebang fix (`parse_shebang` in `jit_run`) **works**: `docker-entrypoint.sh` now loads and runs. The
research doc guessed "initdb daemonize/fork" was next; the live trace says the real next blockers form a
**chain** — there are *four* distinct gaps, three in the JIT and one in the daemon. Each is independently
reproduced and root-caused below.

| # | layer | blocker | where it stops | root cause (file:line) |
|---|---|---|---|---|
| B1 | JIT (env) | duplicate `PATH` in guest env; shells take the **last** → container PATH (with `/usr/local/bin`) is clobbered by the default `PATH=/usr/bin:/bin` | entrypoint: `exec: gosu: not found` (exit 127) | `os/linux/elf.c:114` + `:132-148` |
| B2 | JIT (loader) | `gosu` is a **non-PIE `ET_EXEC`**; the JIT biases it off its fixed vaddr (`__PAGEZERO`), its absolute **data** pointers stay at the low link vaddr → wild deref | `exec gosu postgres …` → **SIGSEGV** in JIT host code | `os/linux/elf.c:73-86` (`:71-72` comment) faulting at `service.c:1002` |
| B3 | JIT (clone) | glibc `posix_spawn`/`popen` issue **vfork** (`clone(CLONE_VM\|CLONE_VFORK, child_stack)`); `case 220` does a plain `fork()` and **ignores the child-stack arg** → child runs on the wrong SP → glibc's clone trampoline branches to garbage | `initdb` child `postgres -V` → **SIGILL** (deterministic 3/3) | `os/linux/service.c:1824-1851` (`:1831`) |
| B4 | daemon (metadata) | `pull_image` writes `dd-image.json` as `{name,cmd}` only — drops the OCI config's **Entrypoint / Env / WorkingDir** | bare `docker run postgres:alpine` runs `/bin/bash`; no `PATH`/`PGDATA` | `dd-daemon/src/images.rs:234-242` |

B1+B2 are on the **default (root) path** (`docker run postgres` → entrypoint runs as root → `exec gosu postgres`).
B3 is on the **uid-dropped path** (when gosu is skipped, `initdb` is the next wall). B4 is why a bare
`docker run postgres` never even reaches the entrypoint. All four must be fixed for the test gate.

postgres/initdb/psql/bash/busybox are **all PIE** and load+run fine standalone (`postgres --version`,
`initdb --version`, a single `sh -c 'postgres -V'` fork+exec all succeed). The only non-PIE binary in the
image is **gosu**; the only fork mechanism that breaks is **vfork**.

---

## B1 — duplicate `PATH`, shells take the last → container PATH lost

### Evidence
Run the real entrypoint via the daemon (as root, the default):
```
$ docker run -d --name pg1 -e POSTGRES_PASSWORD=pw -e POSTGRES_HOST_AUTH_METHOD=trust \
      postgres:alpine docker-entrypoint.sh postgres
$ docker logs pg1
/usr/local/bin/docker-entrypoint.sh: line 343: exec: gosu: not found      # exit 127
```
`gosu` exists at `/usr/local/bin/gosu` (static aarch64 ELF), but the guest PATH excludes `/usr/local/bin`.
Dumping the **raw** guest `envp` (a C program, so duplicates are visible) shows two `PATH` entries:
```
$ <jit> --rootfs <rootfs> /usr/bin/env        # with DD_GUEST_ENV='PATH=/usr/local/bin:/usr/bin:/bin\n…'
PATH=/usr/local/bin:/usr/bin:/bin     ← from DD_GUEST_ENV (first)
…
PATH=/usr/bin:/bin                    ← built-in default (last)
```
C `getenv` returns the **first** match (so the comment at `elf.c:131` "getenv returns the first match" holds
for C programs), but **bash imports all of envp and the last assignment wins** — so every shell in the
entrypoint sees `PATH=/usr/bin:/bin` and can't find `gosu`/`initdb`/`psql` (all in `/usr/local/bin`) by bare
name. Confirmed: inside the container `command -v initdb` is empty, `initdb --version` → `command not found`,
but `/usr/local/bin/initdb --version` → `initdb (PostgreSQL) 18.4`.

### Root cause
`build_stack` (`os/linux/elf.c:132-148`) pushes the container's `DD_GUEST_ENV` entries **first** (`:133-142`),
then unconditionally appends the built-in defaults — including `PATH=/usr/bin:/bin` (`g_guest_env[]`,
`elf.c:114`) — **after** (`:143-148`). The intent (defaults as fallback) only works for first-wins `getenv`;
it actively breaks shells, which last-wins.

### Fix (JIT) — make defaults true fallbacks (dedup)
In `build_stack`, only append a default whose key was **not** already provided by `DD_GUEST_ENV`. Smallest
form: before the defaults loop (`elf.c:143`), record the `K=` prefixes pushed from `DD_GUEST_ENV`, and skip a
default `g_guest_env[i]` whose `K=` prefix matches one already present. One short helper; no behavior change
when the container didn't set the key. This also future-proofs `HOME`/`TERM`/`LANG` collisions.

---

## B2 — `gosu` is non-PIE; biased load faults on its own data pointers

### Evidence
With PATH corrected (absolute path), `gosu` itself crashes:
```
$ docker run --rm postgres:alpine /bin/bash -c '/usr/local/bin/gosu postgres id'
[CRASH] sig1X fault=0x00000000001a0940 pc=0x0000000101a2d474 tid=0x0      # exit 139 (SIGSEGV)
```
(`[CRASH] sig=X` with `b[11]='0'+(sig%10)`, `targets/linux_aarch64.c:86-87`; `sig%10=1`, exit 139 ⇒ **SIGSEGV/11**.)
Direct JIT run with `JT=1` pinpoints the host fault:
```
[sys] 56 (ffffffffffffff9c,1a0940,0)            openat(AT_FDCWD, 0x1a0940, 0)
[MACH] exc=0x1 fault=0x0 hpc=0x18c052aa0 x28=0 off=0x4aa0 _platform_strncmp$VARIANT$Base
```
The fault is in the JIT's **own** `openat` handler — `service.c:1002` `if (rp && !strncmp(rp, "/proc/", 6))`
with `rp = (const char*)a1 = 0x1a0940`. `gosu`'s ELF header explains the pointer:
```
$ readelf -hlW gosu
  Type: EXEC (Executable file)            ← non-PIE
  LOAD … 0x1a0000 … RW                    ← data/bss segment at fixed vaddr 0x1a0000
```
`0x1a0940` is inside gosu's **RW data** (`0x1a0000 + 0x940`) — a global string pointer. But the JIT loaded
gosu's `[blk]` code at `~0x10c3a8000`: a non-PIE `ET_EXEC` can't be placed at its link vaddr `0x10000`
because macOS `__PAGEZERO` reserves the low 4 GB, so `load_elf` biases it (`elf.c:73` `mmap(NULL, …)`,
`:79` `bias = base - basepage`). Code keeps working (PC-relative + the dispatcher's `g_nonpie_*` code-jump
redirect, `elf.c:80`), but gosu's **absolute data pointer** `0x1a0940` is never relocated, so it points at
unmapped low memory; the next syscall that hands that pointer to the host (`openat`) faults in `strncmp`.

This is exactly the documented **non-PIE `ET_EXEC` deep bug** — `elf.c:71-72`: *"A non-PIE ET_EXEC gets biased
here; the dispatcher redirects its absolute code jumps (g_nonpie_*) but its absolute DATA refs to the low link
vaddr still fault (xfail)."* postgres surfaces it via **gosu**, not initdb as the research doc guessed.

### Fix (JIT) — the scoped non-PIE fix from `docs/PLAN.md`
Per PLAN "Deep bugs": small `__PAGEZERO` + force every *other* mapping (PIE image / heap / stack / mmaps) to a
high hint so only a non-PIE `ET_EXEC` uses the low region and loads at its real vaddr (no bias ⇒ absolute data
refs are correct); **or** an arm64 load/store fault-fixup that re-adds `g_nonpie_bias` (`elf.c:80`) to a guest
data address that faults inside `[g_nonpie_lo, g_nonpie_hi)`. The fixup variant also rescues the *syscall*
path: a guest pointer arriving in `service()` within `[g_nonpie_lo,g_nonpie_hi)` should be rewritten to
`ptr + g_nonpie_bias` before the host dereferences it (covers the `strncmp` fault above).

### Workaround that avoids B2 entirely (cheap, ships now)
gosu is only invoked because the entrypoint runs as root and re-execs as the `postgres` user
(`docker-entrypoint.sh:343` `exec gosu postgres "$BASH_SOURCE" "$@"`, guarded by `[ "$(id -u)" = '0' ]`). The
JIT's `--uid/--gid` already work (`<jit> --uid 70 --gid 70 … id` → `uid=70(postgres)`). If the daemon applies
`--user`→`--uid` (see B4), `id -u` returns 70, the `gosu` branch is skipped, and postgres runs directly as
uid 70 — **no non-PIE binary in the path at all**. Verified: with `--uid 70`, the entrypoint sails past gosu
and reaches `docker_init_database_dir` → `initdb` (where B3 is the next wall). This makes B3, not B2, the
critical blocker for the gate; B2's full fix can land independently.

---

## B3 — vfork child-stack ignored → `initdb`'s `postgres -V` SIGILLs

### Evidence
As uid 70 (gosu skipped), `initdb` fails **deterministically (3/3)** at the very first thing it does — a
version probe of the `postgres` helper:
```
$ <jit> --uid 70 --gid 70 --rootfs <rootfs> /usr/local/bin/initdb -D /tmp/pgd --username=postgres --auth=trust
no data was returned by command ""/usr/local/bin/postgres" -V"
child process was terminated by signal 4: Illegal instruction
initdb: error: program "postgres" is needed by initdb but was not found …
```
`postgres -V` standalone works; a single `sh -c '/usr/local/bin/postgres -V'`, a pipe `postgres -V | cat`, a
command-substitution `$(postgres -V)`, and 5 parallel forked `postgres -V &` **all succeed**. The one thing
that differs is the **spawn primitive**. The `JT` trace shows exactly one clone before the child dies:
```
[sys] 220 (4111,11c31ef20,0)        clone(flags=0x4111, child_stack=0x11c31ef20, ptid=0)
…
child process was terminated by signal 4: Illegal instruction
```
`flags = 0x4111 = CLONE_VM(0x100) | CLONE_VFORK(0x4000) | SIGCHLD(0x11)` — i.e. **vfork**, the exact flags
glibc `posix_spawn`/`popen` use. `a1 = 0x11c31ef20` is the **child stack** glibc carved for the trampoline.
bash uses a plain `fork()` (no `CLONE_VM`, `a1=0`), which is why bash forks work and initdb's doesn't.

### Root cause
`case 220` (`os/linux/service.c:1824-1851`) only special-cases `CLONE_THREAD` (`a0 & 0x10000`, `:1826`,
routed to `spawn_thread` which *does* use `a1` as the stack, `:1827`). Everything else falls to a plain
`fork()` (`:1831`) and the child resumes at the post-`svc` guest PC **on the parent's copied SP** — the
child-stack arg `a1` is dropped. For `fork`/plain `vfork` (`a1=0`) that's correct. But glibc's
`posix_spawn`/`popen` call the raw `clone` syscall with `CLONE_VM` **and a real child stack**, then its
aarch64 clone trampoline, in the child, loads the child function pointer + arg from `[sp]` (the stack it
passed). With `sp` still pointing at the parent's stack, the trampoline reads a garbage "function pointer"
and branches to it → the JIT translates/executes garbage → **SIGILL**.

### Fix (JIT) — honor the child stack for `CLONE_VM` clones
In `case 220`, after `fork()`, in the child (`pid==0`) branch (alongside the existing
`pthread_jit_write_protect_np(1)` / `G_SHADOW_RESET` re-asserts at `service.c:1836-1837`), set the child SP to
the provided stack when a `CLONE_VM` clone supplied one:
```c
if ((a0 & 0x100) && a1)      // CLONE_VM with an explicit child stack (glibc posix_spawn/popen)
    G_SP(c) = a1;            // run the clone trampoline on the stack glibc set up (else garbage fn-ptr → SIGILL)
```
(`G_SP(c)` ⇒ `c->sp`, `frontend/aarch64/abi.h:37`; `CLONE_VM=0x100`.) Guarding on `a1 != 0` keeps plain
`fork()` and plain `vfork()` (`a1=0`, shares the parent stack) unchanged. The COW-fork semantics are otherwise
fine: the `posix_spawn` child only runs the trampoline then `execve` (`case 221`, which tears the address space
down and reloads anyway, `service.c:1903`), so emulating `CLONE_VM` as a private fork is acceptable; only the
**stack** must be honored.

### Confirming microtest (`edge`/`compile` group)
A two-process guest test: parent `posix_spawn(&pid, "/bin/true", …, argv, env)` then `waitpid` — faults
SIGILL today, exits 0 after the fix. (Or raw `syscall(SYS_clone, CLONE_VM|CLONE_VFORK|SIGCHLD, stack, …)` with
a trampoline that `execve`s.) End-to-end: `initdb` then runs to completion and `postgres` reaches the gate.

---

## B4 — daemon drops OCI Entrypoint/Env at pull (`dd-image.json` metadata gap)

### Evidence
```
$ cat images/docker.io_library_postgres_alpine/dd-image.json
{"cmd":["/bin/bash"],"name":"postgres:alpine"}        # no entrypoint, no env, no workdir
```
`cmd:["/bin/bash"]` is the `default_shell` fallback (`images.rs:235`) — the OCI config's
`Entrypoint=["docker-entrypoint.sh"]`, `Cmd=["postgres"]`, and `Env` (which carries
`PATH=/usr/local/sbin:/usr/local/bin:…`, `PGDATA`, `LANG`, …) were never persisted. So a bare
`docker run postgres:alpine` runs `/bin/bash` (not the entrypoint), and even with an explicit command the guest
gets no `PATH`/`PGDATA`.

### Root cause
`pull_image` (`dd-daemon/src/images.rs:220-242`) extracts only `cmd` (via `config_cmd`, `:234`) and writes
`dd-image.json` as `json!({ "name": name, "cmd": cmd })` (`:239`), dropping Entrypoint/Env/WorkingDir. Note
`config_cmd` (`:273-279`) **flattens** Entrypoint+Cmd into one argv — wrong granularity for round-tripping
(docker keeps them separate so `--entrypoint`/`CMD` override semantics work, see `containers.rs:84-91`).

The downstream is already correct and just waiting for the data:
* `discover_images` already **reads** `env`/`entrypoint`/`workdir` from `dd-image.json` (`util.rs:198`).
* `containers_create` already builds `argv = entrypoint ++ cmd` and `env = img.env ++ run -e`
  (`containers.rs:84-95`).
* `spawn_cfg` already forwards `c.env` as `DD_GUEST_ENV` and `c.working_dir` as `DD_CWD`
  (`runtime.rs:39-44`).
* `image_load` (the `docker load` path) already persists the full set —
  `json!({"name","cmd","env","entrypoint","workdir"})` at `images.rs:518` — **this is the exact template the
  pull path should match.**

### Fix (daemon)
In `pull_image` (`images.rs:234-242`): pull `entrypoint`, `cmd`, `env`, `workdir` **separately** from the OCI
config (`config["config"]["Entrypoint"|"Cmd"|"Env"|"WorkingDir"]`) and:
1. persist all of them: `json!({ "name": name, "cmd": cmd, "entrypoint": entrypoint, "env": env, "workdir": workdir })`
   (mirroring `image_load`/`images.rs:518`), and
2. populate the returned `Image { …, env, entrypoint, workdir, … }` (today it sets only `cmd`).

Add a `config_env`/`config_entrypoint`/`config_workdir` helper trio (analogous to `config_cmd`) or read the
fields inline. Keep `cmd = config.Cmd` and `entrypoint = config.Entrypoint` **separate** (don't reuse the
flattening `config_cmd`), so `docker run postgres` → `entrypoint(["docker-entrypoint.sh"]) ++ cmd(["postgres"])`
and the env (incl. `PATH` with `/usr/local/bin` and `PGDATA`) reaches `DD_GUEST_ENV`.

**Also apply `--user`** (the tracked "User uid not applied" gap, `containers.rs:319/333` capture it,
`runtime.rs` never sets it): in `spawn_cfg` set `cfg.uid/cfg.gid` from the container's `user`
(`"name"`/`"uid:gid"` → look up `/etc/passwd` in the rootfs for a name). This both fixes `docker run --user`
fidelity **and** lets a user dodge B2 (the entrypoint skips gosu when uid≠0). For full default-path
compatibility (`docker run postgres` with no `--user`, which legitimately starts as root and drops via gosu),
B2's non-PIE fix is still required.

---

## Test gate

Add to the realsw scenario (`dd-tests/scenarios/realsw.sh`, the postgres block already exists) — must go green:

1. **Pull populates metadata.** After `docker pull postgres:alpine`,
   `images/docker.io_library_postgres_alpine/dd-image.json` contains non-empty `entrypoint`
   (`["docker-entrypoint.sh"]`) and `env` containing `/usr/local/bin` in `PATH` and a `PGDATA=` entry. (B4)
2. **Bare run starts the DB.** `docker run -d --name rsw-pg -e POSTGRES_PASSWORD=pw
   -e POSTGRES_HOST_AUTH_METHOD=trust postgres:alpine` — `docker logs rsw-pg` reaches
   **`database system is ready to accept connections`** within ~40 s (exercises B1 entrypoint PATH, B2 gosu *or*
   the `--user` skip, B3 initdb vfork). (B1+B2+B3)
3. **Query round-trips.** `docker exec rsw-pg psql -U postgres -tAc 'CREATE TABLE t(v int);
   INSERT INTO t SELECT generate_series(1,1000); SELECT count(*), sum(v) FROM t;'` → `1000|500500`
   (forking backend per connection — additional vfork/fork+exec coverage). (B3)
4. **TCP connect.** `docker run -p 5432:5432 …` then a host `psql -h 127.0.0.1 -p 5432 -U postgres -c 'select 1'`
   (or a raw TCP connect to 5432) succeeds — exercises the listener `bind`/`accept` path.

Microtest gates (cheaper, land with the JIT PRs):
* **B1:** a guest that sets `PATH` via env then `execvp("gosu-like-name-in-PATH")` — resolves after dedup.
* **B3:** `posix_spawn("/bin/true")` + `waitpid` — XPASS (SIGILL→exit 0) after the child-stack fix.
* **B2:** run a tiny non-PIE `ET_EXEC` (`gcc -no-pie -static`) that opens a file via a global path string —
  faults today, runs after the non-PIE fix (tracked under the PLAN "compile" xfail group).

### Current reproducer (drives all four findings)
```bash
# daemon on a UNIQUE socket, env forwarded through the mac bridge, CRASHDBG on:
mac bash -lc "env CRASHDBG=1 DD_IMAGES=/Users/x/dd/poc/images DDOCKERD_SOCK=/Users/x/dd/dd/dd-pg.sock \
  DD_STATE=/Users/x/dd/dd/dd-pg-state.json DDJIT_DIR=/Users/x/dd/dd/target/release/build/ddjit-f5c3afad57f04f24/out \
  nohup target/release/dd-daemon >dd-pg.log 2>&1 </dev/null & disown"
# B1+B2 (default/root path → gosu):
mac bash -lc 'export DOCKER_HOST=unix:///Users/x/dd/dd/dd-pg.sock; docker run --rm postgres:alpine \
  docker-entrypoint.sh postgres'                                          # → exec: gosu: not found
# B3 (uid-dropped path → initdb vfork):  ROOT=images/docker.io_library_postgres_alpine/rootfs
mac bash -lc "exec env CRASHDBG=1 JT=1 <jit> --uid 70 --gid 70 --rootfs \$ROOT \
  /usr/local/bin/initdb -D /tmp/pgd --username=postgres --auth=trust"     # → clone(0x4111,…) child SIGILL
```
(Note: the daemon's Unix socket is a mac-host kernel object — drive `docker` **through `mac`**, not from the
Linux side; a Linux-side `docker --host unix://…dd-pg.sock` can't cross the OrbStack boundary.)
