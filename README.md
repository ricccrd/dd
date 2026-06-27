<p align="center">
  <img src="assets/logo.png" alt="dd" width="128" height="128" />
</p>

<h1 align="center">dd</h1>

<p align="center">
  <strong>Run Linux containers on macOS — with no VM.</strong>
</p>

<p align="center">
  <a href="https://github.com/ricccrd/dd/releases/latest"><img alt="Download" src="https://img.shields.io/badge/download-.dmg-2f6fe6?style=flat-square" /></a>
  <img alt="Platform" src="https://img.shields.io/badge/macOS-Apple%20Silicon-4f88ff?style=flat-square" />
  <img alt="License" src="https://img.shields.io/badge/license-MIT-81b5ff?style=flat-square" />
  <a href="https://ricccrd.github.io/dd/"><img alt="Website" src="https://img.shields.io/badge/website-docs-97cdff?style=flat-square" /></a>
</p>

---

## What is dd?

`dd` runs **Linux containers natively on Apple-Silicon macOS without a virtual machine**. There is no
Linux kernel and no hypervisor underneath: a **JIT** translates the container's code and services its
Linux **syscalls in userspace** (the gVisor / PRoot lineage). The JIT *is* the guest's Linux kernel —
namespaces, cgroups, overlay image layers and networking are maintained as userspace state. It speaks
the **Docker Engine API**, so the ordinary `docker` CLI drives it.

The container's *compute* runs as native Apple-Silicon instructions; only its *syscalls* are
interpreted. No VM to boot, no daemon-in-a-VM, no virtualization cost.

> 🌐 **Website & docs:** https://ricccrd.github.io/dd/

```sh
make jit                                          # build.rs compiles + codesigns the JITs
DD_IMAGES=/path/to/images cargo run -p dd-daemon  # start the daemon
export DOCKER_HOST=unix://$PWD/dd.sock
docker run -p 8080:80 -m 256m alpine sh -c 'echo hi from $(hostname)'
```

## Features

- **🚫 No virtual machine.** No hypervisor, no Linux kernel, no VM to keep resident. The guest's
  instructions run natively on arm64; only the syscall boundary is trapped and serviced in userspace.
- **🐳 Drop-in Docker.** dd implements the Docker Engine API. Point `DOCKER_HOST` at its socket and your
  existing `docker run / ps / images / build` commands work unchanged.
- **🧠 The JIT is the kernel.** Namespaces, cgroups, overlay image layers and networking are ordinary
  userspace state — a userspace kernel in the gVisor / PRoot lineage, with none of a VM's cost.
- **🏗️ aarch64 *and* x86-64 guests.** Run native arm64 Linux images, or x86-64 images via a JIT
  (`jit86`) that decodes x86, synthesizes its flags, and lowers SSE/x87 onto NEON. glibc binaries run.
- **📦 Real container isolation.** Overlay image layers (copy-up / `.wh.` whiteout, merged `getdents`),
  a TOCTOU-free path-jail VFS, PID / UTS / USER namespaces, a private loopback netns with `-p` port
  publishing, and cgroup memory + pids limits (OOM at the limit).
- **🖥️ Desktop app, no root.** A native GTK4 app (**dd-app**) plus a `dd` CLI install a per-user
  background daemon and a `docker context` — everything under `$HOME`, never `sudo`.

## Why a JIT, not a VM?

Every other way to run Linux containers on a Mac — Docker Desktop, Colima, Rancher, OrbStack — boots a
Linux **VM** under a hypervisor and runs the daemon *inside* it. That VM is a tax you pay all day. dd
deletes it: a container is a plain macOS process whose syscalls happen to be serviced by a userspace
Linux kernel.

|                              | **dd** — userspace kernel (JIT)                              | VM-based Docker (Desktop / Colima / …)            |
| ---------------------------- | ----------------------------------------------------------- | ------------------------------------------------- |
| Underlying model             | A JIT services Linux syscalls in userspace (gVisor lineage) | A full Linux kernel inside a hypervisor VM        |
| Resident RAM when idle       | **None** — per-container, freed on exit                     | Gigabytes reserved for the VM, always on          |
| Startup                      | **Process spawn** — no VM to boot                           | Boot a Linux VM + the in-VM daemon first          |
| Bind-mount / file I/O        | **Direct host filesystem** through a path jail              | `virtiofs`/gRPC-FUSE bridge across the VM boundary |
| Port publishing              | Straight to host sockets                                    | Through the VM's NAT/forwarding layer             |
| Battery / background cost    | **Nothing** running when no container is                    | A VM idling and draining battery                  |
| Footprint to ship &amp; patch | No Linux kernel — nothing to CVE-track                      | Ships, patches and tracks a whole Linux kernel    |
| Observability                | **A normal macOS process** — sample, debug, Activity Monitor | An opaque VM; the workload is invisible to host tools |

The win is structural: the guest's *compute* runs as **native Apple-Silicon instructions** (no
hardware-virtualization layer in the hot path), and the notorious Docker-Desktop file-sharing
bottleneck — the `virtiofs`/FUSE bridge between macOS and the VM — simply doesn't exist, because dd's
VFS *is* the host filesystem behind a path jail.

> **Honest trade-off:** a userspace kernel is only as complete as the syscalls it implements, and today
> dd runs as a single process — great for *trusted* images. A VM still gives a harder isolation boundary
> for untrusted code; dd's sentry split ([`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md)) is the roadmap
> answer there.

## How it works

dd runs a Linux container by **being its kernel in userspace**. A JIT translates the guest's machine
code and traps every syscall instruction; the trap handler — `service()` in `dd-jit/src/runtime/os/linux/`
— *is* the Linux syscall ABI, implemented against the macOS host.

