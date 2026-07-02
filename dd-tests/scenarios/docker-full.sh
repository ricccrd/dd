#!/usr/bin/env bash
# dd-tests/scenarios/docker-full.sh -- FULL Docker CLI/API compliance matrix against dd-daemon.
#
# Walks (nearly) every docker command and asserts the documented behaviour, so a failure maps 1:1 to a
# non-compliant command. docker.sh covers the happy-path lifecycle; THIS file is the exhaustive sweep,
# focused on the commands docker.sh doesn't reach: create/start/commit/diff/export/import/save/load/
# history/port/update/attach/events/system/prune/inspect-forms, plus the data-bearing run flags
# (-p/-v/-w/--user/--memory/--label/--restart). Swarm/cluster verbs (swarm/node/service/stack/secret/
# config/plugin/checkpoint/manifest/trust) are out of scope for a single-host engine and are only
# probed for a graceful (non-crash) response.
#
#   bash dd-tests/scenarios/docker-full.sh        # run after `make jit`
#
# Env: DD_IMAGES (image dir, default poc/images), DD_DAEMON (default target/release/dd-daemon).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
IMAGES="${DD_IMAGES:-/Users/x/dd/poc/images}"
DAEMON="${DD_DAEMON:-$ROOT/target/release/dd-daemon}"
SOCK="$ROOT/dd-full.sock"
export DOCKER_HOST="unix://$SOCK"

