// dd/runtime/frontend/x86_64 -- the container/OS layer (jit86's own copy pending the os/linux dedup):
// rootfs path-jail, Linux<->Darwin socket xlate, NET-ns loopback + port-map, signalfd + signal delivery,
// overlay image layers.

// ---------------- rootfs path rewriting (ported from jit.c) ----------------
static const char *g_rootfs = NULL;
static char g_cwd[4096] = "/";        // guest cwd (container model). host cwd is kept at xlate(g_cwd).
static unsigned g_hostuid, g_hostgid; // real host ids -> mapped to container root in stat (userns model)
static unsigned map_uid(unsigned u) { return u == g_hostuid ? 0 : u; }
static unsigned map_gid(unsigned g) { return g == g_hostgid ? 0 : g; }
static const char *xlate(const char *p, char *buf, size_t n); // fwd: guest path -> host (rootfs-jailed)
int g_ngroups;
int g_groups[64]; // container root's supplementary groups (from rootfs /etc/group)
void build_root_groups(void) {
    static int done;
    if (done) return;
    done = 1;
    g_groups[g_ngroups++] = 0; // primary gid 0 (root)
    char path[4200];
    snprintf(path, sizeof path, "%s/etc/group", g_rootfs ? g_rootfs : "");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f) && g_ngroups < 64) { // name:passwd:gid:member,member,...
        char *save, *name = strtok_r(line, ":", &save);
        (void)strtok_r(NULL, ":", &save);
        char *gids = strtok_r(NULL, ":", &save), *members = strtok_r(NULL, "\n", &save);
        (void)name;
        if (!gids) continue;
        int gid = atoi(gids);
        if (gid == 0) continue;
        int is = 0;
        if (members) {
            char *s2;
            for (char *m = strtok_r(members, ",", &s2); m; m = strtok_r(NULL, ",", &s2))
                if (strcmp(m, "root") == 0) {
                    is = 1;
                    break;
                }
        }
        if (is && g_ngroups < 64) g_groups[g_ngroups++] = gid;
    }
    fclose(f);
}

