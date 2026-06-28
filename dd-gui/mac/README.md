# macOS dev containers (`ddcli mac`)

`ddcli mac` drops you into a **native macOS (arm64) dev container** — real Mach-O binaries running
under the `darwinjail` (DYLD-interpose) jail, no VM, no DBT. The current directory is mounted, host
networking is on, and the container has its own jailed filesystem (rootfs upper + the host `/` as a
read-only lower for system frameworks).

```
ddcli mac                 # pull (first run) + drop into a dev shell here
ddcli mac uname -a        # run a one-off command
DD_MAC_IMAGE=ddmac-base ddcli mac    # use a different/local image
```

## The image

The userland is built from **nixpkgs** (native aarch64-darwin), packed into the image so it's
self-contained. Two tags are published to `huttarichard/ddmac`:

| Tag | Contents |
|-----|----------|
| `base` | lean: bash, coreutils, grep/sed/awk, find, tar/gzip, less, ncurses, CA certs |
| `dev` (= `latest`) | base **+** zsh, fish, git, curl, wget, openssh, htop, tree, jq, ripgrep, fd, fzf, tmux, neovim, gnupg, **make/cmake/pkg-config/clang**, **python3, node, go, rust** |

`ddcli mac` pulls `huttarichard/ddmac:latest` by default (override with `DD_MAC_IMAGE`). The package
set lives in `nix/flake.nix` (`mac-base` / `mac-dev`) — edit there to add tools.

## Build & publish (on a Mac with nix)

```bash
# build both images into ~/.dd/images and re-register them with the daemon
make mac-image

# publish to Docker Hub (huttarichard/ddmac:{base,dev,latest})
export DDMAC_TOKEN=<your docker hub PAT>     # do NOT commit this; rotate after use
make mac-push
```

`make mac-push` does `docker login -u huttarichard --password-stdin` (token from `$DDMAC_TOKEN`),
then tags and pushes. Override the repo with `DDMAC_REPO=…`, or the daemon socket with `DD_DOCKER=…`.

> **Security:** the Docker Hub PAT is a secret. It is only ever read from `$DDMAC_TOKEN` at push time
> and never written to the repo. If a token has been shared in plaintext anywhere, rotate it.

## How a pulled image is recognized

A pushed image loses the `dd-image.json` sidecar, so on pull the daemon:
- detects **`os:darwin`** from the packed Mach-O (probes `profile/bin/bash`, `opt/homebrew/bin/brew`, …),
- defaults the command to a bare **`bash`** (resolved via the in-jail PATH `/profile/bin`),
- forwards the image **env** (TLS cert bundle, `LANG`, `HOME`, …) to the jailed process.

## Known limitation — store portability

The packed binaries reference their dylibs by absolute **`/nix/store/…`** paths, which **dyld resolves
against the host store before the jail engages**. So the target machine needs those store paths present:

- **Same machine / any nix host** → works (the closure is already in `/nix`, or `nix copy` it).
- **A Mac without nix** → not yet supported; the binaries can't find their dylibs.

Making this fully portable (e.g. the daemon materializing the packed `rootfs/nix/store` into the host
store on pull, or a `DYLD_ROOT_PATH`/rpath rewrite) is the main open follow-up for `ddcli mac`.
