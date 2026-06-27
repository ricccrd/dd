#!/usr/bin/env bash
# dev.sh -- one command to build dd and run it locally for testing.
#
#   make dev          (or:  ./tools/dev.sh)
#
# Builds dd-daemon + ddcli, links `ddcli` onto your PATH (~/.local/bin), and runs the daemon in the
# foreground (logs print here; Ctrl-C stops it). Then open a NEW iTerm window and run `ddcli ubuntu`.
#
# Env: DD_IMAGES (image dir, default ~/.dd/images -- pulls land here on demand).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SOCK="$HOME/.dd/run/docker.sock"
IMAGES="${DD_IMAGES:-$HOME/.dd/images}"
BIN="$HOME/.local/bin"
mkdir -p "$HOME/.dd/run" "$IMAGES" "$BIN"

echo "==> building dd-daemon + the CLI (release) ..."
cargo build --release -p dd-daemon -p dd-cli

# Link whichever name the CLI built as (the bin is `ddcli` or `dd`) onto PATH.
CLI=""
for n in ddcli dd; do [ -x "$ROOT/target/release/$n" ] && CLI="$n" && break; done
[ -n "$CLI" ] || { echo "CLI binary not found in target/release (looked for ddcli, dd)"; exit 1; }
ln -sf "$ROOT/target/release/$CLI" "$BIN/$CLI"

pkill -x dd-daemon 2>/dev/null || true
rm -f "$SOCK"

on_path=no
case ":$PATH:" in *":$BIN:"*) on_path=yes ;; esac

printf '\n────────────────────────────────────────────────────────────────────\n'
printf '  dd is built. The daemon runs below — Ctrl-C to stop it.\n\n'
printf '  Open a NEW iTerm window and try:\n\n'
printf '      %s ubuntu              # a shell in ubuntu, here in your dir\n' "$CLI"
printf '      %s run alpine echo hi\n' "$CLI"
printf '      %s run alpine uname -m\n' "$CLI"
if [ "$on_path" = no ]; then
  printf '\n  ⚠ ~/.local/bin is not on your PATH. In the new window first run:\n'
  printf '      export PATH="$HOME/.local/bin:$PATH"\n'
  printf '    (or call it by path:  %s/target/release/%s ubuntu)\n' "$ROOT" "$CLI"
fi
printf '────────────────────────────────────────────────────────────────────\n\n'

# Run the daemon in the foreground on the canonical socket ddcli talks to.
exec env DDOCKERD_SOCK="$SOCK" DD_IMAGES="$IMAGES" "$ROOT/target/release/dd-daemon"
