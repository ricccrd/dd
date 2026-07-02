// dd/runtime/os/linux/container -- container config state (UTS/cgroup/USER-ns/port-map) + parsers.
#include "../../container_parse.h" // strict numeric parsing (the config trust boundary; see LAUNCH.md)

// ---- container namespace + cgroup state (SentryConfig: ddockerd -> jit) ----
// UTS ns: container hostname (uname/sethostname); "" = host default
static char g_hostname[65] = "";
// cgroup memory.max bytes (0 = unlimited); charged in mmap
static uint64_t g_mem_max = 0;
// cgroup pids.max (0 = unlimited); checked in clone
static int g_pids_max = 0;
// current anon charge (bytes)
static _Atomic uint64_t g_mem_charged = 0;
// live task count (init = 1)
static _Atomic int g_pids_cur = 1;
// PID ns: host pid of the container init -> guest sees it as PID 1
static int g_init_hostpid = 0;
static int container_pid(void) {
    int h = getpid();
    return (g_init_hostpid && h == g_init_hostpid) ? 1 : h;
}
static int g_uid = -1,
           // USER ns: container uid/gid (-1 = passthrough host id; container defaults to 0=root)
           g_gid = -1;
static int cuid(void) { return g_uid >= 0 ? g_uid : (int)getuid(); }
static int cgid(void) { return g_gid >= 0 ? g_gid : (int)getgid(); }
// ---- BUG #181: guest-set ownership persistence (chown(2)/fchownat on overlay-upper files) ----
// Rootless: a guest chown can't change the host file's REAL owner, and #156 reports host-owned files
// as the container uid/gid -- so a guest-set owner was silently lost (chown returned 0 but a re-stat
// still showed the #156 default). Persist the guest-set (uid,gid) as host xattrs on the overlay
// backing file; fill_linux_stat prefers them over the cuid/cgid default. A guest id of -1 means
// "don't change" (POSIX chown) -> leave that xattr untouched so the other id / the default survives.
// xattrs live on the real APFS upper file, so they persist across a re-stat AND across processes.
#include <sys/xattr.h>
#define DD_XATTR_UID "user.dd.uid"
#define DD_XATTR_GID "user.dd.gid"
static void chown_xattr_set_path(const char *hostpath, int uid, int gid, int nofollow) {
    int opt = nofollow ? XATTR_NOFOLLOW : 0;
    if (uid >= 0) {
        uint32_t v = (uint32_t)uid;
        setxattr(hostpath, DD_XATTR_UID, &v, sizeof v, 0, opt);
    }
    if (gid >= 0) {
        uint32_t v = (uint32_t)gid;
        setxattr(hostpath, DD_XATTR_GID, &v, sizeof v, 0, opt);
    }
}
static void chown_xattr_set_fd(int fd, int uid, int gid) {
    if (uid >= 0) {
        uint32_t v = (uint32_t)uid;
        fsetxattr(fd, DD_XATTR_UID, &v, sizeof v, 0, 0);
    }
    if (gid >= 0) {
        uint32_t v = (uint32_t)gid;
        fsetxattr(fd, DD_XATTR_GID, &v, sizeof v, 0, 0);
    }
}
// Read back the guest-set ids (fd preferred when fd>=0, else hostpath). Each out is the set id or -1
// (no xattr -> keep the #156 cuid/cgid default). Returns 1 if either id was guest-set.
static int chown_xattr_get(const char *hostpath, int fd, int *uid, int *gid) {
    *uid = -1;
    *gid = -1;
    uint32_t v;
    if (fd >= 0) {
        if (fgetxattr(fd, DD_XATTR_UID, &v, sizeof v, 0, 0) == (ssize_t)sizeof v) *uid = (int)v;
        if (fgetxattr(fd, DD_XATTR_GID, &v, sizeof v, 0, 0) == (ssize_t)sizeof v) *gid = (int)v;
    } else if (hostpath) {
        if (getxattr(hostpath, DD_XATTR_UID, &v, sizeof v, 0, 0) == (ssize_t)sizeof v) *uid = (int)v;
        if (getxattr(hostpath, DD_XATTR_GID, &v, sizeof v, 0, 0) == (ssize_t)sizeof v) *gid = (int)v;
    }
    return (*uid >= 0 || *gid >= 0);
}
// ---- runtime credential overlay (USER ns) -- defined here (BEFORE fs.c AND proc.c in the unity TU) --
// cuid()/cgid() give the container's CONFIGURED identity (default 0=root); a privileged guest may drop
// to an unprivileged id at runtime (apt forks /usr/lib/apt/methods/http, switching to `_apt`; gosu
// switches postgres to uid 70) and then VERIFIES the drop took -- and that it can NOT regain root. We
// track real/effective/saved uid+gid and honour the Linux permission model (a euid==0 task is
// privileged; otherwise a new id must already be one of its three) so both the drop AND the
// regain-must-fail check behave as on Linux. The base is cuid()/cgid() (fork inherits the copy, exec
// re-seeds from the container default). The set*id syscall HANDLERS live in proc.c and mutate these.
static int g_cred_init = 0;
static int g_ruid, g_euid, g_suid; // real / effective / saved-set uid
static int g_rgid, g_egid, g_sgid; // real / effective / saved-set gid
static void cred_init(void) {
    if (g_cred_init) return;
    g_ruid = g_euid = g_suid = cuid();
    g_rgid = g_egid = g_sgid = cgid();
    g_cred_init = 1;
}
static int cred_euid(void) {
    cred_init();
    return g_euid;
}
static int cred_egid(void) {
    cred_init();
    return g_egid;
}
// An unprivileged task (euid != 0) may only set an id it already holds (real/effective/saved). -1 means
// "leave unchanged". Returns 1 if id is permitted, 0 -> EPERM.
static int uid_permitted(int id) {
    return id == -1 || g_euid == 0 || id == g_ruid || id == g_euid || id == g_suid;
}
static int gid_permitted(int id) {
    return id == -1 || g_euid == 0 || id == g_rgid || id == g_egid || id == g_sgid;
}
// ---- BUG #255: new-file ownership stamp (runtime setuid/setgid drop) ----------------------------
// A guest that drops privilege at runtime (setuid/setresuid/setfsuid -> gosu's postgres) and then
// CREATES a file/dir must have the new inode owned by its CURRENT effective fsuid/fsgid, NOT the
// cuid/cgid container default that fill_linux_stat applies to host-owned files. #181 tracked only
// EXPLICIT chown(2); a plain create left no xattr, so a new file re-appeared as the container id (0),
// which broke initdb ("data directory has wrong ownership"). fsuid/fsgid follow the overlay's
// euid/egid unless setfsuid/setfsgid override them (g_fs*_ovr >= 0); any subsequent set*id resets the
// override (POSIX: fsuid tracks euid). We persist the intended owner as the SAME dd.uid/gid xattr the
// chown path uses, so a later stat reports it. The create sites in fs.c call the helpers below.
static int g_fsuid_ovr = -1, g_fsgid_ovr = -1; // -1 = follow euid/egid
static int newfile_uid(void) { return g_fsuid_ovr >= 0 ? g_fsuid_ovr : cred_euid(); }
static int newfile_gid(void) { return g_fsgid_ovr >= 0 ? g_fsgid_ovr : cred_egid(); }
// True only when a runtime cred drop makes the new-file owner differ from the cuid/cgid default -- the
// create paths gate their pre-existence probe + stamp on this so the common (no-drop) case is free.
static int newfile_stamp_wanted(void) { return newfile_uid() != cuid() || newfile_gid() != cgid(); }
// Stamp a freshly-created inode's owner, but only the id(s) that differ from the default (so a
// root-created file stays xattr-free). fd form for openat(O_CREAT); path form for mkdir/mknod.
static void newfile_stamp_fd(int fd) {
    int u = newfile_uid(), g = newfile_gid();
    chown_xattr_set_fd(fd, u != cuid() ? u : -1, g != cgid() ? g : -1);
}
static void newfile_stamp_path(const char *hostpath, int nofollow) {
    int u = newfile_uid(), g = newfile_gid();
    chown_xattr_set_path(hostpath, u != cuid() ? u : -1, g != cgid() ? g : -1, nofollow);
}
// ---- NET ns Phase 1: port-map (docker run -p H:C). bind(:C) actually binds the host port :H;
// getsockname reports :C back so the guest sees the port it asked for. {cport->hport} table.
static struct {
    uint16_t cport, hport;
} g_portmap[32];
static int g_nportmap = 0;
// fd -> the container port it bound (for getsockname)
static uint16_t g_fd_cport[1024];
static uint16_t pm_host(uint16_t c) {
    for (int i = 0; i < g_nportmap; i++)
        if (g_portmap[i].cport == c) return g_portmap[i].hport;
    return c;
}
// "H:C,H:C,..." (docker -p order: host:container). Ports are strictly validated (1..65535);
// a bad field or more than the cap of entries is an error, not a silent drop.
static void parse_publish(const char *s) {
    while (s && *s) {
        if (g_nportmap >= 32) {
            fprintf(stderr, "dd: too many DD_PUBLISH entries (max 32)\n");
            exit(2);
        }
        const char *colon = strchr(s, ':');
        const char *comma = strchr(s, ',');
        if (!colon || (comma && colon > comma)) {
            fprintf(stderr, "dd: invalid DD_PUBLISH '%s': expected HOST:CONTAINER\n", s);
            exit(2);
        }
        unsigned h = dd_parse_port_field("DD_PUBLISH host port", s, colon);
        unsigned cc = dd_parse_port_field("DD_PUBLISH container port", colon + 1, comma);
        g_portmap[g_nportmap].cport = (uint16_t)cc;
        g_portmap[g_nportmap].hport = (uint16_t)h;
        g_nportmap++;
        if (!comma) break;
        s = comma + 1;
    }
}
// "128M"/"2G"/"512K"/"1048576" -> bytes (docker-style suffixes). Strict: empty/non-numeric/an
// unknown suffix is an error (atoi/strtoull would have silently yielded 0 = unlimited).
static uint64_t parse_size(const char *s) {
    if (!s || !*s) return 0;
    errno = 0;
    char *e = NULL;
    uint64_t v = strtoull(s, &e, 10);
    if (errno != 0 || e == s) {
        fprintf(stderr, "dd: invalid size '%s': not a number\n", s);
        exit(2);
    }
    switch (*e) {
    case '\0': return v;
    case 'k':
    case 'K': return v << 10;
    case 'm':
    case 'M': return v << 20;
    case 'g':
    case 'G': return v << 30;
    default:
        fprintf(stderr, "dd: invalid size '%s': bad suffix\n", s);
        exit(2);
    }
}

// guest PC -> (host = prologue entry for a fresh dispatcher entry,
//             body = post-prologue entry for a CHAINED jump with regs already live)
#define MAP_N 65536
