# dd — run Linux containers on macOS, with no VM

`dd` runs **Linux containers natively on Apple-Silicon macOS without a virtual machine**. There is no
Linux kernel and no hypervisor underneath: a **JIT** translates the container's code and services its
Linux **syscalls in userspace** (the gVisor/PRoot lineage). The JIT *is* the guest's Linux kernel —
namespaces, cgroups, overlay image layers and networking are maintained as userspace state. It speaks
the **Docker Engine API**, so the ordinary `docker` CLI drives it.

```sh
make jit                                          # build.rs compiles + codesigns the JITs
DD_IMAGES=/path/to/images cargo run -p dd-daemon  # start the daemon
export DOCKER_HOST=unix://$PWD/dd.sock
docker run -p 8080:80 -m 256m alpine sh -c 'echo hi from $(hostname)'
```

## Workspace

A Cargo workspace with two members:

- **`dd-jit/`** — the JIT runtime (C, under `src/runtime/`) **plus its Rust bindings**. `build.rs`
  compiles and codesigns **one JIT binary per guest architecture** (`aarch64`, `x86_64`); `src/lib.rs`
  exposes `Guest` + the typed `SpawnConfig` launch contract + the built binaries' paths. The aarch64
  guest is fully decomposed (`jit/` engine + `os/linux/` personality + `frontend/aarch64/`); the
  x86-64 guest (jit86) is brought in whole pending the dedup onto the shared layer. See
  [`dd-jit/README.md`](dd-jit/README.md).
- **`dd-daemon/`** — the Docker Engine API daemon. Depends on `dd-jit`; detects each image's guest
  architecture from its ELF, picks the matching JIT, and launches it via `SpawnConfig`.

## Status

- **Guest:** Linux **aarch64** (decomposed, full container engine) + **x86-64** (jit86, runs glibc).
- **Host:** macOS **arm64** (Apple Silicon). The JIT needs `clang` + `codesign` (Xcode CLT).
- **Containers:** rootfs + overlay image layers (copy-up/whiteout), bind volumes, port publishing
  (`-p`), private-loopback netns, cgroup memory+pids limits, UTS/PID/USER namespaces (root by default).
- **Roadmap** (`docs/PLAN.md`): OCI registry pull/unpack (today images are pre-extracted rootfs dirs),
  the jit86 dedup onto the shared engine, a full external netstack, and the sentry split for untrusted
  images.

## Build

`make jit` (= `cargo build --release`) builds everything, including both JIT binaries via `build.rs`.
On a non-macOS dev host the C build is routed through the `mac` bridge automatically.

## License

MIT.
