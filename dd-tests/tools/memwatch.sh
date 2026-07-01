#!/usr/bin/env bash
# memwatch — external host-RSS leak probe for the dd DAEMON/engine. Boots a private dd-daemon, runs a
# churn workload, and samples the daemon's mac-side RSS before/after to catch ENGINE-INTERNAL leaks that
# a guest's own RSS can't see (e.g. D1: the daemon `execs` HashMap grows per `docker exec`). Read-only on
# the engine — it MEASURES, it never fixes. Verdict: PASS (bounded) / LEAK (per-iter growth).
#
#   WORKLOAD=exec N=300 bash dd-tests/tools/memwatch.sh        # docker exec churn (D1)
#   WORKLOAD=create-rm N=300 bash dd-tests/tools/memwatch.sh   # create+rm churn (daemon record leak)
#
# Env: DD_DAEMON, DD_IMAGES, N (iters), THRESH_KB (growth budget). Drives the daemon via the mac bridge.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
N="${N:-300}"; THRESH_KB="${THRESH_KB:-20480}"; WORKLOAD="${WORKLOAD:-exec}"

DIR="$ROOT/target/dd-scen/memwatch-$$"; mkdir -p "$DIR"; SOCK="$DIR/dd.sock"
cat > "$DIR/boot.sh" <<EOF
echo \$\$ > "$DIR/daemon.pid"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK" DD_STATE="$DIR/state.json" DD_VOLUMES="$DIR/vol"
exec "$DAEMON" > "$DIR/daemon.log" 2>&1
EOF
mac bash "$DIR/boot.sh" </dev/null & BPID=$!
for i in $(seq 1 80); do [ -S "$SOCK" ] && break; sleep 0.25; done
[ -S "$SOCK" ] || { echo "daemon failed to start"; cat "$DIR/daemon.log" 2>/dev/null; exit 2; }
DPID="$(cat "$DIR/daemon.pid")"
rss(){ mac bash -lc "ps -o rss= -p $DPID 2>/dev/null" </dev/null | tr -d ' \r'; }
dk(){ mac bash -lc "DOCKER_HOST=unix://$SOCK docker $*" </dev/null 2>&1; }
cleanup(){ mac bash -lc "kill $DPID 2>/dev/null" </dev/null >/dev/null 2>&1; kill $BPID 2>/dev/null; rm -rf "$DIR"; }
trap cleanup EXIT

CID="$(dk run -d alpine sleep 900 | tail -1 | tr -d '\r')"
sleep 1; base="$(rss)"; [ -n "$base" ] || { echo "could not sample daemon rss"; exit 2; }
echo "warming: $WORKLOAD ×$N (daemon pid=$DPID, base=${base}KB)…"
case "$WORKLOAD" in
  exec)      for i in $(seq 1 "$N"); do dk exec "$CID" true >/dev/null 2>&1; done;;
  create-rm) for i in $(seq 1 "$N"); do c="$(dk create alpine true | tail -1 | tr -d '\r')"; dk rm "$c" >/dev/null 2>&1; done;;
  *) echo "unknown WORKLOAD=$WORKLOAD"; exit 2;;
esac
fin="$(rss)"; grew=$((fin-base)); [ "$grew" -lt 0 ] && grew=0
per=$(( N>0 ? grew*1024/N : 0 ))
echo "workload=$WORKLOAD N=$N daemon_rss base=${base}KB fin=${fin}KB grew=${grew}KB (~${per} B/iter) thresh=${THRESH_KB}KB"
if [ "$grew" -lt "$THRESH_KB" ]; then echo "PASS — daemon RSS bounded under $WORKLOAD churn"; exit 0
else echo "LEAK — daemon RSS grew ${grew}KB over $N iters (~${per} B/iter)"; exit 1; fi