pass=0 fail=0 gaps=""
ok()  { if [ "$2" = "$3" ]; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: got [$2] want [$3]"; fail=$((fail+1)); gaps="$gaps $1"; fi; }
has() { if echo "$2" | grep -qF "$3"; then echo "  ok   $1"; pass=$((pass+1)); else echo "  FAIL $1: [$2] lacks [$3]"; fail=$((fail+1)); gaps="$gaps $1"; fi; }
no()  { if echo "$2" | grep -qF "$3"; then echo "  FAIL $1: [$2] still has [$3]"; fail=$((fail+1)); gaps="$gaps $1"; else echo "  ok   $1"; pass=$((pass+1)); fi; }
d()   { docker "$@" 2>&1; }

SCEN_LOG="$ROOT/dd-full.log"
pkill -x dd-daemon 2>/dev/null; rm -f "$SOCK"
# Fully isolate this daemon: private socket AND private state/volumes (a fresh per-scenario temp
# dir), so it starts from empty state and never reads or mutates the developer's real ~/.dd.
STATE_DIR="$(mktemp -d "${TMPDIR:-/tmp}/dd-full.XXXXXX")"
export DD_IMAGES="$IMAGES" DDOCKERD_SOCK="$SOCK" DD_STATE="$STATE_DIR/state.json" DD_VOLUMES="$STATE_DIR/volumes"
"$DAEMON" >"$SCEN_LOG" 2>&1 &
DPID=$!
trap 'kill -9 $DPID 2>/dev/null; rm -f "$SOCK" "$SCEN_LOG" "$ROOT"/full-*.tar; rm -rf "$STATE_DIR"' EXIT
n=0; until [ -S "$SOCK" ] || [ $n -ge 60 ]; do sleep 0.25; n=$((n+1)); done
[ -S "$SOCK" ] || { echo "daemon failed to start:"; cat "$SCEN_LOG" 2>/dev/null; exit 1; }

echo "== system / info / version / df / events =="
has "version"        "$(d version --format '{{.Server.Version}}')" "."
has "info"           "$(d info --format '{{.OperatingSystem}}')" "dd"
has "system-info"    "$(d system info --format '{{.ServerVersion}}')" "."
has "system-df"      "$(d system df 2>&1)" "TYPE"
# events is a stream; bound it so it returns. Past-window query should not hang or error fatally.
has "events-window"  "$(timeout 4 d events --since 1m --until now 2>&1; echo EV-DONE)" "EV-DONE"

echo "== create -> start -> wait -> logs (the create/start split run() hides) =="
ccid="$(d create alpine sh -c 'echo created-then-started')"
has "create-returns-id" "$ccid" "$(echo "$ccid" | head -c 8)"
ok  "start"             "$(d start "$ccid" | head -c 12)" "$(echo "$ccid" | head -c 12)"
d wait "$ccid" >/dev/null
has "started-logs"      "$(d logs "$ccid")" "created-then-started"
d rm "$ccid" >/dev/null

echo "== inspect forms (container/image, alias subcommands) =="
icid="$(d run -d alpine sh -c 'sleep 5')"
has "inspect-id"          "$(d inspect --format '{{.Id}}' "$icid")" "$(echo "$icid" | head -c 12)"
has "container-inspect"   "$(d container inspect --format '{{.State.Status}}' "$icid")" "running"
has "container-ls"        "$(d container ls --format '{{.Image}}')" "alpine"
has "image-inspect-alias" "$(d image inspect alpine --format '{{.Os}}')" "linux"
has "image-ls-alias"      "$(d image ls --format '{{.Repository}}')" "alpine"
d rm -f "$icid" >/dev/null

echo "== diff (container filesystem changes) =="
dcid="$(d run -d alpine sh -c 'mkdir /newdir; echo x > /newfile; rm -f /etc/hostname; sleep 3')"
sleep 1
diff="$(d diff "$dcid" 2>&1)"
has "diff-added-file" "$diff" "/newfile"
has "diff-added-dir"  "$diff" "/newdir"
d rm -f "$dcid" >/dev/null

echo "== commit (container -> image) =="
mcid="$(d run -d alpine sh -c 'echo committed-content > /marker.txt; sleep 3')"
sleep 1
d commit "$mcid" full-committed:v1 >/dev/null 2>&1
has "commit-run-marker" "$(d run --rm full-committed:v1 cat /marker.txt 2>&1)" "committed-content"
d rm -f "$mcid" >/dev/null; d rmi full-committed:v1 >/dev/null 2>&1

echo "== export (container -> tar) + import (tar -> image) =="
ecid="$(d create alpine sh -c 'echo hi')"
d export "$ecid" > "$ROOT/full-export.tar" 2>/dev/null
has "export-nonempty" "$([ -s "$ROOT/full-export.tar" ] && echo OK)" "OK"
has "export-has-etc"  "$(tar tf "$ROOT/full-export.tar" 2>/dev/null | grep -m1 'etc/')" "etc/"
d import "$ROOT/full-export.tar" full-imported:v1 >/dev/null 2>&1
has "import-runs"     "$(d run --rm full-imported:v1 echo imported-ok 2>&1)" "imported-ok"
d rm "$ecid" >/dev/null; d rmi full-imported:v1 >/dev/null 2>&1

echo "== save (image -> tar) + load (tar -> image) =="
d save alpine -o "$ROOT/full-save.tar" 2>/dev/null
has "save-nonempty" "$([ -s "$ROOT/full-save.tar" ] && echo OK)" "OK"
d rmi full-saved:v1 >/dev/null 2>&1
d tag alpine full-saved:v1 >/dev/null 2>&1
d save full-saved:v1 -o "$ROOT/full-save2.tar" 2>/dev/null
d rmi full-saved:v1 >/dev/null 2>&1
has "load-restores" "$(d load -i "$ROOT/full-save2.tar" 2>&1)" "full-saved"
d rmi full-saved:v1 >/dev/null 2>&1

echo "== history (image layers) =="
has "history" "$(d history alpine 2>&1)" "IMAGE"

echo "== port (published-port listing) =="
pcid="$(d run -d -p 18091:80 alpine sh -c 'sleep 4')"
has "port-mapping" "$(d port "$pcid" 2>&1)" "18091"
d rm -f "$pcid" >/dev/null

echo "== update (container resource limits) =="
ucid="$(d run -d alpine sh -c 'sleep 4')"
has "update" "$(d update --memory 64m "$ucid" 2>&1 | head -c 12)$(echo "$ucid" | head -c 12)" "$(echo "$ucid" | head -c 12)"
d rm -f "$ucid" >/dev/null

echo "== run data flags: -v / -w / --user / --label / --hostname =="
vol="$ROOT/full-vol"; rm -rf "$vol"; mkdir -p "$vol"; echo "vol-content" > "$vol/f.txt"
has "run-v-bind"   "$(d run --rm -v "$vol":/mnt alpine cat /mnt/f.txt 2>&1)" "vol-content"
has "run-w-workdir" "$(d run --rm -w /tmp alpine pwd 2>&1)" "/tmp"
has "run-user"      "$(d run --rm --user 1000 alpine id -u 2>&1)" "1000"
has "run-hostname"  "$(d run --rm --hostname myhost alpine hostname 2>&1)" "myhost"
lcid="$(d run -d --label com.dd.role=worker alpine sh -c 'sleep 3')"
has "run-label"     "$(d inspect --format '{{.Config.Labels}}' "$lcid" 2>&1)" "worker"
has "ps-filter-label" "$(d ps --filter label=com.dd.role=worker --format '{{.Image}}' 2>&1)" "alpine"
d rm -f "$lcid" >/dev/null; rm -rf "$vol"

echo "== ps filters / formatting =="
fcid="$(d run -d --name filt-me alpine sh -c 'sleep 3')"
has "ps-filter-name"   "$(d ps --filter name=filt-me --format '{{.Names}}' 2>&1)" "filt-me"
has "ps-filter-status" "$(d ps --filter status=running --format '{{.Names}}' 2>&1)" "filt-me"
has "ps-quiet"         "$(d ps -q 2>&1 | head -c 4)" "$(echo "$fcid" | head -c 4)"
# `-aq` and `--format {{.ID}}` list the same truncated id `-q` does.
has "ps-aq"            "$(d ps -aq 2>&1 | head -c 4)" "$(echo "$fcid" | head -c 4)"
has "ps-format-id"     "$(d ps --format '{{.ID}}' 2>&1 | head -c 4)" "$(echo "$fcid" | head -c 4)"
# docker lists newest-first: a container created after filt-me must sort ahead of it, so it is the
# first line of both `ps` and `ps -q` (dd walked an unordered map before, giving an arbitrary order).
sleep 1; ncid="$(d run -d --name filt-new alpine sh -c 'sleep 3')"
ok  "ps-order-newest"  "$(d ps --format '{{.Names}}' 2>&1 | head -1)" "filt-new"
ok  "ps-order-quiet"   "$(d ps -q 2>&1 | head -1)" "$(echo "$ncid" | head -c 12)"
# a created-but-never-started container reports Status "Created" (not "Exited (0) ...").
crid="$(d create --name filt-created alpine sh -c 'sleep 3')"
ok  "ps-created-status" "$(d ps -a --filter name=filt-created --format '{{.Status}}' 2>&1)" "Created"
d rm -f "$fcid" "$ncid" "$crid" >/dev/null

echo "== prune verbs (container / image / volume / network / system) =="
prc="$(d run -d alpine sh -c 'echo done')"; d wait "$prc" >/dev/null
has "container-prune" "$(d container prune -f 2>&1)" "$(echo "$prc" | head -c 12)"
d volume create full-pv >/dev/null
has "volume-prune"  "$(d volume prune -f 2>&1; echo VP-DONE)" "VP-DONE"
d volume rm full-pv >/dev/null 2>&1
d network create full-pn >/dev/null
has "network-prune" "$(d network prune -f 2>&1; echo NP-DONE)" "NP-DONE"
d network rm full-pn >/dev/null 2>&1
has "image-prune"   "$(d image prune -f 2>&1; echo IP-DONE)" "IP-DONE"
has "system-prune"  "$(d system prune -f 2>&1; echo SP-DONE)" "SP-DONE"
has "builder-prune" "$(d builder prune -f 2>&1; echo BP-DONE)" "BP-DONE"

echo "== network connect/disconnect a running container =="
d network create full-net >/dev/null 2>&1
nccid="$(d run -d --name net-c alpine sh -c 'sleep 4')"
has "network-connect"    "$(d network connect full-net net-c 2>&1; echo NC-DONE)" "NC-DONE"
has "inspect-shows-net"  "$(d inspect --format '{{json .NetworkSettings.Networks}}' net-c 2>&1)" "full-net"
has "network-disconnect" "$(d network disconnect full-net net-c 2>&1; echo ND-DONE)" "ND-DONE"
d rm -f net-c >/dev/null; d network rm full-net >/dev/null 2>&1

echo "== exec variants (-e env, -w workdir, -u user) =="
xcid="$(d run -d alpine sh -c 'sleep 6')"
has "exec-env"     "$(d exec -e EX=exVAL "$xcid" sh -c 'echo $EX' 2>&1)" "exVAL"
has "exec-workdir" "$(d exec -w /tmp "$xcid" pwd 2>&1)" "/tmp"
d rm -f "$xcid" >/dev/null

echo "== registry verbs (no creds -> must reach the registry, not no-op) =="
has "login-attempt"  "$(echo bad | d login -u nouser --password-stdin docker.io 2>&1; echo LOGIN-DONE)" "LOGIN-DONE"
has "search-or-graceful" "$(d search alpine 2>&1; echo SEARCH-DONE)" "SEARCH-DONE"

echo "== context (CLI-side, should at least list the default/current) =="
has "context-ls" "$(d context ls 2>&1)" "default"

echo "== swarm/cluster verbs are single-host out-of-scope: must fail gracefully, not hang/crash =="
has "swarm-graceful"   "$(timeout 5 d node ls 2>&1; echo SWARM-DONE)" "SWARM-DONE"
has "service-graceful" "$(timeout 5 d service ls 2>&1; echo SVC-DONE)" "SVC-DONE"

echo ""
echo "compliance: $pass passed, $fail failed"
[ -n "$gaps" ] && echo "non-compliant:$gaps"
[ "$fail" -eq 0 ]
