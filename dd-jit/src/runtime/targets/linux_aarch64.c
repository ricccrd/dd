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
#include <mach/mach_vm.h> // mach_vm_remap/protect for the dual-mapped RW/RX code cache
#define DD_HAS_MACH_EXC 1 // service.c gates its CRASHDBG fork-child Mach re-arm on this
#include <dlfcn.h>
#include <sys/event.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <stdatomic.h>

#include "../include/cpu_aarch64.h"
#include "../frontend/aarch64/abi.h"       // the cpu interface os/linux/ is written against
#include "../frontend/aarch64/fill_stat.c" // the per-arch struct-stat layout os/linux/ fills

// container/ns config state + parsers (early globals)
#include "../os/linux/container/state.c"
// code cache + block map + chaining
#include "../jit/cache.c"
// aarch64 host emitters + IBTC/IC
#include "../jit/emit_arm64.c"
// transliterate + mangle + §B + LSE + depth-gate
#include "../frontend/aarch64/translate.c"
// clone/futex/threads (declares run_guest)
#include "../os/linux/thread.c"
// signal delivery
#include "../os/linux/signal.c"
#include "../frontend/aarch64/sigframe.c" // per-arch rt_sigframe build/restore (uses signal.c state)
// path jail + overlay + /proc synth
#include "../os/linux/container/vfs.c"
// termios + NET-ns loopback
#include "../os/linux/container/netns.c"
// ELF fwd-decls + FS-metadata cache
#include "../os/linux/fscache.c"
// the syscall layer (service())
#include "../os/linux/service.c"
// untrusted-guest isolation: SPSC ring + sentry split (g_untrusted; OFF by default)
#include "../os/linux/sentry.c"
// host trampoline + run_guest
#include "../jit/dispatch.c"
// ELF loader + initial stack
#include "../os/linux/elf.c"

