# Rendering — Linux GUI apps onto the macOS host, through the container

Status: **design** (no code yet). This is the plan for letting a Linux GUI app inside a `dd`
container draw a real window on the Mac — no VM, no X11 round-trip, ideally zero-copy. It builds on
the §14 graphics notes in the research doc and grounds them in `dd`'s actual seams.

## The one idea

A `dd` container has no display. We give it one by running a **Wayland compositor on the host** and
pointing the guest at it. The guest's GTK/Qt/SDL app is an ordinary Wayland *client*; our compositor
is the *server*. Each guest top-level window becomes one native **`NSWindow` + `CAMetalLayer`**
(rootless, exactly XQuartz/quartz-wm's model). The guest's pixel buffer is **POSIX-shared memory**,
so the compositor `mmap`s it and — on Apple-Silicon unified memory — wraps it **zero-copy** as an
`MTLBuffer` via `makeBuffer(bytesNoCopy:)`, blits to the layer, and presents. No IOSurface needed
for the shm path, which sidesteps the deprecated, insecure `kIOSurfaceIsGlobal`.

Why Wayland and not X11: Wayland's client/server split is a single Unix socket plus fd-passing —
which `dd` already supports — and every modern toolkit speaks it. X11 clients come along for free
later via **XWayland**.

## Why this is unusually easy for `dd` (and hard for everyone else)

`dd` runs the guest **in the same process and address space** as the personality (no VM). The hard
part of every prior macOS/Linux-GUI bridge is moving the framebuffer across an isolation boundary:

- **muvm/sommelier** (Asahi) get zero-copy only via **virtio-gpu / virtiofs-DAX** — VM-specific, *not*
  transferable here.
- **waypipe / wprs** serialize or stream buffers across a socket/network — `dd` needs neither.

For `dd` the buffer is just memory. Two ways to exploit that, both zero-copy:

1. **Standard Wayland fd-passing (recommended).** The guest's `wl_shm` pool is a POSIX-shm fd; it
   travels to the compositor over the Wayland socket via **`SCM_RIGHTS`** (already works), and the
   compositor `mmap`s the *same* physical pages. This is the normal Wayland mechanism — it happens to
   be zero-copy because both ends map one shm object. Works whether the compositor is in-process or a
   separate host process.
2. **Same-address-space fast path (optional).** If the compositor lives in the JIT process, the guest
   pool is *already* mapped at a known host pointer; the compositor can skip the second `mmap`. A micro
   optimization, not the foundation.

The decision below is built on (1) so the compositor can be a clean, isolated host process.

## Architecture: a separate `dd-wayland` host process

```
┌─────────────────────────┐         AF_UNIX (wayland-0)          ┌──────────────────────────────┐
│  JIT process (1/container)│  ───── protocol + SCM_RIGHTS fd ───▶ │  dd-wayland (compositor)      │
│  ┌────────────────────┐  │                                      │  AppKit main thread + Metal   │
│  │ guest GTK/Qt/SDL   │  │   wl_shm pool = POSIX-shm fd         │  ┌─────────────────────────┐  │
│  │  (Wayland client)  │  │   ───────────────────────────────▶  │  │ per xdg_toplevel:       │  │
│  └────────────────────┘  │   compositor mmaps same pages       │  │  NSWindow+CAMetalLayer  │  │
│  personality services    │                                      │  │  MTLBuffer(bytesNoCopy) │  │
│  socket/mmap/memfd/scm    │   ◀──── input: NSEvent→evdev ─────  │  └─────────────────────────┘  │
└─────────────────────────┘                                      └──────────────────────────────┘
        spawned by  ────────────────  dd-daemon (Docker Engine API)  ──────────────  spawns
```

**Why a separate process, not in-process in the personality.** Cocoa/AppKit + Metal **must own the
process main thread and run an `NSApplication` run loop**; the JIT already runs the guest on the main
thread. Fighting over the main thread inside one process is the single biggest source of risk. A
separate `dd-wayland` process: (a) owns its main thread cleanly; (b) survives a guest crash; (c) can
host windows from **several containers** in one desktop session; (d) keeps the entitlement/codesign
story for AppKit separate from the JIT's `allow-jit`. The cost — crossing a process boundary — is
paid back to zero by `SCM_RIGHTS` + shared shm. This is also what every piece of prior art does
(XQuartz, Owl, cocoa-way are all server processes).

**Compositor stack — three candidates** (this is a real decision, see end):

- **Rust + Smithay, fork `cocoa-way`.** `github.com/J-x-Z/cocoa-way` is a native macOS Wayland
  compositor in Rust on **Smithay** with a **Metal/`CAMetalLayer`** backend — the closest existing
  prior art to exactly this goal. Smithay is "building blocks, not a framework"; its Linux-only
  backends (udev/libinput/gbm/seat) are simply not used — only the protocol/desktop core. Pairs
  naturally with the Rust `dd-daemon`. Risk: cocoa-way is WIP/unmaintained.
- **C + libwayland-server + Cocoa, à la `Owl`.** `owl-compositor/owl` is an Obj-C/Cocoa libwayland
  compositor ("server to Wayland clients, client to Quartz… like XWayland/XQuartz"). Closest to the C
  personality; smallest dependency surface. Risk: more protocol glue to write by hand; also WIP.
- **From scratch on libwayland-server.** Most control, most work. Only if the above don't fit.

## The buffer/present path (the core loop)

1. Guest creates a pool: `wl_shm_create_pool(shm, fd, size)` where `fd` is our `memfd_create`
   (= POSIX shm). It carves `wl_buffer`s (`offset`, `width`, `height`, `stride`,
   `WL_SHM_FORMAT_ARGB8888` / `XRGB8888` — the two mandatory formats).
2. `fd` reaches `dd-wayland` over the socket (`SCM_RIGHTS`). The compositor `mmap`s it once
   (`MAP_SHARED`), keyed by the pool; re-used for every buffer in that pool.
3. On `wl_surface.commit`, read `width/height/stride/format/offset` and the damage region.
4. **Present, zero-copy:** `device.makeBuffer(bytesNoCopy: base, length: roundUp(size,16KiB),
   options: .storageModeShared, deallocator: nil)` → a compute/blit pass copies the ARGB rows into the
   drawable texture of that surface's `CAMetalLayer` (handling stride≠width·4, BGRA/ARGB channel
   order, and premultiplied alpha), then `presentDrawable`. (`bytesNoCopy` needs a **page-aligned base
   and page-rounded length** — Apple-Silicon pages are **16 KiB**; an `mmap` base is page-aligned, so
   only the length rounding is on us.)
