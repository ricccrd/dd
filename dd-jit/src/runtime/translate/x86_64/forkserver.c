// frontend/x86_64/forkserver.c -- W3D: resident "ddjitd" fork-server for the jit86 x86 engine.
//
// WHY: per-launch wall is ~2.9 ms, of which ~2 ms is the irreducible-per-process posix_spawn +
// dyld + codesign-validation floor of ddjit-x86 ITSELF (opt8 measured this), paid on EVERY
// container launch. A resident server pays dyld + engine init (MAP_JIT arena, pthread key, signal
// handlers) + optional pre-translation ONCE, then fork()s a copy-on-write worker per launch. The
// worker inherits the warm translated arena + (optionally) the pre-loaded guest image COW, so a
// warm launch skips spawn + dyld + engine init + ELF load + translation entirely -- only the guest
// itself (and its container fd setup) is paid.
//
// PROTOCOL (AF_UNIX SOCK_STREAM):
//   client -> server:  one sendmsg carrying  [int32 argc][argc * (int32 len + bytes)]  in the iov,
//                      plus the client's {stdin,stdout,stderr} as 3 fds in an SCM_RIGHTS cmsg.
//   server -> client:  [int32 exit_code]  on the same connection (written by the forked worker).
// The worker dup2()s the passed fds onto 0/1/2 so guest stdout/stderr/exit-code reach the client
// byte-identically to a standalone run. COW gives isolation: a worker's translations / data writes
// never touch the parent or sibling workers.
//
// This file is #included by targets/linux_x86_64.c AFTER jit86_run / run_loaded / load_program.
//
// NOTE vs the W3D research diff: that diff carried the loaded-image span in a new `span` field on
// `struct loaded` (set in elf.c). To keep this change inside the x86 frontend (no edits to the
// shared netns.c/elf.c), the span is instead re-derived here from the loaded image's in-memory
// program headers (loaded_span), which reproduces elf.c's mmap span byte-for-byte.

// ---- W3D: full mapped span [base,base+span) of a loaded image, from its in-memory program headers.
// out->phdr = base + phoff and phnum/phent come from struct loaded; rd32/rd64 are elf.c's helpers
// (this file is #included after elf.c in the unity TU). Matches elf.c's span computation exactly so
// the pristine-image snapshot/restore covers the identical region load_elf() mmap'd.
static uint64_t loaded_span(const struct loaded *L) {
    const uint8_t *ph = (const uint8_t *)L->phdr;
    uint64_t minv = ~0ull, maxv = 0;
    for (int i = 0; i < L->phnum; i++) {
        const uint8_t *p = ph + (size_t)i * (size_t)L->phent;
        if (rd32(p) != 1) continue; // PT_LOAD
        uint64_t v = rd64(p + 16), msz = rd64(p + 40);
        if (v < minv) minv = v;
        if (v + msz > maxv) maxv = v + msz;
    }
    uint64_t basepage = minv & ~0xFFFull;
    return (maxv - basepage + 0xFFFF) & ~0xFFFFull;
}

// ---- warm preload state (parent-side; inherited COW by every worker) ----
static int g_warm_ready;                 // set once the parent has pre-loaded + pre-translated g_wprog
static struct loaded g_wmain, g_winterp; // parent-loaded guest image (same base in every COW worker)
static uint64_t g_wmain_span, g_winterp_span; // mapped span of each (for the pristine snapshot/restore)
static uint64_t g_wjump, g_wat_base;     // entry + AT_BASE for the warm re-run
static int g_whave_interp;
static void *g_wsnap_main, *g_wsnap_interp; // pristine copies of the writable image (data+bss)
static char g_wprog[1024];                  // the prewarmed program (warm path only fires on a match)
static char g_srv_rootfs[4200];

// ---- SCM_RIGHTS fd passing ----
static int send_fds_msg(int sock, const void *buf, size_t len, const int *fds, int nfd) {
    struct iovec iov = {.iov_base = (void *)buf, .iov_len = len};
    char cbuf[CMSG_SPACE(sizeof(int) * 8)];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (nfd > 0) {
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * nfd);
        struct cmsghdr *cm = CMSG_FIRSTHDR(&mh);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(sizeof(int) * nfd);
        memcpy(CMSG_DATA(cm), fds, sizeof(int) * nfd);
    }
    ssize_t r;
    do {
        r = sendmsg(sock, &mh, 0);
    } while (r < 0 && errno == EINTR);
    return r < 0 ? -1 : 0;
}

