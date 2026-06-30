// Extracted from service(): Misc -- uname/sysinfo/getrandom/sethostname + rseq. Returns 1 if nr was
// handled, 0 otherwise. Included by service.c after service/event.c, before service() -- same TU scope.

static int svc_misc(struct cpu *c, uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                    uint64_t a4, uint64_t a5) {
    switch (nr) {
    case 160: {
        char *u = (char *)a0;
        // uname
        memset(u, 0, 6 * 65);
        strcpy(u, "Linux");
        strcpy(u + 65, g_hostname[0] ? g_hostname : "jit");
        strcpy(u + 130, "6.1.0");
        strcpy(u + 195, "#1 jit");
        strcpy(u + 260, G_UNAME_MACHINE); // per guest ISA: "x86_64" on jit86, "aarch64" on the arm engine
        G_RET(c) = 0;
        break;
    }
    case 161: {
        int n = (int)a1;
        if (n > 64) n = 64;
        if (n > 0) {
            memcpy(g_hostname, (void *)a0, n);
            g_hostname[n] = 0;
            // sethostname (UTS ns)
        }
        G_RET(c) = 0;
        break;
    }
    // setdomainname -> ignore
    case 162: G_RET(c) = 0; break;
    case 179: {
        memset((void *)a0, 0, 112);
        // sysinfo: report a plausible FINITE machine. A zeroed struct gives totalram=0, which the JVM
        // reads as "Too small maximum heap" -> abort. 64-bit struct sysinfo byte offsets: uptime@0,
        // loads@8, totalram@32, freeram@40, procs@80, mem_unit@104.
        char *si = (char *)a0;
        *(unsigned long *)(si + 0) = 3600;        // uptime (s)
        *(unsigned long *)(si + 32) = 8UL << 30;  // totalram = 8 GiB (mem_unit=1)
        *(unsigned long *)(si + 40) = 6UL << 30;  // freeram
        *(unsigned short *)(si + 80) = 64;        // procs
        *(unsigned int *)(si + 104) = 1;          // mem_unit
        G_RET(c) = 0;
        // sysinfo
        break;
    }
    case 278:
        arc4random_buf((void *)a0, (size_t)a1);
        G_RET(c) = a1;
        // getrandom
        break;
    case 293:
        G_RET(c) = (uint64_t)(-ENOSYS);
        // rseq -> ENOSYS (glibc falls back)
        break;
    default:
        return 0;
    }
    return svc_done(c); // boundary errno xlate (host macOS -> Linux); see helpers.c svc_done
}
