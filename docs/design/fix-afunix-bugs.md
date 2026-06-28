# Fix: AF_UNIX edge bugs — SCM_RIGHTS fd-passing + abstract namespace

Two `edge` group cases are `xfail`-tracked in `dd-tests/src/cases/mod.rs:137` (`scmrights`)
and `mod.rs:143` (`abstract`). Both fail because `dd-jit/src/runtime/os/linux/service.c`
forwards Linux socket structures to the macOS host without translating the layout/constant
differences that AF_UNIX ancillary data and abstract addresses depend on. This doc gives the
exact failing path and the exact fix for each. No source is edited here (a `service.c` change
is in flight); helpers below are described against current line numbers.

The runtime model that makes both fixes simple: **dd uses host fds directly as guest fds** (see
`socket`/`socketpair`/`open` in `service.c` returning the raw host fd as the guest's return value,
e.g. case 198 at `service.c:2216`). So a host fd received over SCM_RIGHTS *is already* a valid
guest fd — no fd renumbering is needed, only the cmsg framing must be translated. Likewise the
guest's AF_UNIX socket is already a real host AF_UNIX socket; only the *address* must be rewritten.

---

## Bug 1 — SCM_RIGHTS fd passing

### (a) What the test expects + exact failing path

`dd-tests/guests/edge_scmrights.c`: child opens `/tmp/dd_scm_payload`, writes `"fd-passed-ok"`,
and `sendmsg`s the *fd* (not the bytes) over a `socketpair(AF_UNIX, SOCK_STREAM)` using a single
`SCM_RIGHTS` cmsg (`edge_scmrights.c:30-33`). Parent `recvmsg`s, pulls the fd out of the cmsg
(`edge_scmrights.c:47-48`), and `read()`s through it. Expected stdout:

```
scmrights got_fd=1 data=fd-passed-ok
```

Failing path: `service.c:2479-2505`, the `case 211 / case 212` (sendmsg/recvmsg) block. It builds
a host `struct msghdr` and points `mh.msg_control` **directly at the guest control buffer**:

```
service.c:2490   mh.msg_control    = (void *)*(uint64_t *)(g + 32);
service.c:2491   mh.msg_controllen = (socklen_t) *(uint64_t *)(g + 40);
...
service.c:2496   (nr == 211) ? sendmsg(...,&mh,...) : recvmsg(...,&mh,...);
```

The control buffer is a **Linux** `cmsghdr`; macOS reads it as a **macOS** `cmsghdr`. The two
layouts disagree (see (b)), so on `sendmsg` the host kernel never sees a valid `SOL_SOCKET/
SCM_RIGHTS` cmsg and passes no fd; on `recvmsg` the guest finds no cmsg. `got_fd=0`, `data=`.

The identical defect exists in the `sendmmsg/recvmmsg` block `case 269 / case 243`
(`service.c:2506-2543`, control copied raw at `service.c:2521-2522`).

### (b) macOS vs Linux constant/layout differences

`struct cmsghdr`:

| field        | Linux (x86-64)            | macOS (arm64)             |
|--------------|---------------------------|---------------------------|
| `cmsg_len`   | `size_t` — **8 bytes** @0 | `socklen_t` — **4 bytes** @0 |
| `cmsg_level` | `int` @8                  | `int` @4                  |
| `cmsg_type`  | `int` @12                 | `int` @8                  |
| header size  | **16** bytes              | **12** bytes              |
| `CMSG_DATA`  | hdr + `CMSG_ALIGN(16)` = +16 | hdr + `__DARWIN_ALIGN32(12)` = **+12** |
| align        | 8-byte (`sizeof(long)`)   | 4-byte (`__DARWIN_ALIGN32`) |
| `CMSG_LEN(l)`| `16 + l`                  | `12 + l`                  |

Constants:

| name         | Linux  | macOS    |
|--------------|--------|----------|
| `SOL_SOCKET` | `1`    | `0xffff` |
| `SCM_RIGHTS` | `1`    | `1`      |

So a one-fd cmsg is **Linux**: `cmsg_len=20`, level@8=`1`, type@12=`1`, fd@16. **macOS** wants:
`cmsg_len=16`, level@4=`0xffff`, type@8=`1`, fd@12. The header width, the data offset, *and* the
level value all differ. The fd integer itself needs **no** translation (host fd == guest fd).

