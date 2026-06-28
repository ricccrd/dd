// dd/runtime/os/linux/container -- termios (Linux<->macOS) + NET-ns private loopback (127/8 -> AF_UNIX).

// ---- termios: Linux <-> macOS. Different field width (4 vs 8B flags), bit values, and c_cc order.
// Linux struct termios (TCGETS): c_iflag/oflag/cflag/lflag @0,4,8,12 (u32); c_line@16; c_cc[19]@17.
static const uint32_t TIO_I[][2] = {{0x1, IGNBRK},  {0x2, BRKINT},   {0x4, IGNPAR},    {0x8, PARMRK},  {0x10, INPCK},
                                    {0x20, ISTRIP}, {0x40, INLCR},   {0x80, IGNCR},    {0x100, ICRNL}, {0x400, IXON},
                                    {0x800, IXANY}, {0x1000, IXOFF}, {0x2000, IMAXBEL}};
static const uint32_t TIO_O[][2] = {{0x1, OPOST}, {0x4, ONLCR}, {0x8, OCRNL}, {0x10, ONOCR}, {0x20, ONLRET}};
static const uint32_t TIO_C[][2] = {{0x40, CSTOPB},  {0x80, CREAD},  {0x100, PARENB},
                                    {0x200, PARODD}, {0x400, HUPCL}, {0x800, CLOCAL}};
static const uint32_t TIO_L[][2] = {{0x1, ISIG},    {0x2, ICANON},  {0x8, ECHO},     {0x10, ECHOE},   {0x20, ECHOK},
                                    {0x40, ECHONL}, {0x80, NOFLSH}, {0x100, TOSTOP}, {0x8000, IEXTEN}};
static const int CC_L2M[17] = {
    VINTR, VQUIT, VERASE, VKILL,    VEOF,     VTIME,   VMIN,   -1,   VSTART,
    // Linux c_cc index -> macOS index
    VSTOP, VSUSP, VEOL,   VREPRINT, VDISCARD, VWERASE, VLNEXT, VEOL2};
