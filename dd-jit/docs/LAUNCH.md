# dd-jit LAUNCH — the one launch contract (entrypoint, bindings, env/flags)

How a container is started, and how to make that **identical** across all targets. This is
the public surface; `ARCHITECTURE.md` is the internals, `REFACTOR.md` the code-move plan.

## How launch works today (it is NOT an FFI call)

The Rust binding does **not** dlopen/call the engine. `SpawnConfig::script()` (src/lib.rs)
builds a shell line and spawns `bash -lc`:

```
exec env  <K=V …>   <engine-binary>  <--flags …>   <guest argv …>
          └ config   └ ddjit-<target>  └ config       └ what the container runs
```

So the launch contract = **(env vars) + (CLI flags) + (argv)**. Today these diverge three ways.

## The divergence today (the mess to remove)

Same concept, three different spellings — and a mix of flag-vs-env per target:

| concept | linux_aarch64 (jit) | linux_x86_64 (jit86) | darwin (jail) |
|---|---|---|---|
| rootfs | `--rootfs` flag | `--rootfs` flag | `DD_ROOTFS` env |
| volumes | `DDVOL` env | `--vol` / `JIT86_VOL` | `DD_VOLUMES` env |
| lowers | `--lower` / `DD_LOWER` | `--lower` / `JIT86_LOWER` | `DD_LOWERS` env |
| netns | `--netns` / `DD_NETNS` | `JIT86_NETNS` / `JIT86_NONETNS` | `DD_NET_ISOLATE` |
| publish | `--publish` / `DD_PUBLISH` | `--publish`/`-p` / `JIT86_PUBLISH` | `DD_PUBLISH` |
| hostname | `--hostname` / `DD_HOSTNAME` | **(missing)** | `DD_HOSTNAME` |
| mem/pids max | `--mem-max`/`--pids-max` / `DD_*` | **(missing)** | `DD_MEM_MAX`/`DD_PIDS_MAX` |
| uid/gid | `--uid`/`--gid` / `DD_UID`/`DD_GID` | `--uid`/`--gid` / `DD_*` | (none) |
| sandbox | `DDJIT_SANDBOX`/`DDJIT_UNTRUSTED` | `DDJIT_SANDBOX`/`DDJIT_UNTRUSTED` | `DD_SANDBOX` |
| guest env | `DD_GUEST_ENV` (clean block) | **uses static defaults only** | (host env) |
| entry symbol | `jit_run()` | `jit86_run()` | jitdarwin `main` |

Three naming schemes (`DD_*`, `JIT86_*`, bare flags), inconsistent plurals (`DD_LOWER` vs
`DD_LOWERS`, `DDVOL` vs `DD_VOLUMES`), x86 silently missing hostname/limits, and x86 not even
honoring container-supplied guest env (`DD_GUEST_ENV`). The binding has to branch per OS.

## The key insight: env is the ONLY universal channel

The darwin **jail** is a DYLD-interposed dylib loaded into the *native* guest process — there
is no translator argv to receive flags; the argv belongs to the guest program itself. So
**flags cannot reach all three targets; env vars can.** → standardize the container contract
on env. Flags become at most a thin human-CLI convenience that maps onto the same env.

## The unified contract

### One env namespace `DD_*` for the container (the stable, public contract)

| var | meaning | format |
|---|---|---|
| `DD_ROOTFS` | writable rootfs (overlay upper or plain) | path |
| `DD_LOWERS` | overlay read-only layers, highest first | `p1,p2,…` |
| `DD_VOLUMES` | bind mounts | `[ro:]guest:host,…` |
| `DD_PUBLISH` | port maps | `host:container,…` |
| `DD_NETNS` | private-loopback ns id (unset = shared host stack) | id |
| `DD_HOSTNAME` | UTS hostname | string |
| `DD_MEM_MAX` / `DD_PIDS_MAX` | cgroup limits (0 = unlimited) | int |
| `DD_UID` / `DD_GID` | container uid/gid (default 0) | int |
| `DD_CWD` | initial working dir inside container | path |
| `DD_SANDBOX` | untrusted-guest isolation on | 0/1 |
| `DD_GUEST_ENV` | the guest process's OWN environment | `K=V\nK=V\n…` |

Retire `JIT86_*`, `DDVOL`, `DD_LOWER` (singular), `DD_NET_ISOLATE`, `DDJIT_SANDBOX/UNTRUSTED`.
Every target reads the SAME names from the SAME shared parser (the linux container code is
already shared — point all targets at it).

### Two env classes — never mix them

| class | namespace | stability | examples |
|---|---|---|---|
| **container contract** | `DD_*` | public, stable | the table above |
| **engine tuning / debug** | `DDJIT_*` | internal, unstable, A/B kill-switches | `DDJIT_PCACHE`, `JT`→`DDJIT_TRACE`, `PROF`, `NO*`, `TIER2*` |

The clean rule: a front-end (daemon/CLI) sets only `DD_*`. The bare/legacy debug names
(`JT`, `PROF`, `NOMTIBTC`, `NOTIER2X`, …) are developer-only — fold them under `DDJIT_*` over
time so a reader can tell "container config" from "engine knob" by prefix alone.

### Engine-config vs guest env (the boundary that prevents leaks)

The container's config (`DD_*`) is consumed by the engine and must **not** appear in the
guest's environment. The guest's own env arrives as the single block `DD_GUEST_ENV` and is the
ONLY thing copied onto the guest stack. aarch64 already does this (os/linux/elf.c:521); **x86
does not** (frontend/x86_64/elf.c:270 uses static defaults) — unify x86 onto the aarch64
`DD_GUEST_ENV` path. One env-construction function, shared.

### One entrypoint symbol → one binding

Rename `jit_run`/`jit86_run` → **`dd_run(rootfs, argc, argv)`** in every target; darwin gets a
matching wrapper. With the env contract unified, `SpawnConfig::script()` collapses to ONE
template + a single per-target prefix:

```
linux  : exec env <DD_*> <ddjit-linux_{arch}> <guest argv>
darwin : exec env <DD_*> <ddjit-darwin_aarch64> <guest argv>     # DBT path
jail   : exec env DYLD_INSERT_LIBRARIES=<jail> <DD_*> <guest argv>   # only delta: inject vs exec
```

The `os()` branch in `script()` disappears; the only remaining difference is the launch
**prefix** (which binary, or the jail's DYLD_INSERT) — pick it from one small match, build the
`DD_*` block once for all. The binding becomes data-driven and obviously uniform.

## Why this is dead-obvious afterward

- One channel (env), one namespace (`DD_*`), one parser, one entry symbol, one binding template.
- Prefix tells you the class: `DD_` = container contract, `DDJIT_` = engine knob.
- A new option is added in exactly one place and works on all three targets automatically.

## Migration (low-risk, behavior-preserving)

1. **Accept the new names alongside the old** in the shared container parser (read `DD_VOLUMES`
   OR `DDVOL`, `DD_LOWERS` OR `DD_LOWER`/`JIT86_LOWER`, …). No behavior change; both work.
2. **Switch the binding** (`SpawnConfig::script`) to emit only `DD_*` + collapse the os-branch.
   The matrix still passes via the back-compat readers.
3. **Unify guest env**: point x86 `build_stack` at the shared `DD_GUEST_ENV` path (fixes the
   missing-env + hostname/limits gaps on x86 as a side effect).
4. **Rename entry symbols** to `dd_run` (mechanical; ride with REFACTOR.md step 0).
5. **Drop the legacy aliases** once nothing emits them; fold debug names under `DDJIT_*`.
