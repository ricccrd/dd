#!/usr/bin/env bash
# dev.sh -- build dd and set it up so you can open a shell and just use `ddcli`.
#
#   bash tools/dev.sh
#
# Builds dd-daemon + ddcli, puts `ddcli` on your PATH (~/.local/bin, added to your shell rc), and runs
# the daemon in the background. Then open a NEW terminal window and run `ddcli ubuntu`.
#
# Env: DD_IMAGES (image dir, default ~/.dd/images -- pulls land here on demand).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

SOCK="$HOME/.dd/run/docker.sock"
IMAGES="${DD_IMAGES:-$HOME/.dd/images}"
LOG="$HOME/.dd/daemon.log"
BIN="$HOME/.local/bin"
mkdir -p "$HOME/.dd/run" "$IMAGES" "$BIN"

echo "==> building dd-daemon + ddcli (release) ..."
cargo build --release -p dd-daemon -p dd-cli

echo "==> building the macOS-container userland (for 'ddcli mac') ..."
if DD_IMAGES="$IMAGES" bash "$ROOT/tools/mac-userland.sh" >/dev/null 2>&1; then
  echo "    macos image ready in $IMAGES/macos"
else
  echo "    (skipped -- needs a nix arm64 toolchain; 'ddcli mac' will be unavailable)"
fi

echo "==> putting ddcli on your PATH ($BIN/ddcli)"
ln -sf "$ROOT/target/release/ddcli" "$BIN/ddcli"
# Ensure ~/.local/bin is on PATH for future shells. zsh is the macOS default; cover bash too.
PATHLINE='export PATH="$HOME/.local/bin:$PATH"'
added=""
for rc in "$HOME/.zshrc" "$HOME/.bashrc"; do
  [ -e "$rc" ] || continue
  if ! grep -qF "$PATHLINE" "$rc"; then
    printf '\n# dd: ddcli lives here\n%s\n' "$PATHLINE" >> "$rc"
    added="$added $rc"
  fi
done
# Fresh macOS with no rc yet: create ~/.zshrc.
if [ ! -e "$HOME/.zshrc" ] && [ ! -e "$HOME/.bashrc" ]; then
  printf '# dd: ddcli lives here\n%s\n' "$PATHLINE" > "$HOME/.zshrc"
  added=" $HOME/.zshrc"
fi
[ -n "$added" ] && echo "    added ~/.local/bin to PATH in:$added"

echo "==> (re)starting the daemon in the background (log: $LOG)"
pkill -x dd-daemon 2>/dev/null || true
rm -f "$SOCK"
DDOCKERD_SOCK="$SOCK" DD_IMAGES="$IMAGES" nohup "$ROOT/target/release/dd-daemon" >"$LOG" 2>&1 &
disown 2>/dev/null || true
for _ in $(seq 1 40); do [ -S "$SOCK" ] && break; sleep 0.25; done
[ -S "$SOCK" ] || { echo "daemon failed to start; see $LOG"; tail -20 "$LOG"; exit 1; }

cat <<EOF

────────────────────────────────────────────────────────────────────
  ✓ ddcli is on your PATH and the daemon is running.

  Open a NEW terminal window and just use it:

      ddcli ubuntu             # a shell in ubuntu, here in your current dir
      ddcli run alpine echo hi
      ddcli run alpine uname -m

  (Already-open shells won't see the PATH change — open a fresh window,
   or run:  export PATH="\$HOME/.local/bin:\$PATH")

  Daemon log:  tail -f $LOG       Stop it:  pkill -x dd-daemon
────────────────────────────────────────────────────────────────────
EOF
