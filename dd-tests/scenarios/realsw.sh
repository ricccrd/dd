#!/usr/bin/env bash
# dd-tests/scenarios/realsw.sh -- run REAL upstream software (pulled from Docker Hub) under dd, with
# deterministic workloads. This is the "does production software actually work" tier: a database
# (postgres), an in-memory store (redis), a message broker (nats), and a language runtime (python) --
# each a large, syscall-heavy, fork/thread/mmap-heavy program that surfaces edge cases microtests miss.
#
#   bash dd-tests/scenarios/realsw.sh        # run after `make jit`; needs network to pull
#
# Each app self-skips if its image can't be pulled. A FAIL is a real gap (the workload is deterministic).
# Env: DD_IMAGES, DD_DAEMON.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
SOCK="$ROOT/dd-realsw.sock"
export DOCKER_HOST="unix://$SOCK"

pass=0 fail=0 skip=0
has()  { if echo "$2" | grep -qF "$3"; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: [$(echo "$2" | tr '\n' '|' | head -c 200)] lacks [$3]"; fail=$((fail+1)); fi; }
d()    { docker "$@" 2>&1; }
ensure() { # $1=image ; echo "ready" if present/pullable, else ""
    docker image inspect "$1" >/dev/null 2>&1 && { echo ready; return; }
    if timeout 150 docker pull "$1" >/dev/null 2>&1; then echo ready; fi
}

SCEN_LOG="$ROOT/dd-realsw.log"
pkill -x dd-daemon 2>/dev/null; rm -f "$SOCK"
# Fully isolate this daemon: private socket AND private state/volumes (a fresh per-scenario temp
# dir), so it starts from empty state and never reads or mutates the developer's real ~/.dd.
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dd-realsw.XXXXXX")"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK" DD_STATE="$STATE_DIR/state.json" DD_VOLUMES="$STATE_DIR/volumes"
"$DAEMON" >"$SCEN_LOG" 2>&1 &
DPID=$!
trap 'docker rm -f rsw-pg rsw-nats >/dev/null 2>&1; kill -9 $DPID 2>/dev/null; rm -f "$SOCK" "$SCEN_LOG"; rm -rf "$STATE_DIR"' EXIT
n=0; until [ -S "$SOCK" ] || [ $n -ge 60 ]; do sleep 0.25; n=$((n+1)); done
[ -S "$SOCK" ] || { echo "daemon failed to start:"; cat "$SCEN_LOG" 2>/dev/null; exit 1; }

echo "== redis (C, in-memory store; jemalloc, loopback, fork) =="
if [ -n "$(ensure redis:alpine)" ]; then
    out="$(timeout 90 docker run --rm redis:alpine sh -c '
        P=/usr/local/bin; "$P/redis-server" --save "" --appendonly no --daemonize no &
        sleep 3
        echo "ping=$("$P/redis-cli" ping)"
        "$P/redis-cli" set k hello-redis >/dev/null
        echo "get=$("$P/redis-cli" get k)"
        "$P/redis-cli" incr ctr >/dev/null; echo "incr=$("$P/redis-cli" incr ctr)"' 2>&1 | grep -vE 'jemalloc|QEMU')"
    has "redis-ping" "$out" "ping=PONG"
    has "redis-get"  "$out" "get=hello-redis"
    has "redis-incr" "$out" "incr=2"
else echo "  SKIP redis (pull failed)"; skip=$((skip+1)); fi

echo "== python (large C runtime; bytecode VM, import machinery, lots of syscalls) =="
if [ -n "$(ensure python:alpine)" ]; then
    # a deterministic mixed workload: arithmetic, dict/list, string, sorting, recursion.
    py='import functools
@functools.lru_cache(None)
def fib(n): return n if n<2 else fib(n-1)+fib(n-2)
d={}
for i in range(100000): d[i%1000]=d.get(i%1000,0)+i
print("py", "fib35="+str(fib(35)), "dictsum="+str(sum(d.values())), "sorted="+str(sorted([3,1,2])))'
    out="$(timeout 90 docker run --rm python:alpine python3 -c "$py" 2>&1)"
    has "python-runs" "$out" "py fib35=9227465 dictsum=4999950000 sorted=[1, 2, 3]"
else echo "  SKIP python (pull failed)"; skip=$((skip+1)); fi

echo "== postgres (C RDBMS; fork-per-connection, shared mem, WAL) =="
if [ -n "$(ensure postgres:alpine)" ]; then
    docker rm -f rsw-pg >/dev/null 2>&1
    d run -d --name rsw-pg -e POSTGRES_PASSWORD=pw -e POSTGRES_HOST_AUTH_METHOD=trust postgres:alpine >/dev/null 2>&1
    # wait for readiness (up to ~40s)
    ready=""; for i in $(seq 1 40); do
        if d logs rsw-pg 2>&1 | grep -q "database system is ready to accept connections"; then ready=1; break; fi
        sleep 1
    done
    has "postgres-ready" "${ready:-$(d logs rsw-pg 2>&1 | tail -3)}" "1"
    if [ -n "$ready" ]; then
        q="$(d exec rsw-pg psql -U postgres -tAc 'CREATE TABLE t(v int); INSERT INTO t SELECT generate_series(1,1000); SELECT count(*), sum(v) FROM t;' 2>&1)"
        has "postgres-query" "$q" "1000|500500"
    fi
    docker rm -f rsw-pg >/dev/null 2>&1
else echo "  SKIP postgres (pull failed)"; skip=$((skip+1)); fi

echo "== nats (Go message broker; goroutines, futex, netpoller) =="
if [ -n "$(ensure nats:latest)" ]; then
    docker rm -f rsw-nats >/dev/null 2>&1
    d run -d --name rsw-nats nats:latest >/dev/null 2>&1
    sleep 4
    has "nats-ready" "$(d logs rsw-nats 2>&1)" "Server is ready"
    docker rm -f rsw-nats >/dev/null 2>&1
else echo "  SKIP nats (pull failed / image arch undetectable)"; skip=$((skip+1)); fi

echo ""
echo "real-software: $pass passed, $fail failed, $skip skipped"
[ "$fail" -eq 0 ]
