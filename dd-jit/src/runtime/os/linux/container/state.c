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
