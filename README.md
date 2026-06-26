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

A Cargo workspace with three members:

- **`dd-jit/`** — the JIT runtime (C, under `src/runtime/`) **plus its Rust bindings**. `build.rs`
  compiles and codesigns **one JIT binary per guest architecture** (`aarch64`, `x86_64`); `src/lib.rs`
  exposes `Guest` + the typed `SpawnConfig` launch contract + the built binaries' paths. The aarch64
  guest is fully decomposed (`jit/` engine + `os/linux/` personality + `frontend/aarch64/`); the
  x86-64 guest (jit86) is brought in whole pending the dedup onto the shared layer. See
  [`dd-jit/README.md`](dd-jit/README.md).
- **`dd-daemon/`** — the Docker Engine API daemon. Depends on `dd-jit`; detects each image's guest
  architecture from its ELF, picks the matching JIT, and launches it via `SpawnConfig`.
- **`dd-tests/`** — a declarative test harness. Cases are grouped (`compat`, `system`, `container`,
  `x86`) and run across **every engine**; a runner prints a grouped report. See [Testing](#testing).

## Testing

```sh
make test                       # the engine × case matrix, grouped report
make test ENGINE=x86_64         # one engine
make test FILTER=container      # one group / cases matching a name
cargo run -p dd-tests -- --list # list groups + cases
make test-ci                    # the cargo-test path (CI)
```

Cases are declared in `dd-tests/src/cases/`. A case is a guest program + assertions:

```rust
src("hello", "hello.c").exit(42).out("hi\n"),          // compile guests/hello.c, run on aarch64
src("math",  "math.c").oracle(),                        // diff stdout+exit vs a native run
in_rootfs("id-root", "alpine", &["/bin/sh","-c","id -u"]).out("0\n"),   // container behaviour
fixture("glibc", &[(Engine::X86_64, "guests/x86/g_x64")]).has("glibc ok"), // prebuilt x86 binary
```

aarch64 guests are compiled on the fly (`gcc -static-pie`) and can be diffed against a native oracle;
x86-64 guests come from prebuilt fixtures (no local cross-compiler). Each case runs on every engine it
has a guest for; the rest are reported as skipped.

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