5. Send `wl_buffer.release` + `wl_surface.frame` callback so the client draws the next frame.

For an MVP the "blit" can even be a plain `CGImage`/`CALayer.contents` upload — but the
`MTLBuffer(bytesNoCopy)` path is the one that scales and is genuinely zero-copy on unified memory.

## Exact seam changes in the personality (`os/linux/`)

These are the concrete edits. They are small; the heavy lifting is in `dd-wayland`.

1. **`memfd_create` (aarch64 nr 279) — REQUIRED.** Today: unhandled → `ENOSYS` (`service.c`
   default). Implement the canonical portable fallback:
   ```c
   // shm_open a unique name, O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC, 0600; then shm_unlink it (anonymous);
   // ftruncate to 0; return the fd. The object lives as long as an fd or mapping exists.
   ```
   Gotcha: macOS `PSHMNAMLEN` ≈ **31 chars** — keep the temp name short (e.g. `/dd<pid><ctr>`).
   `MFD_CLOEXEC`→`O_CLOEXEC`; ignore `MFD_ALLOW_SEALING` (no F_ADD_SEALS on macOS — toolkits tolerate
   its absence). This single syscall is what unlocks `wl_shm`.
2. **AF_UNIX Wayland socket routing.** Today a real `sun_path` `bind`/`connect` falls through raw to
   the host (`service.c:1899/1950`) — *not* jail-rewritten. Two options:
   - **Simplest (no code):** bind-mount the host compositor socket into the rootfs at the guest path
     and let the raw passthrough hit it (volumes already work). Set `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY`
     so the guest's `sun_path` *is* the bind-mount target.
   - **Cleaner (small code):** add explicit jail translation for AF_UNIX `sun_path` (resolve through
     the VFS like every other path) so the socket can live anywhere and arbitrary guest unix sockets
     stay confined. Do this eventually for correctness; the bind-mount unblocks the MVP today.
3. **`SCM_RIGHTS` — verify, likely no change.** `sendmsg/recvmsg` pass ancillary data opaque and
   guest-fd == host-fd, so a passed shm/pool fd already works. Add a test that a fd survives the
   round-trip and the receiver can `mmap` it at the same size.
4. **Later, for GPU:** `/dev/dri/renderD128` + `/dev/dri/card0` synthesis (extend the `/dev` handler
   in `openat`), DRM ioctl shims, and `linux-dmabuf` → IOSurface-backed `MTLTexture`. Not needed for
   software MVP.

## Daemon changes (`dd-jit` `SpawnConfig` + `dd-daemon`)

1. **Run `dd-wayland`** (lazily, one shared compositor for the session, or one per container) and
   create a per-container socket, e.g. `/run/dd/<ctr>/wayland-0` on the host.
2. **Inject env + mount** in `SpawnConfig`:
   ```rust
   cfg.env.push(("WAYLAND_DISPLAY".into(), "wayland-0".into()));
   cfg.env.push(("XDG_RUNTIME_DIR".into(), "/run/user/0".into()));
   cfg.volumes.push(Volume { container: "/run/user/0/wayland-0".into(),
                             host: "/run/dd/<ctr>/wayland-0".into() });
   ```