// ---- library entry (Rust binding) + main() ----
// ---------------- library entry (Rust bindings call this) ----------------
// Loads `argv[0]` (a guest aarch64 ELF, path resolved inside `rootfs` if given),
// runs it to completion, and returns the guest's exit code. argv is the guest
// argv (program + args). Single-shot per process: the daemon forks a child per
// container and calls this once. Declared in jit.h.
static void diag_hx(char *b, uint64_t v) {
    for (int i = 0; i < 16; i++) {
        int d = (v >> ((15 - i) * 4)) & 0xf;
        b[i] = d < 10 ? '0' + d : 'a' + d - 10;
    }
}
// async-signal-safe (write only)
static void diag_crash(int s, siginfo_t *si, void *uc) {
    (void)uc;
    struct cpu *c = (struct cpu *)pthread_getspecific(g_cpu_key);
    char b[96];
    for (int i = 0; i < 96; i++)
        b[i] = ' ';
    memcpy(b, "[CRASH] sig=X fault=0x", 22);
    b[11] = '0' + (s % 10);
    diag_hx(b + 22, (uint64_t)si->si_addr);
    memcpy(b + 38, " pc=0x", 6);
    diag_hx(b + 44, c ? c->pc : 0);
    memcpy(b + 60, " tid=0x", 7);
    diag_hx(b + 67, (uint64_t)(c ? c->ctid : 0));
    b[83] = '\n';
    if (write(2, b, 84) < 0) {}
    _exit(139);
}
static void diag_hx8(char *b, uint32_t v) {
    for (int i = 0; i < 8; i++) {
        int d = (v >> ((7 - i) * 4)) & 0xf;
        b[i] = d < 10 ? '0' + d : 'a' + d - 10;
    }
}
static mach_port_t g_exc_port;
typedef struct {
    mach_msg_header_t Head;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t thread, task;
    NDR_record_t NDR;
    exception_type_t exception;
    mach_msg_type_number_t codeCnt;
    int64_t code[2];
    char pad[64];
} exc_msg_t;
// catches faults on ALL threads (incl MAP_JIT workers)
static void *exc_thread(void *arg) {
    (void)arg;
    exc_msg_t msg;
    for (;;) {
        if (mach_msg(&msg.Head, MACH_RCV_MSG, 0, sizeof msg, g_exc_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) !=
            MACH_MSG_SUCCESS)
            continue;
        arm_thread_state64_t st;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        kern_return_t gs = thread_get_state(msg.thread.name, ARM_THREAD_STATE64, (thread_state_t)&st, &cnt);
        char b[200];
        for (int i = 0; i < 200; i++)
            b[i] = ' ';
        memcpy(b, "[MACH] exc=0x", 13);
        diag_hx8(b + 13, msg.exception);
        memcpy(b + 21, " gs=0x", 6);
        diag_hx8(b + 27, gs);
        memcpy(b + 35, " fault=0x", 9);
        diag_hx(b + 44, (uint64_t)msg.code[1]);
        memcpy(b + 60, " hpc=0x", 7);
        diag_hx(b + 67, st.__pc);
        memcpy(b + 83, " x28=0x", 7);
        diag_hx(b + 90, st.__x[28]);
        Dl_info info;
        uint64_t off = 0;
        const char *sn = "?";
        if (dladdr((void *)st.__pc, &info)) {
            off = st.__pc - (uint64_t)info.dli_fbase;
            if (info.dli_sname) sn = info.dli_sname;
        }
        memcpy(b + 106, " off=0x", 7);
        diag_hx(b + 113, off);
        b[129] = ' ';
        int sl = 0;
        while (sn[sl] && sl < 40) {
            b[130 + sl] = sn[sl];
            sl++;
        }
        b[130 + sl] = '\n';
        if (write(2, b, 131 + sl) < 0) {}
        _exit(139);
    }
    return NULL;
}
static void install_mach_exc(void) {
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &g_exc_port) != KERN_SUCCESS) return;
    mach_port_insert_right(mach_task_self(), g_exc_port, g_exc_port, MACH_MSG_TYPE_MAKE_SEND);
    task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION, g_exc_port,
                             EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES, ARM_THREAD_STATE64);
    pthread_t t;
    pthread_create(&t, NULL, exc_thread, NULL);
}
// DD_FAULTCOUNT=1: measurement-only wrapper around nonpie_guard that tallies served low-address faults
// (the guest_base bias-fold's whole point is to drive this to ~0). Per-process; printed at jit_run exit.
static volatile uint64_t g_nonpie_faults;
static volatile uint64_t g_fhist[16];
static const char *g_fhname[16] = {"uoff", "unscaled", "wb",    "regoff", "ldp",   "ldp_wb", "excl", "lse",
                                   "cas",  "advmult",  "advsi", "dczva",  "ldlit", "ldar",   "other", "x"};