static int recv_fds_msg(int sock, void *buf, size_t len, int *fds, int *nfd) {
    struct iovec iov = {.iov_base = buf, .iov_len = len};
    char cbuf[CMSG_SPACE(sizeof(int) * 8)];
    struct msghdr mh;
    memset(&mh, 0, sizeof mh);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof cbuf;
    ssize_t r;
    do {
        r = recvmsg(sock, &mh, 0);
    } while (r < 0 && errno == EINTR);
    if (r < 0) return -1;
    *nfd = 0;
    for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
        if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_RIGHTS) {
            int n = (int)((cm->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            memcpy(fds, CMSG_DATA(cm), sizeof(int) * n);
            *nfd = n;
        }
    }
    return (int)r;
}

// Serialize argv -> [int32 argc][argc*(int32 len + bytes)]. Returns total byte length.
static size_t pack_argv(char *out, size_t cap, int argc, char *const argv[]) {
    size_t o = 0;
    int32_t ac = argc;
    memcpy(out + o, &ac, 4);
    o += 4;
    for (int i = 0; i < argc; i++) {
        int32_t l = (int32_t)strlen(argv[i]) + 1; // include NUL
        memcpy(out + o, &l, 4);
        o += 4;
        if (o + (size_t)l > cap) return 0;
        memcpy(out + o, argv[i], l);
        o += l;
    }
    return o;
}

static int unpack_argv(const char *in, size_t len, char **argv, int max) {
    size_t o = 0;
    if (len < 4) return -1;
    int32_t ac;
    memcpy(&ac, in + o, 4);
    o += 4;
    if (ac < 1 || ac >= max) return -1;
    for (int i = 0; i < ac; i++) {
        if (o + 4 > len) return -1;
        int32_t l;
        memcpy(&l, in + o, 4);
        o += 4;
        if (o + (size_t)l > len) return -1;
        argv[i] = (char *)in + o;
        o += l;
    }
    argv[ac] = NULL;
    return ac;
}

// ---- worker: run one launch, then send the exit code back and _exit ----
// Runs in the forked child. fds[0..2] are the client's stdin/stdout/stderr; conn is the control
// socket to report the exit code on.
static void ddjitd_worker(int conn, int *fds, int nfd, int argc, char **argv) {
    // dup the client's stdio onto 0/1/2 so the guest's writes reach the client.
    if (nfd >= 1 && fds[0] != 0) dup2(fds[0], 0);
    if (nfd >= 2 && fds[1] != 1) dup2(fds[1], 1);
    if (nfd >= 3 && fds[2] != 2) dup2(fds[2], 2);
    for (int i = 0; i < nfd; i++)
        if (fds[i] > 2) close(fds[i]);

    // W^X / APRR per-thread execute state is NOT reliably inherited across fork() on Apple Silicon
    // (see os/linux/service.c fork path) -- re-assert RX so the first run_block can fetch from the
    // (COW-inherited, warm) MAP_JIT arena instead of instruction-aborting.
    pthread_jit_write_protect_np(1);
    // The cpu struct (incl. the shadow return stack) is memset fresh inside run_loaded below, so no
    // explicit shadow reset is needed here.
    // Make the guest's exit_group UNWIND run_guest (return the code) instead of _exit()ing the worker
    // immediately -- otherwise the worker dies before it can report the exit code to the client.
    g_noexit = 1;

    size_t arena_before = (size_t)(g_cp - g_cache); // W3D diag: bytes appended == fresh translation
    int rc;
    int warm = g_warm_ready && argc >= 1 && strcmp(argv[0], g_wprog) == 0;
    if (warm) {
        // Restore the pristine writable image over the COW-inherited (prewarm-dirtied) copy, so the
        // guest starts from byte-identical initial memory -- then re-run from the same entry/base.
        // The translated arena is already warm (COW), so run_guest finds every startup block mapped.
        memcpy((void *)g_wmain.base, g_wsnap_main, g_wmain_span);
        if (g_whave_interp) memcpy((void *)g_winterp.base, g_wsnap_interp, g_winterp_span);
        g_loadbase = g_wmain.base;
        rc = run_loaded(argc, argv, &g_wmain, g_wjump, g_wat_base);
    } else {
        // Cold: no matching prewarm. Pay a full per-launch load + translate in the worker (still no
        // spawn/dyld/engine-init -- those were paid by the resident parent). Translations are
        // COW-private to this worker and discarded on exit.
        rc = jit86_run(g_srv_rootfs[0] ? g_srv_rootfs : NULL, argc, argv);
    }
    if (getenv("DDJITD_DIAG")) { // W3D: report fresh translation done by THIS worker (0 == fully warm)
        FILE *df = fopen("/tmp/ddjitd-worker.log", "a");
        if (df) {
            fprintf(df, "worker pid=%d %s arena_translated=%zu bytes rc=%d\n", (int)getpid(), warm ? "WARM" : "COLD",
                    (size_t)(g_cp - g_cache) - arena_before, rc);
            fclose(df);
        }
    }
    int32_t r32 = rc;
    (void)send_fds_msg(conn, &r32, 4, NULL, 0);
    _exit(rc);
}

