
## Open — memory leaks / sustainability (found by the read-only memory scan, 2026-06-29)

| id | targets | symptom | location | testable on mac? | diag |
|----|---------|---------|----------|------------------|------|
| _leak-stw-cache_ | arm-linux (Linux-host future) | STW code-cache flush allocs a fresh 64MB×2 cache and never munmaps the old one → ~128MB host-RSS leak per flush, unbounded | `jit/cache.c:556` jit_flush_to_fresh | hard — needs multithread >64MB distinct-code/SMC churn; SMC=RWX is EPERM on macOS W^X. Latent now, bites on Linux host | MEMORY-SCAN-jit.md L1 |
| _x86-cachefull-exit_ | amd-linux | x86 engine has no STW path; a threaded cache-full `_exit(70)` (liveness, not a leak) | `frontend/x86_64/dispatch.c:90` | same trigger as above | MEMORY-SCAN-jit.md L3 |
| _x86-growpage-budget_ | amd-linux | lazy grow-page mmap bounded by a monotonic never-reset ~1GB budget → long large-working-set guest aborts SIGSEGV (guest-visible, not host leak) | `frontend/x86_64/elf.c:586` | yes — sustained grow/free over >256K distinct pages | MEMORY-SCAN-jit.md L2 |

GUEST-VISIBLE memory is CLEAN: the `memory` group (mmapfree/mallocfree/threadrss/forkrss/fdrss/mmapfilerss)
passes bounded=1 on all 3 engines (installed + fresh build). Engine-INTERNAL candidates above + the
os-layer scan (mc_ stat cache, execve teardown, per-thread struct cpu) need external host-RSS tests.

### Empirical host-RSS results (tools/memwatch.sh, shipping daemon, 2026-06-29)
- **Guest-visible memory: CLEAN.** The `memory` group (6 probes × 3 engines) passes bounded=1 on the
  installed AND fresh engine — mmap/munmap, malloc/free, thread, fork, fd all release.
- **Daemon under `docker exec` churn: BOUNDED (plateaus).** RSS grew +1072KB @ N=200 and +1168KB @ N=500
  → warmup, not a linear leak. D1 (`execs` HashMap never pruned, dd-daemon exec.rs:106) is a real logical
  retention worth capping for extreme longevity (100K+ execs), but NOT an acute RSS leak at realistic N.
- Untested (need a Mach-O daemon / Linux host): L1 STW cache-flush leak (needs RWX/SMC, EPERM on mac);
  C1 getdents >64 dir streams; C2 inotify watch churn. Scans: MEMORY-SCAN-{jit,os}.md.
