// dd/runtime -- ddjit_aarch64: the aarch64-Linux-guest JIT runner (unity translation unit).
//
// A same-ISA aarch64->aarch64 JIT services the guest's Linux syscalls in userspace (no VM). This TU
// pulls in the engine (jit/), the aarch64 guest frontend (frontend/aarch64/), the Linux personality +
// container layer (os/linux/), and defines jit_run() (the Rust binding's entry) + main(). The x86-64
// guest reuses os/linux/ + jit/ with frontend/x86_64/ (see ddjit_x86_64.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <poll.h>
#include <signal.h>
#include <dirent.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <sys/event.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <stdatomic.h>

#include "include/cpu_aarch64.h"

#include "os/linux/container/state.c"   // container/ns config state + parsers (early globals)
#include "jit/cache.c"                  // code cache + block map + chaining
#include "jit/emit_arm64.c"             // aarch64 host emitters + IBTC/IC
#include "frontend/aarch64/translate.c" // transliterate + mangle + §B + LSE + depth-gate
#include "os/linux/thread.c"            // clone/futex/threads (declares run_guest)
#include "os/linux/signal.c"            // signal delivery
#include "os/linux/container/vfs.c"     // path jail + overlay + /proc synth
#include "os/linux/container/netns.c"   // termios + NET-ns loopback
#include "os/linux/fscache.c"           // ELF fwd-decls + FS-metadata cache
#include "os/linux/service.c"           // the syscall layer (service())
#include "jit/dispatch.c"               // host trampoline + run_guest
#include "os/linux/elf.c"               // ELF loader + initial stack

// ---- library entry (Rust binding) + main() ----
// ---------------- library entry (Rust bindings call this) ----------------
// Loads `argv[0]` (a guest aarch64 ELF, path resolved inside `rootfs` if given),
// runs it to completion, and returns the guest's exit code. argv is the guest
// argv (program + args). Single-shot per process: the daemon forks a child per
// container and calls this once. Declared in jit.h.
static void diag_hx(char *b, uint64_t v) { for (int i = 0; i < 16; i++) { int d = (v >> ((15 - i) * 4)) & 0xf; b[i] = d < 10 ? '0' + d : 'a' + d - 10; } }
static void diag_crash(int s, siginfo_t *si, void *uc) {            // async-signal-safe (write only)
    (void)uc; struct cpu *c = (struct cpu *)pthread_getspecific(g_cpu_key);
    char b[96]; for (int i = 0; i < 96; i++) b[i] = ' ';
    memcpy(b, "[CRASH] sig=X fault=0x", 22); b[11] = '0' + (s % 10);
    diag_hx(b + 22, (uint64_t)si->si_addr); memcpy(b + 38, " pc=0x", 6);
    diag_hx(b + 44, c ? c->pc : 0); memcpy(b + 60, " tid=0x", 7);
    diag_hx(b + 67, (uint64_t)(c ? c->ctid : 0)); b[83] = '\n';
    if (write(2, b, 84) < 0) {}
    _exit(139);
}
static void diag_hx8(char *b, uint32_t v) { for (int i = 0; i < 8; i++) { int d = (v >> ((7 - i) * 4)) & 0xf; b[i] = d < 10 ? '0' + d : 'a' + d - 10; } }
static mach_port_t g_exc_port;
typedef struct { mach_msg_header_t Head; mach_msg_body_t body;
    mach_msg_port_descriptor_t thread, task; NDR_record_t NDR;
    exception_type_t exception; mach_msg_type_number_t codeCnt; int64_t code[2]; char pad[64]; } exc_msg_t;
