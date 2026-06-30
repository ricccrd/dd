# dd — todo

Shipped **v0.9.0**; **v0.9.1 batch staged** (11 commits, not yet tagged). Open work below. Found by
the real-software discovery sweep (#125). Most arm64 paths are solid; the bulk is x86_64 heavy-software.
Keep this file current after each batch.

> Daemon resolves the engine via `resolve_bundled` → installed `/Applications/dd.app` unless
> `DDJIT_DIR` is set. Install the new DMG for fixes to take effect.

## Fixed since v0.9.0 (staged in the v0.9.1 batch)
readlinkat/realpath of lower-only files (#124) · execve forwards guest envp both arches (#127) ·
x86 SHA-NI + PTEST + VEX ANDN/BLS (#128/#137) · julia past-EOF mmap SIGBUS (#133) · chroot(51) (#140) ·
ioctl(FIOASYNC) for nginx (#141) · epoll EPOLLET prime (#142) · x86 raw select/poll/pipe (#144) ·
by-CL shift CF.

## In progress (agents active)
- [ ] **#132 jq/x86 codegen memory corruption** — jq `munmap_chunk` (deterministic); ld/git/rustc/haproxy
  SIGSEGV. Aimed at the **#104** store-drop root (memory/EA suspect) — may fix both.
- [ ] **#130 non-PIE Go loader** — static Go (etcd/caddy/traefik/`go run`) `moduledataverify` fatal: textStart
  not honored after high rebase.
- [ ] **#146 poll/ppoll/pselect EINTR-restart without delivering handler** → servers blocking in poll + SA_RESTART
  SIGCHLD reaper hang.
- [ ] **#148 getrlimit NOFILE=∞** (memcached) + **#134 R hangs at startup**.
- [ ] **#136 x86 base64 -d SIMD shuffle miscompile** (busybox).
- [ ] **#143 procfs** — /proc/self/{fd,maps,smaps,status,environ} + /proc/[pid]/stat (redis/httpd/mongo).

## Open / queued (share files with the active wave)
- [ ] **#104 guest-JIT store-drop** — V8 Turboshaft MachineLowering drops Stores under OSR (java/.NET/node-opt).
  Suspect: a memory load/store-width or effective-address edge (see #132).
- [ ] **#151 execve doesn't reset g_sigact[]** — caught handlers survive exec → crash (redis via `sh -c`; broad). proc.c.
- [ ] **#150 java sysinfo totalram=0** ("Too small maximum heap") then futex spin. misc.c.
- [ ] **#149 AF_UNIX bind doesn't create the socket file** (mongo/mariadb/postgres unix sockets).
- [ ] **#145 x86 flag residuals** — ror %cl CF, imul CF, shift OF(count==1).
- [ ] **#135 PyPy x86 JIT backend asserts** (2nd guest-JIT). **#138 git write-tree** wrong hash (likely #128 — re-test).
- [ ] **#131 .NET host** can't resolve its own exe path (re-test after #124). **#139 clang** link fails both arches.
- [ ] **#117 flaky PIE fork+exec base** · **#119 mongosh SEA crash** · **#120 RFLAGS ID flag** · **#123 node-via-daemon ENOENT**.

## Deferred / non-blocking
- [ ] **#78** gcc-bundle /hello.c fixture · **#93** host-asm encoder de-dup · **#94** README benchmarks

## Test-env notes (not code bugs)
- Host `~/.docker/config.json` bogus Hub creds → real pulls 401 unless `DOCKER_CONFIG=<empty>`.
- docker.sh + scenarios share `dd-scenarios.sock` → not concurrent. Shared poc/images mutated by parallel agents.
- Engine-direct: upper under the shared project dir (not /tmp); export PATH in-guest; bare-name entry resolves
  only against /usr/local/bin:/usr/bin:/bin (use absolute paths for tools elsewhere).

## Process
- Each fix: verify on the real path → batch → gate (basics both arches + docker.sh) → tag → push.
