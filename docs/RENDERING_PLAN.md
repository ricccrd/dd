# Rendering — detailed implementation plan (the next plan)

Companion to [`RENDERING.md`](RENDERING.md) (the architecture). That doc says *what* the display layer
is; this one is the *how* — the DDP wire spec, exact API sequences, per-rung steps, the seam edits, and
the validation order. Grounded in the dd source and a verified technical research pass.

Status: **plan, no code yet.** Locked decisions (from `RENDERING.md`): unified internal **DDP**;
separate host renderer `dd-display` (Rust + Smithay, structural fork of `cocoa-way`); **GPU rung 3 =
Vulkan-level forwarding** committed; MVP = `weston-simple-shm` → SDL2.

---

## 1. DDP — the unified wire protocol (concrete v0)

A small length-prefixed message protocol over a per-container `AF_UNIX` `SOCK_STREAM` socket, plus
out-of-band buffer handles (an fd via `SCM_RIGHTS` for shm; a mach-port name for IOSurface). One
`wl_display`/DDP endpoint per container (isolation). Frontend = client of the container; `dd-display` =
server. All ints little-endian; ids are u32; coordinates are i32 in surface-local logical pixels;
scale is a u32 (×120, like `wl_pointer` axis_value120) to avoid floats on the wire.

**Frontend → host (window/surface):**
| msg | fields | notes |
|---|---|---|
| `SURFACE_CREATE` | `sid`, `role`(toplevel/popup/cursor/subsurface), `parent_sid` | |
| `SURFACE_TITLE` | `sid`, `utf8[]` | xdg_toplevel.set_title |
| `SURFACE_GEOMETRY` | `sid`, `x,y,w,h`, `scale120` | window geometry hint |
| `SURFACE_MINMAX` | `sid`, `min_w,min_h,max_w,max_h` | constraints |
| `BUFFER_ATTACH` | `sid`, `buf_kind`(SHM\|IOSURFACE), `w,h,stride,format`(ARGB8888/XRGB8888/BGRA), `offset` | fd or mach-port arrives OOB, correlated by a `buf_seq` |
| `SURFACE_DAMAGE` | `sid`, `x,y,w,h` (repeatable) | dirty rects |
| `SURFACE_COMMIT` | `sid`, `buf_seq`, `frame_token` | atomic apply + request frame callback |
| `SURFACE_DESTROY` | `sid` | |
| `CURSOR_SET` | `sid`, `hotspot_x,hotspot_y` | pointer cursor surface |

**Host → frontend (events + lifecycle):**
| msg | fields | notes |
|---|---|---|
| `FRAME_DONE` | `sid`, `frame_token` | vsync pacing / release prev buffer |
| `BUFFER_RELEASE` | `buf_seq` | client may reuse the buffer |
| `CONFIGURE` | `sid`, `w,h`, `states`(maximized/activated/resizing/fullscreen) | xdg_toplevel.configure |
| `CLOSE` | `sid` | user hit the red button |
| `POINTER_ENTER/LEAVE` | `sid`, `x,y` | focus follows window |
| `POINTER_MOTION` | `sid`, `x,y` (wl_fixed→i32.8) | |
| `POINTER_BUTTON` | `sid`, `button`(BTN_LEFT/RIGHT/MIDDLE), `state` | evdev button codes |
| `POINTER_AXIS` | `sid`, `axis`(vert/horiz), `value120`, `source`(wheel/finger/continuous), `momentum` | precise + momentum |
| `KEY` | `sid`, `evdev_keycode`(already −8-normalized? **no: send raw evdev; frontend adds +8**), `state` | |
| `MODIFIERS` | `depressed,latched,locked,group` | drives `xkb_state_update_mask` |
| `KEYMAP` | size; fd OOB | once per seat; mmap'd `XKB_KEYMAP_FORMAT_TEXT_V1` |

DDP carries **either** an shm fd **or** an IOSurface mach-port as the buffer; everything else is
identical, so the software and GPU paths differ only in `buf_kind`. The Wayland frontend produces DDP
from `wl_*`/`xdg_*`; the Cocoa frontend produces DDP `SURFACE_*` + `IOSURFACE` from adopted windows.

---

## 2. Seam changes in the Linux personality (`os/linux/`)

Exact, code-grounded edits (line refs against current `service.c`).