// Darwin errno -> Linux errno (values diverge from 35 up; <35 are identical). Used for host
// syscall failures so the Linux guest sees the right code (EAGAIN, EINPROGRESS, ECONNREFUSED...).
static int derr(int e) {
    switch (e) {
    case 35: return 11;
    case 36: return 115;
    case 37: return 114;
    case 38: return 88;
    case 39: return 89;
    case 40: return 90;
    case 41: return 91;
    case 42: return 92;
    case 43: return 93;
    case 44: return 94;
    case 45: return 95;
    case 46: return 96;
    case 47: return 97;
    case 48: return 98;
    case 49: return 99;
    case 50: return 100;
    case 51: return 101;
    case 52: return 102;
    case 53: return 103;
    case 54: return 104;
    case 55: return 105;
    case 56: return 106;
    case 57: return 107;
    case 58: return 108;
    case 59: return 109;
    case 60: return 110;
    case 61: return 111;
    case 62: return 40;
    case 63: return 36;
    case 64: return 112;
    case 65: return 113;
    case 66: return 39;
    case 68: return 87;
    case 69: return 122;
    case 70: return 116;
    case 78: return 38;
    case 89: return 125;
    case 92: return 61;
    case 93: return 62;
    case 94: return 67;
    default: return e;
    }
}
// ---- Linux<->Darwin socket translation (jit86 runs on macOS; the guest speaks Linux ABI) ----
// SOL_SOCKET option numbers differ; map the common ones (level 1 == Linux SOL_SOCKET -> Darwin 0xffff).
static int so_l2d(int opt) {
    switch (opt) {
    case 2: return 0x0004;
    case 9: return 0x0008;
    case 5: return 0x0010;
    case 6: return 0x0020;
    case 13: return 0x0080;
    case 10: return 0x0100;
    case 15: return 0x0200;
    case 7: return 0x1001;
    case 8: return 0x1002;
    case 21: return 0x1005;
    case 20: return 0x1006;
    case 4: return 0x1007;
    case 3: return 0x1008;
    default: return opt;
    }
}
static int socktype_l2d(int t) {
    return t & 0xff;
} // Linux SOCK_NONBLOCK(0x800)/CLOEXEC(0x80000) live in the type field; Darwin sets them via fcntl
// guest (Linux) sockaddr -> host (Darwin); returns host len (0 = unknown family)
static socklen_t l2d_sa(const void *lin, socklen_t llen, struct sockaddr_storage *d) {
    memset(d, 0, sizeof *d);
    if (!lin || llen < 2) return 0;
    int fam = *(const uint16_t *)lin;
    if (fam == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)d;
        s->sin_len = sizeof *s;
        s->sin_family = AF_INET;
        memcpy(&s->sin_port, (const char *)lin + 2, 2);
        memcpy(&s->sin_addr, (const char *)lin + 4, 4);
        return sizeof *s;
    } else if (fam == 10) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)d;
        s->sin6_len = sizeof *s;
        s->sin6_family = AF_INET6;
        memcpy(&s->sin6_port, (const char *)lin + 2, 2);
        memcpy(&s->sin6_flowinfo, (const char *)lin + 4, 4);
        memcpy(&s->sin6_addr, (const char *)lin + 8, 16);
        memcpy(&s->sin6_scope_id, (const char *)lin + 24, 4);
        return sizeof *s;
    } else if (fam == AF_UNIX) {
        struct sockaddr_un *s = (struct sockaddr_un *)d;
        s->sun_family = AF_UNIX;
        const char *p = (const char *)lin + 2;
        if (p[0] == 0) {
            socklen_t n = llen - 2;
            if (n > (socklen_t)sizeof s->sun_path) n = sizeof s->sun_path;
            memcpy(s->sun_path, p, n);
            s->sun_len = (uint8_t)llen;
            return llen;
        } // abstract socket
        char pb[4200];
        const char *hp = xlate(p, pb, sizeof pb);
        snprintf(s->sun_path, sizeof s->sun_path, "%s", hp);
        s->sun_len = sizeof *s;
        return (socklen_t)(2 + strlen(s->sun_path) + 1 + sizeof(struct sockaddr_un) - sizeof s->sun_path - 2);
    }
    return 0;
}
// host (Darwin) sockaddr -> guest (Linux) buffer; updates *glen to the full (untruncated) length
static void d2l_sa(const struct sockaddr *d, void *gout, socklen_t cap, socklen_t *glen) {
    char buf[128];
    memset(buf, 0, sizeof buf);
    socklen_t full = 0;
    if (!d) {
        if (glen) *glen = 0;
        return;
    }
    if (d->sa_family == AF_INET) {
        struct sockaddr_in *s = (struct sockaddr_in *)d;
        *(uint16_t *)buf = AF_INET;
        memcpy(buf + 2, &s->sin_port, 2);
        memcpy(buf + 4, &s->sin_addr, 4);
        full = 16;
    } else if (d->sa_family == AF_INET6) {
        struct sockaddr_in6 *s = (struct sockaddr_in6 *)d;
        *(uint16_t *)buf = 10;
        memcpy(buf + 2, &s->sin6_port, 2);
        memcpy(buf + 4, &s->sin6_flowinfo, 4);
        memcpy(buf + 8, &s->sin6_addr, 16);
        memcpy(buf + 24, &s->sin6_scope_id, 4);
        full = 28;
    } else if (d->sa_family == AF_UNIX) {
        struct sockaddr_un *s = (struct sockaddr_un *)d;
        *(uint16_t *)buf = AF_UNIX;
        snprintf(buf + 2, sizeof buf - 2, "%s", s->sun_path);
        full = (socklen_t)(2 + strlen(s->sun_path) + 1);
    } else {
        *(uint16_t *)buf = d->sa_family;
        full = 2;
    }
    if (gout && cap) memcpy(gout, buf, cap < full ? cap : full);
    if (glen) *glen = full;
}