Note also the `recvmsg` flags writeback at `service.c:2501` writes `mh.msg_flags` raw, but
`MSG_CTRUNC`/`MSG_TRUNC` differ (Linux `MSG_CTRUNC=0x8`, `MSG_TRUNC=0x20`; macOS `MSG_CTRUNC=0x20`,
`MSG_TRUNC=0x10`). That must be translated macOS→Linux on the way back (see edge cases).

### (c) Exact solution — translate the msg_control cmsg array

Forward the cmsg array through a **host-layout scratch buffer**, translating per-cmsg. Add two
helpers next to `msgflags_l2m` in `dd-jit/src/runtime/os/linux/container/netns.c` (compiled against
macOS headers, so `CMSG_*`, `SOL_SOCKET`, `SCM_RIGHTS` resolve to the macOS values):

```c
// Linux cmsg layout constants (we parse/emit guest buffers by hand)
#define LX_CMSG_ALIGN(n) (((n) + 7u) & ~7u)   // 8-byte align
#define LX_CMSGHDR      16u                    // 8(len)+4(level)+4(type)
#define LX_SOL_SOCKET    1
// Linux cmsg_level value -> macOS
static int cmsg_level_l2m(int lv){ return lv==LX_SOL_SOCKET ? SOL_SOCKET : lv; }
static int cmsg_level_m2l(int lv){ return lv==SOL_SOCKET ? LX_SOL_SOCKET : lv; }

// guest(Linux) control buf -> host(macOS) control buf. Returns host bytes written (<=cap), or -1.
static ssize_t cmsg_l2m(const uint8_t *g, size_t glen, uint8_t *h, size_t cap){
    size_t go = 0, ho = 0;
    while (go + LX_CMSGHDR <= glen){
        uint64_t clen = *(const uint64_t *)(g + go);          // Linux cmsg_len (8B)
        int lvl = *(const int *)(g + go + 8);
        int typ = *(const int *)(g + go + 12);
        if (clen < LX_CMSGHDR || go + clen > glen) break;
        size_t dlen = (size_t)clen - LX_CMSGHDR;              // payload bytes
        struct cmsghdr ch; size_t need = CMSG_SPACE(dlen);
        if (ho + need > cap) break;                            // MSG_CTRUNC territory
        ch.cmsg_len   = CMSG_LEN(dlen);                        // macOS 12+dlen
        ch.cmsg_level = cmsg_level_l2m(lvl);
        ch.cmsg_type  = typ;                                   // SCM_RIGHTS==1 both
        memcpy(h + ho, &ch, sizeof ch);
        memcpy(CMSG_DATA((struct cmsghdr *)(h + ho)), g + go + LX_CMSGHDR, dlen);
        ho += need;
        go += LX_CMSG_ALIGN(clen);
    }
    return (ssize_t)ho;
}

// host(macOS) control buf -> guest(Linux) control buf. Returns Linux bytes written, sets *ctrunc.
static ssize_t cmsg_m2l(const struct msghdr *mh, uint8_t *g, size_t cap){
    size_t go = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR((struct msghdr *)mh); c; c = CMSG_NXTHDR((struct msghdr *)mh, c)){
        size_t dlen = (size_t)c->cmsg_len - CMSG_LEN(0);      // payload bytes (macOS hdr=12)
        size_t need = LX_CMSG_ALIGN(LX_CMSGHDR + dlen);
        if (go + LX_CMSGHDR + dlen > cap) break;              // guest buf full -> truncate
        *(uint64_t *)(g + go)      = (uint64_t)(LX_CMSGHDR + dlen);   // Linux cmsg_len
        *(int *)(g + go + 8)       = cmsg_level_m2l(c->cmsg_level);
        *(int *)(g + go + 12)      = c->cmsg_type;
        memcpy(g + go + LX_CMSGHDR, CMSG_DATA(c), dlen);
        go += need;
    }
    return (ssize_t)go;
}
```

Rewrite the `case 211/212` body (`service.c:2479-2505`) to route control through scratch:

