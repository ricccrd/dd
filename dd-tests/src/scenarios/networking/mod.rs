//! Networking inside ONE container — the loopback + resolver paths. A server binds a TCP/UDP port and a
//! client connects over 127.0.0.1; a real server (redis) does a SET/GET round-trip over loopback;
//! `/etc/hosts` + `getent` resolution. Outbound-to-the-internet cases are gated to the `Long` class so
//! the fast Quick net never depends on external reachability (they run under `--long`). busybox/alpine/
//! redis, each a few seconds. Owner: networking agent. Verified on the Real oracle. Edit ONLY this folder.

use crate::scenario::{scen, sgroup, ScenGroup};

pub fn group() -> ScenGroup {
    sgroup("networking", vec![
        // TCP loopback: a busybox `nc -l` server, a `nc` client, same container
        scen("networking/tcp-loopback", "busybox:latest")
            .exec("( echo TCPOK | nc -l -p 9100 ) & sleep 0.4\nnc -w 2 127.0.0.1 9100").has("TCPOK"),
        // UDP loopback
        scen("networking/udp-loopback", "busybox:latest")
            .exec("( echo UDPOK | nc -u -l -p 9101 -w 1 ) & sleep 0.4\necho ping | nc -u -w 1 127.0.0.1 9101").has("UDPOK"),
        // redis over loopback: a real server, SET then GET
        scen("networking/redis-loopback-setget", "redis:alpine")
            .exec("redis-server --save '' --appendonly no --daemonize yes >/dev/null 2>&1\nfor i in $(seq 1 40); do redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.1; done\nredis-cli set k loopval >/dev/null\nredis-cli get k").has("loopval"),
        // redis INCR aggregate over loopback (deterministic 1..1000 -> 1000 increments = 1000)
        scen("networking/redis-loopback-incr", "redis:alpine")
            .exec("redis-server --save '' --appendonly no --daemonize yes >/dev/null 2>&1\nfor i in $(seq 1 40); do redis-cli ping 2>/dev/null | grep -q PONG && break; sleep 0.1; done\nfor i in $(seq 1 1000); do redis-cli incr c >/dev/null; done\nredis-cli get c").has("1000"),
        // two ports bound at once on loopback, both reachable
        scen("networking/two-ports", "busybox:latest")
            .exec("( echo AAA | nc -l -p 9110 ) & ( echo BBB | nc -l -p 9111 ) & sleep 0.4\necho \"$(nc -w 2 127.0.0.1 9110)-$(nc -w 2 127.0.0.1 9111)\"").has("AAA-BBB"),
        // /etc/hosts + getent resolution of localhost (debian has getent)
        scen("networking/getent-localhost", "debian:bookworm")
            .exec("getent hosts localhost").has("localhost"),
        // loopback interface is up (127.0.0.1)
        scen("networking/loopback-iface", "alpine:latest")
            .exec("ip addr show lo 2>/dev/null | grep -q '127.0.0.1' && echo LO_UP || { ifconfig lo 2>/dev/null | grep -q '127.0.0.1' && echo LO_UP; }").has("LO_UP"),

        // ---- outbound to the internet — gated to Long (fast Quick net stays offline-deterministic) ----
        scen("networking/outbound-dns", "alpine:latest")
            .exec("nslookup one.one.one.one 2>&1 | grep -qiE '1\\.1\\.1\\.1|1\\.0\\.0\\.1' && echo DNS_OUT_OK").has("DNS_OUT_OK").long().timeout(30),
        scen("networking/outbound-tcp", "busybox:latest")
            .exec("nc -z -w 3 1.1.1.1 53 && echo OUT_TCP_OK").has("OUT_TCP_OK").long().timeout(30),
    ])
}