static void *exc_thread(void *arg) {   // catches faults on ALL threads (incl MAP_JIT workers)
    (void)arg; exc_msg_t msg;
    for (;;) {
        if (mach_msg(&msg.Head, MACH_RCV_MSG, 0, sizeof msg, g_exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != MACH_MSG_SUCCESS) continue;
        arm_thread_state64_t st; mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        kern_return_t gs = thread_get_state(msg.thread.name, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
        char b[200]; for (int i = 0; i < 200; i++) b[i] = ' ';
        memcpy(b, "[MACH] exc=0x", 13); diag_hx8(b + 13, msg.exception);
        memcpy(b + 21, " gs=0x", 6); diag_hx8(b + 27, gs);
        memcpy(b + 35, " fault=0x", 9); diag_hx(b + 44, (uint64_t)msg.code[1]);
        memcpy(b + 60, " hpc=0x", 7); diag_hx(b + 67, st.__pc);
        memcpy(b + 83, " x28=0x", 7); diag_hx(b + 90, st.__x[28]);
        Dl_info info; uint64_t off = 0; const char *sn = "?";
        if (dladdr((void *)st.__pc, &info)) { off = st.__pc - (uint64_t)info.dli_fbase; if (info.dli_sname) sn = info.dli_sname; }
        memcpy(b + 106, " off=0x", 7); diag_hx(b + 113, off); b[129] = ' ';
        int sl = 0; while (sn[sl] && sl < 40) { b[130 + sl] = sn[sl]; sl++; } b[130 + sl] = '\n';
        if (write(2, b, 131 + sl) < 0) {}
        _exit(139);
    }
    return NULL;
}
static void install_mach_exc(void) {
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_exc_port) != KERN_SUCCESS) return;
    mach_port_insert_right(mach_task_self(), g_exc_port, g_exc_port, MACH_MSG_TYPE_MAKE_SEND);
    task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION,
        g_exc_port, EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    pthread_t t; pthread_create(&t, NULL, exc_thread, NULL);
}
int jit_run(const char *rootfs, int argc, char *const argv[]) {
    if (argc < 1 || !argv || !argv[0]) return 2;
    if (rootfs) g_init_hostpid = getpid();                                              // PID ns: only containers (rootfs) get PID 1
    { const char *h = getenv("DD_HOSTNAME"); if (h && !g_hostname[0]) { strncpy(g_hostname, h, 64); }  // ddockerd -> jit config
      const char *m = getenv("DD_MEM_MAX");  if (m && !g_mem_max)  g_mem_max  = parse_size(m);
      const char *p = getenv("DD_PIDS_MAX"); if (p && !g_pids_max) g_pids_max = atoi(p);
      const char *pub = getenv("DD_PUBLISH"); if (pub && !g_nportmap) parse_publish(pub);
      const char *low = getenv("DD_LOWER"); if (low && !g_nlower) { char tb[8192]; snprintf(tb, sizeof tb, "%s", low);  // ro lowers, colon-sep (highest first)
          char *sv = NULL; for (char *t = strtok_r(tb, ":", &sv); t; t = strtok_r(NULL, ":", &sv)) add_lower(t); }
      const char *nn = getenv("DD_NETNS"); if (nn && nn[0] && !g_netns[0]) { snprintf(g_netns, sizeof g_netns, "/tmp/.ddnet-%.40s", nn); mkdir(g_netns, 0700); }  // private loopback ns
      const char *eu = getenv("DD_UID"); if (eu && g_uid < 0) g_uid = atoi(eu);
      const char *eg = getenv("DD_GID"); if (eg && g_gid < 0) g_gid = atoi(eg); }  // USER ns (process.user)
    if (getenv("CRASHDBG")) { struct sigaction sa; memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = diag_crash; sa.sa_flags = SA_SIGINFO; sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL);
        install_mach_exc(); }
    if (rootfs && rootfs[0]) {
        g_rootfs = (char *)rootfs;
        if (!realpath(g_rootfs, g_rootfs_canon)) snprintf(g_rootfs_canon, sizeof g_rootfs_canon, "%s", g_rootfs);
        g_rootfs_canon_len = strlen(g_rootfs_canon);   // the immutable jail boundary for secure_resolve
        g_root_fd = open(g_rootfs_canon, O_RDONLY | O_DIRECTORY);   // pinned root for the per-component resolver
        if (g_uid < 0) g_uid = 0; if (g_gid < 0) g_gid = 0;         // container default: run as root (0); unless DD_UID/--uid set
        const char *vspec = getenv("DDVOL");           // bind-mount volumes: "guestpath:hostdir,..."
        if (vspec) { char tmp[4096]; snprintf(tmp, sizeof tmp, "%s", vspec); char *sv = NULL;
            for (char *t = strtok_r(tmp, ",", &sv); t && g_nvols < 32; t = strtok_r(NULL, ",", &sv)) {
                char *col = strchr(t, ':'); if (!col || t[0] != '/') continue; *col = 0;
                struct vol *v = &g_vols[g_nvols];
                snprintf(v->guest, sizeof v->guest, "%s", t);
                v->glen = strlen(v->guest); while (v->glen > 1 && v->guest[v->glen-1] == '/') v->guest[--v->glen] = 0;
                if (!realpath(col + 1, v->hcanon)) continue;
                v->hlen = strlen(v->hcanon);
                if ((v->fd = open(v->hcanon, O_RDONLY | O_DIRECTORY)) < 0) continue;
                g_nvols++;
            } }
    }
    const char *prog = argv[0];

    if (getenv("TIER2_SELFTEST")) {   // verify the purity gate, PoC-style: pure->1, impure->0
        uint32_t pure[]   = { 0x8b010000u, 0x9b027c00u, 0xd65f03c0u };           // add x0,x0,x1; mul x0,x0,x2; ret
        uint32_t impure[] = { 0xf9400001u, 0x8b010000u, 0xd65f03c0u };           // ldr x1,[x0]; add; ret
        uint32_t sidef[]  = { 0xf9000001u, 0xd65f03c0u };                        // str x1,[x0]; ret
        fprintf(stderr, "[tier2] purity gate: pure=%d(want1) load=%d(want0) store=%d(want0)\n",
                region_pure(pure, 3), region_pure(impure, 3), region_pure(sidef, 2));
        struct cpu sc; sc.ssp = 0; int errs = 0, fast = 0;        // §B shadow-stack mechanism
        for (int d = 0; d < 200; d++) shadow_push(&sc, 0x1000 + d, 0);            // 200 nested calls
        for (int d = 199; d >= 0; d--) (shadow_classify(&sc, 0x1000 + d) == SS_FAST) ? fast++ : errs++;
        sc.ssp = 0; shadow_push(&sc, 0xA, 0); shadow_push(&sc, 0xB, 0); shadow_push(&sc, 0xC, 0);
        int unwind = (shadow_classify(&sc, 0xA) == SS_UNWIND);                 // longjmp past B,C -> A
        int foreign = (shadow_classify(&sc, 0xDEAD) == SS_FOREIGN);           // computed return
        fprintf(stderr, "[tier2] shadow stack: fast=%d/200 errs=%d(want0) unwind=%d(want1) foreign=%d(want1)\n",
                fast, errs, unwind, foreign);
    }
    if (pthread_key_create(&g_cpu_key, NULL) != 0) { perror("pthread_key_create"); return 1; }
    if (g_cpu_key >= 4096) { fprintf(stderr, "[jit] TSD key %u too large for inline ldr\n", (unsigned)g_cpu_key); return 1; }

    g_cache = mmap(NULL, CACHE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (g_cache == MAP_FAILED) { perror("mmap jit"); return 1; }
    g_cp = g_cache;

    g_trace = getenv("JT") != NULL; g_prof = getenv("PROF") != NULL;
    g_exe_path = prog;
    char pb[4200]; const char *prog_host = xresolve_exec(prog, pb, sizeof pb);  // follow entry symlink rootfs-relative (/bin/sh->busybox)
    struct loaded lm; load_elf(prog_host, &lm);

    // Dynamic: load the PT_INTERP (ld.so) and enter THERE; it loads libs + relocates.
    uint64_t jump = lm.entry, at_base = 0; char interp[256];
    if (elf_interp(prog_host, interp, sizeof interp) == 0) {
        char ib[4200]; const char *ihost = xresolve_exec(interp, ib, sizeof ib);   // follow+confine ld.so symlink
        struct loaded li; load_elf(ihost, &li);
        jump = li.entry; at_base = li.base;
    }

    uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    brk_lo = brk_cur = (uint64_t)heap; brk_hi = brk_lo + (256u << 20);

    struct cpu c; memset(&c, 0, sizeof c);
    c.sp = build_stack(argc, (char **)argv, &lm, at_base);   // guest argv = prog + its args
    c.pc = jump;

    run_guest(&c);
    return c.exit_code;
}

