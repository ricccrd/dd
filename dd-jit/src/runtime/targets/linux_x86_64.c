// dd/runtime/frontend/x86_64 -- the x86-64-Linux-guest JIT (jit86), brought in WHOLE.
//
// jit86 is under active improvement upstream (poc/runtime/jit86/jit86.c) and has a DIFFERENT cpu
// struct + its own (basic) container runtime, so it is not yet decomposed onto the shared jit/ +
// os/linux/ layer. Stage-1 goal: build the x86 binary alongside aarch64. DEDUP (next stage): lift it
// onto the shared engine + container layer via cpu-access accessors + canonical syscall ids, then
// split it the way aarch64 already is. Re-sync with: make sync-jit86.

// jit86.c — an x86-64-guest JIT (x86-64 -> ARM64) for Linux guests on macOS/arm64.
//
// Sibling of runtime/jit/jit.c (which is aarch64->aarch64). See DESIGN.md for the
// full "what breaks / what doesn't" rationale. Short version:
//
//   * The ISA-AGNOSTIC scaffolding (code cache, guest-PC->host-code map, direct-
//     branch chaining, the run_block/block_return trampolines, the Linux->macOS
//     syscall bodies, the ELF loader, rootfs path rewriting) is COPIED+ADAPTED from
//     jit.c. We can't refactor jit.c (it's under active dev), so we duplicate.
//   * The FRONT-END is new: an x86-64 decoder + per-opcode ARM64 codegen, replacing
//     jit.c's "copy the instruction verbatim" core (which only works same-arch).
//
// Register model (the win from x86 having only 16 GPRs, see DESIGN.md §4):
//   guest  rax rcx rdx rbx rsp rbp rsi rdi  r8..r15
//   host    x0  x1  x2  x3  x4  x5  x6  x7  x8..x15   (guest reg# == host reg#)
//   cpu ptr : x28 (PINNED for the whole block)   scratch : x16,x17   forbidden: x18
//   flags   : ARM nzcv saved/restored to cpu->nzcv (exact for cmp/test->jcc, §9)
//
// Status: BOOTSTRAP. Implements enough to run a freestanding write+exit guest and a
// growing slice toward simple busybox. Unknown opcodes print their bytes and exit —
// that is the iterative workflow (run -> see unimpl -> add it -> repeat).

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
#include <netinet/tcp.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <sys/times.h>
#include <poll.h>
#include <sys/event.h> // kqueue: backs epoll/timerfd/inotify on macOS
#include <dirent.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <stdatomic.h>
#include <libkern/OSCacheControl.h>

#include "../include/cpu_x86_64.h"
#include "../translate/x86_64/abi.h"          // cpu-interface seam (G_* contract + sysmap + normalize)
#include "../translate/x86_64/dispatch_hooks.h" // x86 dispatch seam for the SHARED engine/dispatch.c (engine-dedup)
#include "../translate/x86_64/fill_stat.c"    // per-arch struct-stat layout os/linux fills

#include "../os/linux/container/state.c"     // SHARED: container globals (rootfs/cwd/netns/ids/fd tables)
#include "../translate/x86_64/engine_glue.c"  // x86-only engine globals (trace/diag) the shared cache.c omits
#include "../engine/cache.c"                     // SHARED engine: code cache + block map (hash via G_GPC_HASH_SHIFT)
#include "../translate/x86_64/emit.c"         // x86 engine: arm64 emitters + SSE + x87
#include "../translate/x86_64/decode.c"       // x86-64 decoder
#include "../translate/x86_64/translate.c"    // x86-64 translate_block + trampolines
#include "../translate/x86_64/pcache.c"       // opt8: persistent translated-code cache (DDJIT_PCACHE=1)
#include "../os/linux/thread.c"              // SHARED: clone->pthread, per-thread cpu, futex
#include "../os/linux/signal.c"              // SHARED: signal delivery driver + translation
#include "../translate/x86_64/sigframe.c"     // x86-64 rt_sigframe build/restore (uses signal.c state)
#include "../translate/x86_64/legacy.c"       // x86 legacy-syscall -> *at normalization (G_NORMALIZE)
#include "../os/linux/container/vfs.c"       // SHARED: rootfs jail, overlay, /proc synth, stat
#include "../os/linux/container/netns.c"     // SHARED: sockets, loopback netns, termios
#include "../os/linux/fscache.c"             // SHARED: fd/path cache
#include "../os/linux/syscall/dispatch.c"             // SHARED: the canonical syscall layer
#include "../os/linux/sentry.c"              // untrusted-guest isolation: SPSC ring + sentry split (g_untrusted)
#include "../translate/x86_64/x86_ops.c"        // x86 cpuid + x87 m80 block-exit helpers
#include "../translate/x86_64/avx.c"            // AVX/AVX2/AVX-512 (VEX/EVEX) emulation (R_AVX block-exit)
#include "../engine/dispatch.c"                  // SHARED engine: run_guest loop (x86 drives it via dispatch_hooks.h;
                                              // keeps its own run_block/block_return in translate.c, G_OWN_TRAMPOLINES)
