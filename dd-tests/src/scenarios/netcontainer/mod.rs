//! Networking BETWEEN containers — a server container and a client container on a shared user-defined
//! network (`docker network create` + `--network`). Exercises the daemon's network plumbing, embedded
//! DNS (reach the server by container NAME), direct IP reach, and a real data exchange (redis SET/GET
//! and busybox `nc` echo) across the link. Host-orchestrated: `$NET` is a unique network, `$C…` are
//! reaped automatically. redis:alpine / busybox / alpine, each a few seconds. Owner: netcontainer agent.
//! Verified on the Real docker oracle. Edit ONLY this folder.

use crate::scenario::{scen, sgroup, ScenGroup};

pub fn group() -> ScenGroup {
    sgroup("netcontainer", vec![
        // reach the redis server by CONTAINER NAME (embedded DNS), SET then GET across containers
        scen("netcontainer/redis-by-name", "redis:alpine")
            .host("docker network create \"$NET\" >/dev/null\n\
                   docker run -d --rm --name ${C}srv --network \"$NET\" $PLAT $IMG >/dev/null\n\
                   for i in $(seq 1 40); do docker run --rm --network \"$NET\" $PLAT $IMG redis-cli -h ${C}srv ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done\n\
                   docker run --rm --network \"$NET\" $PLAT $IMG redis-cli -h ${C}srv set foo barbar >/dev/null\n\
                   echo \"BYNAME=$(docker run --rm --network \"$NET\" $PLAT $IMG redis-cli -h ${C}srv get foo)\"")
            .has("BYNAME=barbar").timeout(60),
        // reach the same server by its IP address
        scen("netcontainer/redis-by-ip", "redis:alpine")
            .host("docker network create \"$NET\" >/dev/null\n\
                   docker run -d --rm --name ${C}srv --network \"$NET\" $PLAT $IMG >/dev/null\n\
                   sleep 0.5\n\
                   IP=$(docker inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' ${C}srv)\n\
                   for i in $(seq 1 40); do docker run --rm --network \"$NET\" $PLAT $IMG redis-cli -h \"$IP\" ping 2>/dev/null | grep -q PONG && break; sleep 0.2; done\n\
                   docker run --rm --network \"$NET\" $PLAT $IMG redis-cli -h \"$IP\" set k ipval >/dev/null\n\
                   echo \"BYIP=$(docker run --rm --network \"$NET\" $PLAT $IMG redis-cli -h \"$IP\" get k)\"")
            .has("BYIP=ipval").timeout(60),
        // busybox nc echo: server sends a line, client (different container) reads it by name
        scen("netcontainer/nc-echo-by-name", "busybox:latest")
            .host("docker network create \"$NET\" >/dev/null\n\
                   docker run -d --rm --name ${C}srv --network \"$NET\" $PLAT $IMG sh -c 'while true; do echo NCREPLY | nc -l -p 7000; done' >/dev/null\n\
                   sleep 0.8\n\
                   docker run --rm --network \"$NET\" $PLAT $IMG nc -w 3 ${C}srv 7000")
            .has("NCREPLY").timeout(60),
        // DNS name resolution + reachability: ping the server container by name
        scen("netcontainer/ping-by-name", "alpine:latest")
            .host("docker network create \"$NET\" >/dev/null\n\
                   docker run -d --rm --name ${C}srv --network \"$NET\" $PLAT $IMG sleep 60 >/dev/null\n\
                   sleep 0.5\n\
                   docker run --rm --network \"$NET\" $PLAT $IMG ping -c 1 -W 3 ${C}srv >/dev/null 2>&1 && echo PINGED_BY_NAME")
            .has("PINGED_BY_NAME").timeout(60),
        // a container ON the network resolves the server by name; one OFF the network cannot (embedded
        // DNS is per user-network) — proves network isolation/scoping of the name.
        scen("netcontainer/isolation-off-network", "alpine:latest")
            .host("docker network create \"$NET\" >/dev/null\n\
                   docker run -d --rm --name ${C}srv --network \"$NET\" $PLAT $IMG sleep 60 >/dev/null\n\
                   sleep 0.5\n\
                   docker run --rm --network \"$NET\" $PLAT $IMG ping -c 1 -W 3 ${C}srv >/dev/null 2>&1 && echo ON_NET_RESOLVES\n\
                   docker run --rm $PLAT $IMG ping -c 1 -W 3 ${C}srv >/dev/null 2>&1 && echo OFF_NET_RESOLVES || echo OFF_NET_ISOLATED")
            .has("ON_NET_RESOLVES").has("OFF_NET_ISOLATED").timeout(60),
    ])
}
