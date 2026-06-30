// Extracted from service(): Process & scheduling -- clone/fork/execve/wait/exit, pid/uid/gid identity,
// prctl/futex/caps/sched/affinity. Returns 1 if nr was handled, 0 otherwise. Because its cases call
// service.c-local helpers (nonpie_p/cpu_online_mask/affinity_mask), it is #included after them, before
// service(). NOTE: execve sets c->redirect; svc_done() (the shared tail) skips errno xlate when redirect
// is set, so a redirect's already-Linux G_RET is never re-translated.

// Restore guest GPRs that a per-arch fork/vfork->clone normalization repurposed as clone arguments (the x86
// frontend defines this in legacy.c; other frontends issue clone directly and need no fixup -> no-op).
#ifndef G_FORK_PRESERVE
#define G_FORK_PRESERVE(c) ((void)0)
#endif

static int svc_proc(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
    switch (nr) {
    // ===================== Process & scheduling — clone/exec/wait/ids/prctl/futex/caps/sched =====================
    case 90: {
        if (a1) memset((void *)a1, 0xff, 12);
        G_RET(c) = 0;
        break;
        // capget -> all caps present
    }
    // capset -> ok
    case 91: G_RET(c) = 0; break;
    case 93:
        c->exited = 1;
        c->exit_code = (int)a0;
        // exit: end THIS thread
        break;
    // exit_group: end the whole process
    case 94:
        if (getenv("PROF"))
            fprintf(stderr,
                    "[prof] crossings=%llu syscalls=%llu ibtc_miss=%llu branch_cross=%llu translations=%llu lse=%llu "
                    "wx_toggles=%llu dualmap=%d xlate_ms=%.3f mtibtc=%d mtfill=%llu futexq=%d "
                    "fwake_fast=%llu fwake_slow=%llu fwait=%llu\n",
                    (unsigned long long)g_prof_cross, (unsigned long long)g_prof_sys, (unsigned long long)g_prof_miss,
                    (unsigned long long)(g_prof_cross - g_prof_sys - g_prof_miss), (unsigned long long)g_prof_xlate,
                    (unsigned long long)g_lse_n, (unsigned long long)g_wx_toggles, g_dualmap, g_xlate_ns / 1e6,
                    g_mtibtc, (unsigned long long)g_mtfill, g_futexq, (unsigned long long)g_futex_wake_fast,
                    (unsigned long long)g_futex_wake_slow, (unsigned long long)g_futex_wait_n);
        // A3: §B shadow-return coverage. hit-rate = shret_hit / (shret_hit + shret_fb). bl_shadow /
        // bl_leaf show how the depth-gate split call sites at translate time. PROF-only (keep dark).
        if (getenv("PROF")) {
            unsigned long long h = (unsigned long long)g_prof_shret_hit, f = (unsigned long long)g_prof_shret_fb;
            double hr = (h + f) ? 100.0 * (double)h / (double)(h + f) : 0.0;
            fprintf(
                stderr,
                "[prof] shadow_push=%llu shret_hit=%llu shret_fb=%llu hit_rate=%.1f%% bl_shadow=%llu bl_leaf=%llu\n",
                (unsigned long long)g_prof_shpush, h, f, hr, (unsigned long long)g_prof_bl_shadow,
                (unsigned long long)g_prof_bl_leaf);
        }
#ifdef R_REPSTR // W4-C: x86-only rep cmps/scas idiom firing counts
        if (getenv("PROF"))
            fprintf(stderr, "[prof] repstr=%llu repstr_elems=%llu\n", (unsigned long long)g_repstr_n,
                    (unsigned long long)g_repstr_elems);
#endif
#ifdef G_PROF_EXTRA
        G_PROF_EXTRA; // W5B: x86 tier-2 promotion counters
#endif
        ep_prof_dump(); // w3e: flush epoll kevent-syscall counter (atexit is bypassed by _exit)
        ib_dump();      // ARM-B1 IBPROF: indirect-branch traffic + stability report (no-op unless IBPROF)
        vt_dump();      // ARM-B1 VDBETRACE: threading prototype counters (no-op unless VDBETRACE)
        if (g_noexit) { // W3D fork-server prewarm: don't kill the resident parent; unwind run_guest instead
            c->exited = 1;
            c->exit_code = (int)a0;
            break;
        }
#ifdef PCACHE_SAVE_HOOK
        PCACHE_SAVE_HOOK; // opt8: persist the translated arena before the one-shot _exit (DDJIT_PCACHE only)
#endif
        _exit((int)a0);
    case 96:
        G_RET(c) = (uint64_t)getpid();
        // set_tid_address -> returns caller's TID (musl stores it; 0 -> a_crash())
        break;
    case 97:
    // unshare / setns -> ok (no real ns)
    case 268: G_RET(c) = 0; break;
    // futex
    case 98: G_RET(c) = (uint64_t)futex_op((int *)a0, (int)a1 & 0x7f, (int)a2, (struct timespec *)a3); break;
    // set_robust_list
    case 99: G_RET(c) = 0; break;
    // syslog
    case 116: G_RET(c) = 0; break;
    // sched_setaffinity(pid, size, MASK=a2) -- record the requested mask (intersected with the online
    // set) so a later getaffinity reflects the pin; -EINVAL if it selects no online CPU, as on Linux.
    case 122: {
        size_t n = (size_t)a1;
        if (n > sizeof g_affinity) n = sizeof g_affinity;
        if (a2 && n) {
            uint8_t online[sizeof g_affinity];
            cpu_online_mask(online, sizeof online);
            uint8_t want[sizeof g_affinity];
            int any = 0;
            for (size_t i = 0; i < n; i++) {
                want[i] = ((const uint8_t *)a2)[i] & online[i];
                if (want[i]) any = 1;
            }
            if (!any) {
                G_RET(c) = (uint64_t)(-EINVAL);
                break;
            }
            memset(g_affinity, 0, sizeof g_affinity);
            memcpy(g_affinity, want, n);
            g_affinity_set = 1;
        }
        G_RET(c) = 0;
        break;
    }
    case 123: {
        size_t n = (size_t)a1;
        // sched_getaffinity(pid,size,MASK=a2!) -- return the current mask (all online CPUs by default),
        // not just CPU 0, so CPU_COUNT() and tcmalloc's enumeration see the real width (mongod aborts).
        if (n > 128) n = 128;
        if (a2 && n) memcpy((void *)a2, affinity_mask(), n);
        // Return the number of bytes the mask spans (glibc zeroes the remainder); 8 covers <=64 CPUs.
        G_RET(c) = n < 8 ? (uint64_t)n : 8;
        break;
    }
    // sched_yield
    case 124: G_RET(c) = 0; break;
    case 140:
        setpriority((int)a0, (int)a1, (int)a2);
        G_RET(c) = 0;
        // setpriority (best-effort)
        break;
    case 141: {
        errno = 0;
        // getpriority -> Linux raw (20-nice)
        int r = getpriority((int)a0, (int)a1);
        G_RET(c) = (r == -1 && errno) ? (uint64_t)(-errno) : (uint64_t)(20 - r);
        break;
    }
    case 144:
    case 146:
    case 147:
    // setgid/setfsuid/setresuid/setresgid -> ok
    case 149: G_RET(c) = 0; break;
    // getpgid
    case 145: G_RET(c) = (uint64_t)getpgrp(); break;
    case 148: {
        // getresuid(r,e,s)
        if (a0) *(uint32_t *)a0 = cuid();
        if (a1) *(uint32_t *)a1 = cuid();
        if (a2) *(uint32_t *)a2 = cuid();
        G_RET(c) = 0;
        break;
    }
    case 150: {
        // getresgid(r,e,s)
        if (a0) *(uint32_t *)a0 = cgid();
        if (a1) *(uint32_t *)a1 = cgid();
        if (a2) *(uint32_t *)a2 = cgid();
        G_RET(c) = 0;
        break;
    }
    // setpgid -- bash job control. The container init has getpid()==1 (container_pid), so bash issues
    // setpgid(0, 1); forwarded verbatim that names launchd (host pid 1) -> EPERM ("initialize_job_control:
    // setpgid: Operation not permitted"). Map the faked PID1 self-reference to the host's own process, and
    // treat a residual EPERM as success -- a container is its own session, so guest process groups are virtual.
    case 154: {
        // Map the guest's view of the init (pid/pgid 1) to its real host pid/group, then do the REAL setpgid.
        // Children already carry real host pids, so they pass straight through and get real process groups.
        // EPERM is benign (the init is a session leader, already its own group leader) -> report success.
        pid_t pid = ((pid_t)a0 == 1 && g_init_hostpid) ? g_init_hostpid : (pid_t)a0;
        pid_t pgid = ((pid_t)a1 == 1 && g_init_hostpid) ? g_init_hostpid : (pid_t)a1;
        int r = setpgid(pid, pgid);
        G_RET(c) = (r < 0 && errno != EPERM) ? (uint64_t)(-errno) : 0;
        break;
    }
    // getpgid / getsid -- translate the init's real host group/session id to the guest's pgid 1 so the guest's
    // identity is self-consistent (getpid 1 == getpgrp 1 == getsid 1). bash then sees itself as session+group
    // leader and initializes job control WITHOUT the setpgid EPERM / "cannot set terminal process group"
    // warning -- it enables job control cleanly, and the real terminal handoff works (see TIOCSPGRP above +
    // the rt_sigprocmask stop-signal mirroring).
    case 155: {
        pid_t r = getpgid((pid_t)a0);
        if (g_init_hostpid && r == g_init_hostpid) r = 1;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 156: {
        pid_t r = getsid((pid_t)a0);
        if (g_init_hostpid && r == g_init_hostpid) r = 1;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 158: {
        if (g_gid >= 0) {
            if ((int)a0 >= 1 && a1) *(gid_t *)a1 = (gid_t)cgid();
            G_RET(c) = 1;
            break;
            // getgroups -> [container gid]
        }
        int r = getgroups((int)a0, (gid_t *)a1);
        G_RET(c) = r < 0 ? (uint64_t)(-errno) : (uint64_t)r;
        break;
    }
    // setgroups (privileged; ignore)
    case 159: G_RET(c) = 0; break;
    // getrusage(who, *usage) -- a1 is the buffer, not a0!
    case 165: {
        struct rusage ru;
        // Linux RUSAGE_THREAD(1) -> SELF
        int who = ((int)a0 == -1) ? RUSAGE_CHILDREN : RUSAGE_SELF;
        if (a1) {
            uint8_t *d = (uint8_t *)a1;
            // Linux struct rusage layout (18 longs)
            memset(d, 0, 144);
            if (getrusage(who, &ru) == 0) {
                *(int64_t *)(d + 0) = ru.ru_utime.tv_sec;
                *(int64_t *)(d + 8) = ru.ru_utime.tv_usec;
                *(int64_t *)(d + 16) = ru.ru_stime.tv_sec;
                *(int64_t *)(d + 24) = ru.ru_stime.tv_usec;
                // macOS bytes -> Linux KB
                *(int64_t *)(d + 32) = ru.ru_maxrss / 1024;
                *(int64_t *)(d + 64) = ru.ru_minflt;
                *(int64_t *)(d + 72) = ru.ru_majflt;
                *(int64_t *)(d + 88) = ru.ru_inblock;
                *(int64_t *)(d + 96) = ru.ru_oublock;
                *(int64_t *)(d + 120) = ru.ru_nsignals;
                *(int64_t *)(d + 128) = ru.ru_nvcsw;
                *(int64_t *)(d + 136) = ru.ru_nivcsw;
            }
        }
        G_RET(c) = 0;
        break;
    }
    // prctl(option,...)
    case 167: {
        if ((int)a0 == 15) {
            snprintf(g_procname, sizeof g_procname, "%.15s", (const char *)a1);
            G_RET(c) = 0;
            break;
        } // PR_SET_NAME
        if ((int)a0 == 16) {
            snprintf((char *)a1, 16, "%s", g_procname);
            G_RET(c) = 0;
            break;
        } // PR_GET_NAME
        // 0 for known no-ops; EINVAL for unknown (kernel does)
        switch ((int)a0) {
        case 1:
        case 3:
        case 4:
        case 8:
        case 15:
        case 35:
        case 36:
        case 38:
        case 53:
        case 55:
        // PDEATHSIG/DUMPABLE/NAME/SECCOMP/TIMERSLACK/THP/SPECCTRL...
        case 59: G_RET(c) = 0; break;
        // EINVAL -- so feature probes (e.g. magic "AUXV") fail as on Linux
        default: G_RET(c) = (uint64_t)(-22); break;
        }
        break;
    }
    // getpid (PID ns: init -> 1)
    case 172: G_RET(c) = (uint64_t)container_pid(); break;
    case 173:
        G_RET(c) = (container_pid() == 1) ? 0 : (uint64_t)getppid();
        // getppid (init's parent is 0 in the ns)
        break;
    case 174:
    // getuid/geteuid -> container uid (0=root by default)
    case 175: G_RET(c) = (uint64_t)cuid(); break;
    case 176:
    // getgid/getegid
    case 177: G_RET(c) = (uint64_t)cgid(); break;
    // gettid
    case 178: G_RET(c) = (uint64_t)container_pid(); break;
    // clone(flags,stack,ptid,tls,ctid)
    case 220: {
        // CLONE_THREAD: stack arg IS the top
        if (a0 & 0x10000) {
            G_RET(c) = (uint64_t)spawn_thread(c, a0, a1, a3, a2, a4);
            break;
        }
        // fork/vfork: COW copy; child continues. Flush RAM-backed scratch into the real (shared) fds so
        // parent and child see one coherent file via the inherited description, exactly as POSIX requires
        // (the heap-resident buffers would otherwise COW-diverge while the fd stays shared).
        memf_materialize_all();
        pid_t pid = fork();
        if (pid == 0) {
            // clone(CLONE_VM, child_stack): glibc posix_spawn/popen/vfork pass a separate child stack in a1
            // and seed the clone trampoline (fn ptr + args) at its top. We fork() (COW) instead of sharing
            // the VM, but the child MUST run on a1 or glibc reads the trampoline off the parent's SP ->
            // garbage branch (SIGILL — broke initdb). a1==0 for a plain fork (bash), keeping the inherited SP.
            if ((a0 & 0x100) && a1) G_SP(c) = a1;
            // Re-assert MAP_JIT execute mode: the per-thread W^X/APRR state isn't reliable across fork(),
            // so the child's first run_block can instruction-abort fetching from the (non-executable) code
            // cache -> the intermittent fork+exec SIGBUS. pthread_jit_write_protect_np(1) = RX (executable).
            pthread_jit_write_protect_np(1);
            jit_after_fork();  // dual map: COW split the RW/RX aliases -> rebuild a fresh aliased cache
            G_SHADOW_RESET(c); // §B: child's pre-fork host_rets crossed run_block -> drop, use IBTC
            rc_reset();        // S2: drop the inherited (COW) path-resolution cache so the child can never
                               // serve a guest->host mapping that the parent populated before the FS diverged
            g_ndirs = 0;       // the getdents DIR* cache is the PARENT's -- closedir'ing inherited handles
                               // (on the child's close) crashes; drop it so the child re-fdopendir's fresh
#ifdef DD_HAS_MACH_EXC
            // The CRASHDBG Mach exception port + its receiver thread do NOT survive fork, so a crash in the
            // child silently dies. Clear the inherited task exception port so a fault falls through to the
            // POSIX diag_crash handler (which IS inherited) and reports fault=/pc=.
            if (getenv("CRASHDBG"))
                task_set_exception_ports(mach_task_self(), EXC_MASK_BAD_ACCESS | EXC_MASK_BAD_INSTRUCTION,
                                         MACH_PORT_NULL, EXCEPTION_DEFAULT, 0);
#endif
        }
        // parent: pid, child: 0
        G_RET(c) = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        // A fork/vfork that was normalized to clone repurposed the guest's arg registers; put them back so
        // the syscall preserves every GPR but rax, as the real kernel does (no-op for a genuine clone).
        G_FORK_PRESERVE(c);
        break;
    }
    // execve(path, argv, envp)
    case 221: {
        memf_materialize_all(); // non-CLOEXEC scratch fds survive exec -> flush RAM into the real files
        char pb[4200];
        const char *p =
            // follow symlink rootfs-relative (busybox applets), through the overlay (upper then lowers)
            xresolve_overlay((const char *)a0, pb, sizeof pb);
        if (access(p, F_OK) != 0) {
            G_RET(c) = (uint64_t)(-2);
            break;
            // ENOENT
        }
        char *argv[256];
        int ac = 0;
        uint64_t *gv = (uint64_t *)a1; // a1 (argv array base) already nonpie_p()'d at the top redirect
        while (gv && gv[ac] && ac < 255) {
            argv[ac] = (char *)nonpie_p(gv[ac]); // each argv[] element may itself be a low-image pointer
            ac++;
        }
        argv[ac] = NULL;
        // Capture the guest-absolute exec path NOW (a0 is still mapped) so /proc/self/exe can name the new
        // image after the teardown below. ld.so resolves a binary's $ORIGIN (DT_RUNPATH) via readlink of
        // /proc/self/exe; a stale value makes an exec'd dynamic binary fail to find its own libraries (e.g.
        // rustup's proxy execs the real rustc, whose RUNPATH $ORIGIN/../lib must point into the toolchain).
        char gexe[4200];
        abs_guest(-100, (const char *)a0, gexe, sizeof gexe);
        // shebang: exec the #! interpreter instead (parse_shebang is shared with the initial loader)
        char sh_interp[256], sh_arg[256], shpb[4200];
        if (parse_shebang(p, sh_interp, sizeof sh_interp, sh_arg, sizeof sh_arg) == 1) {
            snprintf(gexe, sizeof gexe, "%s", sh_interp); // a script exec: /proc/self/exe names the interpreter
            char *na[258];
            int ni = 0;
            // [interp, (optarg), scriptpath, args...]
            na[ni++] = sh_interp;
            if (sh_arg[0]) na[ni++] = sh_arg;
            // the guest script path (interp re-opens it)
            na[ni++] = (char *)a0;
            for (int i = 1; i < ac && ni < 256; i++)
                na[ni++] = argv[i];
            na[ni] = NULL;
            // load the interpreter, not the script -- through the overlay (the #! interp, e.g. /bin/sh, may
            // live only in a read-only lower in a fresh container; xresolve_exec sees the upper alone -> ENOENT)
            p = xresolve_overlay(sh_interp, shpb, sizeof shpb);
            if (access(p, F_OK) != 0) {
                G_RET(c) = (uint64_t)(-2);
                break;
            }
            for (int i = 0; i <= ni; i++)
                argv[i] = na[i];
            ac = ni;
        }
        // Tear down the inherited guest address space before loading the new image: a post-fork exec
        // otherwise keeps the parent's DENSE layout, and load_elf must bias a non-PIE ET_EXEC off its
        // fixed vaddr (__PAGEZERO blocks the low 4 GB) -> its baked absolute refs collide -> SIGSEGV.
        // argv + path live in guest memory we're about to munmap, so copy them to the host heap first.
        char *xpath = strdup(p);
        char *xargv[256];
        for (int i = 0; i < ac && i < 255; i++)
            xargv[i] = strdup(argv[i]);
        xargv[ac < 255 ? ac : 255] = NULL;
        gmap_reset_all();
        g_nonpie_lo = g_nonpie_hi = 0; // reset; load_elf re-sets it iff the new main image is non-PIE
        p = xpath;
        for (int i = 0; i < ac && i < 255; i++)
            argv[i] = xargv[i];
        argv[ac < 255 ? ac : 255] = NULL;
        struct loaded lm;
        load_elf(p, &lm);
        uint64_t jump = lm.entry, at_base = 0;
        char interp[256];
        if (elf_interp(p, interp, sizeof interp) == 0) {
            char ib[4200];
            // follow+confine ld.so symlink (through the overlay)
            const char *ih = xresolve_overlay(interp, ib, sizeof ib);
            struct loaded li;
            load_elf(ih, &li);
            jump = li.entry;
            at_base = li.base;
        }
        g_cp = g_cache;
        memset(g_map, 0, sizeof g_map);
        // flush old translations
        g_npend = 0;
        memset(g_ibtc, 0, sizeof g_ibtc);
        // execve: drop IBTC + §B shadow (old image)
        G_SHADOW_RESET(c);
        uint8_t *heap = mmap(NULL, 256u << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        gmap_add((uint64_t)heap, 256u << 20);
        brk_lo = brk_cur = (uint64_t)heap;
        brk_hi = brk_lo + (256u << 20);
        uint64_t sp = build_stack(ac, argv, &lm, at_base);
        free(xpath);
        for (int i = 0; i < ac && i < 255; i++)
            free(xargv[i]);
        snprintf(g_exe_path_store, sizeof g_exe_path_store, "%s", gexe); // /proc/self/exe -> the new image
        g_exe_path = g_exe_path_store;
        G_RESET_REGS(c);
        c->nzcv = 0;
        G_TLS(c) = 0;
        G_SP(c) = sp;
        G_PC(c) = jump;
        // jump to new program; don't advance pc
        c->redirect = 1;
        break;
    }
    // wait4(pid, *status, opts, *rusage)
    case 260: {
        int st = 0;
        pid_t r;
        // SA_RESTART: a wait interrupted by a handler that asked to restart (e.g. a SIGCHLD reaper, or
        // gcc's driver) must transparently retry instead of failing the guest with EINTR.
        do { r = wait4((pid_t)(int)a0, &st, (int)a2, (struct rusage *)a3); } while (r < 0 && SVC_EINTR_RESTART(c));
        if (r < 0) {
            G_RET(c) = (uint64_t)(-errno);
            break;
        }
        // WIFSIGNALED: macOS termsig -> Linux
        if ((st & 0x7f) != 0 && (st & 0x7f) != 0x7f) st = (st & ~0x7f) | (sig_m2l(st & 0x7f) & 0x7f);
        // WIFSTOPPED: macOS stopsig -> Linux
        else if ((st & 0xff) == 0x7f)
            st = (st & ~0xff00) | ((sig_m2l((st >> 8) & 0xff) & 0xff) << 8);
        if (a1) *(int *)a1 = st;
        G_RET(c) = (uint64_t)r;
        break;
    }
    case 261: {
        if (a3) {
            // prlimit64(pid,res,new,OLD): old=a3!
            uint64_t *o = (uint64_t *)a3;
            // RLIMIT_STACK=8MB, else unlimited
            o[0] = ((int)a1 == 3) ? (8ull << 20) : ~0ull;
            o[1] = ~0ull;
        }
        G_RET(c) = 0;
        break;
    }
    // clone3(clone_args*, size)
    case 435: {
        uint64_t *ca = (uint64_t *)a0;
        uint64_t flags = ca[0];
        // CLONE_THREAD: sp = stack + stack_size
        if (flags & 0x10000) {
            G_RET(c) = (uint64_t)spawn_thread(c, flags, ca[5] + ca[6], ca[7], ca[3], ca[2]);
            break;
        }
        pid_t pid = fork();
        // §B: same -- child drops the inherited shadow; S2: and the inherited path-resolution cache
        if (pid == 0) {
            jit_after_fork(); // dual map: rebuild the child's aliased cache (COW split RW/RX)
            G_SHADOW_RESET(c);
            rc_reset();
        }
        G_RET(c) = pid < 0 ? (uint64_t)(-errno) : (uint64_t)pid;
        break;
    }
    default:
        return 0;
    }
    return svc_done(c); // boundary errno xlate (host macOS -> Linux); see helpers.c svc_done
}
