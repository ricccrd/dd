# Rendering — a unified display layer for dd containers on macOS

Status: **design — no code yet** (build gated on review). Goal: any `dd` container — Linux *or*
macOS guest — can put its GUI on the Mac screen, efficiently (zero-copy, GPU where possible), with
input flowing back. This generalizes the earlier "Wayland bridge" into the same decomposition `dd`
already uses for compute: a **shared host renderer** (backend) fed by **per-guest-OS translation
frontends** through one **unified display protocol**.

**This doc is the architecture (the *what*). The detailed implementation plan — DDP wire spec, exact
API call sequences, per-rung steps, seam edits, milestones, and validation order — is in
[`RENDERING_PLAN.md`](RENDERING_PLAN.md) (the *how*).**

## Decisions (locked)

- **Unified format:** a **new minimal DDP** (not Wayland-as-universal) — it must carry IOSurface Mach
  ports and serve both Wayland and Cocoa frontends.
- **Backend:** shared host process `dd-display`, Rust + **Smithay**, forking **`cocoa-way`** (Metal
  backend). Owns the AppKit main thread.
- **GPU:** commit to **rung 3** — guest Mesa (Vulkan/Zink) → **host Metal via a command-forward pipe
  over dd's socket (no VM)** — as a first-class goal, reached through rungs 1→2.
- **MVP target:** `weston-simple-shm` then SDL2. **Scope now:** design only, build gated on review.

## The one idea

Mirror `dd`'s compute architecture (`engine + frontend + OS-personality`) for display:

```
   guest GUI app
   ── Linux: Wayland/X11 ──┐         ┌── macOS: AppKit/CoreAnimation/Metal ──┐
                           ▼         ▼                                        │
        [ DISPLAY FRONTEND — per guest OS: TRANSLATES native → unified ]      │
          os/linux/display (Wayland+XWayland)   os/darwin/display (Cocoa/Metal adopt)
                           │
                  unified DD Display Protocol (DDP)
                  surfaces · buffers(shm|IOSurface) · damage · frames · input · window-ops
                           ▼
        [ DISPLAY BACKEND — shared host renderer `dd-display` ]
          AppKit main thread + Metal · one NSWindow+CAMetalLayer per surface · NSEvent capture
                           ▼
                    macOS screen  ◀── input ──  back through DDP to the owning container
```

**The translation lives per guest OS (the frontend); the rendering is shared (the backend).** A
Linux container translates Wayland→DDP; a macOS container translates Cocoa/Metal→DDP; the Mac renders
DDP. One pipeline, two (eventually more) frontends — exactly how `os/linux/` and `os/darwin/` already
split the syscall personality.

## Why dd makes this efficient (no VM, shared memory, real host frameworks)

- **Linux guest, CPU buffer:** the `wl_shm` pool is a POSIX-shm fd; it reaches the renderer over the
  socket (`SCM_RIGHTS`, already works) and is `mmap`'d to the *same pages* → `MTLBuffer(bytesNoCopy:)`
  on unified memory. Zero-copy, no IOSurface, no deprecated `kIOSurfaceIsGlobal`.
- **Linux guest, GPU buffer:** the guest's `linux-dmabuf` allocation is **backed by a host
  IOSurface**; DDP carries the IOSurface (Mach port), the renderer wraps it as an `MTLTexture`. The
  guest rendered on the real GPU; the host composites with zero copy and no readback.
- **macOS guest:** darwinjail runs it under the **real host dyld and real host frameworks**, so its
  `NSWindow` and its `CAMetalLayer`'s `IOSurface` are *genuine host objects in the same process*.
  Rendering is almost free; the frontend's job is to **adopt** those windows/surfaces into the unified
  model (labeling, isolation, routing, input confinement), not to translate pixels.

So the natural unified buffer types are **shm** (CPU, zero-copy via mmap) and **IOSurface** (GPU,
zero-copy via Mach-port handoff). DDP carries either; the software path and the GPU path differ only
in buffer type, not in protocol.

## The unified format: DD Display Protocol (DDP)

A small, transport-agnostic protocol over a per-container Unix socket (control) + shared memory /
Mach-port handoff (buffers). Deliberately minimal — it is an *internal* contract, not a public API.

- **Surface**: id, role (`toplevel`/`popup`/`cursor`/`subsurface`), parent, app-id, title, geometry,
  output, scale.
- **Buffer**: `SHM{region, offset, width, height, stride, format}` **or**
  `IOSURFACE{mach_port, format}`; plus damage rects. (ARGB8888/XRGB8888 mandatory; BGRA for Metal.)
- **Frame**: `commit` (atomic surface state) + `frame-callback` (vsync pacing / release).
- **Input (host → frontend)**: pointer motion/button/axis(scroll), keyboard (a **canonical keycode
  space** + modifier bitmap), focus enter/leave, touch, and window lifecycle (configure/resize/
  activate/close/minimize). Designed first-class and bidirectional — not bolted on.
- **Window ops (frontend → host)**: create/destroy/configure/set-title/set-min-max/set-parent/cursor.

