# Docker CLI compatibility

`dd-daemon` speaks the Docker Engine API, so the ordinary `docker` CLI drives it unchanged
(`export DOCKER_HOST=unix://…/dd.sock`). The end-to-end behaviour is meant to be indistinguishable from
real docker for the day-to-day workflow. `make test-docker` runs `dd-tests/scenarios/docker.sh`, which
drives the *real* `docker` binary against the daemon and asserts every step — **38/38 green**.

## Works (verified, `make test-docker`)

| area | commands |
|---|---|
| **images** | `images`, `image inspect`, `pull` (local), `tag`, `rmi`, `push` (local no-op) |
| **run** | `run` (foreground, streams stdout/stderr), `run -d`, `run -i`, `run -it`, `run --name`, `--entrypoint` |
| **exec** | `exec`, `exec -i`, `exec -it` (runs in the container's rootfs) |
| **inspect/logs** | `inspect` (container + image), `logs`, `wait`, `ps`, `ps -a`, `top`, `stats` |
| **lifecycle** | `stop`, `kill`, `restart`, `pause`, `unpause`, `rename`, `rm` |
| **volumes** | `volume create/ls/inspect/rm`, `-v` bind mounts |
| **networks** | `network create/ls/inspect/rm/connect/disconnect`, default bridge/host/none |

How: containers run as **live background JIT processes** the daemon tracks; foreground `run`/`-it`/`exec`
stream over a hijacked connection (the docker multiplexed-frame protocol, raw in TTY mode); `wait`
streams its response so the CLI's create→attach→wait→start sequence doesn't deadlock.

## Not yet (the honest gaps)

- **registry** — `pull`/`push` are local-only (images come from `DD_IMAGES`); no registry transfer yet.
  This is the next big piece: OCI registry pull/unpack in `images_create`, then `push`.
- **`docker build`** — needs a BuildKit-compatible builder.
- **`docker cp`** — the `/archive` tar endpoints aren't implemented.
- **freezer pause** — `pause`/`unpause` are accepted but no-ops (dd has no freezer cgroup).

The core interactive workflow — pull an image, `run -it` a shell, `exec` into it, check `logs`, `stop`/
`rm` — is fully working.