### 2.1 `memfd_create` (aarch64 nr 279) — REQUIRED for `wl_shm`
Today unhandled → `default: -ENOSYS` (`service.c:2359`). Add a case. Recipe (the canonical portable
`os_create_anonymous_file` fallback, since macOS has no memfd):
```c
case 279: { // memfd_create(name, flags)
    char nm[31];                                 // macOS PSHMNAMLEN ~31 — keep it short
    snprintf(nm, sizeof nm, "/dd%d.%u", getpid(), g_memfd_seq++);
    int oflag = O_RDWR | O_CREAT | O_EXCL;        // (no O_CLOEXEC on shm_open path; set via fcntl)
    int fd = shm_open(nm, oflag, 0600);
    if (fd < 0) { G_RET(c) = (uint64_t)(-m2l_errno(errno)); break; }
    shm_unlink(nm);                              // anonymous: lives as long as fd/mapping
    if ((int)a1 & 1 /*MFD_CLOEXEC*/) fcntl(fd, F_SETFD, FD_CLOEXEC);
    // ignore MFD_ALLOW_SEALING (no F_ADD_SEALS on macOS; toolkits tolerate its absence)
    G_RET(c) = (uint64_t)fd; break;
}
```
The returned fd is `ftruncate`-able and `mmap(MAP_SHARED)`-able (both already handled: `ftruncate`
case 46; `mmap` case 222 → `mmap_flags()` maps `MAP_SHARED`=0x01). **This single syscall unlocks
`wl_shm`.** Validate: a guest creates an fd, `ftruncate`s it, `mmap`s it shared, writes; a second
`mmap` of the same fd sees the bytes.

### 2.2 AF_UNIX Wayland socket reaches `dd-display`
Real `sun_path` `bind`/`connect` currently passes **raw** to the host (`service.c:1899/1950`), not
jail-rewritten. MVP (no personality code): the daemon **bind-mounts** the host DDP socket into the
rootfs and sets `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR` so the guest's `sun_path` *is* the mount target →
the raw `connect` hits the real socket. Hardening later: jail-translate `sun_path` through the VFS so
arbitrary guest unix sockets stay confined and the socket can live anywhere.

### 2.3 `SCM_RIGHTS` — verify, likely no code
`sendmsg/recvmsg` (cases 211/212) pass ancillary data opaque and guest-fd == host-fd, so a passed shm
pool fd already crosses. **Test** that the fd survives and the receiver `mmap`s it at the right size
(M0 acceptance).

### 2.4 GPU (rungs 2–3) — later
`/dev/dri/renderD128` synthesis (extend the `/dev` handler in `openat` case 56), the Venus **vtest**
socket transport (a new fd kind), and IOSurface-backed memory. §6.

---

## 3. Daemon changes (`dd-jit` `SpawnConfig` + `dd-daemon`)

`SpawnConfig` already injects `env: Vec<(String,String)>` straight into the launch script and
`volumes` via `DDVOL=container:host,...` (`lib.rs`). So:
```rust
// gate on a --gui label / flag
cfg.env.push(("WAYLAND_DISPLAY".into(), "wayland-0".into()));
cfg.env.push(("XDG_RUNTIME_DIR".into(), "/run/user/0".into()));
cfg.volumes.push(Volume { container: "/run/user/0/wayland-0".into(),
                          host: format!("/run/dd/{ctr}/wayland-0") });
```
`dd-daemon` lazily spawns one shared `dd-display` for the session and creates the per-container socket
under `/run/dd/<ctr>/`. Headless containers set no GUI flag and are unaffected.

---

## 4. The renderer `dd-display` (host process, Rust)

Structure (following cocoa-way, corrected):
- **Event loop = winit.** Owns the AppKit main thread; create windows on `Resumed` with
  `MainThreadMarker`. Drive Smithay client dispatch from `Event::AboutToWait`:
  `display.dispatch_clients(&mut state)?; display.flush_clients()?;`.
- **Smithay = protocol only.** Use `wayland::compositor`, `shell::xdg`, `wl_shm`, `seat`, `output`;
  **do not** pull drm/gbm/libinput/udev/libseat. Inject input into the seat manually
  (`keyboard.input(...)`, `pointer.motion(...)`) from winit events. One `Display`/socket per container.