static uint32_t map_bits(uint32_t v, const uint32_t t[][2], int n, int fwd) {
    uint32_t o = 0;
    for (int i = 0; i < n; i++) {
        if (fwd) {
            if (v & t[i][0]) o |= t[i][1];
        } else {
            if (v & t[i][1]) o |= t[i][0];
        }
    }
    return o;
}
static void termios_l2m(const uint8_t *L, struct termios *M) {
    memset(M, 0, sizeof *M);
    uint32_t li = *(uint32_t *)(L + 0), lo = *(uint32_t *)(L + 4), lc = *(uint32_t *)(L + 8),
             ll = *(uint32_t *)(L + 12);
    M->c_iflag = map_bits(li, TIO_I, 13, 1);
    M->c_oflag = map_bits(lo, TIO_O, 5, 1);
    M->c_cflag = map_bits(lc, TIO_C, 6, 1);
    M->c_lflag = map_bits(ll, TIO_L, 9, 1);
    int csz = lc & 0x30;
    M->c_cflag |= (csz == 0x30 ? CS8 : csz == 0x20 ? CS7 : csz == 0x10 ? CS6 : CS5);
    const uint8_t *lcc = L + 17;
    for (int i = 0; i < 17; i++)
        if (CC_L2M[i] >= 0) M->c_cc[CC_L2M[i]] = lcc[i];
}
static void termios_m2l(const struct termios *M, uint8_t *L) {
    memset(L, 0, 36);
    uint32_t li = map_bits((uint32_t)M->c_iflag, TIO_I, 13, 0), lo = map_bits((uint32_t)M->c_oflag, TIO_O, 5, 0);
    uint32_t lc = map_bits((uint32_t)M->c_cflag, TIO_C, 6, 0), ll = map_bits((uint32_t)M->c_lflag, TIO_L, 9, 0);
    int csz = M->c_cflag & CSIZE;
    lc |= (csz == CS8 ? 0x30 : csz == CS7 ? 0x20 : csz == CS6 ? 0x10 : 0);
    *(uint32_t *)(L + 0) = li;
    *(uint32_t *)(L + 4) = lo;
    *(uint32_t *)(L + 8) = lc;
    *(uint32_t *)(L + 12) = ll;
    uint8_t *lcc = L + 17;
    for (int i = 0; i < 17; i++)
        if (CC_L2M[i] >= 0) lcc[i] = M->c_cc[CC_L2M[i]];
}
// Linux MSG_* -> macOS MSG_* (they differ for TRUNC/DONTWAIT/EOR/WAITALL).
static int msgflags_l2m(int lf) {
    // OOB/PEEK/DONTROUTE identical
    int mf = lf & (0x1 | 0x2 | 0x4);
    // MSG_TRUNC
    if (lf & 0x20) mf |= 0x10;
    // MSG_DONTWAIT
    if (lf & 0x40) mf |= 0x80;
    // MSG_EOR
    if (lf & 0x80) mf |= 0x8;
    // MSG_WAITALL
    if (lf & 0x100) mf |= 0x40;
    return mf;
}
// macOS MSG_* -> Linux MSG_* (inverse of msgflags_l2m; used for recvmsg msg_flags writeback). The
// returned-flags set differs: notably MSG_CTRUNC is macOS 0x20 / Linux 0x8, MSG_TRUNC macOS 0x10 /
// Linux 0x20, MSG_EOR macOS 0x8 / Linux 0x80. OOB/DONTROUTE map straight through.
static int msgflags_m2l(int mf) {
    // MSG_OOB(0x1)/MSG_DONTROUTE(0x4) identical; MSG_PEEK isn't a returned flag but is harmless
    int lf = mf & (0x1 | 0x2 | 0x4);
    // MSG_TRUNC: macOS 0x10 -> Linux 0x20
    if (mf & 0x10) lf |= 0x20;
    // MSG_CTRUNC: macOS 0x20 -> Linux 0x8
    if (mf & 0x20) lf |= 0x8;
    // MSG_EOR: macOS 0x8 -> Linux 0x80
    if (mf & 0x8) lf |= 0x80;
    return lf;
}
// ---- SCM ancillary data: Linux<->macOS cmsg framing translation (SOL_SOCKET/SCM_RIGHTS fd passing).
// dd uses host fds directly as guest fds, so the fd integers in an SCM_RIGHTS payload need no remap --
// only the cmsg framing differs: Linux hdr=16B (8B len @0, int level @8, int type @12), 8-byte align,
// SOL_SOCKET=1; macOS hdr=12B (4B len @0, int level @4, int type @8), 4-byte align, SOL_SOCKET=0xffff.
#define LX_CMSG_ALIGN(n) (((n) + 7u) & ~(size_t)7u) // Linux: 8-byte align
#define LX_CMSGHDR 16u                               // Linux cmsg header: 8(len)+4(level)+4(type)
#define LX_SOL_SOCKET 1
static int cmsg_level_l2m(int lv) { return lv == LX_SOL_SOCKET ? SOL_SOCKET : lv; }
static int cmsg_level_m2l(int lv) { return lv == SOL_SOCKET ? LX_SOL_SOCKET : lv; }
// guest(Linux) control buf -> host(macOS) control buf. Returns host bytes written (<=cap), or 0/none.
static ssize_t cmsg_l2m(const uint8_t *g, size_t glen, uint8_t *h, size_t cap) {
    size_t go = 0, ho = 0;
    while (go + LX_CMSGHDR <= glen) {
        uint64_t clen = *(const uint64_t *)(g + go); // Linux cmsg_len (8B)
        int lvl = *(const int *)(g + go + 8);
        int typ = *(const int *)(g + go + 12);
        if (clen < LX_CMSGHDR || go + clen > glen) break; // malformed guest input
        size_t dlen = (size_t)clen - LX_CMSGHDR;          // payload bytes (e.g. N*4 fds)
        size_t need = CMSG_SPACE(dlen);
        if (ho + need > cap) break; // host scratch full
        struct cmsghdr ch;
        memset(&ch, 0, sizeof ch);
        ch.cmsg_len = CMSG_LEN(dlen); // macOS 12+dlen
        ch.cmsg_level = cmsg_level_l2m(lvl);
        ch.cmsg_type = typ; // SCM_RIGHTS==1 on both
        memcpy(h + ho, &ch, sizeof ch);
        memcpy(CMSG_DATA((struct cmsghdr *)(h + ho)), g + go + LX_CMSGHDR, dlen);
        ho += need;
        go += LX_CMSG_ALIGN(clen);
    }
    return (ssize_t)ho;
}
// host(macOS) control buf -> guest(Linux) control buf. Returns Linux bytes written (<=cap; stops at
// the guest-buffer boundary, leaving the kernel's MSG_CTRUNC in mh->msg_flags to be translated).
static ssize_t cmsg_m2l(const struct msghdr *mh, uint8_t *g, size_t cap) {
    size_t go = 0;
    for (struct cmsghdr *c = CMSG_FIRSTHDR((struct msghdr *)mh); c;
         c = CMSG_NXTHDR((struct msghdr *)mh, c)) {
        size_t dlen = (size_t)c->cmsg_len - CMSG_LEN(0); // payload bytes (macOS hdr=12)
        size_t need = LX_CMSG_ALIGN(LX_CMSGHDR + dlen);
        if (go + LX_CMSGHDR + dlen > cap) break; // guest buf full -> truncate (kernel set MSG_CTRUNC)
        *(uint64_t *)(g + go) = (uint64_t)(LX_CMSGHDR + dlen); // Linux cmsg_len
        *(int *)(g + go + 8) = cmsg_level_m2l(c->cmsg_level);
        *(int *)(g + go + 12) = c->cmsg_type;
        memcpy(g + go + LX_CMSGHDR, CMSG_DATA(c), dlen);
        go += need;
    }
    return (ssize_t)go;
}
// SOL_SOCKET option name: Linux -> macOS (they differ). -1 = ignore (unsupported here).
static int so_opt_l2m(int o) {
    switch (o) {
    // SO_DEBUG
    case 1: return 0x0001;
    // SO_REUSEADDR
    case 2: return 0x0004;
    // SO_ERROR  (async-connect completion!)
    case 4: return 0x1007;
    // SO_DONTROUTE
    case 5: return 0x0010;
    // SO_BROADCAST
    case 6: return 0x0020;
    // SO_SNDBUF
    case 7: return 0x1001;
    // SO_RCVBUF
    case 8: return 0x1002;
    // SO_KEEPALIVE
    case 9: return 0x0008;
    // SO_OOBINLINE
    case 10: return 0x0100;
    // SO_LINGER (struct linger: same layout)
    case 13: return 0x0080;
    // SO_REUSEPORT
    case 15: return 0x0200;
    // SO_ACCEPTCONN
    case 30: return 0x0002;
    // SO_TYPE
    case 3: return 0x1008;
    // SO_RCVTIMEO/SNDTIMEO (struct differs) + unknown -> ignore
    default: return -1;
    }
}
// IPPROTO_TCP optname Linux -> macOS. CRITICAL: these numbers diverge, and a raw pass-through maps
// Linux TCP_KEEPIDLE(4)/TCP_CORK(3) onto macOS TCP_NOPUSH(4)/TCP_NOOPT(3) — TCP_NOPUSH *corks* the
// socket so a server's reply is never delivered until close (breaks redis & every keepalive-setting
// server). Map the known ones; ignore (-1) unknown rather than pass through and accidentally cork.
static int tcp_opt_l2m(int o) {
    switch (o) {
    case 1: return 0x01;  // TCP_NODELAY  (same)
    case 2: return 0x02;  // TCP_MAXSEG   (same)
    case 3: return 0x04;  // Linux TCP_CORK     -> macOS TCP_NOPUSH (deliberate; guest asked to cork)
    case 4: return 0x10;  // Linux TCP_KEEPIDLE -> macOS TCP_KEEPALIVE (seconds)
    case 5: return 0x101; // Linux TCP_KEEPINTVL-> macOS TCP_KEEPINTVL
    case 6: return 0x102; // Linux TCP_KEEPCNT  -> macOS TCP_KEEPCNT
    default: return -1;   // unknown -> ignore (never pass a Linux number straight to macOS IPPROTO_TCP)
    }
}
// ---- NET namespace: per-container private loopback. A container's explicit 127.0.0.0/8 TCP sockets
// are routed to AF_UNIX sockets under a per-namespace host dir the guest can't name (it's path-jailed),
// so each container's localhost is isolated from the host + other containers. 0.0.0.0/external stay
// host-passthrough (so `-p` publishing still works). Off when g_netns[0]==0.
// host dir for this container's loopback unix sockets ("" = no isolation)
static char g_netns[200];
// fd -> the loopback port it's bound/connected to (0 = not a private-lo socket)
static uint16_t g_lo_port[1024];
// fd -> 1 if created SOCK_STREAM (only those get loopback isolation)
static uint8_t g_sock_stream[1024];
static int lo_on(void) { return g_netns[0] != 0; }
static int lo_is(const uint8_t *sa, socklen_t l) {
    return sa && l >= 8 && *(uint16_t *)sa == AF_INET && sa[4] == 127;
// 127.x.x.x
}
static void lo_path(uint16_t port, char *out, size_t n) { snprintf(out, n, "%s/p%u", g_netns, (unsigned)port); }
// Allocate an ephemeral loopback port for a bind(127.0.0.1:0). The kernel would assign a real port;
// under the unix-socket emulation we instead pick a port whose `p<port>` path is still free so that a
// later getsockname()/connect() round-trips to the same socket. (Without this, port 0 collapsed to a
// fixed sentinel and the client connected to a path that was never bound -> ENOENT.)
static uint16_t lo_alloc_ephemeral(void) {
    static uint16_t next; // seeded once per process; the path-existence check guards collisions
    if (next < 1024) next = (uint16_t)(20000 + (getpid() & 0x3fff));
    for (int tries = 0; tries < 45000; tries++) {
        uint16_t cand = next++;
        if (next < 1024) next = 1024; // wrapped through 0
        if (cand < 1024) continue;    // stay out of the privileged range
        char path[200];
        lo_path(cand, path, sizeof path);
        if (access(path, F_OK) != 0) return cand; // unbound -> usable
    }
    return 0;
}
// Swap the AF_INET socket at `fd` for a fresh AF_UNIX SOCK_STREAM one (keeping the fd number + flags).
static int lo_swap(int fd) {
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
    // keep non-blocking (async connect)
    if (fl >= 0 && (fl & O_NONBLOCK)) fcntl(fd, F_SETFL, O_NONBLOCK);
    if (df >= 0 && (df & FD_CLOEXEC)) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return 0;
}
// report AF_INET 127.0.0.1:port back to the guest
static void fill_inet_lo(uint8_t *sa, socklen_t *l, uint16_t port) {
    if (!sa) return;
    *(uint16_t *)(sa + 0) = AF_INET;
    *(uint16_t *)(sa + 2) = htons(port);
    *(uint32_t *)(sa + 4) = 0x0100007fu;
    // 127.0.0.1, zero-pad
    memset(sa + 8, 0, 8);
    if (l) *l = 16;
}