// ---- server ----
static volatile sig_atomic_t g_srv_stop;
static void srv_sigint(int s) {
    (void)s;
    g_srv_stop = 1;
}

static int ddjitd_server_main(int argc, char **argv) {
    const char *sock = NULL, *rootfs = NULL, *prewarm = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) sock = argv[++i];
        else if (strcmp(argv[i], "--rootfs") == 0 && i + 1 < argc) rootfs = argv[++i];
        else if (strcmp(argv[i], "--prewarm") == 0 && i + 1 < argc) prewarm = argv[++i];
    }
    if (!sock) {
        fprintf(stderr, "usage: ddjit-x86 --server SOCK [--rootfs DIR] [--prewarm PROG]\n");
        return 2;
    }
    if (rootfs) snprintf(g_srv_rootfs, sizeof g_srv_rootfs, "%s", rootfs);

    // Pay the expensive, per-launch-amortizable work ONCE.
    container_init(rootfs);
    if (engine_global_init()) return 1;

    if (prewarm && prewarm[0]) {
        snprintf(g_wprog, sizeof g_wprog, "%s", prewarm);
        // Load the guest image (this fixes the base every COW worker will share).
        load_program(prewarm, &g_wmain, &g_winterp, &g_wjump, &g_wat_base, &g_whave_interp);
        g_wmain_span = loaded_span(&g_wmain);
        if (g_whave_interp) g_winterp_span = loaded_span(&g_winterp);
        // Snapshot the PRISTINE writable image BEFORE running anything (data+bss are about to be
        // dirtied by the prewarm run; workers restore from these snapshots).
        g_wsnap_main = malloc(g_wmain_span);
        memcpy(g_wsnap_main, (void *)g_wmain.base, g_wmain_span);
        if (g_whave_interp) {
            g_wsnap_interp = malloc(g_winterp_span);
            memcpy(g_wsnap_interp, (void *)g_winterp.base, g_winterp_span);
        }
        // Pre-translate: run the program to completion in the PARENT so its blocks land in the COW
        // arena. g_noexit makes the guest's exit_group unwind run_guest instead of killing us. To
        // cover not just the shared ld.so+startup but the per-APPLET code paths too, we run a small
        // UNION of common busybox applets -- COW lets every later warm worker inherit all of them.
        // The pristine image is restored between runs so each applet starts from clean memory.
        int devnull = open("/dev/null", O_WRONLY);
        int sv1 = dup(1), sv2 = dup(2);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        g_noexit = 1;
        char pa0[1024];
        snprintf(pa0, sizeof pa0, "%s", prewarm);
        // Each row is an argv vector starting with the prewarmed program. Extra rows widen coverage;
        // the default set covers the launch-test workloads (true/echo/pwd/ls/cat/uname/sh).
        const char *applets[] = {NULL, "true", "echo", "pwd", "ls", "cat", "uname", "sh"};
        int nap = (int)(sizeof applets / sizeof applets[0]);
        for (int ai = 0; ai < nap; ai++) {
            // restore pristine before each prewarm run so it starts from clean guest memory
            memcpy((void *)g_wmain.base, g_wsnap_main, g_wmain_span);
            if (g_whave_interp) memcpy((void *)g_winterp.base, g_wsnap_interp, g_winterp_span);
            char a1[256] = "x";
            char *pargv[3];
            int pac = 1;
            pargv[0] = pa0;
            if (applets[ai]) {
                pargv[pac++] = (char *)applets[ai];
                // give applets that need an operand a harmless one (echo x / ls / cat reads stdin=null)
                if (strcmp(applets[ai], "echo") == 0) pargv[pac++] = a1;
            }
            pargv[pac] = NULL;
            run_loaded(pac, pargv, &g_wmain, g_wjump, g_wat_base);
        }
        g_noexit = 0;
        if (devnull >= 0) {
            dup2(sv1, 1);
            dup2(sv2, 2);
            close(devnull);
        }
        close(sv1);
        close(sv2);
        g_warm_ready = 1;
        fprintf(stderr, "[ddjitd] prewarmed %s: %llu blocks translated, arena=%lld KB\n", prewarm,
                (unsigned long long)g_disp_n, (long long)((g_cp - g_cache) / 1024));
    }

    signal(SIGCHLD, SIG_IGN); // auto-reap workers (no zombies)
    signal(SIGINT, srv_sigint);
    signal(SIGTERM, srv_sigint);
    signal(SIGPIPE, SIG_IGN);

    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", sock);
    unlink(sock);
    if (bind(ls, (struct sockaddr *)&un, sizeof un) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(ls, 128) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "[ddjitd] listening on %s (warm=%d rootfs=%s)\n", sock, g_warm_ready,
            g_srv_rootfs[0] ? g_srv_rootfs : "(none)");

    while (!g_srv_stop) {
        int conn = accept(ls, NULL, NULL);
        if (conn < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        static char buf[65536];
        int fds[8];
        int nfd = 0;
        int n = recv_fds_msg(conn, buf, sizeof buf, fds, &nfd);
        if (n <= 0) {
            close(conn);
            for (int i = 0; i < nfd; i++) close(fds[i]);
            continue;
        }
        char *wargv[256];
        int wac = unpack_argv(buf, (size_t)n, wargv, 256);
        if (wac < 1) {
            close(conn);
            for (int i = 0; i < nfd; i++) close(fds[i]);
            continue;
        }
        pid_t pid = fork();
        if (pid == 0) {
            close(ls);
            ddjitd_worker(conn, fds, nfd, wac, wargv); // never returns
        }
        // parent: hand the connection + client fds to the worker, drop our copies.
        close(conn);
        for (int i = 0; i < nfd; i++) close(fds[i]);
    }
    close(ls);
    unlink(sock);
    return 0;
}

