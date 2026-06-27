# Running real software on dd — what works, what doesn't (research)

dd runs unmodified Linux binaries by JIT-translating them and servicing their syscalls in userspace.
This is the survey of **real, production software** run through the engines (aarch64 today; x86_64 for the
microbench/codegen matrix). Each "works" entry is covered by a test in `dd-tests` (`make test`) or the
docker scenarios (`make test-docker`).

## Works today (tested)

| software | what it exercises | test |
|---|---|---|
| **SQLite 3** | WAL, fsync, a 5000-row transaction, aggregate query — **intensive IO** | `realsw/sqlite` (oracle-diffed) |
| **Perl 5** (Ubuntu) | full interpreter: a prime sieve to 10k — heavy loop + dynamic dispatch | `realsw/perl-sieve` |
| **shell IO churn** | 200× create/write/read/unlink through the rootfs jail — **intensive IO** | `realsw/io-churn` |
| **busybox** (alpine) | ~20 applets: sed/awk/grep/sort/tr/base64/md5sum/find/… | `busybox/*` |
| **glibc / musl** | dynamic loader, TLS, RELRO, malloc (mmap path), pthreads, futex | the whole matrix |
| **coreutils** (ubuntu) | sort, stat, id, ln/readlink, mkdir/chmod | `container/*` |

The acid test is that these are the *actual upstream binaries* from real images, not microbenchmarks —
they hammer file IO, mmap, fork/exec (sh pipelines), signals, and the libc breadth.

## Docker workflow (tested, `make test-docker`)

22 end-to-end scenarios against `dd-daemon` via the real `docker` CLI: `version/info`, image discovery,
`run -d` → `logs` → `inspect` → `rm`, `ps -a`, exit-code propagation, `pull`, `stop`/`kill`/`restart`,
`volume create/ls/rm`, `network ls`.

## Doesn't work yet (the next codegen/coverage frontier)

| software | symptom | likely cause |
|---|---|---|
| **gcc / cc** | `cc1` segfaults | a huge C++ binary — an unimplemented opcode or a codegen edge in cc1; needs the instruction-trace differ |
| **python3** | hangs during interpreter init | a syscall or futex/threading path Python's startup hits that the runtime stalls on |
| **postgres / redis / nginx** | not installed in any image | need an OCI pull + unpack (the daemon's `images/create` is still a local-rootfs TODO), or apt/apk into a rootfs — and they'll then surface the same deep gcc/python-class bugs |

## To run postgres-class software

1. **Get the binary into a rootfs** — either finish OCI registry pull/unpack in `dd-daemon`
   (`images_create`), or `apt-get install postgresql` into an Ubuntu rootfs on a real Linux box and copy
   the rootfs dir into `DD_IMAGES`.
2. **Expect deep bugs** — postgres is multi-process (postmaster + backends via fork), shared-memory
   (`shmget`/`mmap(MAP_SHARED)`), and fsync-heavy. It will exercise the same frontier gcc/python do, plus
   real SysV shared memory. The path is: run it under the instruction-trace differ (the method that fixed
   the jit86 SSE/atomic bugs), fix what it hits, repeat. SQLite already proves the single-process
   intensive-IO path; postgres is the multi-process + shared-memory step beyond it.
