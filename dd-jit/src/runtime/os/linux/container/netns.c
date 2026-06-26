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

struct loaded {
    uint64_t entry, phdr, base;
    int phent, phnum;
};