// ---- client ----
static int ddjitd_client_main(int argc, char **argv) {
    const char *sock = NULL;
    int ai = 1;
    while (ai < argc) {
        if (strcmp(argv[ai], "--client") == 0 && ai + 1 < argc) {
            sock = argv[ai + 1];
            ai += 2;
        } else if (strcmp(argv[ai], "--rootfs") == 0 && ai + 1 < argc) {
            ai += 2; // server already holds the rootfs; accept+ignore for CLI symmetry
        } else
            break;
    }
    if (!sock || ai >= argc) {
        fprintf(stderr, "usage: ddjit-x86 --client SOCK [--rootfs DIR] PROG [args...]\n");
        return 2;
    }
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un un;
    memset(&un, 0, sizeof un);
    un.sun_family = AF_UNIX;
    snprintf(un.sun_path, sizeof un.sun_path, "%s", sock);
    if (connect(s, (struct sockaddr *)&un, sizeof un) < 0) {
        perror("connect");
        return 1;
    }
    static char buf[65536];
    size_t len = pack_argv(buf, sizeof buf, argc - ai, argv + ai);
    if (!len) {
        fprintf(stderr, "argv too large\n");
        return 1;
    }
    int fds[3] = {0, 1, 2};
    if (send_fds_msg(s, buf, len, fds, 3) < 0) {
        perror("sendmsg");
        return 1;
    }
    int32_t rc = 0;
    ssize_t r;
    do {
        r = recv(s, &rc, 4, MSG_WAITALL);
    } while (r < 0 && errno == EINTR);
    close(s);
    if (r != 4) return 125; // server died / protocol error
    return (int)rc;
}
