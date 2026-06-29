//! net — basics expansion (in-process JIT matrix). Owner: net agent. Edit ONLY this file.
//! Builders: src(name,file).oracle()/.exit()/.out()/.has(); port(name,file) for cross-engine golden.
//! Keep this module compiling at all times (`cargo build -p dd-tests`).
//!
//! Breadth over the sockets surface a real networked service leans on, loopback-only and deterministic:
//! TCP (multi-client echo, half-close/shutdown, bulk streamed transfer), UDP (connected send/recv),
//! AF_UNIX (named stream + dgram), socket options (SNDBUF/RCVBUF/LINGER/KEEPALIVE/TYPE), non-blocking
//! accept + poll, recv flags (MSG_PEEK/MSG_DONTWAIT/MSG_WAITALL), sendmsg/recvmsg with iovec + msg_name,
//! writev/readv, getsockname/getpeername, select over multiple fds, getaddrinfo(numeric), inet_pton/ntop,
//! gethostname, fcntl socket flags, and connect-refused error semantics.
//!
//! `port(...)` cases prove the networking is byte-identical emulated-on-Linux and native-on-macOS — the
//! acid test that a postgres/redis-shaped service behaves the same. Linux-only extensions (accept4,
//! SO_PEERCRED) are `src(...)` diffed against the native oracle.
#![allow(unused_imports)]
use crate::{group, src, port, fixture, in_rootfs, Case, Engine, Group};

pub fn groups() -> Vec<Group> {
    vec![ext_net()]
}

fn ext_net() -> Group {
    group("ext_net", vec![
        // ---- TCP ----
        port("tcp-multi", "ext_net/net_tcp_multi.c").out("tcp_multi total=1266\n"),
        port("tcp-shutdown", "ext_net/net_tcp_shutdown.c").out("tcp_shutdown got=25 eof=1\n"),
        port("tcp-bulk", "ext_net/net_tcp_bulk.c").out("tcp_bulk sum=13056000\n"),
        // ---- UDP ----
        port("udp-connected", "ext_net/net_udp_connected.c").out("udp_connected total=756\n"),
        // ---- AF_UNIX ----
        port("unix-stream", "ext_net/net_unix_stream.c").out("unix_stream reply=UNIX-STREAM\n"),
        port("unix-dgram", "ext_net/net_unix_dgram.c").out("unix_dgram reply=dgram-unix\n"),
        // ---- socket options ----
        port("sockopt-buf", "ext_net/net_sockopt_buf.c").out("sockopt_buf set_ok=1 snd_ge=1 rcv_ge=1\n"),
        port("so-linger", "ext_net/net_so_linger.c").out("so_linger on=1 t=5 keepalive=1 type_stream=1\n"),
        port("sock-flags", "ext_net/net_socket_cloexec.c").out("sock_cloexec before=0 after=1 nonblock=1\n"),
        // ---- non-blocking accept + poll ----
        port("poll-accept", "ext_net/net_poll_accept.c").out("poll_accept ready=1 got=ping\n"),
        // ---- recv flags ----
        port("msg-peek", "ext_net/net_msg_peek.c").out("msg_peek peeked=peekdata read=peekdata same=1\n"),
        port("msg-dontwait", "ext_net/net_msg_dontwait.c").out("msg_dontwait eagain=1 then=2\n"),
        port("msg-waitall", "ext_net/net_msg_waitall.c").out("msg_waitall n=10 data=ABCDEFGHIJ\n"),
        // ---- vectored / ancillary IO ----
        port("writev", "ext_net/net_writev.c").out("writev w=9 r=9 data=foobarbaz\n"),
        port("sendmsg-addr", "ext_net/net_sendmsg_addr.c").out("sendmsg_addr n=8 lo=1\n"),
        // ---- name introspection / multiplexing ----
        port("getpeername", "ext_net/net_getpeername.c").out("getpeername peer_ok=1 srvport=1\n"),
        port("select-multi", "ext_net/net_select_multi.c").out("select_multi ready=2 both=1\n"),
        // ---- address conversion / resolution ----
        port("getaddrinfo", "ext_net/net_getaddrinfo.c").out("getaddrinfo r=0 ip=127.0.0.1 port_ok=1\n"),
        port("inet-pton", "ext_net/net_inet_pton.c").out("inet_pton v4=192.168.1.42 v6=2001:db8::1 bad=0\n"),
        port("gethostname", "ext_net/net_gethostname.c").out("gethostname r=0 nonempty=1\n"),
        // ---- error semantics ----
        port("connect-refused", "ext_net/net_connect_refused.c").out("connect_refused refused=1\n"),
        // ---- Linux-only extensions — diffed vs native oracle ----
        src("accept4", "ext_net/net_accept4.c").oracle(),     // accept4 flag inheritance (no macOS)
        // SO_PEERCRED returns a zeroed ucred under the JIT (uid/pid not populated). xfail Linux;
        // see GAPS "ext-peercred".
        src("peercred", "ext_net/net_peercred.c").oracle(),
    ])
}