#include "../translate/x86_64/elf.c"      // x86 ELF loader + stack + fault handlers (per-arch: machine/platform)

// ---- entry + main ----
// ---------------- entry ----------------
// W3D fork-server refactor: the original dd_run inlined (1) container init, (2) engine init
// (pthread key + MAP_JIT arena + signal handlers + trace env), and (3) per-launch load+run. The
// resident ddjitd parent must pay (1)+(2) ONCE and share them COW with every forked worker, so
// those two phases are factored into container_init()/engine_global_init(). engine_global_init()
// is idempotent (g_engine_inited) so the standalone path is byte-for-byte unchanged: standalone
// dd_run() composes container_init -> engine_global_init -> load_program -> run_loaded in the
// exact original order, with the identical operations in each phase.
static int g_engine_inited;

static void container_init(const char *rootfs) {
    // PID ns: only containers (rootfs) get PID 1. Record the init's real host pid so the shared Linux
    // personality can virtualize just the init's identity (getpid()==1, host pgid<->guest pgid 1) and
    // pass real child pids straight through -- this is what makes bash job control (setpgid / TIOCSPGRP)
    // work on x86-64 the way it already does on aarch64. Without it g_init_hostpid stayed 0, getpid()
    // returned the real host pid, and bash's setpgid(0,1)/tcsetpgrp targeted host pid 1 (launchd) -> the
    // foreground command got SIGTTOU/SIGTTIN-stopped ("[N]+ Stopped  ls") instead of running.
    if (rootfs) g_init_hostpid = getpid();
    if (rootfs && rootfs[0]) { // the shared container jails against the canonical rootfs + its dir fd
        g_rootfs = (char *)rootfs;
        if (!realpath(g_rootfs, g_rootfs_canon)) snprintf(g_rootfs_canon, sizeof g_rootfs_canon, "%s", g_rootfs);
        g_rootfs_canon_len = strlen(g_rootfs_canon);
        g_root_fd = open(g_rootfs_canon, O_RDONLY | O_DIRECTORY);
        container_populate_dev(); // /dev/{fd,stdin,stdout,stderr,ptmx,pts,shm,console,...} the unpacker stripped
        // Container identity = root (0) by default, matching linux_aarch64.c; DD_UID/DD_GID (or --uid/--gid)
        // override. Without this g_uid stayed -1 and cuid() fell back to the HOST uid -> the guest saw
        // getuid()/geteuid() == the host's 501 ("I have no name!", non-root shell) on x86-64 only.
        const char *eu = getenv("DD_UID");
        if (eu && g_uid < 0) g_uid = dd_parse_id("DD_UID", eu);
        const char *eg = getenv("DD_GID");
        if (eg && g_gid < 0) g_gid = dd_parse_id("DD_GID", eg);
        if (g_uid < 0) g_uid = 0;
        if (g_gid < 0) g_gid = 0;
    }
    if (!getenv("DD_NONETNS")) { // opt-out: leave g_netns empty -> 127/8 uses the REAL host TCP stack
        // DD_NETNS is a short KEY (not a path) -- the SAME key netns.c derives the abstract-socket / IPC
        // namespace dirs from (/tmp/.ddabs-<key>, ...) and that the daemon + aarch64 engine use. The
        // private-loopback dir is derived FROM it. Inherit the key across exec / from the daemon, else
        // mint one from our pid. (Setting DD_NETNS to the full loopback path put slashes in those derived
        // dir names -> mkdir failed -> abstract-socket bind broke.)
        const char *ns = getenv("DD_NETNS");
        char key[40];
        if (ns && ns[0])
            snprintf(key, sizeof key, "%.39s", ns);
        else
            snprintf(key, sizeof key, "%d", (int)getpid());
        snprintf(g_netns, sizeof g_netns, "/tmp/dd-lo-%s", key);
        if ((mkdir(g_netns, 0700) == 0 || errno == EEXIST) && !(ns && ns[0]))
            setenv("DD_NETNS", key, 1);
    }
    {
        const char *vs =
            getenv("DDVOL"); // bind-mount volumes (env path; bridge usually can't pass env, so --vol too)
        if (vs && vs[0]) {
            char tmp[2048];
            snprintf(tmp, sizeof tmp, "%s", vs);
            char *sv;
            for (char *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                add_vol(t);
        }
    }
    {
        const char *pub = getenv("DD_PUBLISH");
        if (pub && pub[0] && !g_nportmap) parse_publish(pub);
    } // docker -p (inherit across exec)
    {
        const char *ls = getenv("DD_LOWER"); // overlay lower layers (inherit across exec)
        if (ls && ls[0] && !g_nlower) {
            char tmp[4096];
            snprintf(tmp, sizeof tmp, "%s", ls);
            char *sv;
            for (char *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                add_lower(t);
        }
    }
    if (g_rootfs) chdir(g_rootfs); // container model: guest cwd "/" maps to the rootfs root
    // docker -w / initial working directory: start the guest in DD_CWD (must be reachable inside the
    // container -- typically a bind-mounted volume). confine() normalizes + clamps it to the rootfs.
    const char *icwd = getenv("DD_CWD");
    if (icwd && icwd[0]) confine(icwd, g_cwd, sizeof g_cwd);
}

// W3D: idempotent engine init (pthread key + MAP_JIT arena + trace env + fault handlers). Returns 0
// on success, nonzero exit code on failure. First call wins; later calls are no-ops (g_engine_inited),
// so the resident parent pays this once and the standalone path runs it exactly as before.
static int engine_global_init(void) {
    if (g_engine_inited) return 0;
    if (pthread_key_create(&g_cpu_key, NULL) != 0) {
        perror("pthread_key_create");
        return 1;
    }
    g_cache = mmap(NULL, CACHE_SZ, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (g_cache == MAP_FAILED) {
        perror("mmap jit");
        return 1;
    }
    g_cp = g_cache;
    g_trace = getenv("JT") != NULL;
    g_systrace = getenv("JTS") != NULL;
    g_prof = getenv("PROF") != NULL;
    // W5B adaptive tier-2 controls (x86 engine)
    g_notier2x = getenv("NOTIER2X") != NULL;
    { const char *t = getenv("TIER2X_THRESHOLD"); if (t && atoll(t) > 0) g_t2thresh = (uint64_t)atoll(t); }
    // The OrbStack `mac` bridge does NOT propagate env vars; trace via a trigger file
    // and redirect stderr to a shared log (visible from the Linux side).
    int want_trace =
        access("runtime/jit86/TRACE_ON", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/TRACE_ON", F_OK) == 0;
    int want_watch =
        access("runtime/jit86/WATCH", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/WATCH", F_OK) == 0;
    if (want_watch) g_nochain = 1;
    if (access("runtime/jit86/ITRACE_ON", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/ITRACE_ON", F_OK) == 0) {
        g_itrace = 1;
        want_trace = 1;
    }
    if (access("runtime/jit86/PROF", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/PROF", F_OK) == 0) g_prof = 1;
    if (access("runtime/jit86/NOIBTC", F_OK) == 0 || access("/Users/x/dd/poc/runtime/jit86/NOIBTC", F_OK) == 0)
        g_noibtc = 1;
    int want_fault = want_trace || want_watch || access("runtime/jit86/FAULT_ON", F_OK) == 0 ||
                     access("/Users/x/dd/poc/runtime/jit86/FAULT_ON", F_OK) == 0;
    extern int g_diag;
    g_diag = want_fault;
    if (want_fault) { // FAULT_ON installs the fault handler WITHOUT slow per-block tracing (chaining stays on)
        if (want_trace) {
            g_trace = 1;
            g_tracecap = 200000; // cap runaway trace volume (override via TRACE_CAP file)
            FILE *cf = fopen("/Users/x/dd/poc/runtime/jit86/TRACE_CAP", "r");
            if (cf) {
                unsigned long long v = 0;
                if (fscanf(cf, "%llu", &v) == 1) g_tracecap = v;
                fclose(cf);
            }
        }
        freopen("/Users/x/dd/poc/runtime/jit86/trace.log", "w", stderr);
        setvbuf(stderr, NULL, _IONBF, 0);
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_flags = SA_SIGINFO;
        extern void jit86_faulth(int, siginfo_t *, void *);
        sa.sa_sigaction = jit86_faulth;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
    } else { // normal runs: lazy-guard handler maps over-read pages and retries
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_flags = SA_SIGINFO;
        extern void jit86_lazyguard(int, siginfo_t *, void *);
        sa.sa_sigaction = jit86_lazyguard;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
    }
    // Untrusted-guest isolation (the sentry process-split). OFF by default -> trusted path unchanged.
    g_untrusted = getenv("DDJIT_UNTRUSTED") != NULL;
    g_sentry_sandbox = getenv("DDJIT_SANDBOX") != NULL;
    g_engine_inited = 1;
    return 0;
}

// W3D: load main program + (optional) interp, recording the load base/entry/at_base into *lm/*li.
// Used both by the standalone path and by the fork-server's parent preload (so the COW-inherited
// image is byte-identical and the warm worker re-runs from the same entry at the same base). The
// gb/pb/ib buffers are static because g_exe_path = prog points into gb and must outlive this call.
static const char *load_program(const char *prog, struct loaded *lm, struct loaded *li, uint64_t *jump,
                                uint64_t *at_base, int *have_interp) {
    static char gb[1024];
    prog = find_in_path(prog, gb, sizeof gb); // bare "sh" (docker) -> "/bin/sh" via the container PATH
    g_exe_path = prog;

    static char pb[4200];
    const char *prog_host = xresolve_overlay(prog, pb, sizeof pb); // upper, then lowers (pure --lower image)
    // opt8: load the guest image + interp at FIXED VAs so the translated arena is byte-identical across
    // runs (one-shot g_force_base, cleared inside load_elf). Only when the persistent cache is enabled.
    if (g_pcache) g_force_base = PC_IMG_BASE;
    load_elf(prog_host, lm);
    g_loadbase = lm->base;
    *jump = lm->entry;
    *at_base = 0;
    *have_interp = 0;
    const char *interp_host = NULL;
    char interp[256];
    if (elf_interp(prog_host, interp, sizeof interp) == 0) {
        static char ib[4200];
        interp_host = xresolve_overlay(interp, ib, sizeof ib);
        if (g_pcache) g_force_base = PC_INTERP_BASE;
        load_elf(interp_host, li);
        *jump = li->entry;
        *at_base = li->base;
        *have_interp = 1;
    }
    // opt8: key the cache by the identity (dev/ino/size/mtime) of the guest binary AND its interpreter.
    if (g_pcache) g_pc_binid = pcache_make_id(prog_host, interp_host);
    return prog;
}

// W3D: fresh per-launch guest run from a loaded image. Allocates a private heap + stack + cpu and
// runs from `jump`. Shared by dd_run (standalone/cold) and the warm worker (which restores a
// pristine COW image first, then calls this against the parent-preloaded base). Body is the original
// dd_run tail verbatim (incl. the committed s1_calibrate + the fastsys/prof prints), so standalone
// behavior is byte-identical.
static int run_loaded(int argc, char *const argv[], struct loaded *lm, uint64_t jump, uint64_t at_base) {
    uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    brk_lo = brk_cur = (uint64_t)heap;
    brk_hi = brk_lo + (256u << 20);

    struct cpu c;
    memset(&c, 0, sizeof c);
    c.r[RSP] = build_stack(argc, (char **)argv, lm, at_base); // rsp -> argc
    c.r[RDX] = 0;                                             // rtld_fini = 0
    c.rip = jump;

    s1_calibrate(); // S1: anchor CNTVCT vs host REALTIME/MONOTONIC for the inline time fast path
                    // (also honors DDJIT_NOFASTSYS=1 kill-switch -> byte-identical old syscall path)
    proc_reg_publish(g_exe_path, argc, argv); // publish this process into the /proc table
    if (g_untrusted) sentry_init(); // fork the host-authority sentry + (optionally) confine the worker
    run_guest(&c);
    if (g_untrusted) sentry_shutdown(); // signal quit + waitpid (reap, no orphan)
    if (getenv("DDJIT_FASTSTAT") || g_fast_count)
        fprintf(stderr, "[fastsys] enabled=%d inline-served=%llu\n", g_fastsys, (unsigned long long)g_fast_count);
    if (g_prof)
        fprintf(stderr, "[prof] dispatcher round-trips=%llu  IBTC fills=%llu  (IBTC %s)\n",
                (unsigned long long)g_disp_n, (unsigned long long)g_ibtc_fill, g_noibtc ? "OFF" : "ON");
    return c.exit_code;
}

int dd_run(const char *rootfs, int argc, char *const argv[]) {
    if (argc < 1 || !argv || !argv[0]) return 2;
    // opt8 persistent translated-code cache: OPT-IN via DDJIT_PCACHE (default OFF -> byte-identical to the
    // baseline; the cross-engine matrix never sets it, so it is unaffected). Read once.
    g_coldprof = getenv("COLDPROF") != NULL;
    g_pcache = getenv("DDJIT_PCACHE") != NULL;
    container_init(rootfs);
    int rc = engine_global_init();
    if (rc) return rc;
    struct loaded lm, li;
    uint64_t jump, at_base;
    int have_interp;
    load_program(argv[0], &lm, &li, &jump, &at_base, &have_interp); // (sets g_pc_binid + fixed bases when g_pcache)
    if (g_pcache) {
        g_pc_entry = jump;
        int hit = pcache_load(jump); // graceful MISS on any stale/corrupt/truncated cache -> translate fresh
        if (g_coldprof)
            fprintf(stderr, "[pcache] %s reloc=%d\n", hit ? "HIT (translation skipped)" : "MISS", g_nreloc);
    }
    int ec = run_loaded(argc, argv, &lm, jump, at_base);
    pcache_save(); // exit via syscall 93 returns here; syscall 94 saves before _exit (idempotent atomic rename)
    return ec;
}

#include "../translate/x86_64/forkserver.c" // W3D: resident ddjitd fork-server (server/client/worker)

#ifndef DDJIT_LIB
int main(int argc, char **argv) {
    int ai = 1;
    const char *rootfs = NULL;
    static char self[4200];
    if (realpath(argv[0], self))
        g_self_path = self;
    else
        g_self_path = argv[0];
    // W3D fork-server dispatch (gated; standalone path untouched when neither flag is present):
    //   --server SOCK [--rootfs DIR] [--prewarm PROG] : run resident ddjitd, listen on SOCK
    //   --client SOCK [--rootfs DIR] PROG [args...]   : forward a launch request to a ddjitd
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--server") == 0) return ddjitd_server_main(argc, argv);
        if (strcmp(argv[i], "--client") == 0) return ddjitd_client_main(argc, argv);
    }
    while (ai + 1 < argc) { // --rootfs DIR / --vol guest:host (repeatable)
        if (strcmp(argv[ai], "--rootfs") == 0) {
            rootfs = argv[ai + 1];
            ai += 2;
        } else if (strcmp(argv[ai], "--vol") == 0) {
            add_vol(argv[ai + 1]);
            ai += 2;
        } else if (strcmp(argv[ai], "--publish") == 0 || strcmp(argv[ai], "-p") == 0) { // docker -p H:C (port-map)
            parse_publish(argv[ai + 1]);
            setenv("DD_PUBLISH", argv[ai + 1], 1);
            ai += 2;
        } else if (strcmp(argv[ai], "--lower") == 0) {
            add_lower(argv[ai + 1]);
            ai += 2;
        } // overlay read-only layer
        else if (strcmp(argv[ai], "--uid") == 0) { // docker --user uid (USER-ns uid); else container default 0
            g_uid = dd_parse_id("--uid", argv[ai + 1]);
            ai += 2;
        } else if (strcmp(argv[ai], "--gid") == 0) {
            g_gid = dd_parse_id("--gid", argv[ai + 1]);
            ai += 2;
        } else
            break;
    }
    if (ai >= argc) {
        fprintf(stderr, "usage: %s [--rootfs DIR] [--vol guest:host]... [-p H:C]... <x86-64-elf> [args...]\n", argv[0]);
        return 2;
    }
    return dd_run(rootfs, argc - ai, argv + ai);
}
#endif