// ---- NET bridge (2A "virtual switch"): per-USER-NETWORK rendezvous for container<->container traffic.
// Generalizes the loopback redirect from "127/8 -> per-container dir" to "this user network's subnet ->
// SHARED per-network dir". A guest TCP socket whose peer is ANOTHER container's IP on the same user
// network (same /16 as our own DD_IP, and not 127/8) is routed to an AF_UNIX socket at
//   /tmp/.ddbr-<DD_NETBR>/<ip>:<port>
// The listening container (bind 0.0.0.0:<port> or its own IP) LISTENS on /tmp/.ddbr-<netid>/<ownip>:<port>;
// a peer connect(<ownip>:<port>) dials the same path. Because every container on the host is a JIT
// process under the same user, the two AF_UNIX endpoints rendezvous with no bridge / TUN / root. The dir
// is keyed by <netid> (mode 0700, the guest is path-jailed) so other networks never share sockets. The
// 127/8 loopback path (g_netns / lo_*) is untouched and stays per-container; only non-127 in-subnet
// AF_INET is bridged. Off when g_netbr[0]==0 || g_myip==0.
static char g_netbr[200];        // shared per-network rendezvous dir ("" = bridge off)
static uint32_t g_myip;          // this endpoint's IP, network byte order (0 = unset)
static uint16_t g_br_port[1024]; // fd -> virtual port of a bridge socket (0 = not a bridge socket)
static uint32_t g_br_ip[1024];   // fd -> virtual IP (network order) reported via getsockname/getpeername
static int g_br_init;
// Carry the per-fd socket-emulation metadata (SOCK_STREAM-ness, loopback/bridge port + ip) from `src`
// to `dst` when an fd is duplicated/moved (dup/dup3/fcntl F_DUPFD). Without this, a guest that creates a
// TCP socket then relocates it to another fd number (e.g. busybox's xmove_fd -> a fixed low fd) loses the
// `g_sock_stream` flag that gates the loopback + per-network bridge bind/connect redirection, so its
// AF_INET traffic silently falls through to host passthrough and never rendezvous with a peer container.
static void fd_carry_sock(int dst, int src) {
    if (dst < 0 || dst >= 1024 || src < 0 || src >= 1024) return;
    g_sock_stream[dst] = g_sock_stream[src];
    g_lo_port[dst] = g_lo_port[src];
    g_br_port[dst] = g_br_port[src];
    g_br_ip[dst] = g_br_ip[src];
}
// dotted-quad -> network-order u32 (bytes a.b.c.d), 0 on parse failure
static uint32_t br_parse_ip(const char *s) {
    unsigned a = 0, b = 0, cc = 0, d = 0;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &cc, &d) != 4) return 0;
    if (a > 255 || b > 255 || cc > 255 || d > 255) return 0;
    return (uint32_t)(a | (b << 8) | (cc << 16) | (d << 24));
}
// Lazy, self-contained env ingestion (mirrors the net_isolate getenv pattern in service.c case 203), so
// the bridge needs no edit to the per-target startup code: DD_NETBR=<netid>, DD_IP=<dotted-quad>.
static void br_init(void) {
    if (g_br_init) return;
    g_br_init = 1;
    const char *nbr = getenv("DD_NETBR");
    if (nbr && nbr[0]) {
        snprintf(g_netbr, sizeof g_netbr, "/tmp/.ddbr-%.40s", nbr);
        mkdir(g_netbr, 0700); // shared per-network dir; EEXIST is fine (peers share it)
    }
    const char *dip = getenv("DD_IP");
    if (dip && dip[0]) g_myip = br_parse_ip(dip);
}
static int br_on(void) {
    if (!g_br_init) br_init();
    return g_netbr[0] != 0 && g_myip != 0;
}
// dest IP is on our user network (same /16 as g_myip == first two octets a.b)
static int br_in_subnet(uint32_t ip_be) { return (ip_be & 0x0000ffffu) == (g_myip & 0x0000ffffu); }
// connect(dest): bridge if AF_INET, non-127, in our subnet
static int br_connect_is(const uint8_t *sa, socklen_t l) {
    if (!sa || l < 8 || *(uint16_t *)sa != AF_INET || sa[4] == 127) return 0;
    return br_in_subnet(*(uint32_t *)(sa + 4));
}
// bind(addr): bridge if AF_INET, non-127, and 0.0.0.0 (ANY) / our own IP / in-subnet
static int br_bind_is(const uint8_t *sa, socklen_t l) {
    if (!sa || l < 8 || *(uint16_t *)sa != AF_INET || sa[4] == 127) return 0;
    uint32_t ip = *(uint32_t *)(sa + 4);
    return ip == 0 || ip == g_myip || br_in_subnet(ip);
}
// rendezvous path for <ip_be>:<port> under the shared per-network dir
static void br_path(uint32_t ip_be, uint16_t port, char *out, size_t n) {
    const uint8_t *b = (const uint8_t *)&ip_be;
    snprintf(out, n, "%s/%u.%u.%u.%u:%u", g_netbr, b[0], b[1], b[2], b[3], (unsigned)port);
}
// bind(:0) on the bridge -> a free, round-trippable ephemeral port keyed by OUR ip (cf. lo_alloc_ephemeral)
static uint16_t br_alloc_ephemeral(void) {
    static uint16_t next;
    if (next < 1024) next = (uint16_t)(20000 + (getpid() & 0x3fff));
    for (int tries = 0; tries < 45000; tries++) {
        uint16_t cand = next++;
        if (next < 1024) next = 1024;
        if (cand < 1024) continue;
        char path[200];
        br_path(g_myip, cand, path, sizeof path);
        if (access(path, F_OK) != 0) return cand;
    }
    return 0;
}
// report the VIRTUAL AF_INET <ip_be>:<port> (not the AF_UNIX path) back to the guest
static void fill_inet_br(uint8_t *sa, socklen_t *l, uint32_t ip_be, uint16_t port) {
    if (!sa) return;
    *(uint16_t *)(sa + 0) = AF_INET;
    *(uint16_t *)(sa + 2) = htons(port);
    *(uint32_t *)(sa + 4) = ip_be;
    memset(sa + 8, 0, 8);
    if (l) *l = 16;
}

