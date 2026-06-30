// Extracted from service(): Network -- sockets/bind/connect/accept/send/recv + socketopt; port-map (-p) and
// the private NET-ns loopback (af_l2m / cmsg / msg-flag translation live in container/netns.c). Returns 1 if
// nr was handled, 0 otherwise. Included by service.c after service/io.c, before service() -- same TU scope.

static int svc_net(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                   uint64_t a5) {
    switch (nr) {
    case 198: {
        int ty = (int)a1;
        // socket (translate Linux domain -> macOS: AF_INET6 10->30, others unchanged)
        int r = socket(af_l2m((int)a0), ty & 0xf, (int)a2);
        if (r >= 0) {
            // SIGPIPE suppression: Linux delivers EPIPE (not a fatal signal) to a guest that has
            // SIG_IGN'd SIGPIPE or passes MSG_NOSIGNAL; macOS has no per-call MSG_NOSIGNAL, so make the
            // suppression sticky on the fd. With SO_NOSIGPIPE set at creation, ANY write to a broken
            // socket -- write(2), writev(2), send(2) without MSG_NOSIGNAL -- returns -1/EPIPE instead
            // of raising SIGPIPE. Benign on healthy sockets; only sockets get it, so real pipes/FIFOs
            // keep Linux's default SIGPIPE-on-write semantics.
            {
                int on = 1;
                setsockopt(r, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
            }
            if (ty & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
            if (ty & 0x800) fcntl(r, F_SETFL, O_NONBLOCK);
            if (r < 1024) {
                g_sock_stream[r] = ((ty & 0xf) == SOCK_STREAM && (int)a0 == AF_INET);
                g_sock_dgram[r] = ((ty & 0xf) == SOCK_DGRAM && (int)a0 == AF_INET);
                g_lo_port[r] = 0;
                g_br_port[r] = 0;
                g_br_ip[r] = 0;
            }
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 199: {
        int sv[2];
        // socketpair (translate Linux domain -> macOS). macOS AF_UNIX has no SOCK_SEQPACKET socketpair;
        // emulate it with SOCK_DGRAM, which over a local AF_UNIX pair is reliable, ordered, and preserves
        // message boundaries -- exactly the SEQPACKET guarantees the guest relies on.
        int lty = (int)a1 & 0xf;
        int hty = (lty == SOCK_SEQPACKET) ? SOCK_DGRAM : lty;
        int r = socketpair(af_l2m((int)a0), hty, (int)a2, sv);
        if (r == 0) {
            // SO_NOSIGPIPE on both ends so a write/send to a peer-closed pair returns EPIPE, never a
            // fatal SIGPIPE (matches Linux EPIPE-to-guest behaviour). See case 198 for the rationale.
            int on = 1;
            setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
            setsockopt(sv[1], SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
            ((int *)a3)[0] = sv[0];
            ((int *)a3)[1] = sv[1];
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    // bind -- port-map: bind the published host port
    case 200: {
        // GUEST Linux sockaddr_in: family@0(u16 LE), port@2(BE)
        uint8_t *sa = (uint8_t *)a1;
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            // private loopback
            lo_is(sa, (socklen_t)a2)) {
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            if (p == 0) p = lo_alloc_ephemeral(); // bind(:0) -> a real, round-trippable port
            char up[200];
            lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            unlink(up);
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0) g_lo_port[(int)a0] = p ? p : 1;
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // NET bridge: bind(0.0.0.0 / own-ip / in-subnet :port) -> LISTEN on /tmp/.ddbr-<netid>/<ownip>:<port>
        if (br_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] && br_bind_is(sa, (socklen_t)a2)) {
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            if (p == 0) p = br_alloc_ephemeral(); // bind(:0) -> a real, round-trippable port
            char up[200];
            br_path(g_myip, p, up, sizeof up); // we always listen on OUR endpoint IP
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            unlink(up);
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0) {
                g_br_port[(int)a0] = p ? p : 1;
                g_br_ip[(int)a0] = g_myip;
            }
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // abstract AF_UNIX (sun_path[0]==0): macOS has no abstract ns -> bind a real fs socket keyed by
        // DD_NETNS. Must run BEFORE any general AF_UNIX passthrough below.
        if (abs_is(sa, (socklen_t)a2)) {
            char up[200];
            abs_path(sa, (socklen_t)a2, up, sizeof up);
            unlink(up); // replace stale (cf. lo_/br_ above)
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = bind((int)a0, (struct sockaddr *)&un, sizeof un);
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // Published UDP (`-p H:C/udp`): swap an AF_INET datagram socket bound to a published port onto
        // the AF_UNIX switch + start its host->guest datagram forwarder. No-op (returns 0) for
        // non-published UDP, non-switch nets, or non-datagram sockets -> they fall through unchanged.
        {
            int64_t uret;
            if (udp_bind_maybe((int)a0, sa, (socklen_t)a2, &uret)) {
                G_RET(c) = (uint64_t)uret;
                break;
            }
        }
        if (g_nportmap && sa && a2 >= 8 && *(uint16_t *)(sa + 0) == AF_INET) {
            uint16_t cp = ntohs(*(uint16_t *)(sa + 2)), hp = pm_host(cp);
            // remember for getsockname
            if ((int)a0 >= 0 && (int)a0 < 1024) g_fd_cport[(int)a0] = cp;
            if (hp != cp) {
                uint8_t buf[128];
                socklen_t L = a2 < 128 ? (socklen_t)a2 : 128;
                memcpy(buf, sa, L);
                // publish on :H instead of :C (port @2)
                *(uint16_t *)(buf + 2) = htons(hp);
                // Linux->macOS sockaddr translation (sin_len/family) before the real host bind.
                struct sockaddr_storage ss;
                socklen_t hl = sa_l2m(buf, L, &ss);
                int br = (hl != (socklen_t)-1) ? bind((int)a0, (struct sockaddr *)&ss, hl)
                                               : bind((int)a0, (struct sockaddr *)buf, L);
                G_RET(c) = br < 0 ? (uint64_t)(-errno) : 0;
                break;
            }
        }
        // Real host bind: translate Linux AF_INET/INET6 sockaddr -> macOS (sin_len/family); AF_UNIX
        // and others pass through unchanged. (Was: raw bind of the Linux struct -> AF_UNSPEC bind.)
        {
            struct sockaddr_storage ss;
            socklen_t hl = sa_l2m(sa, (socklen_t)a2, &ss);
            int br = (hl != (socklen_t)-1) ? bind((int)a0, (struct sockaddr *)&ss, hl)
                                           : bind((int)a0, (void *)a1, (socklen_t)a2);
            G_RET(c) = br < 0 ? (uint64_t)(-errno) : 0;
        }
        break;
    }
    case 201: {
        int lr = listen((int)a0, (int)a1);
        // Published-port (`-p H:C`) host bridge: if this is a switch-backed listen on a published
        // container port, spin up a real host AF_INET listener on :H that relays into the guest.
        if (lr == 0) fwd_maybe_start((int)a0);
        G_RET(c) = lr < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 202:
    case 242: {
        int lfd = (int)a0;
        // accept / accept4
        int pl = (lfd >= 0 && lfd < 1024) ? g_lo_port[lfd] : 0;
        int pbr = (lfd >= 0 && lfd < 1024) ? g_br_port[lfd] : 0;
        uint32_t pbrip = (lfd >= 0 && lfd < 1024) ? g_br_ip[lfd] : 0;
        // Real host accept writes a macOS sockaddr; receive into a host scratch then translate the
        // peer addr back to Linux layout for the guest. (private-lo / bridge: don't expose unix peer.)
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int want_peer = (!pl && !pbr && a1);
        int r;
        do {
            r = (pl || pbr)
                    ? accept(lfd, NULL, NULL)
                    : accept(lfd, want_peer ? (struct sockaddr *)&hss : NULL, want_peer ? &hsl : (socklen_t *)a2);
        } while (r < 0 && SVC_EINTR_RESTART(c));
        if (r >= 0) {
            // Accepted connections are sockets too: make SIGPIPE suppression sticky on the new fd so a
            // write/send to a peer that closes returns EPIPE instead of killing the guest (see case 198).
            {
                int on = 1;
                setsockopt(r, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
            }
            if (nr == 242) {
                if ((int)a3 & 0x800) fcntl(r, F_SETFL, fcntl(r, F_GETFL) | O_NONBLOCK);
                if ((int)a3 & 0x80000) fcntl(r, F_SETFD, FD_CLOEXEC);
            }
            if (want_peer) {
                socklen_t gcap = a2 ? *(socklen_t *)a2 : 0;
                int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a1, gcap);
                if (ll < 0) { // non-inet peer (e.g. AF_UNIX): copy raw host bytes
                    socklen_t n = hsl < gcap ? hsl : gcap;
                    if (gcap) memcpy((void *)a1, &hss, n);
                    if (a2) *(socklen_t *)a2 = hsl;
                } else if (a2)
                    *(socklen_t *)a2 = (socklen_t)ll;
            }
            if (pl) {
                if (r < 1024) {
                    g_lo_port[r] = pl;
                    g_sock_stream[r] = 1;
                }
                fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, pl);
            } else if (pbr) {
                if (r < 1024) {
                    g_br_port[r] = pbr;
                    g_br_ip[r] = pbrip;
                    g_sock_stream[r] = 1;
                }
                // peer reported as our virtual listen addr (cf. lo_* simplification)
                fill_inet_br((uint8_t *)a1, (socklen_t *)a2, pbrip, pbr);
            }
            // peer = 127.0.0.1:lport
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // connect
    case 203: {
        // --network none: no external egress (DD_NET_ISOLATE). Loopback is redirected by the lo_* path
        // below; any non-127/8 AF_INET destination is refused, matching docker's null network.
        static int net_isolate = -1;
        if (net_isolate < 0) net_isolate = getenv("DD_NET_ISOLATE") != NULL;
        uint8_t *sa = (uint8_t *)a1;
        if (net_isolate && sa && (socklen_t)a2 >= 8 && *(uint16_t *)(sa + 0) == AF_INET &&
            (ntohl(*(uint32_t *)(sa + 4)) >> 24) != 127) {
            G_RET(c) = (uint64_t)(-ENETUNREACH);
            break;
        }
        if (lo_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] &&
            // private loopback
            lo_is(sa, (socklen_t)a2)) {
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            char up[200];
            lo_path(p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0 || errno == EINPROGRESS) g_lo_port[(int)a0] = p ? p : 1;
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // NET bridge: connect(peer-ip:port in our subnet) -> dial /tmp/.ddbr-<netid>/<peerip>:<port>
        if (br_on() && (int)a0 >= 0 && (int)a0 < 1024 && g_sock_stream[(int)a0] && br_connect_is(sa, (socklen_t)a2)) {
            uint32_t dip = *(uint32_t *)(sa + 4);
            uint16_t p = ntohs(*(uint16_t *)(sa + 2));
            char up[200];
            br_path(dip, p, up, sizeof up);
            if (lo_swap((int)a0) < 0) {
                G_RET(c) = (uint64_t)(-errno);
                break;
            }
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            if (r == 0 || errno == EINPROGRESS) {
                g_br_port[(int)a0] = p ? p : 1;
                g_br_ip[(int)a0] = dip; // peer ip for getpeername
            }
            G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
            break;
        }
        // abstract AF_UNIX (sun_path[0]==0): dial the same DD_NETNS-keyed fs socket bind used. Must run
        // BEFORE the general AF_UNIX passthrough below.
        if (abs_is(sa, (socklen_t)a2)) {
            char up[200];
            abs_path(sa, (socklen_t)a2, up, sizeof up);
            struct sockaddr_un un;
            memset(&un, 0, sizeof un);
            un.sun_family = AF_UNIX;
            snprintf(un.sun_path, sizeof un.sun_path, "%s", up);
            int r = connect((int)a0, (struct sockaddr *)&un, sizeof un);
            G_RET(c) = (r < 0 && errno != EINPROGRESS) ? (uint64_t)(-errno) : 0;
            break;
        }
        // Real host connect: translate Linux AF_INET/INET6 sockaddr -> macOS; others pass through.
        {
            struct sockaddr_storage ss;
            socklen_t hl = sa_l2m(sa, (socklen_t)a2, &ss);
            int cr;
            do {
                cr = (hl != (socklen_t)-1) ? connect((int)a0, (struct sockaddr *)&ss, hl)
                                           : connect((int)a0, (void *)a1, (socklen_t)a2);
            } while (cr < 0 && SVC_EINTR_RESTART(c));
            G_RET(c) = cr < 0 ? (uint64_t)(-errno) : 0;
        }
        break;
    }
    case 204: {
        // getsockname
        int fd = (int)a0;
        if (fd >= 0 && fd < 1024 && g_lo_port[fd]) {
            fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]);
            G_RET(c) = 0;
            break;
        }
        if (fd >= 0 && fd < 1024 && g_br_port[fd]) {
            fill_inet_br((uint8_t *)a1, (socklen_t *)a2, g_br_ip[fd], g_br_port[fd]);
            G_RET(c) = 0;
            break;
        }
        // Real host getsockname returns a macOS sockaddr; receive into host scratch, translate back to
        // Linux layout for the guest (fixes sin_family/sin_len), preserving the portmap port rewrite.
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int r = getsockname(fd, (struct sockaddr *)&hss, &hsl);
        if (r == 0 && a1) {
            socklen_t gcap = a2 ? *(socklen_t *)a2 : 0;
            int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a1, gcap);
            if (ll < 0) {
                socklen_t n = hsl < gcap ? hsl : gcap;
                if (gcap) memcpy((void *)a1, &hss, n);
                if (a2) *(socklen_t *)a2 = hsl;
            } else {
                if (a2) *(socklen_t *)a2 = (socklen_t)ll;
                if (g_nportmap && fd >= 0 && fd < 1024 && g_fd_cport[fd] && gcap >= 4)
                    // app sees the port it asked for (port @2)
                    *(uint16_t *)((uint8_t *)a1 + 2) = htons(g_fd_cport[fd]);
            }
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 205: {
        // getpeername
        int fd = (int)a0;
        if (fd >= 0 && fd < 1024 && g_lo_port[fd]) {
            fill_inet_lo((uint8_t *)a1, (socklen_t *)a2, g_lo_port[fd]);
            G_RET(c) = 0;
            break;
        }
        if (fd >= 0 && fd < 1024 && g_br_port[fd]) {
            fill_inet_br((uint8_t *)a1, (socklen_t *)a2, g_br_ip[fd], g_br_port[fd]);
            G_RET(c) = 0;
            break;
        }
        // Real host getpeername: translate macOS sockaddr back to Linux layout for the guest.
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int r = getpeername(fd, (struct sockaddr *)&hss, &hsl);
        if (r == 0 && a1) {
            socklen_t gcap = a2 ? *(socklen_t *)a2 : 0;
            int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a1, gcap);
            if (ll < 0) {
                socklen_t n = hsl < gcap ? hsl : gcap;
                if (gcap) memcpy((void *)a1, &hss, n);
                if (a2) *(socklen_t *)a2 = hsl;
            } else if (a2)
                *(socklen_t *)a2 = (socklen_t)ll;
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 206: {
        // MSG_NOSIGNAL(0x4000) has no per-call equivalent on macOS; emulate it with the SO_NOSIGPIPE
        // socket option so the send returns EPIPE instead of raising a fatal SIGPIPE.
        if ((int)a3 & 0x4000) {
            int on = 1;
            setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        }
        // dest addr (UDP): translate Linux AF_INET/INET6 sockaddr -> macOS; NULL/non-inet pass through.
        struct sockaddr_storage dss;
        socklen_t dhl = a4 ? sa_l2m((uint8_t *)a4, (socklen_t)a5, &dss) : (socklen_t)-1;
        const void *dst = (dhl != (socklen_t)-1) ? (void *)&dss : (void *)a4;
        socklen_t dl = (dhl != (socklen_t)-1) ? dhl : (socklen_t)a5;
        ssize_t r;
        do { r = sendto((int)a0, (void *)a1, (size_t)a2, msgflags_l2m((int)a3), dst, dl); } while (r < 0 && SVC_EINTR_RESTART(c));
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 207: {
        // src addr: receive into host scratch (macOS layout) then translate back to Linux for the guest.
        struct sockaddr_storage hss;
        socklen_t hsl = sizeof hss;
        int want = a4 != 0;
        ssize_t r;
        do {
            hsl = sizeof hss;
            r = recvfrom((int)a0, (void *)a1, (size_t)a2, msgflags_l2m((int)a3),
                         want ? (struct sockaddr *)&hss : NULL, want ? &hsl : NULL);
        } while (r < 0 && SVC_EINTR_RESTART(c));
        if (r >= 0 && want) {
            socklen_t gcap = a5 ? *(socklen_t *)a5 : 0;
            int ll = sa_m2l((struct sockaddr *)&hss, (uint8_t *)a4, gcap);
            if (ll < 0) {
                socklen_t n = hsl < gcap ? hsl : gcap;
                if (gcap) memcpy((void *)a4, &hss, n);
                if (a5) *(socklen_t *)a5 = hsl;
            } else if (a5)
                *(socklen_t *)a5 = (socklen_t)ll;
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // setsockopt(fd, level, optname, val, len)
    case 208: {
        int lvl = (int)a1, opt = (int)a2;
        if (lvl == 1) {
            lvl = SOL_SOCKET;
            opt = so_opt_l2m((int)a2);
            if (opt < 0) {
                G_RET(c) = 0;
                break;
            }
            // translate SOL_SOCKET; ignore unknown
        } else if (lvl == 6) { // IPPROTO_TCP: optnames diverge — translate, ignore unknown (never cork by accident)
            opt = tcp_opt_l2m((int)a2);
            if (opt < 0) {
                G_RET(c) = 0;
                break;
            }
        }
        int r = setsockopt((int)a0, lvl, opt, (void *)a3,
                           // other levels (IP/IPv6) pass through
                           (socklen_t)a4);
        G_RET(c) = r < 0 ? 0 : 0;
        (void)r;
        // never fail the guest on an unsupported option
        break;
    }
    // getsockopt(fd, level, optname, val, len)
    case 209: {
        int lvl = (int)a1, opt = (int)a2;
        // SO_PEERCRED (Linux SOL_SOCKET/17): macOS has no SO_PEERCRED. Report the peer's credentials as the
        // container identity (so cr.uid/gid match the guest's getuid/getgid) and the peer pid via macOS
        // LOCAL_PEERPID. struct ucred is { pid_t pid; uid_t uid; gid_t gid; } (3x u32 = 12 bytes).
        if (lvl == 1 && opt == 17) {
            if (a3 && a4 && *(socklen_t *)a4 >= 12) {
                pid_t ppid = 0;
                socklen_t pl = sizeof ppid;
                if (getsockopt((int)a0, SOL_LOCAL, LOCAL_PEERPID, &ppid, &pl) < 0 || ppid <= 0 ||
                    ppid == getpid())
                    ppid = container_pid(); // self/own-process peer (e.g. socketpair) -> this guest's pid
                uint32_t *u = (uint32_t *)a3;
                u[0] = (uint32_t)ppid;   // pid
                u[1] = (uint32_t)cuid(); // uid
                u[2] = (uint32_t)cgid(); // gid
                *(socklen_t *)a4 = 12;
            }
            G_RET(c) = 0;
            break;
        }
        if (lvl == 1) {
            lvl = SOL_SOCKET;
            opt = so_opt_l2m((int)a2);
            if (opt < 0) {
                if (a4 && *(socklen_t *)a4 >= 4 && a3) *(int *)a3 = 0;
                G_RET(c) = 0;
                break;
            }
            // unknown -> report 0
        } else if (lvl == 6) { // IPPROTO_TCP: translate optname, report 0 for unknown
            opt = tcp_opt_l2m((int)a2);
            if (opt < 0) {
                if (a4 && *(socklen_t *)a4 >= 4 && a3) *(int *)a3 = 0;
                G_RET(c) = 0;
                break;
            }
        }
        int r = getsockopt((int)a0, lvl, opt, (void *)a3, (socklen_t *)a4);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : 0;
        break;
    }
    case 210:
        G_RET(c) = shutdown((int)a0, (int)a1) < 0 ? (uint64_t)(-errno) : 0;
        // shutdown(fd, how) -- SHUT_RD/WR/RDWR match
        break;
    case 211:
    // sendmsg/recvmsg -- translate Linux msghdr -> macOS
    case 212: {
        uint8_t *g = (uint8_t *)a1;
        struct msghdr mh;
        // Linux: iovlen/controllen are 8-byte; macOS 4
        memset(&mh, 0, sizeof mh);
        mh.msg_name = (void *)*(uint64_t *)(g + 0);
        mh.msg_namelen = *(uint32_t *)(g + 8);
        mh.msg_iov = (void *)*(uint64_t *)(g + 16);
        mh.msg_iovlen = (int)*(uint64_t *)(g + 24);
        mh.msg_flags = *(uint32_t *)(g + 48);
        // msg_name sockaddr: Linux<->macOS translation through a host scratch (AF_INET/INET6 only).
        struct sockaddr_storage nss;
        uint8_t *gname = (uint8_t *)mh.msg_name;
        socklen_t gnamelen = mh.msg_namelen;
        if (nr == 211 && gname && gnamelen) { // sendmsg: guest -> host
            socklen_t hl = sa_l2m(gname, gnamelen, &nss);
            if (hl != (socklen_t)-1) {
                mh.msg_name = &nss;
                mh.msg_namelen = hl;
            }
        } else if (nr == 212 && gname && gnamelen) { // recvmsg: receive into host scratch
            mh.msg_name = &nss;
            mh.msg_namelen = sizeof nss;
        }
        // Ancillary data: the guest control buf is Linux-cmsg layout; macOS reads a different cmsghdr,
        // so route it through a host-layout scratch buffer (NULL-control left untouched, so edge/msgflags
        // with no control buffer stays on the old path).
        uint8_t *gc = (void *)*(uint64_t *)(g + 32);
        size_t gcl = *(uint64_t *)(g + 40);
        uint8_t hctl[4096]; // host-layout scratch (macOS hdr is smaller, so this is ample)
        if (nr == 211) {    // sendmsg: translate guest -> host before the call
            // Ancillary data may carry SCM_RIGHTS fds to another process; flush all RAM-backed scratch so a
            // passed fd (and any other) is a coherent host file on the receiving side.
            if (gc && gcl) memf_materialize_all();
            ssize_t hn = (gc && gcl) ? cmsg_l2m(gc, gcl, hctl, sizeof hctl) : 0;
            mh.msg_control = hn > 0 ? hctl : NULL;
            mh.msg_controllen = hn > 0 ? (socklen_t)hn : 0;
        } else { // recvmsg: receive into host scratch
            mh.msg_control = (gc && gcl) ? hctl : NULL;
            mh.msg_controllen = (gc && gcl) ? (socklen_t)sizeof hctl : 0;
        }
        // MSG_NOSIGNAL(0x4000) -> SO_NOSIGPIPE (macOS has no per-call flag); EPIPE instead of SIGPIPE.
        if (nr == 211 && ((int)a2 & 0x4000)) {
            int on = 1;
            setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        }
        ssize_t r;
        do {
            r = (nr == 211) ? sendmsg((int)a0, &mh, msgflags_l2m((int)a2)) : recvmsg((int)a0, &mh, msgflags_l2m((int)a2));
        } while (r < 0 && SVC_EINTR_RESTART(c));
        if (nr == 212 && r >= 0) {
            // recvmsg writes back name len + (host->guest) control + translated flags
            if (gname && gnamelen) { // translate received host sockaddr back to Linux layout
                int ll = sa_m2l((struct sockaddr *)&nss, gname, gnamelen);
                *(uint32_t *)(g + 8) = (ll >= 0) ? (uint32_t)ll : mh.msg_namelen;
                if (ll < 0 && mh.msg_namelen) // non-inet: copy raw host bytes back
                    memcpy(gname, &nss, mh.msg_namelen < gnamelen ? mh.msg_namelen : gnamelen);
            } else
                *(uint32_t *)(g + 8) = mh.msg_namelen;
            size_t ln = (gc && gcl) ? (size_t)cmsg_m2l(&mh, gc, gcl) : 0;
            *(uint64_t *)(g + 40) = ln;
            *(uint32_t *)(g + 48) = (uint32_t)msgflags_m2l((int)mh.msg_flags);
        }
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    case 269:
    // sendmmsg/recvmmsg(fd, mmsghdr[], vlen, flags, [timeout])
    case 243: {
        uint8_t *vec = (uint8_t *)a1;
        unsigned vlen = (unsigned)a2;
        // mmsghdr = msghdr(56) + msg_len(4) + pad
        int done = 0, err = 0;
        // MSG_NOSIGNAL(0x4000) -> SO_NOSIGPIPE once before the fan-out (macOS has no per-call flag).
        if (nr == 269 && ((int)a3 & 0x4000)) {
            int on = 1;
            setsockopt((int)a0, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
        }
        for (unsigned i = 0; i < vlen; i++) {
            uint8_t *g = vec + (size_t)i * 64;
            struct msghdr mh;
            memset(&mh, 0, sizeof mh);
            mh.msg_name = (void *)*(uint64_t *)(g + 0);
            mh.msg_namelen = *(uint32_t *)(g + 8);
            mh.msg_iov = (void *)*(uint64_t *)(g + 16);
            mh.msg_iovlen = (int)*(uint64_t *)(g + 24);
            mh.msg_flags = *(uint32_t *)(g + 48);
            // msg_name sockaddr: Linux<->macOS translation through a host scratch (AF_INET/INET6 only).
            struct sockaddr_storage nss;
            uint8_t *gname = (uint8_t *)mh.msg_name;
            socklen_t gnamelen = mh.msg_namelen;
            if (nr == 269 && gname && gnamelen) { // sendmmsg: guest -> host
                socklen_t hl = sa_l2m(gname, gnamelen, &nss);
                if (hl != (socklen_t)-1) {
                    mh.msg_name = &nss;
                    mh.msg_namelen = hl;
                }
            } else if (nr == 243 && gname && gnamelen) { // recvmmsg: receive into host scratch
                mh.msg_name = &nss;
                mh.msg_namelen = sizeof nss;
            }
            // Ancillary data: route the per-submessage control buf through a host-layout scratch buffer.
            uint8_t *gc = (void *)*(uint64_t *)(g + 32);
            size_t gcl = *(uint64_t *)(g + 40);
            uint8_t hctl[4096];
            if (nr == 269) { // sendmmsg: translate guest -> host
                ssize_t hn = (gc && gcl) ? cmsg_l2m(gc, gcl, hctl, sizeof hctl) : 0;
                mh.msg_control = hn > 0 ? hctl : NULL;
                mh.msg_controllen = hn > 0 ? (socklen_t)hn : 0;
            } else { // recvmmsg: receive into host scratch
                mh.msg_control = (gc && gcl) ? hctl : NULL;
                mh.msg_controllen = (gc && gcl) ? (socklen_t)sizeof hctl : 0;
            }
            int rf = (int)a3;
            // after the first, don't block (MSG_WAITFORONE-ish)
            if (nr == 243 && i > 0) rf |= 0x40;
            ssize_t r = (nr == 269) ? sendmsg((int)a0, &mh, msgflags_l2m(rf)) : recvmsg((int)a0, &mh, msgflags_l2m(rf));
            if (r < 0) {
                err = errno;
                break;
            }
            // msg_len
            *(uint32_t *)(g + 56) = (uint32_t)r;
            if (nr == 243) {
                if (gname && gnamelen) { // translate received host sockaddr back to Linux layout
                    int ll = sa_m2l((struct sockaddr *)&nss, gname, gnamelen);
                    *(uint32_t *)(g + 8) = (ll >= 0) ? (uint32_t)ll : mh.msg_namelen;
                    if (ll < 0 && mh.msg_namelen)
                        memcpy(gname, &nss, mh.msg_namelen < gnamelen ? mh.msg_namelen : gnamelen);
                } else
                    *(uint32_t *)(g + 8) = mh.msg_namelen;
                size_t ln = (gc && gcl) ? (size_t)cmsg_m2l(&mh, gc, gcl) : 0;
                *(uint64_t *)(g + 40) = ln;
                *(uint32_t *)(g + 48) = (uint32_t)msgflags_m2l((int)mh.msg_flags);
            }
            done++;
        }
        G_RET(c) = (done == 0 && err) ? (uint64_t)(-(int64_t)err) : (uint64_t)done;
        break;
    }
    default:
        return 0;
    }
    return svc_done(c); // boundary errno xlate (host macOS -> Linux); see helpers.c svc_done
}