- **Per surface → one `NSWindow` + `CAMetalLayer`** (`BGRA8Unorm`), rootless, with a bidirectional
  `sid ↔ NSWindow` map (quartz-wm's model).
- **Thread rule:** all CoreAnimation/Metal calls on the **main thread** (CA/Metal not thread-safe);
  marshal with `dispatch_sync` if needed.

### 4.1 shm present (MVP path, then zero-copy)
Per `SURFACE_COMMIT` with an SHM buffer:
- **MVP (copy):** `mmap` the pool fd once (`MAP_SHARED`, keyed by pool); on commit, `texture.replace
  (region:mipmapLevel:withBytes: base+offset, bytesPerRow: stride)` into a `BGRA8Unorm` texture sized
  to the surface; handle **stride ≠ w·4** via `bytesPerRow`, and **ARGB↔BGRA** channel order. Then
  `layer.nextDrawable` → blit → `presentDrawable` → `commit`.
- **Zero-copy (later):** wrap the pool directly:
  `device.makeBuffer(bytesNoCopy: base, length: roundUp(size, 16384), options: .storageModeShared,
  deallocator: { munmap })`. **Hard constraints:** base **16 KB page-aligned** (mmap base is), length a
  **page multiple**, memory in a **single VM region**. Then a blit/compute pass to the drawable.

### 4.2 IOSurface present (rungs 2–3)
`device.makeTexture(descriptor:, iosurface:, plane: 0)` → an `MTLTexture` aliasing the IOSurface, zero
copy; set as a layer's content or blit to the drawable. IOSurface pixel format **`'BGRA'`** ↔ Metal
`BGRA8Unorm`. Use `IOSurfaceAlignProperty()` for stride/allocSize (don't hand-compute). Cross-process
handoff: producer `IOSurfaceCreateMachPort(s)` → send the port name in DDP → `dd-display`
`IOSurfaceLookupFromMachPort(port)`; **`mach_port_deallocate()` on both sides or the surface leaks.**
Never `kIOSurfaceIsGlobal` (deprecated, insecure).

### 4.3 input (NSEvent → DDP → xkb)
- **Keymap:** build once with xkbcommon (`xkb_keymap_new_from_names` → `xkb_keymap_get_as_string
  (XKB_KEYMAP_FORMAT_TEXT_V1)`), write to an fd, send via DDP `KEYMAP`; the Wayland frontend hands it to
  `wl_keyboard.keymap` and the client `mmap`s it.
- **Keycodes:** map `NSEvent.keyCode` (`kVK_*`/`CGKeyCode`) → Linux `KEY_*` (evdev). **Send raw evdev
  in DDP; the Wayland frontend feeds xkbcommon with `+8`** (X11 reserves 0–7). The `kVK_*→KEY_*` table
  is **unbuilt** — port from XQuartz `quartzKeyboard` / SDL cocoa.
- **Modifiers:** `NSEventModifierFlags` → `depressed/latched/locked/group` → DDP `MODIFIERS` →
  `xkb_state_update_mask`. Decide Command/Option semantics (map ⌘→Super or Ctrl).
- **Pointer/scroll:** buttons → `BTN_LEFT/RIGHT/MIDDLE`; `scrollingDeltaX/Y` +
  `hasPreciseScrollingDeltas`/momentum → `wl_pointer` axis `value120` + `axis_source` (wheel vs finger)
  + momentum phase. Mapping is **unbuilt**; HiDPI via the window backing scale.

---

## 5. macOS-guest frontend (`os/darwin/display`)

darwinjail runs the guest under the **real host dyld + frameworks**, so its `NSWindow`/`CAMetalLayer`/
IOSurface are genuine host objects in-process. The frontend **adopts** them rather than translating
pixels:
- Interpose/swizzle `NSWindow`/`NSApplication`/`CAMetalLayer`/`CALayer`/`IOSurface` (darwinjail already
  DYLD-`__interpose`s libSystem — extend to AppKit/QuartzCore).
- For each guest top-level: create a `CAContext`, host the window's layer tree, export its
  **`CAContextID`** → emit DDP `SURFACE_CREATE` + the contextID; `dd-display` renders it via a
  **`CALayerHost`** in an NSView (`wantsLayer=YES`). Pixels stay in an IOSurface (its ID/port in DDP).

- Route DDP input back into the guest's event queue; relocate/label/isolate windows in `dd-display`'s
  space. Full AppKit fidelity is a long tail — start with one toplevel + its IOSurface + lifecycle.
- **Risk:** the `contextId`↔windowserver linkage is undocumented; `CAPortalLayer` (private) is the
  fallback. Validate early (§7).

---

## 6. `dd-gpu` — Vulkan command forwarding to host Metal (rung 3)

The committed efficient-rendering path. Forward at the **Vulkan** level (SPIR-V end-to-end; NIR→MSL
once, host-side), reusing Venus.

**Guest side (Linux container):**
- Ship Mesa's **Venus** Vulkan ICD in the guest image; select it with **`VN_DEBUG=vtest`** so its
  transport is the **vtest Unix-socket protocol**, not virtgpu — **no real `/dev/dri` needed** for the
  command path. (GL apps: **Zink → Venus**; `MESA_LOADER_DRIVER_OVERRIDE=zink`,
  `EGL_PLATFORM=surfaceless`.)
- Some clients still probe `/dev/dri` — synthesize a node if needed (Zink "Penny"/`VK_KHR_surfaceless`
  brings up Zink with no DRI/DRM).

**Host side (`dd-display` or a sibling `dd-gpu` service it owns):**
- Run the **vtest server with the Venus backend** (virglrenderer `-Dvenus=true`,
  `virgl_test_server --venus`) replaying serialized Vulkan onto a **host Vulkan driver = KosmicKrisp**
  (Apple Silicon/macOS 26+) **or MoltenVK** (broader), runtime-selected. Borrow **gfxstream's**
  ring-buffer + **1:1 encoder/decoder threading**.
- **Re-implement Venus's memory model for no-VM macOS:** Venus assumes virtio-gpu **blob resources** +
  `VK_EXT_image_drm_format_modifier` + dma-buf/`KVM_SET_USER_MEMORY_REGION` host-visible mapping —
  **none exist here.** Map instead onto: host-visible memory = shared shm regions (we already share
  memory); presentable render targets = **IOSurface-backed `MTLTexture`s** (§4.2), so a guest swapchain
  image *is* the thing `dd-display` composites — zero readback. **This memory-model port is the single
  largest and riskiest piece of the project.**

**The fork, decided:** Vulkan-level, because (a) SPIR-V passes through untranspiled (near-native), (b)
there is **no mature serialization protocol at the Metal level**, (c) the host NIR→MSL happens once in
KosmicKrisp. The most reusable transport is virglrenderer `vtest --venus` over a Unix socket.

**Unknown to spike:** does **virglrenderer build on macOS** with venus + a Metal-backed host Vulkan?
The vtest/venus path is proven on Linux; this port is unproven. Keep **llvmpipe** (software GL, runs on
arm64) as the standing correctness fallback throughout.

---

## 7. Milestones + acceptance tests

- **M0 — seam.** Implement `memfd_create`; a guest creates a `wl_shm`-style pool, passes the fd via
  `SCM_RIGHTS` to a throwaway host listener that `mmap`s it and reads the test bytes. *Accept:* bytes
  match cross-process. (Personality only; retires risk #2.)
- **M1 — first pixels.** Minimal `dd-display` (wl_compositor/shm/xdg_wm_base/wl_seat) + winit window;
  run `weston-simple-shm`, then SDL2 (`SDL_VIDEODRIVER=wayland`). *Accept:* a correct-colored native
  `NSWindow`. Copy-present first (`replaceRegion`), then `makeBuffer(bytesNoCopy)`.
- **M2 — input + lifecycle.** NSEvent→DDP→xkb; resize/close/activate↔xdg_toplevel; multi-window;
  damage-tracked uploads. *Accept:* type/click into the SDL2 app; resize tracks; close works.
- **M3 — DDP + macOS guest.** Factor `dd-display` behind DDP; bring up `os/darwin/display` adopting one
  macOS-guest toplevel via CAContext/CALayerHost. *Accept:* a Mac-guest GUI window renders in
  `dd-display`. (Retires risk #6.)
- **M4 — GPU rung 2.** IOSurface-backed dmabuf present, zero readback, software-filled. *Accept:* an
  SDL2 GL app via llvmpipe-into-IOSurface, no CPU readback.
- **M5 — GPU rung 3.** `dd-gpu`: Venus/vtest over socket, host KosmicKrisp/MoltenVK, IOSurface memory
  model. *Accept:* a Vulkan triangle (e.g. `vkcube`) renders via host Metal. XWayland for X11 apps.

---

## 8. Validation order — spike these before committing ABIs

1. **Toolkit-via-shm reality** (pull M3's risk into M1): does a *current* GTK4/Qt6 app draw via
   `wl_shm`, or demand dmabuf/EGL even for "software"? Test `weston-simple-shm`→SDL2→GTK3→GTK4. Decides
   MVP scope and how soon rung 2 is mandatory.
2. **shm fd cross-process `mmap` on macOS** (M0) — the whole CPU design.
3. **`makeBuffer(bytesNoCopy)` against a real pool** — 16 KB alignment, length rounding, single-VM-
   region, stride/BGRA/premultiply/Y-flip — with a known test pattern.
4. **IOSurface cross-process handoff on *current* macOS** — confirm `IOSurfaceCreateMachPort`/
   `LookupFromMachPort` + `mach_port_deallocate` against today's headers (one Chromium mach claim was a
   refuted iOS-only path — do not copy it).
5. **virglrenderer+venus on macOS with KosmicKrisp** — the rung-3 make-or-break; prototype the vtest
   server replaying onto a Metal-backed Vulkan before designing the IOSurface memory-model port.
6. **CAContext/CALayerHost guest-window adoption** — can an interposed guest export a contextID that
   `dd-display` hosts, without fighting the guest's own `NSApplication`?
7. **winit + Smithay multi-client on one `NSApp`** — many containers' windows + non-blocking dispatch.

---

## 9. Deliberately deferred

XWayland (after software Wayland), clipboard/drag-drop, multi-output/HiDPI polish, PI/robust edge cases,
full AppKit fidelity for macOS guests, and the in-process same-address-space fast path (optimization).