// ---- Published-port host forwarder (`docker run -p HOST:CONTAINER`) -----------------------------
// A guest that binds+listens on a published container port does so on the AF_UNIX virtual switch
// (br_path for a 0.0.0.0/eth0 bind, lo_path for a 127.0.0.1 bind) -- reachable by peer containers, but
// NOT by a host process dialing localhost:HOST_PORT, because nothing on the host listens on an AF_INET
// socket for HOST_PORT. This bridges that gap: when the guest listen()s on a published port we start a
// REAL host AF_INET listener on 0.0.0.0:HOST_PORT (matching docker's default publish address, which the
// daemon also reports via NetworkSettings.Ports), and for each accepted host connection we dial the
// guest's AF_UNIX switch socket and relay bytes both ways. The guest's own accept() returns the relayed
// connection exactly as if a peer container had connected, so container<->container, egress, the switch
// and --network none/host are completely untouched -- this purely ADDS the host->container path.
// (Uses g_portmap/pm_host from state.c, included before this file in the same TU.) TCP only for now;
// published UDP is a follow-up (the guest's UDP path isn't switch-redirected today).

// Is `cport` a published container port? (pm_host() returns cport on a miss, so it can't answer this.)
static int pm_published(uint16_t cport) {
    for (int i = 0; i < g_nportmap; i++)
        if (g_portmap[i].cport == cport) return 1;
    return 0;
}
// One relay connection: pump bytes between host TCP fd `a` and switch AF_UNIX fd `b` until either EOF.
struct fwd_relay {
    int a, b;
};
// Copy one ready direction src->dst. Returns 0 to keep going, -1 to tear the whole connection down.
// On src EOF we half-close dst (shutdown SHUT_WR) so the peer sees the FIN and can finish its reply,
// and mark that direction done; the connection ends only once BOTH directions have closed.
static int fwd_pump(int src, int dst, int *src_done, char *buf, size_t cap) {
    ssize_t n = read(src, buf, cap);
    if (n == 0) { shutdown(dst, SHUT_WR); *src_done = 1; return 0; }
    if (n < 0) { if (errno == EINTR || errno == EAGAIN) return 0; return -1; }
    for (ssize_t off = 0; off < n;) {
        ssize_t w = write(dst, buf + off, (size_t)(n - off));
        if (w <= 0) { if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue; return -1; }
        off += w;
    }
    return 0;
}
static void *fwd_relay_thread(void *p) {
    struct fwd_relay r = *(struct fwd_relay *)p;
    free(p);
    char buf[65536];
    int a_done = 0, b_done = 0; // host->guest / guest->host directions closed
    while (!a_done || !b_done) {
        struct pollfd pf[2] = {{r.a, a_done ? 0 : POLLIN, 0}, {r.b, b_done ? 0 : POLLIN, 0}};
        if (poll(pf, 2, -1) < 0) { if (errno == EINTR) continue; break; }
        if (!a_done && (pf[0].revents & (POLLIN | POLLHUP | POLLERR)))
            if (fwd_pump(r.a, r.b, &a_done, buf, sizeof buf) < 0) break;
        if (!b_done && (pf[1].revents & (POLLIN | POLLHUP | POLLERR)))
            if (fwd_pump(r.b, r.a, &b_done, buf, sizeof buf) < 0) break;
    }
    close(r.a);
    close(r.b);
    return NULL;
}
struct fwd_listen {
    uint16_t hport;
    char upath[200]; // full switch path; truncated into sun_path exactly as the guest's bind did
};
static void *fwd_listen_thread(void *p) {
    struct fwd_listen fl = *(struct fwd_listen *)p;
    free(p);
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    if (ls < 0) return NULL;
    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof sin);
    sin.sin_family = AF_INET;
    sin.sin_port = htons(fl.hport);
    sin.sin_addr.s_addr = htonl(INADDR_ANY); // 0.0.0.0:HOST_PORT (docker's default publish address)
    if (bind(ls, (struct sockaddr *)&sin, sizeof sin) < 0 || listen(ls, 128) < 0) {
        close(ls); // host port busy (e.g. another container already published it) -> no forwarding
        return NULL;
    }
    for (;;) {
        int hc = accept(ls, NULL, NULL);
        if (hc < 0) {
            if (errno == EINTR) continue;
            break;
        }
        // dial the guest's switch listen socket (same truncation the guest used when it bound it)
        int gc = socket(AF_UNIX, SOCK_STREAM, 0);
        if (gc < 0) { close(hc); continue; }
        struct sockaddr_un un;
        memset(&un, 0, sizeof un);
        un.sun_family = AF_UNIX;
        snprintf(un.sun_path, sizeof un.sun_path, "%s", fl.upath);
        if (connect(gc, (struct sockaddr *)&un, sizeof un) < 0) { close(gc); close(hc); continue; }
        struct fwd_relay *fr = malloc(sizeof *fr);
        if (!fr) { close(gc); close(hc); continue; }
        fr->a = hc;
        fr->b = gc;
        pthread_t t;
        if (pthread_create(&t, NULL, fwd_relay_thread, fr) != 0) { free(fr); close(gc); close(hc); continue; }
        pthread_detach(t);
    }
    close(ls);
    return NULL;
}
// Host ports we've already spun up a forwarder for (idempotent across re-listen()/SO_REUSEADDR restart).
static uint16_t g_fwd_started[32];
static int g_nfwd;
// Called from listen(): if `fd` is a published switch-backed listening socket, start its host forwarder.
static void fwd_maybe_start(int fd) {
    if (fd < 0 || fd >= 1024) return;
    uint16_t cport = 0;
    char upath[200];
    if (g_br_port[fd]) { cport = g_br_port[fd]; br_path(g_br_ip[fd], cport, upath, sizeof upath); }
    else if (g_lo_port[fd]) { cport = g_lo_port[fd]; lo_path(cport, upath, sizeof upath); }
    else return; // real host bind (no switch redirect) -> already natively reachable, nothing to do
    if (!pm_published(cport)) return; // not a published port
    uint16_t hport = pm_host(cport);
    for (int i = 0; i < g_nfwd; i++)
        if (g_fwd_started[i] == hport) return; // already forwarding this host port
    if (g_nfwd >= 32) return;
    struct fwd_listen *fl = malloc(sizeof *fl);
    if (!fl) return;
    fl->hport = hport;
    snprintf(fl->upath, sizeof fl->upath, "%s", upath);
    pthread_t t;
    if (pthread_create(&t, NULL, fwd_listen_thread, fl) != 0) { free(fl); return; }
    pthread_detach(t);
    g_fwd_started[g_nfwd++] = hport;
}