#ifndef DDJIT_LIB
int main(int argc, char **argv) {
    int ai = 1; const char *rootfs = NULL;
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] == '-') {                     // container flags (SentryConfig)
        if (!strcmp(argv[ai], "--rootfs") && ai + 1 < argc) { rootfs = argv[ai + 1]; ai += 2; }
        else if (!strcmp(argv[ai], "--hostname") && ai + 1 < argc) { strncpy(g_hostname, argv[ai + 1], 64); ai += 2; }
        else if (!strcmp(argv[ai], "--mem-max") && ai + 1 < argc) { g_mem_max = parse_size(argv[ai + 1]); ai += 2; }
        else if (!strcmp(argv[ai], "--pids-max") && ai + 1 < argc) { g_pids_max = atoi(argv[ai + 1]); ai += 2; }
        else if (!strcmp(argv[ai], "--publish") && ai + 1 < argc) { parse_publish(argv[ai + 1]); ai += 2; }
        else if (!strcmp(argv[ai], "--lower") && ai + 1 < argc) { add_lower(argv[ai + 1]); ai += 2; }   // ro overlay lower layer
        else if (!strcmp(argv[ai], "--netns") && ai + 1 < argc) { snprintf(g_netns, sizeof g_netns, "/tmp/.ddnet-%.40s", argv[ai + 1]); mkdir(g_netns, 0700); ai += 2; }  // private loopback ns
        else if (!strcmp(argv[ai], "--uid") && ai + 1 < argc) { g_uid = atoi(argv[ai + 1]); ai += 2; }   // USER ns uid
        else if (!strcmp(argv[ai], "--gid") && ai + 1 < argc) { g_gid = atoi(argv[ai + 1]); ai += 2; }
        else break;
    }
    if (ai >= argc) { fprintf(stderr, "usage: %s [--rootfs DIR] [--hostname NAME] [--mem-max BYTES] [--pids-max N] [--publish H:C] <aarch64-elf> [args...]\n", argv[0]); return 2; }
    return jit_run(rootfs, argc - ai, argv + ai);
}
#endif
