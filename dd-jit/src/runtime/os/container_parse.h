// dd/runtime/os -- container config parsing: the strict numeric trust boundary (see docs/LAUNCH.md).
//
// dd is the actual runtime that executes untrusted guest images AND is reachable directly via the
// main()/`docker` CLI (bypassing the typed Rust binding), so it must NOT trust its config input.
// Every DD_* numeric value is re-validated here and a bad value FAILS LOUD: a clear message to stderr
// + nonzero exit, never a silent coercion to a privileged/wrong default. The classic footgun this
// kills: `atoi("oops") == 0`, which would silently run the container as uid 0 (root).
//
// Shared by both the linux container code (os/linux/container/state.c) and the darwin jail
// (os/darwin/darwinjail.c) so the rules + error messages are identical across all targets.
#ifndef DD_CONTAINER_PARSE_H
#define DD_CONTAINER_PARSE_H
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Parse a base-10 unsigned integer in [lo, hi]. Rejects empty / non-numeric / trailing garbage /
// negative / overflow. On ANY violation: print "dd: invalid <name>..." to stderr and exit nonzero.
static unsigned long long dd_parse_u64(const char *name, const char *s, unsigned long long lo,
                                       unsigned long long hi) {
    if (!s || !*s || *s == '-') {
        fprintf(stderr, "dd: invalid %s=%s: not a number\n", name, s ? s : "");
        exit(2);
    }
    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        fprintf(stderr, "dd: invalid %s=%s: not a number\n", name, s);
        exit(2);
    }
    if (v < lo || v > hi) {
        fprintf(stderr, "dd: invalid %s=%s: out of range %llu..%llu\n", name, s, lo, hi);
        exit(2);
    }
    return v;
}

// Container uid/gid: a valid id (0..INT_MAX). Garbage MUST error -- never fall back to 0 (= root).
static int dd_parse_id(const char *name, const char *s) {
    return (int)dd_parse_u64(name, s, 0, INT_MAX);
}

// A TCP/UDP port: 1..65535. Rejects 0 and >65535 (which atoi would wrap into a wrong u16).
static unsigned dd_parse_port(const char *name, const char *s) {
    return (unsigned)dd_parse_u64(name, s, 1, 65535);
}

// Parse a port from the field s[0..end) -- 'end' points just past the last char (e.g. at ':'/','),
// or NULL for "to end of string". Used by the HOST:CONTAINER publish parsers (delimited tokens).
static unsigned dd_parse_port_field(const char *name, const char *s, const char *end) {
    char buf[16];
    size_t n = end ? (size_t)(end - s) : strlen(s);
    if (n == 0 || n >= sizeof buf) {
        fprintf(stderr, "dd: invalid %s: bad port field\n", name);
        exit(2);
    }
    memcpy(buf, s, n);
    buf[n] = '\0';
    return dd_parse_port(name, buf);
}
#endif // DD_CONTAINER_PARSE_H