DDP is the seam every frontend targets and the only thing the backend understands. Adding a future
guest OS = one new frontend that emits DDP.

DDP is a **new minimal protocol** (locked), not Wayland-as-universal: it must carry IOSurface Mach
ports, which Wayland's fd-based `linux-dmabuf` doesn't model, and macOS-guest Cocoa semantics don't
fit Wayland. Wayland remains only the Linux *frontend's* wire transport, normalized into DDP.

## The frontends (the per-container translation layers)

### Linux — `os/linux/display` (Wayland + XWayland → DDP)
A Wayland compositor *endpoint* (Smithay) translating `wl_compositor`/`wl_shm`/`xdg_shell`/`wl_seat`/
`linux-dmabuf` (+ XWayland for X11) into DDP. This is the earlier Wayland design, now emitting DDP
instead of driving NSWindow directly. For zero startup cost it can run inside the shared renderer
process, fed by each container's socket (one `wl_display` per container, isolated). MVP target:
**`weston-simple-shm` then SDL2** (`SDL_VIDEODRIVER=wayland`).

### Darwin — `os/darwin/display` (Cocoa/Metal adopt → DDP)
darwinjail already DYLD-`__interpose`s libSystem. Extend it to interpose the **windowing/GPU
frameworks** (AppKit `NSWindow`/`NSApplication`, CoreAnimation `CALayer`/`CAMetalLayer`, IOSurface,
CoreGraphics). A macOS guest's window creation and its `CAMetalLayer`'s `IOSurface` are emitted as DDP
`Surface`+`IOSURFACE` buffers and **relocated into the renderer's window space** so they're labeled,
isolated, and event-routed like any other container — rather than appearing as loose host windows.
Start with the `CAMetalLayer`/IOSurface present path + `NSWindow` lifecycle; full AppKit fidelity is a
long tail. (jitdarwin, the DBT, uses the same frameworks mediated through its syscall layer.)

## The backend (shared host renderer `dd-display`)

One Rust process (Smithay-based, forking **`cocoa-way`** — a native macOS Wayland compositor on
Smithay with a Metal/`CAMetalLayer` backend). Owns the **AppKit main thread + Metal + the event
source**. Per DDP `Surface` → one `NSWindow`+`CAMetalLayer` (rootless, à la XQuartz/quartz-wm, with a
bidirectional surface↔NSWindow map). Composites `SHM` via `MTLBuffer(bytesNoCopy)` and `IOSURFACE`
via `IOSurface`→`MTLTexture`; presents per-surface with damage tracking. Captures `NSEvent`, maps to
DDP input, routes to the owning container's frontend. Marries two run loops: the Cocoa run loop and
the Wayland/`dd` event loop (kqueue `CFRunLoopSource` integration).

**Why a separate host process (not in-process in a personality):** AppKit+Metal must own a process
main thread; the JIT already runs the guest there. Separate `dd-display`: owns its main thread, hosts
windows from **many containers** in one desktop, survives a guest crash, and keeps AppKit codesign/
entitlements apart from the JIT's `allow-jit`. Prior art agrees (XQuartz, Owl, cocoa-way are all
server processes). The cross-process cost is repaid to zero by `SCM_RIGHTS`+shm / IOSurface Mach
ports.

## GPU: exposing Metal into the guest (efficient rendering), in three rungs

The "let Linux use Metal so it renders efficiently" requirement is a ladder, not a switch:

1. **Software composite (MVP).** Guest renders on CPU (its own software path / llvmpipe, which runs on
   arm64); host GPU only does the final blit. No guest GPU. Proves the whole pipeline.
2. **GPU-surface present (zero readback).** Guest's `linux-dmabuf` buffers are **IOSurface-backed**, so
   what the guest hands to Wayland is already a host GPU texture; the host composites it directly. The
   guest still needs *a* GPU API to fill it — initially still software/llvmpipe into an IOSurface, or
   (rung 3) real GPU.