1. **Load** the guest ELF (static-PIE, or dynamic via its `ld.so`) and build the initial stack.
2. **Translate & dispatch** the guest PC block-by-block; same-ISA code is mostly transliterated, x86-64
   is decoded and re-emitted on arm64.
3. **Run** the translated block as native host code until a terminator (branch / indirect jump / syscall).
4. **Service** the syscall — every path passes through the container VFS jail; namespaces and cgroups
   are just process state.

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full picture.

## Examples

```sh
# 1. Start the daemon, point docker at it
make jit
DD_IMAGES=/path/to/images cargo run -p dd-daemon
export DOCKER_HOST=unix://$PWD/dd.sock

# 2. It's just Docker
docker run -p 8080:80 -m 256m alpine sh -c 'echo hi from $(hostname)'
docker ps
docker images
docker run --rm -it ubuntu bash

# 3. Or via the installed desktop app (per-user, no root)
dd install                                  # LaunchAgent + docker context
dd app                                       # open the GUI
docker --context dd run alpine echo hi
```

## Install

dd targets **Apple-Silicon macOS** (arm64, macOS 12+). The JIT needs the Xcode Command Line Tools
(`clang` + `codesign`).

### Download the app (recommended)

Grab the latest `.dmg` from the [**releases page**](https://github.com/ricccrd/dd/releases/latest),
open it, and drag **dd** to Applications. Then in a terminal:

```sh
dd install     # ~/.dd tree + per-user LaunchAgent + `docker context create dd`
dd app         # open the GUI
dd doctor      # check socket / agent / context / app quarantine
```

> **Gatekeeper:** the DMG is unsigned (ad-hoc). On first launch, right-click the app → **Open**, or run
> `xattr -dr com.apple.quarantine /Applications/dd-app.app` (`dd doctor` detects this and prints the fix).

### Build from source

```sh
xcode-select --install                       # clang + codesign
# install Rust (stable) and Nix (for the GTK4 dev shell)
git clone https://github.com/ricccrd/dd && cd dd
make app       # build + assemble & ad-hoc-sign target/dd-app.app
make dmg       # -> target/dist/dd-<ver>-<arch>.dmg
make install   # copy to /Applications and run `dd install`
```

`make app`/`dmg` run the bundling inside the Nix dev shell ([`nix/flake.nix`](nix/flake.nix)), which
provides GTK4 + `dylibbundler` / `create-dmg`. The bundle relocates the GTK dylib graph into
`Contents/Frameworks`, stages the GTK runtime data, and ad-hoc-signs inner→outer.

## Workspace

A Cargo workspace.

- **[`dd-jit/`](dd-jit/)** — the JIT runtime (C, under `src/runtime/`) **plus its Rust bindings**.
  `build.rs` compiles and codesigns **one JIT binary per guest architecture** (`aarch64`, `x86_64`);
  `src/lib.rs` exposes `Guest` + the typed `SpawnConfig` launch contract. The aarch64 guest is fully
  decomposed (`jit/` engine + `os/linux/` personality + `frontend/aarch64/`); the x86-64 guest (jit86)
  shares the `os/linux/` layer.
- **`dd-daemon/`** — the Docker Engine API daemon. Detects each image's guest architecture from its ELF,
  picks the matching JIT, and launches it via `SpawnConfig`.
- **`dd-tests/`** — a declarative test harness; cases run across **every engine** with a grouped report.
- **`dd-client/`** — a small typed Docker-Engine-API client over the daemon's Unix socket (the single
  source of truth for the wire format, shared by the GUI and CLI).
- **`dd-gui/`** (binary **`dd-app`**) — a GTK4 desktop UI. Built only on macOS via the Nix dev shell.
- **`dd-cli/`** (binary **`dd`**) — the install/control surface, all without root.

The daemon listens on `~/.dd/run/docker.sock`; both the GUI and `docker --context dd` use it. State
persists to `~/.dd/state.json`.

## Testing

```sh
make test                       # the engine × case matrix, grouped report
make test ENGINE=x86_64         # one engine
make test FILTER=container      # one group / cases matching a name
cargo run -p dd-tests -- --list # list groups + cases
make test-ci                    # the cargo-test path (CI)
```

Cases are declared in `dd-tests/src/cases/`. A case is a guest program + assertions; aarch64 guests are
compiled on the fly (`gcc -static-pie`) and diffed against a native oracle, x86-64 guests come from
prebuilt fixtures. Each case runs on every engine it has a guest for.

## Status

- **Guest:** Linux **aarch64** (decomposed, full container engine) + **x86-64** (jit86, runs glibc).
- **Host:** macOS **arm64** (Apple Silicon). The JIT needs `clang` + `codesign` (Xcode CLT).
- **Containers:** rootfs + overlay image layers (copy-up/whiteout), bind volumes, port publishing
  (`-p`), private-loopback netns, cgroup memory+pids limits, UTS/PID/USER namespaces.
- **Roadmap** ([`docs/PLAN.md`](docs/PLAN.md)): OCI registry pull/unpack, the jit86 dedup onto the
  shared engine, a full external netstack, and the sentry split for untrusted images.

## Documentation

- [Architecture](docs/ARCHITECTURE.md) — the one idea, the layers, the execution flow.
- [Code structure](docs/STRUCTURE.md) — what lives where, and why.
- [Syscalls](docs/SYSCALLS.md) — the Linux ABI surface implemented against macOS.
- [Optimizations](docs/OPTIMIZATIONS.md) — the JIT's idiom rewrites and fast paths.
- [GUI rendering](docs/RENDERING.md) — rendering Linux GUI apps to macOS (design).
- [Roadmap](docs/PLAN.md) — the work-list.

## Author

**Richard Huttar** — [huttarichard@gmail.com](mailto:huttarichard@gmail.com)

## License

[MIT](LICENSE).
