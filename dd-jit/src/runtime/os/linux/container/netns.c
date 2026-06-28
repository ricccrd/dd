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

struct loaded {
    uint64_t entry, phdr, base;
    int phent, phnum;
};
