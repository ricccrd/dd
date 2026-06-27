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
#include "../frontend/x86_64/abi.h"          // cpu-interface seam (G_* contract + sysmap + normalize)
#include "../frontend/x86_64/fill_stat.c"    // per-arch struct-stat layout os/linux fills

#include "../os/linux/container/state.c"     // SHARED: container globals (rootfs/cwd/netns/ids/fd tables)
#include "../frontend/x86_64/cache.c"        // x86 engine: code cache + block map
#include "../frontend/x86_64/emit.c"         // x86 engine: arm64 emitters + SSE + x87
#include "../frontend/x86_64/decode.c"       // x86-64 decoder
#include "../frontend/x86_64/translate.c"    // x86-64 translate_block + trampolines
#include "../os/linux/thread.c"              // SHARED: clone->pthread, per-thread cpu, futex
#include "../os/linux/signal.c"              // SHARED: signal delivery driver + translation
#include "../frontend/x86_64/sigframe.c"     // x86-64 rt_sigframe build/restore (uses signal.c state)
#include "../frontend/x86_64/legacy.c"       // x86 legacy-syscall -> *at normalization (G_NORMALIZE)
#include "../os/linux/container/vfs.c"       // SHARED: rootfs jail, overlay, /proc synth, stat
#include "../os/linux/container/netns.c"     // SHARED: sockets, loopback netns, termios
#include "../os/linux/fscache.c"             // SHARED: fd/path cache
#include "../os/linux/service.c"             // SHARED: the canonical syscall layer
#include "../frontend/x86_64/x86_ops.c"        // x86 cpuid + x87 m80 block-exit helpers
#include "../frontend/x86_64/dispatch.c"     // x86 run_guest dispatcher
#include "../frontend/x86_64/elf.c"      // x86 ELF loader + stack + fault handlers (per-arch: machine/platform)

// ---- entry + main ----
// ---------------- entry ----------------
int jit86_run(const char *rootfs, int argc, char *const argv[]) {
    if (argc < 1 || !argv || !argv[0]) return 2;
    if (rootfs && rootfs[0]) { // the shared container jails against the canonical rootfs + its dir fd
        g_rootfs = (char *)rootfs;
        if (!realpath(g_rootfs, g_rootfs_canon)) snprintf(g_rootfs_canon, sizeof g_rootfs_canon, "%s", g_rootfs);
        g_rootfs_canon_len = strlen(g_rootfs_canon);
        g_root_fd = open(g_rootfs_canon, O_RDONLY | O_DIRECTORY);
    }
    {
        const char *ns = getenv("JIT86_NETNS"); // private-loopback dir: inherit across exec, else create one
        if (ns && ns[0])
            snprintf(g_netns, sizeof g_netns, "%s", ns);
        else {
            char tmpl[64];
            snprintf(tmpl, sizeof tmpl, "/tmp/jit86-lo-%d", (int)getpid());
            if (mkdir(tmpl, 0700) == 0 || errno == EEXIST) {
                snprintf(g_netns, sizeof g_netns, "%s", tmpl);
                setenv("JIT86_NETNS", g_netns, 1);
            }
        }
    }
    {
        const char *vs =
            getenv("JIT86_VOL"); // bind-mount volumes (env path; bridge usually can't pass env, so --vol too)
        if (vs && vs[0]) {
            char tmp[2048];
            snprintf(tmp, sizeof tmp, "%s", vs);
            char *sv;
            for (char *t = strtok_r(tmp, ",", &sv); t; t = strtok_r(NULL, ",", &sv))
                add_vol(t);
        }
    }
    {
        const char *pub = getenv("JIT86_PUBLISH");
        if (pub && pub[0] && !g_nportmap) parse_publish(pub);
    } // docker -p (inherit across exec)
    {
        const char *ls = getenv("JIT86_LOWER"); // overlay lower layers (inherit across exec)
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
    const char *prog = argv[0];

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
    g_prof = getenv("PROF") != NULL;
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
    char gb[1024];
    prog = find_in_path(prog, gb, sizeof gb); // bare "sh" (docker) -> "/bin/sh" via the container PATH
    g_exe_path = prog;

    char pb[4200];
    const char *prog_host = xresolve_overlay(prog, pb, sizeof pb); // upper, then lowers (pure --lower image)
    struct loaded lm;
    load_elf(prog_host, &lm);
    g_loadbase = lm.base;

    uint64_t jump = lm.entry, at_base = 0;
    char interp[256];
    if (elf_interp(prog_host, interp, sizeof interp) == 0) {
        char ib[4200];
        const char *ihost = xresolve_overlay(interp, ib, sizeof ib);
        struct loaded li;
        load_elf(ihost, &li);
        jump = li.entry;
        at_base = li.base;
    }

    uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    brk_lo = brk_cur = (uint64_t)heap;
    brk_hi = brk_lo + (256u << 20);

    struct cpu c;
    memset(&c, 0, sizeof c);
    c.r[RSP] = build_stack(argc, (char **)argv, &lm, at_base); // rsp -> argc
    c.r[RDX] = 0;                                              // rtld_fini = 0
    c.rip = jump;

    run_guest(&c);
    if (g_prof)
        fprintf(stderr, "[prof] dispatcher round-trips=%llu  IBTC fills=%llu  (IBTC %s)\n",
                (unsigned long long)g_disp_n, (unsigned long long)g_ibtc_fill, g_noibtc ? "OFF" : "ON");
    return c.exit_code;
}

#ifndef JIT86_LIB
int main(int argc, char **argv) {
    int ai = 1;
    const char *rootfs = NULL;
    static char self[4200];
    if (realpath(argv[0], self))
        g_self_path = self;
    else
        g_self_path = argv[0];
    while (ai + 1 < argc) { // --rootfs DIR / --vol guest:host (repeatable)
        if (strcmp(argv[ai], "--rootfs") == 0) {
            rootfs = argv[ai + 1];
            ai += 2;
        } else if (strcmp(argv[ai], "--vol") == 0) {
            add_vol(argv[ai + 1]);
            ai += 2;
        } else if (strcmp(argv[ai], "--publish") == 0 || strcmp(argv[ai], "-p") == 0) { // docker -p H:C (port-map)
            parse_publish(argv[ai + 1]);
            setenv("JIT86_PUBLISH", argv[ai + 1], 1);
            ai += 2;
        } else if (strcmp(argv[ai], "--lower") == 0) {
            add_lower(argv[ai + 1]);
            ai += 2;
        } // overlay read-only layer
        else
            break;
    }
    if (ai >= argc) {
        fprintf(stderr, "usage: %s [--rootfs DIR] [--vol guest:host]... [-p H:C]... <x86-64-elf> [args...]\n", argv[0]);
        return 2;
    }
    return jit86_run(rootfs, argc - ai, argv + ai);
}
#endif