// ---- private loopback: route the container's 127.0.0.0/8 TCP to AF_UNIX sockets under a per-
// container dir (g_netns). Isolates each container's localhost AND sidesteps the macOS-host
// loopback quirk (a forked child can't connect to the parent's 127.x TCP port on the bridge).
static char g_netns[200];           // per-container host dir for loopback unix sockets ("" = off)
static uint16_t g_lo_port[1024];    // fd -> loopback port it's bound/connected to (0 = not private-lo)
static uint8_t g_sock_stream[1024]; // fd -> 1 if AF_INET SOCK_STREAM (only those get loopback isolation)
static int g_eventfd_peer[1024];    // eventfd(read-end) -> pipe write-end + 1 (0 = not an eventfd)
static uint8_t g_timerfd[1024];     // fd is a timerfd (kqueue + EVFILT_TIMER) -> read() drains it
static uint8_t g_inotify[1024];     // fd is an inotify (kqueue + EVFILT_VNODE watches) -> read() drains it
// ---- port-map (docker -p H:C): bind(:C) actually binds host :H; getsockname reports :C back ----
static struct {
    uint16_t cport, hport;
} g_portmap[32];
static int g_nportmap = 0;
static uint16_t g_fd_cport[1024]; // fd -> the container port it bound (for getsockname)
static uint16_t pm_host(uint16_t cp) {
    for (int i = 0; i < g_nportmap; i++)
        if (g_portmap[i].cport == cp) return g_portmap[i].hport;
    return cp;
}
static void parse_publish(const char *s) { // "H:C,H:C,..." (docker -p order: host:container)
    while (s && *s && g_nportmap < 32) {
        int h = atoi(s);
        const char *colon = strchr(s, ':');
        if (!colon) break;
        int cc = atoi(colon + 1);
        if (h > 0 && cc > 0) {
            g_portmap[g_nportmap].cport = (uint16_t)cc;
            g_portmap[g_nportmap].hport = (uint16_t)h;
            g_nportmap++;
        }
        const char *comma = strchr(s, ',');
        if (!comma) break;
        s = comma + 1;
    }
}
// ---- signalfd: a self-pipe poked by a host signal handler (guest reads signalfd_siginfo) ----
// Linux x86-64 signo == Linux aarch64 signo (generic), but macOS differs -> translate at the boundary.
static int g_sigfd_pipe[2] = {-1, -1}; // signalfd self-pipe (write end poked from host_sigh)
static int g_sigfd_read = -1;          // its read end (the guest's signalfd)
static volatile uint64_t g_sigfd_mask; // signals routed to the signalfd (1<<signo)
static volatile uint64_t g_pending;    // pending-signal bitmask (1<<signo), set by host_sigh
static int sig_is_sync(int s) {
    return s == 4 || s == 5 || s == 7 || s == 8 || s == 11;
} // ILL TRAP BUS FPE SEGV (Linux nums)
static int sig_l2m(int s) {
    static const unsigned char T[32] = {0,  1,  2,  3,  4,  5,  6,  10, 8,  9,  30, 11, 31, 13, 14, 15,
                                        16, 20, 19, 17, 18, 21, 22, 16, 24, 25, 26, 27, 28, 23, 30, 12};
    return (s >= 1 && s <= 31) ? T[s] : s;
}
static int sig_m2l(int s) {
    static const unsigned char T[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  7,  11, 31, 13, 14, 15,
                                        23, 19, 20, 18, 17, 21, 22, 29, 24, 25, 26, 27, 28, 29, 10, 12};
    return (s >= 1 && s <= 31) ? T[s] : s;
}
static void host_sigh(int sig) {
    int ls = sig_m2l(sig); // host(macOS) signo -> Linux
    __atomic_or_fetch(&g_pending, 1ull << ls, __ATOMIC_SEQ_CST);
    if ((g_sigfd_mask & (1ull << ls)) && g_sigfd_pipe[1] >= 0) {
        char b = (char)ls;
        if (write(g_sigfd_pipe[1], &b, 1) < 0) {}
    } // wake signalfd/epoll
}
// ---- guest signal delivery: build a Linux x86-64 rt_sigframe and redirect to the handler ----
static struct {
    uint64_t handler, flags, mask;
} g_sigact[65];                        // per-signal disposition (mask in sigset_t convention)
#define SIGRETURN_PC 0xFFFFFFFFFFF0ull // sentinel return address: handler ret -> sigreturn
// x86-64 sigcontext gregs index -> guest cpu->r[] index (r8..r15,rdi,rsi,rbp,rbx,rdx,rax,rcx,rsp; then rip,eflags)
static const int GREG2R[16] = {8, 9, 10, 11, 12, 13, 14, 15, 7, 6, 5, 3, 2, 0, 1, 4}; // gregs[0..15]
static uint64_t nzcv_to_eflags(uint64_t nz) {
    uint64_t f = 0x2; // bit1 reserved (always 1)
    if (!((nz >> 29) & 1)) f |= 1u << 0;
    if ((nz >> 30) & 1) f |= 1u << 6; // CF (stored inverted), ZF
    if ((nz >> 31) & 1) f |= 1u << 7;
    if ((nz >> 28) & 1) f |= 1u << 11; // SF, OF
    return f;
}
static uint64_t eflags_to_nzcv(uint64_t f) {
    uint64_t nz = 0;
    if (!(f & 1)) nz |= 1u << 29;
    if (f & (1u << 6)) nz |= 1u << 30; // CF (invert), ZF
    if (f & (1u << 7)) nz |= 1u << 31;
    if (f & (1u << 11)) nz |= 1u << 28; // SF, OF
    return nz;
}
static void build_signal_frame(struct cpu *c, int sig) {
    uint64_t sp = (c->r[4] - 2048) & ~15ull;                        // 16-aligned frame base; uc lives here
    uint64_t uc = sp, mc = uc + 40, info = uc + 512, xs = uc + 768; // ucontext / mcontext(gregs) / siginfo / xmm save
    memset((void *)sp, 0, 2048);
    for (int i = 0; i < 16; i++)
        *(uint64_t *)(mc + i * 8) = c->r[GREG2R[i]];      // gregs[0..15]
    *(uint64_t *)(mc + 16 * 8) = c->rip;                  // gregs[16] = RIP
    *(uint64_t *)(mc + 17 * 8) = nzcv_to_eflags(c->nzcv); // gregs[17] = EFL
    *(uint64_t *)(uc + 296) = c->sigmask;                 // uc_sigmask (restored on sigreturn)
    memcpy((void *)xs, c->v, sizeof c->v);                // preserve guest xmm across the handler
    *(int *)(info + 0) = sig;                             // siginfo.si_signo
    uint64_t rsp = sp - 8;
    *(uint64_t *)rsp = SIGRETURN_PC; // pushed return address
    c->r[7] = (uint64_t)sig;
    c->r[6] = info;
    c->r[2] = uc; // handler(signo, siginfo*, ucontext*) in rdi,rsi,rdx
    c->r[4] = rsp;
    c->rip = g_sigact[sig].handler;
    c->sigmask |= g_sigact[sig].mask;
    if (!(g_sigact[sig].flags & 0x40000000)) c->sigmask |= (1ull << (sig - 1)); // SA_NODEFER off -> block this signal
    if (g_trace)
        fprintf(stderr, "[sig] deliver %d handler=%llx rsp=%llx\n", sig, (unsigned long long)c->rip,
                (unsigned long long)rsp);
}
static void do_sigreturn(struct cpu *c) {
    uint64_t uc = c->r[4], mc = uc + 40, xs = uc + 768; // after the handler's ret, rsp == uc
    for (int i = 0; i < 16; i++)
        c->r[GREG2R[i]] = *(uint64_t *)(mc + i * 8);
    c->rip = *(uint64_t *)(mc + 16 * 8);
    c->nzcv = eflags_to_nzcv(*(uint64_t *)(mc + 17 * 8));
    c->sigmask = *(uint64_t *)(uc + 296);
    memcpy(c->v, (void *)xs, sizeof c->v);
}
static void maybe_deliver_signal(struct cpu *c) {
    uint64_t p = __atomic_load_n(&g_pending, __ATOMIC_SEQ_CST);
    for (int sig = 1; sig <= 64; sig++) {
        uint64_t bit = 1ull << sig;
        if (!(p & bit) || (c->sigmask & (1ull << (sig - 1)))) continue; // pending and not blocked
        uint64_t h = g_sigact[sig].handler;
        if (h <= 1) {
            __atomic_and_fetch(&g_pending, ~bit, __ATOMIC_SEQ_CST);
            continue;
        } // SIG_DFL/IGN: host already acted
        if (__atomic_fetch_and(&g_pending, ~bit, __ATOMIC_SEQ_CST) & bit) {
            build_signal_frame(c, sig);
            return;
        }
    }
}
// A signal aimed at our own process (raise/abort/kill self): deliver through our machinery
// (host signals into a MAP_JIT thread are fragile) -- custom handler -> pending; else default action.
static void raise_guest_signal(struct cpu *c, int sig) {
    if (sig < 1 || sig > 64) return;
    uint64_t h = g_sigact[sig].handler;
    if (h > 1) {
        __atomic_or_fetch(&g_pending, 1ull << sig, __ATOMIC_SEQ_CST);
        return;
    }                                              // custom handler
    if (h == 1) return;                            // SIG_IGN
    if (c && (c->sigmask & (1ull << (sig - 1)))) { // blocked -> pending (signalfd/unblock)
        __atomic_or_fetch(&g_pending, 1ull << sig, __ATOMIC_SEQ_CST);
        if ((g_sigfd_mask & (1ull << sig)) && g_sigfd_pipe[1] >= 0) {
            char b = (char)sig;
            if (write(g_sigfd_pipe[1], &b, 1) < 0) {}
        }
        return;
    }
    if (sig == 17 || sig == 18 || sig == 23 || sig == 28) return; // SIGCHLD/CONT/URG/WINCH: ignore
    signal(sig_l2m(sig), SIG_DFL);
    raise(sig_l2m(sig)); // default: die by the signal (host signo)
    c->exited = 1;
    c->exit_code = 128 + sig; // fallback
}
static int lo_on(void) { return g_netns[0] != 0; }
static int lo_is(const uint8_t *sa, socklen_t l) { return sa && l >= 8 && *(uint16_t *)sa == AF_INET && sa[4] == 127; }
static void lo_path(uint16_t port, char *out, size_t n) { snprintf(out, n, "%s/p%u", g_netns, (unsigned)port); }
static int lo_swap(int fd) { // replace the AF_INET socket at fd with a fresh AF_UNIX one
    int fl = fcntl(fd, F_GETFL), df = fcntl(fd, F_GETFD);
    int u = socket(AF_UNIX, SOCK_STREAM, 0);
    if (u < 0) return -1;
    if (u != fd) {
        if (dup2(u, fd) < 0) {
            close(u);
            return -1;
        }
        close(u);
    }
    if (fl >= 0 && (fl & O_NONBLOCK)) fcntl(fd, F_SETFL, O_NONBLOCK);
    if (df >= 0 && (df & FD_CLOEXEC)) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}
