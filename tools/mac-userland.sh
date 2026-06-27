#!/usr/bin/env bash
# Build a minimal arm64 macOS userland for `ddcli mac` containers, as a dd image directory.
#
#   tools/mac-userland.sh [IMAGE_DIR]      (default: $DD_IMAGES/macos, else ./target/images/macos)
#
# Produces  <dir>/rootfs            a writable container root (/etc, /tmp, /root, /profile/bin)
#           <dir>/rootfs/profile/bin  symlinks to nix arm64 tools (resolved at runtime via the / lower)
#           <dir>/dd-image.json     {name, os:darwin, cmd:[<host bash>]} so the daemon registers it
#
# The binaries stay in /nix (the container sees them through the read-only `/` lower) -- nothing is
# copied, so this is cheap. Run on macOS (where /nix lives). Re-run to refresh.
set -eu
DIR="${1:-${DD_IMAGES:-$PWD/target/images}/macos}"
ROOT="$DIR/rootfs"; BIN="$ROOT/profile/bin"
mkdir -p "$ROOT/etc" "$ROOT/tmp" "$ROOT/root" "$BIN"

# Pick the newest store path providing a tool as a *native macOS* (Mach-O arm64) binary. The nix store
# can also hold Linux (ELF) builds (cross-compilation), which must be excluded -- they can't exec here.
pick() {
  local t="$1" cand
  for cand in $(ls /nix/store/*/bin/"$t" 2>/dev/null | sort -r); do
    [ -x "$cand" ] || continue
    case "$(file -bL "$cand" 2>/dev/null)" in *Mach-O*arm64*) printf '%s' "$cand"; return;; esac
  done
}

TOOLS="bash sh ls cat cp mv rm mkdir rmdir ln chmod chown pwd echo printf env printenv test true false \
  grep sed awk find xargs which head tail sort uniq wc cut tr date sleep id whoami uname hostname \
  dirname basename realpath readlink stat touch df du less tar gzip curl git make vim nano ps kill"
n=0
for t in $TOOLS; do
  p="$(pick "$t")"; [ -n "$p" ] && [ -e "$p" ] && { ln -sf "$p" "$BIN/$t"; n=$((n+1)); }
done
BASH="$(pick bash)"
: "${BASH:?no nix bash found in /nix/store -- install one (nix profile install nixpkgs#bashInteractive)}"

printf 'root:*:0:0:root:/root:/profile/bin/bash\n' > "$ROOT/etc/passwd"
printf 'maccontainer\n' > "$ROOT/etc/hostname"
# cmd is bare `bash`, resolved via the in-jail PATH (/profile/bin) and exec'd by the daemon's wrapper.
printf '{"name":"macos","os":"darwin","cmd":["bash"]}\n' > "$DIR/dd-image.json"

echo "built mac userland -> $DIR"
echo "  bash:  $BASH"
echo "  tools: $n in $BIN"