3. **Guest GPU on Metal — the committed goal (`dd-gpu` channel).** Give the guest image a Mesa stack —
   **Vulkan via KosmicKrisp** (LunarG's native Vulkan-1.3→Metal driver, upstream in Mesa; **Zink**
   layers GL on top). KosmicKrisp issues *Metal* calls, which the Linux guest can't make directly
   (Metal is a macOS framework). So the GPU command stream is **forwarded to `dd-display`**, which
   issues real Metal — a virtio-gpu/**Venus-style command pipe over `dd`'s socket instead of a VM**.
   macOS guests skip this entirely — they already call Metal in-process.

   Sketch of the `dd-gpu` subsystem (the largest piece of this project; rungs 1–2 ship first):
   - **ICD seam:** ship a thin guest **Vulkan ICD** (or run KosmicKrisp guest-side if it can be made
     to emit a serializable command stream) whose winsys is a `dd`-virtgpu, not DRM. Guest Mesa selects
     it without a real GPU via a synthesized `/dev/dri/renderD128` + a surfaceless platform (Zink's
     "Penny" brings up Zink with no DRI/DRM via `VK_KHR_swapchain`).
   - **Transport:** a ring/command buffer in shared memory (same zero-copy property) carrying Metal-
     or Vulkan-level commands + resource handles; control/fences over the DDP socket. Buffers are
     IOSurfaces (rung 2), so render targets are presentable with no readback.
   - **Host executor:** `dd-display` (or a sibling `dd-gpu` host service it owns) replays the stream on
     a real `MTLDevice`, manages the resource/handle table, and signals fences back.
   - **Open question (validate first):** whether to forward at the **Vulkan** level (host runs
     KosmicKrisp/MoltenVK → Metal; cleaner handle model, reuse conformance) or the **Metal** level
     (guest-side Vulkan→Metal, forward Metal; thinner host but Metal isn't designed for remoting).
     This is rung-3's pivotal design fork and the top thing to prototype before committing the ABI.

## Seam changes by layer (what each translation layer must add)

**Linux personality (`os/linux/`):**
1. **`memfd_create` (aarch64 nr 279) — REQUIRED for MVP.** Today → `ENOSYS`. Implement via
   `shm_open(O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600)` + `shm_unlink` + `ftruncate` (anonymous fd).
   macOS shm name ≤ ~31 chars; ignore `MFD_ALLOW_SEALING`. Unlocks `wl_shm`.
2. **AF_UNIX Wayland-socket routing.** Real `sun_path` `bind`/`connect` falls through raw today
   (`service.c:1899/1950`). MVP: bind-mount the host DDP/Wayland socket into the rootfs + set
   `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`. Cleaner later: jail-translate `sun_path` through the VFS.
3. **`SCM_RIGHTS`** already passes fds (guest-fd == host-fd); add a test that an shm pool fd survives
   and the renderer can `mmap` it at size.
4. **GPU (rungs 2–3):** `/dev/dri` synthesis, DRM/`linux-dmabuf` ioctls → IOSurface allocation, and
   the Metal command-forwarding channel.

**Darwin personality (`os/darwin/`):** interpose AppKit/CoreAnimation/Metal/IOSurface in darwinjail;
emit DDP surfaces; relocate guest windows into the renderer; route input back.

**Daemon (`dd-jit` `SpawnConfig` + `dd-daemon`):** spawn `dd-display` (shared, lazy); create a
per-container DDP socket; inject `WAYLAND_DISPLAY=wayland-0`, `XDG_RUNTIME_DIR=/run/user/0`,
bind-mount the socket; gate on a `--gui` flag / label so headless containers are unaffected.

## Milestones

- **M0 — seam:** `memfd_create`; prove a Linux guest creates a `wl_shm` pool and passes the fd to a
  host listener that `mmap`s it. (Personality only.)
- **M1 — first pixels (Linux):** minimal `dd-display` (the four globals) → `weston-simple-shm`/SDL2
  draws a native `NSWindow`. Present via `CALayer.contents` first, then `MTLBuffer(bytesNoCopy)`.
- **M2 — events + lifecycle:** `NSEvent`→DDP input; resize/close/activate→`xdg_toplevel`; multi-window;
  damage-tracked uploads.
- **M3 — DDP + macOS guest:** factor the renderer behind DDP; bring up `os/darwin/display` so a macOS
  guest GUI app renders through the same backend.
- **M4 — GPU rung 2:** IOSurface-backed dmabuf present (zero readback).
- **M5 — GPU rung 3:** guest Mesa (Zink/KosmicKrisp) with Metal command forwarding. XWayland for X11.

## Highest-risk unknowns — validate before committing

1. **Toolkit-via-shm reality.** Does a *current* GTK4/Qt6 app draw via `wl_shm`, or demand
   dmabuf/EGL even for "software"? Decides MVP scope. Test `weston-simple-shm`→SDL2→GTK3→GTK4 early.
2. **shm fd cross-process mmap** on macOS (POSIX-shm fd via `SCM_RIGHTS` to `dd-display`) — the whole
   CPU design; verify in M0.
3. **`bytesNoCopy` against a real pool:** 16 KiB alignment + length rounding, stride vs Metal
   row-bytes, ARGB↔BGRA, premultiplied alpha, Y-orientation. Validate with a test pattern.
4. **Two run loops, many windows from one `NSApp`:** marrying the Cocoa run loop with the Wayland/DDP
   event loop without blocking; one `NSApplication` hosting many containers' windows.
5. **NSEvent→canonical-keycode mapping** (→ evdev/XKB for Linux frontends). Tedious, bounded; no
   source pinned the exact table — budget time.
6. **macOS-guest window relocation:** can interposed AppKit windows be cleanly re-parented/labeled and
   their IOSurfaces handed to `dd-display`, or do they fight the guest's own `NSApplication`? (M3 risk.)
7. **GPU command forwarding (rung 3):** the Venus-like Metal pipe is the single largest piece; its
   feasibility/perf without a VM is unproven here. Keep llvmpipe as the standing fallback.

## Deliberately deferred

XWayland (after software Wayland), clipboard/drag-drop, multi-output/HiDPI polish, GPU rung 3, the
in-process same-address-space fast path (optimization only), and full AppKit fidelity for macOS guests.
