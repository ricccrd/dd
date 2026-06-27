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
    # Skip aggregate profiles (system-path / buildEnv / *-env) -- their closure is the WHOLE profile;
    # we want the individual package (bash-5.x, coreutils-9.x) so the packed closure stays small.
    case "$cand" in *system-path*|*-env/*|*user-environment*|*profile*) continue;; esac
    [ -x "$cand" ] || continue
    case "$(file -bL "$cand" 2>/dev/null)" in *Mach-O*arm64*) printf '%s' "$cand"; return;; esac
  done
}

# A lean base userland (small closure when packed). Add heavier tools (git/curl/…) for a `:full` variant.
TOOLS="bash sh ls cat cp mv rm mkdir rmdir ln chmod chown pwd echo printf env printenv test true false \
  grep sed awk find xargs which head tail sort uniq wc cut tr date sleep id whoami uname \
  dirname basename realpath readlink stat touch df du less tar gzip"
n=0
for t in $TOOLS; do
  p="$(pick "$t")"; [ -n "$p" ] && [ -e "$p" ] && { ln -sf "$p" "$BIN/$t"; n=$((n+1)); }
done
BASH="$(pick bash)"
: "${BASH:?no nix bash found in /nix/store -- install one (nix profile install nixpkgs#bashInteractive)}"

# DD_PACK=1: copy the nix closure of the tools INTO the rootfs so the image is self-contained and
# portable (pushable to a registry). Without it the binaries stay in the host /nix (cheap, local-only).
if [ "${DD_PACK:-0}" = 1 ]; then
  echo "packing nix closure into the rootfs (portable image) ..."
  mkdir -p "$ROOT/nix/store"
  targets="$(for s in "$BIN"/*; do readlink "$s"; done | sort -u)"
  # shellcheck disable=SC2086
  for p in $(nix-store -qR $targets 2>/dev/null | sort -u); do
    d="$ROOT/nix/store/$(basename "$p")"
    [ -e "$d" ] || cp -R "$p" "$ROOT/nix/store/" 2>/dev/null
  done
  echo "  packed: $(du -sh "$ROOT/nix/store" 2>/dev/null | cut -f1)"
fi

printf 'root:*:0:0:root:/root:/profile/bin/bash\n' > "$ROOT/etc/passwd"
printf 'maccontainer\n' > "$ROOT/etc/hostname"
# cmd is bare `bash`, resolved via the in-jail PATH (/profile/bin) and exec'd by the daemon's wrapper.
printf '{"name":"macos","os":"darwin","cmd":["bash"]}\n' > "$DIR/dd-image.json"

echo "built mac userland -> $DIR"
echo "  bash:  $BASH"
echo "  tools: $n in $BIN"
[ "${DD_PACK:-0}" = 1 ] && echo "  portable (closure packed) -- ready to: docker tag macos <you>/macos && docker push <you>/macos"