```c
uint8_t *gc = (void *)*(uint64_t *)(g + 32);
size_t   gcl =        *(uint64_t *)(g + 40);
uint8_t hctl[4096];               // host-layout scratch (macOS hdr is smaller, so this is ample)
if (nr == 211) {                  // sendmsg: translate guest -> host before the call
    ssize_t hn = (gc && gcl) ? cmsg_l2m(gc, gcl, hctl, sizeof hctl) : 0;
    mh.msg_control = hn > 0 ? hctl : NULL;
    mh.msg_controllen = hn > 0 ? (socklen_t)hn : 0;
} else {                          // recvmsg: receive into host scratch
    mh.msg_control = (gc && gcl) ? hctl : NULL;
    mh.msg_controllen = (gc && gcl) ? (socklen_t)sizeof hctl : 0;
}
... call ...
if (nr == 212 && r >= 0) {
    *(uint32_t *)(g + 8)  = mh.msg_namelen;
    size_t ln = (gc && gcl) ? (size_t)cmsg_m2l(&mh, gc, gcl) : 0;   // host -> guest
    *(uint64_t *)(g + 40) = ln;
    *(uint32_t *)(g + 48) = msgflags_m2l((int)mh.msg_flags);        // translate, incl. MSG_CTRUNC
}
```

Apply the same change to `case 269/243` (`service.c:2506-2543`): translate each submessage's
control with `cmsg_l2m` before `sendmsg`, and on `recvmmsg` write back with `cmsg_m2l` +
`msgflags_m2l`. Add the missing inverse `msgflags_m2l` in `netns.c` (mirror `msgflags_l2m`,
`netns.c:57-68`): macOS→Linux for `MSG_TRUNC 0x10→0x20`, `MSG_CTRUNC 0x20→0x8`, `MSG_EOR 0x8→0x80`,
`MSG_OOB 0x1→0x1`.

### (d) Edge cases

- **Multiple fds in one cmsg**: `dlen = N*4`. `cmsg_l2m`/`cmsg_m2l` copy the whole payload as one
  block, so an N-fd `SCM_RIGHTS` is preserved. No per-fd remap (host fd == guest fd).
- **Multiple cmsgs**: both helpers loop with proper alignment (Linux 8-byte, macOS 4-byte via
  `CMSG_SPACE`/`LX_CMSG_ALIGN`), so chained cmsgs round-trip.
- **Control truncation**: if the host control yields more than the guest buffer holds, `cmsg_m2l`
  stops at the boundary (`go + LX_CMSGHDR + dlen > cap`) — the kernel already set `MSG_CTRUNC` in
  `mh.msg_flags`; `msgflags_m2l` maps `0x20→0x8` so the guest sees Linux `MSG_CTRUNC`. Any fds the
  host installed but we drop on the floor would leak — clamp `hctl`/scratch large (4096) so single-
  message fd batches never truncate in practice; optionally `close()` payload fds from any dropped
  `SCM_RIGHTS` cmsg to avoid leaks (defensive).
