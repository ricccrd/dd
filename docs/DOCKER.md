# Docker CLI compatibility

`dd-daemon` speaks the Docker Engine API, so the ordinary `docker` CLI drives it unchanged
(`export DOCKER_HOST=unix://…/dd.sock`). The end-to-end behaviour is meant to be indistinguishable from
real docker for the day-to-day workflow. `make test-docker` runs `dd-tests/scenarios/docker.sh`, which
drives the *real* `docker` binary against the daemon and asserts every step — **38/38 green**.

## Works (verified, `make test-docker`)

| area | commands |
|---|---|
| **images** | `images`, `image inspect`, `pull` (real, any registry), `tag`, `rmi`, `push` (real upload) |
| **run** | foreground (streams stdout/stderr), `-d`, `-i`, `-it`, `--name`, `--entrypoint`, `--platform`, `-w`, `-v`, `--network host` |
| **exec** | `exec`, `exec -i`, `exec -it` (runs in the container's rootfs) |
| **inspect/logs** | `inspect` (container + image), `logs`, `wait`, `ps`, `ps -a`, `top`, `stats` |
| **lifecycle** | `stop`, `kill`, `restart`, `pause`, `unpause`, `rename`, `rm` |
| **volumes** | `volume create/ls/inspect/rm`, `-v` bind mounts |
| **networks** | `network create/ls/inspect/rm/connect/disconnect`, default bridge/host/none |

How: containers run as **live background JIT processes** the daemon tracks; foreground `run`/`-it`/`exec`
stream over a hijacked connection (the docker multiplexed-frame protocol, raw in TTY mode); `wait`
streams its response so the CLI's create→attach→wait→start sequence doesn't deadlock.

### Registries (`dd-daemon/src/registry.rs`)

`pull`/`push` work against **any** OCI registry, not just Docker Hub — `ghcr.io`, `quay.io`, ECR, a
plain `localhost:5000`. The registry is parsed from the reference the way docker does it (a leading
segment is a host iff it has a `.`/`:` or is `localhost`); auth is the standard `WWW-Authenticate:
Bearer` challenge flow, with credentials taken from the CLI's `X-Registry-Auth` (i.e. `docker login`).

- **pull** — token → manifest (picks `linux/arm64`, falls back to `amd64`) → layer blobs streamed into a
  fresh rootfs (whiteouts applied). Verified end-to-end against Docker Hub *and* quay.io.
- **push** — tar the rootfs into one layer → blob uploads → manifest `PUT`. A successful push needs
  `docker login` + a writable repo, exactly like real docker.

The HTTP/tar/sha256 work is shelled to `curl`/`tar`/`gzip`/`sha256sum` (the offline build can't pull in
an async-HTTP+TLS+tar crate stack); it's confined to a small `http` submodule, typed code above it.

## Not yet (the honest gaps)

- **`docker build`** — needs a BuildKit-compatible builder.
- **`docker cp`** — the `/archive` tar endpoints aren't implemented.
- **freezer pause** — `pause`/`unpause` are accepted but no-ops (dd has no freezer cgroup).

The core workflow — `pull` from any registry, `run -it` a shell, `exec` into it, `logs`, `push` — works.