static void fill_inet_lo(uint8_t *sa, socklen_t *l, uint16_t port) { // report 127.0.0.1:port to the guest
    if (!sa) return;
    *(uint16_t *)(sa + 0) = AF_INET;
    *(uint16_t *)(sa + 2) = htons(port);
    *(uint32_t *)(sa + 4) = 0x0100007fu;
    memset(sa + 8, 0, 8);
    if (l) *l = 16;
}
// Bind-mount volumes: a guest path prefix -> a host directory (docker -v). Set via JIT86_VOL.
struct vol {
    char guest[256];
    size_t glen;
    char host[1024];
};
static struct vol g_vols[32];
static int g_nvols;
// Host root for an absolute guest path: a matching volume (longest prefix), else the rootfs.
// *rel receives the path within that root (so root+rel = host path).
static const char *vol_root(const char *abs, const char **rel) {
    int best = -1;
    size_t bl = 0;
    for (int i = 0; i < g_nvols; i++)
        if (!strncmp(abs, g_vols[i].guest, g_vols[i].glen) &&
            (abs[g_vols[i].glen] == '/' || abs[g_vols[i].glen] == 0) && g_vols[i].glen >= bl) {
            best = i;
            bl = g_vols[i].glen;
        }
    if (best >= 0) {
        *rel = abs[g_vols[best].glen] ? abs + g_vols[best].glen : "/";
        return g_vols[best].host;
    }
    *rel = abs;
    return g_rootfs;
}
static void add_vol(const char *spec) { // "guestpath:hostdir"
    if (!spec || g_nvols >= 32) return;
    char t[2048];
    snprintf(t, sizeof t, "%s", spec);
    char *colon = strrchr(t, ':');
    if (!colon) return;
    *colon = 0;
    char *gp = t, *hp = colon + 1;
    if (gp[0] != '/') return;
    char hc[1024];
    if (!realpath(hp, hc)) snprintf(hc, sizeof hc, "%s", hp);
    snprintf(g_vols[g_nvols].guest, sizeof g_vols[g_nvols].guest, "%s", gp);
    size_t gl = strlen(g_vols[g_nvols].guest);
    while (gl > 1 && g_vols[g_nvols].guest[gl - 1] == '/')
        g_vols[g_nvols].guest[--gl] = 0;
    g_vols[g_nvols].glen = gl;
    snprintf(g_vols[g_nvols].host, sizeof g_vols[g_nvols].host, "%s", hc);
    g_nvols++;
}
// ---- Overlay (OCI image layers): --rootfs is the writable UPPER; --lower dirs are read-only,
// searched top->down when a path isn't in the upper. A .wh.NAME whiteout hides a lower entry;
// copy-up brings a lower file into the upper on write. Off entirely when g_nlower==0. ----
struct olayer {
    char root[1024];
    size_t rlen;
};
static struct olayer g_lower[8];
static int g_nlower = 0;         // [0] = highest-priority lower (searched first)
static char g_ovldir[1024][256]; // dir-fd -> its GUEST path (for merged getdents); "" = not overlay
static int g_ovlcur[1024];       // dir-fd -> merged-listing cursor (entries already emitted)
static void add_lower(const char *dir) {
    if (g_nlower >= 8 || !dir || !dir[0]) return;
    char rc[1024];
    if (!realpath(dir, rc)) snprintf(rc, sizeof rc, "%s", dir);
    snprintf(g_lower[g_nlower].root, sizeof g_lower[g_nlower].root, "%s", rc);
    g_lower[g_nlower].rlen = strlen(rc);
    g_nlower++;
}
// One layer's host path for an absolute guest path, following symlinks LAYER-relative (like xresolve).
static void layer_path(const char *root, const char *guest, char *buf, size_t n, int follow) {
    char cpath[1024];
    snprintf(cpath, sizeof cpath, "%s", guest);
    if (follow)
        for (int i = 0; i < 40; i++) {
            char h[4200];
            snprintf(h, sizeof h, "%s%s", root, cpath);
            char lk[1024];
            ssize_t k = readlink(h, lk, sizeof lk - 1);
            if (k < 0) break;
            lk[k] = 0;
            if (lk[0] == '/')
                snprintf(cpath, sizeof cpath, "%s", lk);
            else {
                char *sl = strrchr(cpath, '/');
                int d = sl ? (int)(sl - cpath) : 0;
                char tmp[1024];
                snprintf(tmp, sizeof tmp, "%.*s/%s", d, cpath, lk);
                snprintf(cpath, sizeof cpath, "%s", tmp);
            }
        }
    snprintf(buf, n, "%s%s", root, cpath);
}
static void wh_path(const char *root, const char *guest, char *buf, size_t n) { // host path of the .wh.NAME marker
    char par[1024];
    snprintf(par, sizeof par, "%s", guest);
    char *sl = strrchr(par, '/');
    char base[256];
    snprintf(base, sizeof base, "%s", sl ? sl + 1 : par);
    if (sl) *sl = 0;
    snprintf(buf, n, "%s%s/.wh.%s", root, par, base);
}
static int wh_exists(const char *root, const char *guest) {
    char h[4300];
    wh_path(root, guest, h, sizeof h);
    struct stat st;
    return lstat(h, &st) == 0;
}
// READ resolve: topmost layer that has `guest`. Returns 1 + host on hit; 0 (+ upper path) if absent/whiteout-hidden.
static int overlay_resolve(const char *guest, char *host, size_t hn, int nofollow) {
    char up[4300];
    layer_path(g_rootfs, guest, up, sizeof up, !nofollow);
    struct stat st;
    if (lstat(up, &st) == 0) {
        snprintf(host, hn, "%s", up);
        return 1;
    } // upper shadows lowers
    if (wh_exists(g_rootfs, guest)) {
        snprintf(host, hn, "%s", up);
        return 0;
    } // deleted in upper
    for (int i = 0; i < g_nlower; i++) {
        char lp[4300];
        layer_path(g_lower[i].root, guest, lp, sizeof lp, !nofollow);
        if (lstat(lp, &st) == 0) {
            snprintf(host, hn, "%s", lp);
            return 1;
        }
        if (wh_exists(g_lower[i].root, guest)) {
            snprintf(host, hn, "%s", up);
            return 0;
        }
    }
    snprintf(host, hn, "%s", up);
    return 0; // absent -> upper path (ENOENT / O_CREAT)
}
// Copy-up: bring a lower file into the UPPER so it can be modified; returns the upper host path.
static void overlay_copyup(const char *guest, char *host, size_t hn) {
    layer_path(g_rootfs, guest, host, hn, 1);
    struct stat st;
    if (lstat(host, &st) == 0) return; // already writable in upper
    if (wh_exists(g_rootfs, guest)) {
        char wh[4300];
        wh_path(g_rootfs, guest, wh, sizeof wh);
        unlink(wh);
        return;
    } // recreate
    char src[4300];
    int have = 0;
    for (int i = 0; i < g_nlower && !have; i++) {
        layer_path(g_lower[i].root, guest, src, sizeof src, 1);
        if (lstat(src, &st) == 0 && S_ISREG(st.st_mode))
            have = 1;
        else if (wh_exists(g_lower[i].root, guest))
            break;
    }
    if (!have) return; // new file -> upper path as-is
    char dir[4300];
    snprintf(dir, sizeof dir, "%s", host);
    char *sl = strrchr(dir, '/');
    size_t rl = strlen(g_rootfs);
    if (sl) {
        *sl = 0;
        for (char *q = dir + rl + 1; *q; q++)
            if (*q == '/') {
                *q = 0;
                mkdir(dir, 0755);
                *q = '/';
            }
        mkdir(dir, 0755);
    }
    int in = open(src, O_RDONLY), out = open(host, O_CREAT | O_WRONLY | O_TRUNC, st.st_mode & 0777);
    if (in >= 0 && out >= 0) {
        char b[1 << 16];
        ssize_t r;
        while ((r = read(in, b, sizeof b)) > 0)
            if (write(out, b, r) < 0) break;
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
}
static int overlay_readdir(const char *gdir, char names[][256], uint8_t *types,
                           int max) { // merged listing (upper first)
    static char seen[2048][256];
    int ns = 0, nout = 0;
    for (int L = -1; L < g_nlower && nout < max; L++) {
        const char *root = (L < 0) ? g_rootfs : g_lower[L].root;
        char host[4300];
        layer_path(root, gdir, host, sizeof host, 1);
        DIR *d = opendir(host);
        if (!d) continue;
        struct dirent *e;
        while ((e = readdir(d)) && nout < max) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            int wh = !strncmp(e->d_name, ".wh.", 4);
            const char *name = wh ? e->d_name + 4 : e->d_name;
            int dup = 0;
            for (int i = 0; i < ns; i++)
                if (!strcmp(seen[i], name)) {
                    dup = 1;
                    break;
                }
            if (dup) continue;
            if (ns < 2048) snprintf(seen[ns++], 256, "%s", name); // higher layer already decided this name
            if (!wh) {
                snprintf(names[nout], 256, "%s", name);
                types[nout] = e->d_type;
                nout++;
            } // whiteout -> hide
        }
        closedir(d);
    }
    return nout;
}
static void overlay_whiteout(const char *guest) { // delete: drop upper copy + .wh marker
    char up[4300];
    layer_path(g_rootfs, guest, up, sizeof up, 1);
    remove(up);
    char wh[4300];
    wh_path(g_rootfs, guest, wh, sizeof wh);
    char dir[4300];
    snprintf(dir, sizeof dir, "%s", wh);
    char *s2 = strrchr(dir, '/');
    size_t rl = strlen(g_rootfs);
    if (s2) {
        *s2 = 0;
        for (char *q = dir + rl + 1; *q; q++)
            if (*q == '/') {
                *q = 0;
                mkdir(dir, 0755);
                *q = '/';
            }
        mkdir(dir, 0755);
    }
    int fd = open(wh, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
}
static const char *xlate(const char *p, char *buf, size_t n) {
    if (p && p[0] == '/') {
        const char *rel;
        const char *root = vol_root(p, &rel);
        if (root == g_rootfs && g_nlower) {
            overlay_resolve(p, buf, n, 1);
            return buf;
        } // overlay (nofollow)
        if (root) {
            snprintf(buf, n, "%s%s", root, rel);
            return buf;
        }
    }
    return p;
}
static const char *xresolve(const char *p, char *buf, size_t n) {
    if (!p || p[0] != '/') return p;
    const char *rel0;
    const char *root = vol_root(p, &rel0);
    if (!root) return p;
    if (root == g_rootfs && g_nlower) {
        overlay_resolve(p, buf, n, 0);
        return buf;
    } // overlay (follow symlinks)
    char cpath[1024];
    snprintf(cpath, sizeof cpath, "%s", rel0);
    for (int i = 0; i < 40; i++) {
        char h[4200];
        snprintf(h, sizeof h, "%s%s", root, cpath);
        char lk[1024];
        ssize_t k = readlink(h, lk, sizeof lk - 1);
        if (k < 0) break;
        lk[k] = 0;
        if (lk[0] == '/')
            snprintf(cpath, sizeof cpath, "%s", lk);
        else {
            char *sl = strrchr(cpath, '/');
            int d = sl ? (int)(sl - cpath) : 0;
            char tmp[1024];
            snprintf(tmp, sizeof tmp, "%.*s/%s", d, cpath, lk);
            snprintf(cpath, sizeof cpath, "%s", tmp);
        }
    }
    snprintf(buf, n, "%s%s", root, cpath);
    return buf;
}
#define ATFD(a) (((int)(a) == -100) ? AT_FDCWD : (int)(a))
static const char *atpath(const char *raw, char *buf, size_t n) {
    return (raw && raw[0] == '/') ? xresolve(raw, buf, n) : raw;
}
// x86-64 Linux `struct stat` layout (DIFFERS from aarch64): dev@0 ino@8 nlink@16(8B)
// mode@24 uid@28 gid@32 rdev@40 size@48 blksize@56 blocks@64 atime@72 mtime@88 ctime@104.
static void fill_linux_stat(uint8_t *d, const struct stat *s) {
    memset(d, 0, 144);
    *(uint64_t *)(d + 0) = s->st_dev;
    *(uint64_t *)(d + 8) = s->st_ino;
    *(uint64_t *)(d + 16) = s->st_nlink ? s->st_nlink : 1;
    *(uint32_t *)(d + 24) = s->st_mode;
    *(uint32_t *)(d + 28) = map_uid(s->st_uid);
    *(uint32_t *)(d + 32) = map_gid(s->st_gid);
    *(uint64_t *)(d + 40) = s->st_rdev;
    *(uint64_t *)(d + 48) = s->st_size;
    *(uint64_t *)(d + 56) = 4096;
    *(uint64_t *)(d + 64) = s->st_blocks;
    *(uint64_t *)(d + 72) = s->st_atime;  // atime sec
    *(uint64_t *)(d + 88) = s->st_mtime;  // mtime sec
    *(uint64_t *)(d + 104) = s->st_ctime; // ctime sec
}
static int mmap_flags(int lf) {
    int f = 0;
    if (lf & 0x01) f |= MAP_SHARED;
    if (lf & 0x02) f |= MAP_PRIVATE;
    if (lf & 0x10) f |= MAP_FIXED;
    if (lf & 0x20) f |= MAP_ANON;
    return f;
}
