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

echo "==> building dd-daemon + ddcli (release) ..."
cargo build --release -p dd-daemon -p dd-cli

ln -sf "$ROOT/target/release/ddcli" "$BIN/ddcli"
pkill -x dd-daemon 2>/dev/null || true
rm -f "$SOCK"

on_path=no
case ":$PATH:" in *":$BIN:"*) on_path=yes ;; esac

printf '\n────────────────────────────────────────────────────────────────────\n'
printf '  dd is built. The daemon runs below — Ctrl-C to stop it.\n\n'
printf '  Open a NEW iTerm window and try:\n\n'
printf '      ddcli ubuntu              # a shell in ubuntu, here in your dir\n'
printf '      ddcli run alpine echo hi\n'
printf '      ddcli run alpine uname -m\n'
if [ "$on_path" = no ]; then
  printf '\n  ⚠ ~/.local/bin is not on your PATH. In the new window first run:\n'
  printf '      export PATH="$HOME/.local/bin:$PATH"\n'
  printf '    (or call it by path:  %s/target/release/ddcli ubuntu)\n' "$ROOT"
fi
printf '────────────────────────────────────────────────────────────────────\n\n'

# Run the daemon in the foreground on the canonical socket ddcli talks to.
exec env DDOCKERD_SOCK="$SOCK" DD_IMAGES="$IMAGES" "$ROOT/target/release/dd-daemon"
