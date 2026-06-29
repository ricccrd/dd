/* completeness shared: syscall-number fallbacks (newer syscalls glibc may lack) + tiny helpers.
   Numbers below are the cross-arch-stable allocations (>= ~2019 syscalls share the same number on
   x86_64 and aarch64). Older syscalls always come from <sys/syscall.h>'s SYS_* (arch-correct). */
#ifndef DDC_COMPAT_H
#define DDC_COMPAT_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/syscall.h>
#include <unistd.h>
#include <errno.h>
#ifndef __NR_openat2
#define __NR_openat2 437
#endif
#ifndef __NR_faccessat2
#define __NR_faccessat2 439
#endif
#ifndef __NR_close_range
#define __NR_close_range 436
#endif
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_send_signal
#define __NR_pidfd_send_signal 424
#endif
#ifndef __NR_process_madvise
#define __NR_process_madvise 440
#endif
#ifndef __NR_clone3
#define __NR_clone3 435
#endif
#endif
