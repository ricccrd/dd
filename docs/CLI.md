# `ddcli` — the command line

`ddcli` runs containers with **easy-access defaults** and manages the dd daemon — all without root. It
ships inside the macOS app (`/Applications/dd-app.app`) and installs itself onto your `PATH`
(`~/.local/bin/ddcli`) when you run `ddcli install` (or click *Install ddcli* in the app).

## Run containers

```sh
ddcli ubuntu                 # drop into a shell in ubuntu, here in this directory
ddcli alpine                 # …or alpine, debian, fedora, your-image:tag, ghcr.io/owner/app …
ddcli run alpine echo hi     # run a one-off command instead of a shell
ddcli run ubuntu -- ls -la   # arguments after the image are the container command
```

`ddcli <image>` is shorthand for `ddcli run <image>`. With no command, you land in the image's shell.

### Easy access, by default

Every `ddcli run` (and the `ddcli <image>` shortcut) starts the container with:

- **the current directory mounted** at the same path inside the container, and set as the working
  directory — so the container starts *where you are*, with your files right there;
- **host networking** — services bind the host's ports directly, no `-p` juggling;
- **an interactive shell + TTY** when you have a terminal (falls back cleanly when piped);
- **`--rm`** — the container is removed when you exit.

Opt out with `--isolated` (no auto-mount, no host networking) or `--keep` (don't remove on exit).

### Architecture & `--platform`

The image's architecture is detected automatically; native **arm64** images run directly, **amd64**
images run through the x86-64 JIT. Force one with `--platform`:

```sh
ddcli run ubuntu                          # native arm64
ddcli run ubuntu --platform linux/amd64   # amd64, via the x86-64 JIT
```

### macOS containers

```sh
ddcli mac                    # a shell in a macOS (darwin) container — experimental
```

`ddcli mac` runs the host macOS filesystem in a darwin jail via the darwin JIT.

## Manage the daemon

```sh
ddcli install                # set up the per-user daemon + docker context (no sudo)
ddcli doctor                 # health check (socket, daemon, context, app) + fixes
ddcli daemon status          # start | stop | restart | status | logs
ddcli context show           # the docker context endpoint
ddcli app                    # open the desktop app
ddcli uninstall [--purge]    # remove the agent + context (--purge also deletes ~/.dd)
```

## …or plain Docker

`ddcli install` registers a `dd` docker context, so the stock `docker` CLI drives the same daemon:

```sh
docker --context dd run -p 8080:80 alpine httpd
docker --context dd pull ubuntu        # real registry pull (any registry)
docker --context dd ps
```

`ddcli` wraps the Docker Engine API the daemon implements, so the ordinary `docker` CLI works too
(`docker --context dd …`).