// ===== Linux <-> macOS sockaddr translation (AF_INET / AF_INET6) — gate: NOSOCKADDR=1 =====
// The non-isolated socket paths (real host TCP/UDP via bind/connect/accept/getsockname/getpeername/
// sendto/recvfrom/sendmsg) used to hand the guest's *Linux*-layout sockaddr straight to a macOS
// syscall (and vice-versa on output). The two layouts differ in the first two bytes:
//   Linux  sockaddr_in  = { u16 sin_family;  u16 sin_port; u32 sin_addr;  u8 pad[8] }   (AF_INET =2)
//   macOS  sockaddr_in  = { u8 sin_len; u8 sin_family; u16 sin_port; u32 sin_addr; ... } (AF_INET =2)
//   Linux  sockaddr_in6 = { u16 sin6_family; u16 port; u32 flow; u8 addr[16]; u32 scope}(AF_INET6=10)
//   macOS  sockaddr_in6 = { u8 len; u8 sin6_family; u16 port; u32 flow; u8 addr[16]; u32 scope}(=30)
// So a guest AF_INET(2) read as macOS becomes sin_len=2/sin_family=0 (AF_UNSPEC) -> the server never
// really binds; and host output read back as Linux yields sin_family = 0x0210 = 528 (garbage). AF_INET6
// additionally differs in the family *value* (10 vs 30). port/addr/flow/scope share offsets+encoding
// (network byte order) so only family/len need fixing. AF_UNIX and other families pass through.
#define LX_AF_INET 2
#define LX_AF_INET6 10
static int g_saxl = -1;
static int saxl_on(void) {
    if (g_saxl < 0) g_saxl = getenv("NOSOCKADDR") ? 0 : 1;
    return g_saxl;
}
// guest domain (Linux) -> host (macOS), for socket()/socketpair(). AF_INET(2)/AF_UNIX(1) match.
static int af_l2m(int d) { return (saxl_on() && d == LX_AF_INET6) ? AF_INET6 : d; }
// guest(Linux) sockaddr -> host(macOS) into `out`; returns host socklen, or -1 if not AF_INET/INET6
// (or gated off) — caller then uses the original guest pointer/len unchanged.
static socklen_t sa_l2m(const uint8_t *g, socklen_t glen, struct sockaddr_storage *out) {
    if (!saxl_on() || !g || glen < 2) return (socklen_t)-1;
    int fam = *(const uint16_t *)g;
    if (fam == LX_AF_INET && glen >= 8) {
        struct sockaddr_in *m = (struct sockaddr_in *)out;
        memset(m, 0, sizeof *m);
        m->sin_len = sizeof *m;
        m->sin_family = AF_INET;
        memcpy(&m->sin_port, g + 2, 2); // port (BE), same offset
        memcpy(&m->sin_addr, g + 4, 4); // addr (BE), same offset
        return (socklen_t)sizeof *m;    // 16
    }
    if (fam == LX_AF_INET6 && glen >= 24) {
        struct sockaddr_in6 *m = (struct sockaddr_in6 *)out;
        memset(m, 0, sizeof *m);
        m->sin6_len = sizeof *m;
        m->sin6_family = AF_INET6;
        memcpy(&m->sin6_port, g + 2, 2);
        memcpy(&m->sin6_flowinfo, g + 4, 4);
        memcpy(&m->sin6_addr, g + 8, 16);
        if (glen >= 28) memcpy(&m->sin6_scope_id, g + 24, 4);
        return (socklen_t)sizeof *m; // 28
    }
    return (socklen_t)-1;
}
// host(macOS) sockaddr -> guest(Linux) layout written to `g` (capacity gcap, may be 0/NULL). Returns
// the FULL Linux length of the address (16/28) even if it exceeds gcap (Linux truncates the copy but
// reports the full length via *addrlen), or -1 if not AF_INET/INET6 (caller copies raw host bytes).
static int sa_m2l(const struct sockaddr *m, uint8_t *g, socklen_t gcap) {
    if (!saxl_on() || !m) return -1;
    if (m->sa_family == AF_INET) {
        const struct sockaddr_in *s = (const struct sockaddr_in *)m;
        uint8_t t[16];
        memset(t, 0, sizeof t);
        *(uint16_t *)t = LX_AF_INET;
        memcpy(t + 2, &s->sin_port, 2);
        memcpy(t + 4, &s->sin_addr, 4);
        if (g && gcap) memcpy(g, t, gcap < 16 ? gcap : 16);
        return 16;
    }
    if (m->sa_family == AF_INET6) {
        const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)m;
        uint8_t t[28];
        memset(t, 0, sizeof t);
        *(uint16_t *)t = LX_AF_INET6;
        memcpy(t + 2, &s->sin6_port, 2);
        memcpy(t + 4, &s->sin6_flowinfo, 4);
        memcpy(t + 8, &s->sin6_addr, 16);
        memcpy(t + 24, &s->sin6_scope_id, 4);
        if (g && gcap) memcpy(g, t, gcap < 28 ? gcap : 28);
        return 28;
    }
    return -1;
}