static int fault_class(uint32_t in) {
    if ((in & 0xFFFFFFE0u) == 0xD50B7420u) return 11;                            // dc zva
    if ((in & 0x3B000000u) == 0x18000000u) return 12;                           // ldr literal
    if ((in & 0x3A000000u) == 0x28000000u) return ((in >> 23) & 1) ? 5 : 4;     // ldp (wb if op2 odd)
    if ((in & 0x3F200C00u) == 0x38200000u) return 7;                            // lse atomic
    if ((in & 0x3FA07C00u) == 0x08A07C00u) return 8;                            // cas
    if ((in & 0x3F200000u) == 0x08000000u) return (in & 0x00800000u) ? 13 : 6;  // ordered(ldar)/exclusive
    if ((in & 0xBFBF0000u) == 0x0C000000u || (in & 0xBFA00000u) == 0x0C800000u) return 9; // advsimd mult
    if ((in & 0xBF000000u) == 0x0D000000u) return 10;                           // advsimd single
    if ((in & 0x3B000000u) == 0x39000000u) return 0;                            // uoff
    if ((in & 0x3B200C00u) == 0x38200800u) return 3;                            // regoff
    if (((in >> 27) & 7) == 7) {
        int m = (in >> 10) & 3;
        return (!((in >> 24) & 1) && (m == 1 || m == 3)) ? 2 : 1; // wb (pre/post) else unscaled/unpriv
    }
    return 14;
}
static void nonpie_guard_count(int sig, siginfo_t *si, void *uc) {
    uint64_t va = (uint64_t)si->si_addr;
    if (g_nonpie_lo && va >= g_nonpie_lo && va < g_nonpie_hi) {
        ucontext_t *u = (ucontext_t *)uc;
        g_fhist[fault_class(*(uint32_t *)(u->uc_mcontext->__ss.__pc))]++;
        uint64_t n = __atomic_add_fetch(&g_nonpie_faults, 1, __ATOMIC_RELAXED);
        if (n % 50000 == 0) {
            char b[256];
            int o = snprintf(b, sizeof b, "[fhist pid=%d n=%llu]", getpid(), (unsigned long long)n);
            for (int i = 0; i < 15; i++)
                if (g_fhist[i])
                    o += snprintf(b + o, sizeof b - o, " %s=%llu", g_fhname[i], (unsigned long long)g_fhist[i]);
            b[o++] = '\n';
            if (write(2, b, o) < 0) {}
        }
    }
    nonpie_guard(sig, si, uc);
}
int jit_run(const char *rootfs, int argc, char *const argv[]) {
    if (argc < 1 || !argv || !argv[0]) return 2;
    // PID ns: only containers (rootfs) get PID 1
    if (rootfs) g_init_hostpid = getpid();
    {
        const char *h = getenv("DD_HOSTNAME");
        // ddockerd -> jit config
        if (h && !g_hostname[0]) { strncpy(g_hostname, h, 64); }
        const char *m = getenv("DD_MEM_MAX");
        if (m && !g_mem_max) g_mem_max = parse_size(m);
        const char *p = getenv("DD_PIDS_MAX");
        if (p && !g_pids_max) g_pids_max = dd_parse_id("DD_PIDS_MAX", p);
        const char *pub = getenv("DD_PUBLISH");
        if (pub && !g_nportmap) parse_publish(pub);
        const char *low = getenv("DD_LOWER");
        if (low && !g_nlower) {
            char tb[8192];
            // ro lowers, colon-sep (highest first)
            snprintf(tb, sizeof tb, "%s", low);
            char *sv = NULL;
            for (char *t = strtok_r(tb, ":", &sv); t; t = strtok_r(NULL, ":", &sv))
                add_lower(t);
        }
        const char *nn = getenv("DD_NETNS");
        if (nn && nn[0] && !g_netns[0]) {
            snprintf(g_netns, sizeof g_netns, "/tmp/.ddnet-%.40s", nn);
            mkdir(g_netns, 0700);
            // private loopback ns
        }
        const char *eu = getenv("DD_UID");
        if (eu && g_uid < 0) g_uid = dd_parse_id("DD_UID", eu);
        const char *eg = getenv("DD_GID");
        if (eg && g_gid < 0) g_gid = dd_parse_id("DD_GID", eg);
        // USER ns (process.user)
    }
    if (getenv("CRASHDBG")) {
        // SA_ONSTACK + an alternate signal stack so the handler survives a corrupted/overflowed guest
        // stack (otherwise diag_crash double-faults and the process is signal-killed before it can report).
        static char altstk[64 * 1024];
        stack_t ss = {.ss_sp = altstk, .ss_size = sizeof altstk, .ss_flags = 0};
        sigaltstack(&ss, NULL);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = diag_crash;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        install_mach_exc();
    } else {
        // Normal runs: serve a non-PIE ET_EXEC's absolute DATA refs (baked at the low link vaddr) at +bias
        // and resume. Inert for PIE/static-PIE (nonpie_guard checks g_nonpie_lo, set only for ET_EXEC by
        // load_elf below); a fault it doesn't own re-raises with the default action.
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_sigaction = getenv("DD_FAULTCOUNT") ? nonpie_guard_count : nonpie_guard;
        sa.sa_flags = SA_SIGINFO;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
    }
    if (rootfs && rootfs[0]) {
        g_rootfs = (char *)rootfs;
        if (!realpath(g_rootfs, g_rootfs_canon)) snprintf(g_rootfs_canon, sizeof g_rootfs_canon, "%s", g_rootfs);
        // the immutable jail boundary for secure_resolve
        g_rootfs_canon_len = strlen(g_rootfs_canon);
        // pinned root for the per-component resolver
        g_root_fd = open(g_rootfs_canon, O_RDONLY | O_DIRECTORY);
        if (g_uid < 0) g_uid = 0;
        // container default: run as root (0); unless DD_UID/--uid set
        if (g_gid < 0) g_gid = 0;
        // bind-mount volumes: "[ro:]guestpath:hostdir,..." -- delegate to add_vol() (the shared vfs.c
        // parser) so the optional `ro:` read-only marker is handled in ONE place for both engines.
        const char *vspec = getenv("DDVOL");
        if (vspec) {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s", vspec);
            char *sv = NULL;
            for (char *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                add_vol(t);
        }
        // docker -w / initial working directory: start the guest in DD_CWD (must be reachable inside the
        // container -- typically a bind-mounted volume). confine() normalizes + clamps it to the rootfs.
        const char *icwd = getenv("DD_CWD");
        if (icwd && icwd[0]) confine(icwd, g_cwd, sizeof g_cwd);
    }
    const char *prog = argv[0];

    // verify the purity gate, PoC-style: pure->1, impure->0
    if (getenv("TIER2_SELFTEST")) {
        // add x0,x0,x1; mul x0,x0,x2; ret
        uint32_t pure[] = {0x8b010000u, 0x9b027c00u, 0xd65f03c0u};
        // ldr x1,[x0]; add; ret
        uint32_t impure[] = {0xf9400001u, 0x8b010000u, 0xd65f03c0u};
        // str x1,[x0]; ret
        uint32_t sidef[] = {0xf9000001u, 0xd65f03c0u};
        fprintf(stderr, "[tier2] purity gate: pure=%d(want1) load=%d(want0) store=%d(want0)\n", region_pure(pure, 3),
                region_pure(impure, 3), region_pure(sidef, 2));
        struct cpu sc;
        sc.ssp = 0;
        // §B shadow-stack mechanism
        int errs = 0, fast = 0;
        for (int d = 0; d < 200; d++)
            // 200 nested calls
            shadow_push(&sc, 0x1000 + d, 0);
        for (int d = 199; d >= 0; d--)
            (shadow_classify(&sc, 0x1000 + d) == SS_FAST) ? fast++ : errs++;
        sc.ssp = 0;
        shadow_push(&sc, 0xA, 0);
        shadow_push(&sc, 0xB, 0);
        shadow_push(&sc, 0xC, 0);
        // longjmp past B,C -> A
        int unwind = (shadow_classify(&sc, 0xA) == SS_UNWIND);
        // computed return
        int foreign = (shadow_classify(&sc, 0xDEAD) == SS_FOREIGN);
        fprintf(stderr, "[tier2] shadow stack: fast=%d/200 errs=%d(want0) unwind=%d(want1) foreign=%d(want1)\n", fast,
                errs, unwind, foreign);
    }
    if (pthread_key_create(&g_cpu_key, NULL) != 0) {
        perror("pthread_key_create");
        return 1;
    }
    if (g_cpu_key >= 4096) {
        fprintf(stderr, "[jit] TSD key %u too large for inline ldr\n", (unsigned)g_cpu_key);
        return 1;
    }

    // Code cache. Default: dual-mapped RW/RX so the engine never toggles W^X. Allocate a
    // plain anon RW region (the writer alias = g_cache) and vm_remap the SAME physical pages
    // to a second address that we mark RX (the executor alias). Writes through g_cache become
    // visible to execution at g_cache+g_rw2rx after an icache flush, with NO per-region
    // pthread_jit_write_protect_np() flip. NODUALMAP=1 reverts to a single MAP_JIT mapping.
    if (!getenv("NODUALMAP")) {
        uint8_t *rw;
        ptrdiff_t d;
        if (dualmap_alloc(&rw, &d) == 0) {
            g_cache = rw;
            g_rw2rx = d;
            g_dualmap = 1;
        } else {
            fprintf(stderr, "[jit] dual-map unavailable -> W^X-toggle fallback\n");
        }
    }
    if (!g_dualmap) {
        g_cache = mmap(NULL, CACHE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
        if (g_cache == MAP_FAILED) {
            perror("mmap jit");
            return 1;
        }
    }
    g_cp = g_cache;

    g_trace = getenv("JT") != NULL;
    g_prof = getenv("PROF") != NULL;
    g_ibprof = getenv("IBPROF") != NULL;          // ARM-B1 feasibility: indirect-branch traffic + stability log
    g_vdbetrace = getenv("VDBETRACE") != NULL;    // ARM-B1 prototype: VDBE dispatch threading PoC
    g_vt_hitcount = getenv("VTHITCOUNT") != NULL; // ARM-B1: inline SDC guard-hit counter (diagnostic)
    // A1: steal host x16/x17 for the engine (default on). NOSTEAL1617=1 -> legacy 3-reg stolen set
    // (guest x16/x17 in host regs, per-branch red-zone stash/restore). Read once before any translation.
    if (getenv("NOSTEAL1617")) g_steal1617 = 0;
    // guest_base bias-fold (non-PIE ET_EXEC): default on; NOGUESTFOLD=1 reverts to the SIGSEGV-per-low-
    // access fault path (A/B kill-switch). Read once before any translation; inert for PIE (gated on the
    // non-PIE marker g_nonpie_lo, set only for ET_EXEC by load_elf).
    if (getenv("NOGUESTFOLD")) g_guestfold = 0;
    if (getenv("NOMTIBTC")) g_mtibtc = 0; // W5C: disable race-free threaded IBTC fill (A/B kill-switch)
    if (getenv("NOFUTEXQ")) g_futexq = 0; // W5C: disable per-address futex wait queues (A/B kill-switch)
    // Untrusted-guest isolation (the sentry process-split). OFF by default -> trusted path unchanged.
    g_untrusted = getenv("DDJIT_UNTRUSTED") != NULL;
    g_sentry_sandbox = getenv("DDJIT_SANDBOX") != NULL;
    char gb[1024];
    prog = find_in_path(prog, gb, sizeof gb); // bare "sh" (docker) -> "/bin/sh" via the container PATH
    g_exe_path = prog;
    char pb[4200];
    const char *prog_host =
        // resolve through the overlay (upper, then lowers) + follow the entry symlink (/bin/sh->busybox)
        xresolve_overlay(prog, pb, sizeof pb);

    // Initial-exec shebang handling -- mirror of execve case 221 via the shared parse_shebang() helper.
    // The container entry may itself be a "#!" script (e.g. postgres' docker-entrypoint.sh). load_elf has
    // no ELF-magic/#! check, so it would parse the script text as a bogus ELF and fault before any guest
    // syscall runs. If the entry is a shebang, rewrite argv to [interp, (optarg), scriptpath, args...] and
    // load the INTERPRETER instead. A missing/non-shebang ELF falls straight through unchanged below.
    char sh_interp[256], sh_arg[256];
    char *sb_argv[256];
    if (parse_shebang(prog_host, sh_interp, sizeof sh_interp, sh_arg, sizeof sh_arg) == 1) {
        int sb_argc = 0;
        sb_argv[sb_argc++] = sh_interp;
        if (sh_arg[0]) sb_argv[sb_argc++] = sh_arg;
        // the guest script path (interp re-opens it through the jail); `prog` is pre-shebang, find_in_path-resolved
        sb_argv[sb_argc++] = (char *)prog;
        for (int i = 1; i < argc && sb_argc < 255; i++)
            sb_argv[sb_argc++] = (char *)argv[i];
        sb_argv[sb_argc] = NULL;
        argc = sb_argc;
        argv = (char *const *)sb_argv;
        // resolve the interpreter through the overlay/jail, exactly as the main program was resolved above
        prog = sh_interp;
        g_exe_path = sh_interp; // the binary actually loaded (matches /proc/self/exe for a script exec)
        prog_host = xresolve_overlay(sh_interp, pb, sizeof pb);
    }
    struct loaded lm;
    load_elf(prog_host, &lm);

    // Dynamic: load the PT_INTERP (ld.so) and enter THERE; it loads libs + relocates.
    uint64_t jump = lm.entry, at_base = 0;
    char interp[256];
    if (elf_interp(prog_host, interp, sizeof interp) == 0) {
        char ib[4200];
        // follow+confine ld.so symlink (through the overlay)
        const char *ihost = xresolve_overlay(interp, ib, sizeof ib);
        struct loaded li;
        load_elf(ihost, &li);
        jump = li.entry;
        at_base = li.base;
    }

    uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    gmap_add((uint64_t)heap, 256u << 20); // track so a later execve() can reclaim the inherited heap
    brk_lo = brk_cur = (uint64_t)heap;
    brk_hi = brk_lo + (256u << 20);

    struct cpu c;
    memset(&c, 0, sizeof c);
    // guest argv = prog + its args
    c.sp = build_stack(argc, (char **)argv, &lm, at_base);
    c.pc = jump;

    if (g_untrusted) sentry_init(); // fork the host-authority sentry + (optionally) confine the worker
    run_guest(&c);
    if (g_untrusted) sentry_shutdown(); // signal quit + waitpid (reap, no orphan)
    if (getenv("DD_FAULTCOUNT"))
        fprintf(stderr, "[faultcount] pid=%d nonpie_served=%llu\n", getpid(),
                (unsigned long long)g_nonpie_faults);
    return c.exit_code;
}

#ifndef DDJIT_LIB
int main(int argc, char **argv) {
    int ai = 1;
    const char *rootfs = NULL;
    // container flags (SentryConfig)
    while (ai < argc && argv[ai][0] == '-' && argv[ai][1] == '-') {
        if (!strcmp(argv[ai], "--rootfs") && ai + 1 < argc) {
            rootfs = argv[ai + 1];
            ai += 2;
        } else if (!strcmp(argv[ai], "--hostname") && ai + 1 < argc) {
            strncpy(g_hostname, argv[ai + 1], 64);
            ai += 2;
        } else if (!strcmp(argv[ai], "--mem-max") && ai + 1 < argc) {
            g_mem_max = parse_size(argv[ai + 1]);
            ai += 2;
        } else if (!strcmp(argv[ai], "--pids-max") && ai + 1 < argc) {
            g_pids_max = dd_parse_id("--pids-max", argv[ai + 1]);
            ai += 2;
        } else if (!strcmp(argv[ai], "--publish") && ai + 1 < argc) {
            parse_publish(argv[ai + 1]);
            ai += 2;
        } else if (!strcmp(argv[ai], "--lower") && ai + 1 < argc) {
            add_lower(argv[ai + 1]);
            ai += 2;
            // ro overlay lower layer
        } else if (!strcmp(argv[ai], "--netns") && ai + 1 < argc) {
            snprintf(g_netns, sizeof g_netns, "/tmp/.ddnet-%.40s", argv[ai + 1]);
            mkdir(g_netns, 0700);
            ai += 2;
            // private loopback ns
        } else if (!strcmp(argv[ai], "--uid") && ai + 1 < argc) {
            g_uid = dd_parse_id("--uid", argv[ai + 1]);
            ai += 2;
            // USER ns uid
        } else if (!strcmp(argv[ai], "--gid") && ai + 1 < argc) {
            g_gid = dd_parse_id("--gid", argv[ai + 1]);
            ai += 2;
        } else
            break;
    }
    if (ai >= argc) {
        fprintf(stderr,
                "usage: %s [--rootfs DIR] [--hostname NAME] [--mem-max BYTES] [--pids-max N] [--publish H:C] "
                "<aarch64-elf> [args...]\n",
                argv[0]);
        return 2;
    }
    return jit_run(rootfs, argc - ai, argv + ai);
}
#endif