- **`cmsg_len` width**: read Linux len as `uint64_t` (don't truncate to 32-bit); emit macOS len via
  `CMSG_LEN` (32-bit). Guard `clen >= LX_CMSGHDR && go+clen <= glen` against malformed guest input.
- **Non-SCM cmsgs** (e.g. Linux `SCM_CREDENTIALS` type 2): level is translated generically; the
  payload is copied verbatim. `struct ucred` vs macOS `struct cmsgcred` differ, so credentials are
  *not* semantically translated — out of scope, but they won't crash (best-effort passthrough).
- **`msg_control == NULL` / `controllen == 0`**: pass `NULL`/`0` through unchanged (no scratch use).

---

## Bug 2 — abstract-namespace AF_UNIX (`sun_path[0] == '\0'`)

### (a) What the test expects + exact failing path

`dd-tests/guests/edge_abstract.c`: server `bind`s an AF_UNIX socket to abstract name `"\0dd_abstract"`
(`edge_abstract.c:14-20`, `alen = 2 + 1 + 11 = 14`), `listen`s, forks; child `connect`s to the same
abstract address and writes `"Z"` (`edge_abstract.c:23-26`); parent `accept`s and reads. Expected:

```
abstract got=1 byte=Z
```

Failing path: `bind` `case 200` and `connect` `case 203`. The abstract address matches none of the
`lo_*`/`br_*` AF_INET predicates, so it falls straight to the passthrough:

```
service.c:2293   bind((int)a0, (void *)a1, (socklen_t)a2)      // case 200 default
service.c:2386   connect((int)a0, (void *)a1, (socklen_t)a2)   // case 203 default
```

This hands the **guest Linux `sockaddr_un`** to macOS, which (i) has no abstract namespace and
(ii) reads the struct with a different header (see (b)) — `bind` fails, the test prints
`abstract bind_failed` and returns before forking.

### (b) macOS vs Linux differences

- **No abstract namespace on macOS.** Linux denotes it by `sun_path[0] == '\0'`; the name is the
  following `alen - offsetof(sun_path) - 1` bytes (may contain arbitrary bytes incl. NUL and `/`),
  with **no** filesystem entry and **no** NUL terminator.
- **`sockaddr_un` layout differs**, so the guest struct cannot be passed raw even for the address
  bytes:

| field        | Linux                       | macOS                          |
|--------------|-----------------------------|--------------------------------|
| `sun_len`    | (absent)                    | `unsigned char` @0             |
| `sun_family` | `uint16_t` @0               | `sa_family_t` (1 byte) @1      |
| `sun_path`   | `char[108]` @2              | `char[104]` @2                 |

`AF_UNIX == 1` on both. The fix builds a **native** macOS `struct sockaddr_un` (exactly as the
existing `lo_*`/`br_*` code already does at `service.c:2246-2250`), so the layout difference is
handled for free.

### (c) Exact solution — map abstract name to a filesystem socket, keyed by DD_NETNS

Mirror the `lo_*`/`br_*` redirect: an abstract AF_UNIX address is rewritten to a real filesystem
AF_UNIX path under a per-namespace dir, so two guests in the same container rendezvous. Add helpers
to `netns.c` (next to `lo_path`/`br_path`):

```c
static char g_absdir[200]; static int g_abs_init;
static void abs_init(void){
    if (g_abs_init) return; g_abs_init = 1;
    const char *ns = getenv("DD_NETNS");          // same key used by ipc_ns_key (service.c:38)
    snprintf(g_absdir, sizeof g_absdir, "/tmp/.ddabs-%.40s", (ns && ns[0]) ? ns : "default");
    mkdir(g_absdir, 0700);                          // EEXIST fine; peers share it
}
// Is this guest sockaddr an abstract AF_UNIX addr? (family u16==AF_UNIX, sun_path[0]==NUL, name>=1B)
static int abs_is(const uint8_t *sa, socklen_t l){
    return sa && l > 3 && *(uint16_t *)sa == AF_UNIX && sa[2] == 0;   // sun_path[0] @ offset 2
}
// Map abstract name (bytes sa+3 .. for namelen=l-3) to a filesystem path. Hex when it fits, else
// FNV-1a hash (macOS sun_path is only 104 bytes; long D-Bus/X11 names must hash).
static void abs_path(const uint8_t *sa, socklen_t l, char *out, size_t n){
    abs_init();
    const uint8_t *nm = sa + 3; size_t nl = (size_t)l - 3;
    size_t dl = strlen(g_absdir);
    if (dl + 1 + nl*2 + 1 <= n && dl + 1 + nl*2 < 104){           // full hex (unambiguous)
        char hx[210]; static const char *H = "0123456789abcdef";
        for (size_t i = 0; i < nl; i++){ hx[2*i]=H[nm[i]>>4]; hx[2*i+1]=H[nm[i]&15]; }
        hx[2*nl]=0; snprintf(out, n, "%s/%s", g_absdir, hx);
    } else {                                                       // hash fallback
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < nl; i++){ h ^= nm[i]; h *= 1099511628211ull; }
        snprintf(out, n, "%s/h%016llx", g_absdir, (unsigned long long)h);
    }
}
```

In `bind` `case 200`, *before* the `g_nportmap`/default passthrough (i.e. after the `lo_*`/`br_*`
blocks, ~`service.c:2278`):

```c
if (abs_is(sa, (socklen_t)a2)) {
    char up[200]; abs_path(sa, (socklen_t)a2, up, sizeof up);
    unlink(up);                                  // replace stale (cf. lo_ at service.c:2245)
    struct sockaddr_un un; memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
    int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
    G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
    break;
}
```

In `connect` `case 203`, before the default passthrough (~`service.c:2385`):

```c
if (abs_is(sa, (socklen_t)a2)) {
    char up[200]; abs_path(sa, (socklen_t)a2, up, sizeof up);
    struct sockaddr_un un; memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
    int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
    G_RET(c) = (r < 0 && errno != EINPROGRESS) ? (uint64_t)(-errno) : 0;
    break;
}
```

The guest socket is already a host AF_UNIX socket (created via `socket(AF_UNIX,…)`, case 198), so
**no `lo_swap`** is needed — unlike the AF_INET redirects, only the address is rewritten. Both
guests share `DD_NETNS` (the child inherits the parent's env across `fork`), so `abs_path` produces
the identical filesystem path on bind and connect → they rendezvous.

### (d) Edge cases

- **Leading-NUL detection**: key on `family==AF_UNIX && sun_path[0]==0 && alen>3`. A zero-length
  abstract name (`alen==3`) is an autobind request on Linux; treat as not-abstract (fall through) or
  synthesize a unique path — out of test scope.
- **Arbitrary name bytes**: the name may hold NULs, `/`, non-printables → hex/hash encoding makes a
  safe single-path-component filename; no directory traversal.
- **Name length vs `sun_path[104]`**: full hex needs `2*namelen` chars; long names (D-Bus ≈ 36 B,
  X11, systemd) overflow macOS `sun_path`. The FNV-1a fallback keeps every path bounded (`h` + 16
  hex). Collisions are astronomically unlikely and only matter within one `DD_NETNS`.
- **Isolation**: dir is `/tmp/.ddabs-<DD_NETNS>` (0700), matching the `ipc_ns_key`/`lo_`/`br_`
  pattern — two different containers get different dirs and never collide. With `DD_NETNS` unset,
  fall back to `default` (no cross-container isolation, consistent with `lo_on()` being off, and the
  parent/child still share it so the test passes).
- **`getsockname`/`getpeername` on an abstract socket** (`case 204`/`205`): not exercised by the
  test. If needed later, reconstruct the abstract `sockaddr_un` from the stored name — not required
  for the gate.
- **Stale socket files**: `bind` `unlink`s first; files live in `/tmp/.ddabs-*` and are cleaned with
  the rest of the `/tmp/.dd*` rendezvous dirs.

---

## (e) Test gate

- `dd-tests/src/cases/mod.rs:137` — `edge/scmrights` currently `.xfail(lin)`. After Bug 1, it
  produces `scmrights got_fd=1 data=fd-passed-ok`, matches the native oracle, and reports **XPASS**
  (`dd-tests/src/main.rs:88`). Remove the `.xfail(lin)` marker.
- `dd-tests/src/cases/mod.rs:143` — `edge/abstract` currently `.xfail(lin)`. After Bug 2, it
  produces `abstract got=1 byte=Z`, matches the oracle, and reports **XPASS**. Remove `.xfail(lin)`.
- Both run on `Engine::LinuxAarch64` and `Engine::LinuxX86_64` (`lin`, `mod.rs:133`); verify on both.
- No regression: `edge/msgflags` (`edge_msgpeek.c`, `mod.rs:142`, already green) must stay green —
  it exercises `recvmsg` with `MSG_PEEK`/`MSG_DONTWAIT` and **no** control buffer, so the new
  `gc && gcl` NULL-control guard leaves it on the existing path. The cmsg helpers only engage when a
  control buffer is present; the abstract helpers only engage when `family==AF_UNIX && sun_path[0]==0`,
  so AF_INET (`lo_*`/`br_*`/port-map) and filesystem-path AF_UNIX paths are untouched. Run the full
  `edge` group plus the socket-heavy suites (port-map, netns, bridge) to confirm no fallout.

### Related (not required for the gate)

The filesystem-path AF_UNIX passthrough (`service.c:2293`/`2386`) hands the guest's Linux
`sockaddr_un` to macOS raw, which has the same header-layout mismatch as (b) and is likely also
subtly broken. The native-`sockaddr_un`-rebuild approach used above generalizes to it (translate
the path bytes from guest offset 2 into a fresh macOS `sockaddr_un`). The in-flight `service.c`
change may already touch this; coordinate so the abstract `abs_is` check runs *before* any general
AF_UNIX translation.