// ---- abstract-namespace AF_UNIX (sun_path[0]=='\0'): macOS has no abstract namespace, so map the
// abstract name to a real filesystem socket under a per-namespace dir keyed by DD_NETNS (same key as
// ipc_ns_key), so two guests in one container rendezvous and different containers stay isolated. The
// guest socket is already a real host AF_UNIX socket (case 198), so only the ADDRESS is rewritten.
static char g_absdir[200];
static int g_abs_init;
static void abs_init(void) {
    if (g_abs_init) return;
    g_abs_init = 1;
    const char *ns = getenv("DD_NETNS"); // same key used by ipc_ns_key (service.c)
    snprintf(g_absdir, sizeof g_absdir, "/tmp/.ddabs-%.40s", (ns && ns[0]) ? ns : "default");
    mkdir(g_absdir, 0700); // EEXIST fine; peers share it (0700, guest is path-jailed)
}
// Is this guest sockaddr an abstract AF_UNIX addr? family u16==AF_UNIX, sun_path[0]==NUL, name>=1B.
static int abs_is(const uint8_t *sa, socklen_t l) {
    return sa && l > 3 && *(const uint16_t *)sa == AF_UNIX && sa[2] == 0; // sun_path[0] @ offset 2
}
// Map abstract name (bytes sa+3 .. for namelen=l-3) to a filesystem path. Hex when it fits macOS
// sun_path[104], else FNV-1a hash (long D-Bus/X11/systemd names overflow); the name may hold NULs,
// '/', non-printables, so hex/hash makes a safe single path component (no traversal).
static void abs_path(const uint8_t *sa, socklen_t l, char *out, size_t n) {
    abs_init();
    const uint8_t *nm = sa + 3;
    size_t nl = (size_t)l - 3;
    size_t dl = strlen(g_absdir);
    if (dl + 1 + nl * 2 + 1 <= n && dl + 1 + nl * 2 < 104) { // full hex (unambiguous, fits sun_path)
        char hx[210];
        static const char *H = "0123456789abcdef";
        for (size_t i = 0; i < nl; i++) {
            hx[2 * i] = H[nm[i] >> 4];
            hx[2 * i + 1] = H[nm[i] & 15];
        }
        hx[2 * nl] = 0;
        snprintf(out, n, "%s/%s", g_absdir, hx);
    } else { // hash fallback (FNV-1a) keeps the path bounded
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < nl; i++) {
            h ^= nm[i];
            h *= 1099511628211ull;
        }
        snprintf(out, n, "%s/h%016llx", g_absdir, (unsigned long long)h);
    }
}

struct loaded {
    uint64_t entry, phdr, base;
    int phent, phnum;
};