3. **Opt-in:** a `--gui`/label (or auto-detect a GUI toolkit in the image) gates all of the above so
   headless containers are unaffected.

## Minimum viable path to first pixels

Protocols the compositor must implement for a real toolkit window (software-rendered):
**`wl_compositor`** (surfaces) + **`wl_shm`** (buffers) + **`xdg_wm_base`/`xdg_shell`** (windowing) +
**`wl_seat`** (input; strictly optional for literal first-pixels but needed to be usable). Plus
`wl_output` (advertise one screen + scale) and `wl_data_device_manager` later for clipboard.

Milestones:

- **M0 — seam:** implement `memfd_create`; prove a guest can create a `wl_shm` pool and pass the fd to
  a host listener that `mmap`s it. (Pure personality + a throwaway host listener; no windows yet.)
- **M1 — first pixels:** `dd-wayland` implements the four globals; run `weston-simple-shm` or an SDL2
  app with `SDL_VIDEODRIVER=wayland`; a colored window appears as a native `NSWindow`. Present via
  `CALayer.contents`/`CGImage` first; swap in `MTLBuffer(bytesNoCopy)` once it draws.
- **M2 — input + lifecycle:** `wl_seat` keyboard/pointer from `NSEvent`; resize/close/minimize wired
  to `xdg_toplevel`; multiple windows; damage-tracked partial uploads.
- **M3 — real apps:** a GTK or Qt app end-to-end. **Validate which toolkits render via shm** — GTK4
  defaults to GL and may need `GSK_RENDERER=cairo`/`GDK_DEBUG`; SDL2 and Qt5 software are the safest
  first targets. XWayland for X11 apps.
- **M4 — GPU (separate effort):** `/dev/dri` + dmabuf → IOSurface/`MTLTexture`; guest Mesa via **Zink
  "Penny"** (brings up Zink with no DRI/DRM, `VK_KHR_swapchain`) over **KosmicKrisp** (LunarG's
  native, Vulkan-1.3-conformant Vulkan→Metal driver, upstream in Mesa — more complete than MoltenVK,
  but still Alpha). llvmpipe (software GL, runs fine on arm64) is the correctness fallback throughout.

## Highest-risk unknowns — validate before committing

1. **Toolkit-via-shm reality (M3 risk pulled early).** Does a *current* GTK4/Qt6 app actually draw
   through `wl_shm`, or does it demand `linux-dmabuf`/EGL even for "software"? If shm isn't enough for
   the toolkits we care about, the MVP scope (and maybe the dmabuf timeline) changes. **Test first**
   with `weston-simple-shm`, SDL2, GTK3, then GTK4.
2. **shm fd cross-process mmap.** Confirm a POSIX-shm fd passed via `SCM_RIGHTS` to a *separate*
   `dd-wayland` process can be `mmap`'d there at the right size on macOS. (Expected yes; it's the whole
   design — verify in M0.)
3. **`bytesNoCopy` against a real pool.** Pool base alignment (16 KiB), length rounding, stride vs
   Metal row-bytes, ARGB↔BGRA, premultiplied alpha, and Y-orientation. Easy to get a garbled or
   tinted image; validate with a known test pattern.
4. **AppKit main-thread / multi-window from one `NSApp`.** Confirm one `dd-wayland` `NSApplication`
   can host windows for many containers and pump events without blocking the Wayland event loop
   (two run loops to marry: `wl_event_loop` and the Cocoa run loop — likely a `CFRunLoopSource`/kqueue
   integration).
5. **NSEvent→evdev mapping.** Keycode/modifier/scroll translation tables (macOS virtual keycodes →
   Linux evdev → XKB keymap the client `mmap`s). Tedious but bounded; no surviving source pinned the
   exact table, so budget time for it.

## What we deliberately defer

dmabuf/DRM and GPU acceleration (M4), clipboard/drag-drop, multi-output/HiDPI niceties, XWayland
(after software Wayland works), and the in-process same-address-space fast path (optimization only).

## Sources

cocoa-way (`github.com/J-x-Z/cocoa-way`), Owl (`owl-compositor/owl`), quartz-wm
(`XQuartz/quartz-wm`), Smithay (`Smithay/smithay`), wprs (`wayland-transpositor/wprs`), Asahi muvm
X11 bridging (`asahilinux.org/2024/12/muvm-x11-bridging/`), Apple `makeBuffer(bytesNoCopy:)` &
`kIOSurfaceIsGlobal` docs, Mesa Zink/llvmpipe docs, LunarG KosmicKrisp announcement, Weston
`os-compatibility` memfd fallback, hello-wayland (`emersion/hello-wayland`). Full verified claim set:
the deep-research report this doc summarizes.
