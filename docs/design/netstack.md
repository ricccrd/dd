# Networking Phase-2b — Userspace Netstack (DESIGN)

Status: design only. Subsystem #2 in `docs/PLAN.md` ("Networking Phase-2b — userspace netstack").
Owner-seam: `dd-jit/src/runtime/os/linux/service.c` socket cases + `dd-jit/src/runtime/os/linux/container/`
+ `dd-daemon/src/networks.rs` / `runtime.rs`.

This document is the executable plan for giving dd containers a **real per-container L3 identity** (their
own IP on a virtual network) so that container-to-container connectivity, `inspect .NetworkSettings`, and
embedded DNS work, and for laying the path to a full userspace TCP/IP stack for cases that need genuine
L3 semantics. It cites the exact files/syscall cases and ends with a concrete first PR.

---

## 0. Key finding up front (reframing the goal)

The PLAN frames Phase-2b as "a real TCP/IP stack for **external** traffic (guest → internet)". After
reading the seam, **external egress already works today** — and it works *without* root, a TUN, or a NAT
shim, because dd does not virtualize L3 at all today: a guest `AF_INET` socket **is** a host socket.

Trace it:

- `service.c:2026` **case 198 `socket`** → `socket(a0, ty&0xf, a2)` — a real host kernel socket. For
  `AF_INET`/`SOCK_STREAM` it only records `g_sock_stream[r]=1` (netns.c bookkeeping); the fd is a host fd.
- `service.c:2121` **case 203 `connect`** → for any non-`127/8` `AF_INET` destination it falls straight
  through to `connect(a0, a1, a2)` on the host socket (`service.c:2151`). So `connect()` to `1.2.3.4:443`
  is a host connect from the host's IP, over the host's routes. **The container shares the host's network
  identity** (host IP, host routing table, host DNS reachability).
- DNS: the guest's resolver opens `AF_INET`/`SOCK_DGRAM`, `sendto`/`recvfrom` (cases 206/207,
  `service.c:2180`-`2188`) pass straight through to the host. As long as the image's `/etc/resolv.conf`
  names a reachable resolver, name resolution to the outside already works.
- Inbound: `-p H:C` port-map (`state.c:25`-`54`, `service.c` case 200 at `:2077`) binds host `:H`,
  reports `:C` back via `getsockname` (case 204, `:2163`).
- `--network none`: `DD_NET_ISOLATE` (`runtime.rs:51`) makes case 203 refuse non-loopback with
  `-ENETUNREACH` (`service.c:2124`-`2131`).
- Loopback isolation: `127/8`/`SOCK_STREAM` is redirected to per-container `AF_UNIX` sockets under
  `/tmp/.ddnet-<cid>/p<port>` (netns.c `lo_*`, `g_netns` set from `DD_NETNS` in
  `targets/linux_aarch64.c:190`-`195`).

**So the actual Phase-2b gap is not egress.** It is that containers have **no distinct IP**: every
container is "the host" on the wire. That breaks exactly the four things `docker-net.sh` checks and that a
real netstack provides:

1. `inspect .NetworkSettings.Networks[].IPAddress` is `""` (`containers.rs:476`,`:485`).
2. `network inspect` lists no members / no IPAM (`networks.rs:38` → `"Config": []`).
3. container → container **by IP** (no address to dial; the host has nothing on `172.18.0.2`).
4. container → container **by name** (no embedded DNS; `nc net-srv` → "bad address").

This document therefore designs the **L3-identity + intra-network data path** first (the high-value,
no-root part) and treats the full lwIP-style stack as the second arc, needed only when a guest requires
true L3 semantics (a real `eth0` it can introspect via netlink, raw sockets/ICMP `ping`, binding to a
specific private IP). Egress in *that* world is where the host-socket NAT shim becomes load-bearing.

---

## 1. What macOS actually permits (the primitive budget)

dd ships as an unprivileged user binary (Homebrew/DMG, no root, no kext, no entitlement). The host
toolbox is therefore:

| Primitive | Available unprivileged? | Use |
|---|---|---|
| `AF_INET`/`AF_INET6` TCP+UDP host sockets | **yes** | egress + inbound (already the whole data path today) |
| `AF_UNIX` stream/dgram sockets | **yes** | intra-host container↔container rendezvous (already used for loopback) |
| `kqueue` | **yes** | the JIT already maps `epoll`→kqueue |
| `utun` (`SYSPROTO_CONTROL`, `com.apple.net.utun_control`) | **no** — needs root **or** the `com.apple.developer.networking.networkextension` packet-tunnel entitlement + provisioning profile | a real TUN for a kernel-routed bridge |
| `AF_INET` raw sockets / ICMP / `/dev/bpf` | **no** — root only | real `ping`, packet capture |
| `pf` NAT / `route` table edits | **no** — root only | a kernel NAT bridge |

**Conclusion (hard constraint):** dd cannot create a TUN or a kernel bridge by default. Any L3 the guest
sees must be **synthesized in userspace**, and any external packet must leave via an ordinary host socket.
This is precisely the gVisor-netstack / slirp4netns / `gvisor-tap-vsock` (gvproxy) model: a userspace
TCP/IP stack whose "egress NIC" is the host's socket API. We adopt the same shape. (A `utun` fast-path is
kept as an *optional* capability gated on the entitlement; never required.)

---

## 2. Architecture — where the netstack sits

Two viable seams; we use **both, in sequence**, because they have very different cost/value:

### 2A. The "virtual switch" seam (recommended near-term) — reuse the existing socket seam

The loopback isolation already proves a powerful trick: **redirect a chosen IP range's `AF_INET` sockets
to `AF_UNIX` rendezvous sockets** keyed by a path both peers can compute (netns.c `lo_swap` +
`lo_path`). We generalize that from "127/8 → `/tmp/.ddnet-<cid>/p<port>`" to:

> a user network's subnet (e.g. `172.18.0.0/16`) → `/tmp/.ddbr-<netid>/<ip>:<port>` AF_UNIX sockets.

Because every container on a host is a JIT process on that same host, two containers on the same
user-network reach each other by each binding/connecting an `AF_UNIX` socket at the rendezvous path for
their network-assigned IP:port. No bridge, no TUN, no root — it is the loopback mechanism with a wider key
space and a *shared* (per-network, not per-container) directory. External destinations keep the existing
passthrough. This gets `docker-net.sh` green with a small, low-risk diff localized to the same code that
already works.

- **Sits at:** `service.c` cases 198/200/201/202/203/204/205 + a new `container/netbridge.c` peer of
  `container/netns.c`. No separate process.
- **Pros:** reuses a proven mechanism; zero new privileges; SOCK_STREAM semantics are real (it's a real
  kernel AF_UNIX stream — flow control, half-close, `accept` all just work). 
- **Cons:** it is L4 emulation, not L3 — no ICMP/`ping`, no raw sockets, the guest's `eth0` is still
  synthetic (handled by §3 reporting, not by real packets), UDP needs an AF_UNIX/dgram analog.

### 2B. The "userspace stack" seam (second arc) — a real lwIP-style TCP/IP in-process

For guests that need true L3 (introspect `eth0` via `AF_NETLINK`, `SIOCGIFADDR`; raw sockets; ICMP), run
a userspace IP stack (lwIP, or a vendored Rust `smoltcp`, or gVisor's `netstack` if we tolerate Go) **in
the JIT process**, and bind the guest's `AF_INET` sockets to *the stack's* socket API instead of the
host's:

- `socket(AF_INET,…)` (case 198) → allocate a **stack** PCB, return a JIT-managed fd (an `eventfd`/pipe
  for readiness so `epoll`/kqueue integration keeps working). `connect`/`bind`/`send`/`recv` (cases
  203/200/206/207/211/212) drive the stack PCB, not a host fd.
- The stack owns a synthetic `eth0` with the container's private IP (§3). Intra-network packets are
  switched to the peer container's stack over an `AF_UNIX`/`SOCK_DGRAM` "wire" carrying Ethernet/IP frames
  (a userspace L2 segment per network — same idea as gvproxy's `vfkit`/`unixgram` transport).
- **Egress is the host-socket NAT shim:** when the stack opens a flow whose destination is *outside* any
  managed subnet, the stack does **not** emit packets onto a wire it cannot route; instead a NAT module
  opens a real host `AF_INET` socket to that destination and splices bytes between the stack PCB and the
  host socket (TCP→TCP, UDP→UDP). This is the only way out without a TUN. DNS (UDP/53) is one such flow.
- **Sits at:** the same `service.c` socket cases, but dispatching to `container/netstack/` (the stack) for
  managed addresses. Could later be hoisted into the sentry process (subsystem #3) for untrusted guests.
- **Pros:** real L3; a coherent `eth0`/route model; ICMP/ping; the honest "real TCP/IP stack" the PLAN
  names. **Cons:** large; a vendored stack; careful epoll/kqueue readiness bridging; this is the long arc.

**Decision:** ship **2A** to make multi-container Docker workloads work now (it is ~150 lines against
proven code), and pursue **2B** behind a `DD_NETSTACK=1` capability flag for the cases 2A can't serve.
Both share the daemon-side identity/IPAM model in §3, so 2A is not throwaway: its IPAM, `/etc/hosts`,
inspect, and isolation logic are exactly what 2B also needs.

---

## 3. Per-container interface / route / IPAM model

This is daemon-side (`dd-daemon`) and shared by 2A and 2B. It is the part that has to exist regardless of
the data path.

### 3.1 Network model changes (`dd-daemon/src/model.rs`, `networks.rs`)

Extend `Net` (`model.rs:122`) and add an endpoint table:

```rust
struct Net {
    id, name, driver, scope, created,        // existing
    subnet:  String,   // e.g. "172.18.0.0/16"  (IPAM-allocated at create)
    gateway: String,   // e.g. "172.18.0.1"     (.1 of the subnet; the stack/host side)
    endpoints: Vec<Endpoint>,                 // replaces the bare `containers: Vec<String>`
}
struct Endpoint { cid: String, name: String, aliases: Vec<String>, ip: String, mac: String }
```

- **Subnet allocation (IPAM driver "default"):** on `networks_create` (`networks.rs:49`) pick the next
  free `/16` from the `172.18.0.0/12` pool (Docker's default-bridge range), skipping any subnet already
  in use. Predefined `bridge` gets `172.17.0.0/16`. Persisted via `save_state`.
- **Endpoint IP allocation:** when a container joins a network — both at `docker run --network X`
  (`containers.rs:111` sets `network_mode`) **and** at `POST /networks/:id/connect`
  (`networks.rs:82`) — assign the next free host address in the subnet (start at `.2`; `.1` is the
  gateway). Record `Endpoint{cid,name,aliases,ip,mac}`. Free it on stop/disconnect/rm.
  - **Bug to fix in passing:** today `docker run --network ddnet` does *not* add the container to
    `Net.containers` (only the explicit connect API does), which is why `network-inspect-lists-member`
    fails. Container-create must call the same join path.
- **MAC:** deterministic `02:42:` + 4 bytes of the IP (Docker's convention), purely cosmetic for 2A.

### 3.2 Reporting (makes 2 of the 4 failing assertions pass with zero JIT changes)

- `containers.rs:474`-`486` — fill `NetworkSettings.Networks[net].IPAddress`/`Gateway`/`MacAddress`/
  `NetworkID` from the endpoint; set top-level `.NetworkSettings.IPAddress` to the primary endpoint IP.
- `networks.rs:34` `net_json` — emit real `IPAM.Config` (`[{"Subnet","Gateway"}]`) and a populated
  `Containers` map keyed by endpoint (`{Name, IPv4Address, MacAddress}`).

### 3.3 What the guest sees as its interface/route model

- **2A:** the guest does not get a real `eth0` carrying packets; it gets *reported* identity. To satisfy
  apps that read their own IP, the daemon injects the address into the rootfs (`/etc/hosts` self-entry,
  below) and, optionally, a minimal `eth0` reported through the netlink/`SIOCGIFADDR` shim (a new, small
  addition to `service.c`'s ioctl block at `:432` and an `AF_NETLINK` case in `socket`/`200` — *deferred*;
  most containerized servers `bind(0.0.0.0)` and never ask).
- **2B:** the stack presents a genuine `eth0` (the assigned IP, `/16` route to the subnet via the wire,
  default route via the gateway → NAT shim), answerable over `AF_NETLINK` (`RTM_GETADDR`/`RTM_GETLINK`)
  and `SIOCGIFADDR`/`SIOCGIFFLAGS` ioctls.

### 3.4 Wiring the identity into the JIT (`runtime.rs` / `lib.rs` SpawnConfig)

Add to `SpawnConfig` (`dd-jit/src/lib.rs:94`) and the linux `script()` branch (`lib.rs:163`-`191`):

```
DD_NETBR=<netid>            # the per-network rendezvous id  -> /tmp/.ddbr-<netid>
DD_IP=172.18.0.2            # this endpoint's address
DD_SUBNET=172.18.0.0/16     # which AF_INET range to redirect to the rendezvous (2A) / route on eth0 (2B)
DD_GW=172.18.0.1
```

Set in `runtime.rs spawn_cfg` (`runtime.rs:45`-`64`), next to the existing `cfg.netns` / `DD_NET_ISOLATE`
/ `publish` wiring. `DD_NETNS` (loopback) stays unchanged and orthogonal.

---

## 4. Egress path (userspace stack → host sockets NAT) + DNS

### 4A. In the 2A virtual-switch world (near-term)

Egress is **unchanged passthrough** — and that is correct and root-free. The only new routing rule in
`service.c` case 203 `connect` (and 200 `bind`) is a **range check ordering**:

1. dest in `127/8` → existing loopback `AF_UNIX` redirect (`lo_*`).
2. dest in `DD_SUBNET` → **new** `netbridge` `AF_UNIX` redirect to `/tmp/.ddbr-<netid>/<ip>:<port>`.
3. else → existing host passthrough (external internet), unless `DD_NET_ISOLATE`.

DNS: unchanged passthrough to the host resolver — already works for external names. Intra-network names
are resolved by `/etc/hosts` (§5), not DNS, in 2A.

### 4B. In the 2B userspace-stack world (second arc)

Once the guest's sockets terminate on the in-process stack, packets to non-managed destinations cannot
passthrough (there is no host fd). The **NAT shim** is the egress:

- TCP: stack `SYN` to an external dst → shim `socket()/connect()` a host TCP socket to that dst; on
  established, splice stack-PCB ↔ host-socket bidirectionally (zero-copy via the iovec path). Source NAT
  is implicit (the host socket uses the host IP). Connection table keyed by `(guest 5-tuple)`.
- UDP: per guest `(srcip,srcport,dstip,dstport)` flow → a connected host UDP socket; splice datagrams.
- DNS: falls out of the UDP rule (UDP/53 to the resolver in the image's `/etc/resolv.conf`). The stack
  can additionally host an **embedded resolver** on the gateway IP (`172.18.0.1:53`) that answers
  intra-network names locally (from the endpoint table) and forwards everything else to the host resolver
  via the UDP NAT path — this is how 2B does name resolution without `/etc/hosts` edits.
- ICMP echo (`ping`): unprivileged ICMP is root-only on macOS, so the stack answers intra-subnet echo
  *locally* and, for external echo, either replies optimistically or returns unreachable. (Non-goal for
  the first arcs.)

---

## 5. Container-to-container connectivity (the `docker-net.sh` 3/7 gap)

`dd-tests/scenarios/docker-net.sh` is the executable spec. Today 3/7 pass (network-created,
server-running, cross-network-isolated — the last only *trivially*, because nothing reaches anything).
The four failing assertions and how the design turns each green:

| Assertion (test line) | Mechanism |
|---|---|
| `server-has-ip` (`:48`) | §3.1 IPAM assigns the endpoint IP; §3.2 reports it in inspect. **Daemon-only.** |
| `network-inspect-lists-member` (`:49`) | §3.1 fix: `run --network` joins the endpoint; `net_json` lists it. **Daemon-only.** |
| `reach-by-ip` (`:57`) | **2A:** `server` `bind(172.18.0.2:8080)`→AF_UNIX at `/tmp/.ddbr-<netid>/172.18.0.2:8080`; `listen`/`accept` over it; `client` `connect(172.18.0.2:8080)`→same path. (2B: switched as IP packets over the per-network wire.) |
| `reach-by-name` (`:53`) | embedded name→IP: **2A** writes `/etc/hosts` entries (`<ip> <name> <aliases>`) into every co-network container's rootfs at start, regenerated on join/leave; the guest's `nc net-srv` resolves via libc `/etc/hosts`. **2B** additionally offers the gateway resolver (§4B). |

Cross-network isolation (`:66`) holds **non-trivially** under 2A because the rendezvous directory is keyed
by `<netid>`: a container on `ddnet2` computes `/tmp/.ddbr-<ddnet2-id>/…` and never sees `ddnet`'s
sockets. (The directory is mode `0700`, created per network; a guest is path-jailed and cannot name it
directly — same property the loopback dir relies on.)

`/etc/hosts` generation: the daemon owns the rootfs (overlay UPPER), so it writes `<rootfs>/etc/hosts`
before spawn (`runtime.rs spawn_live`, around `:98`) from the network's endpoint table, plus the
container's own name/`localhost`. On topology change while running, rewrite the file (libc re-reads it per
lookup) — no signal needed.

---

## 6. Phased roadmap

Each phase is independently shippable and testable against `docker-net.sh` / `docker-full.sh`.

- **PR1 — IPAM + identity reporting (daemon-only, no JIT changes).** *The first PR; see §7.* Makes
  `server-has-ip` and `network-inspect-lists-member` green; fixes the `run --network` join bug. Lowest
  risk, unblocks everything else (the data path needs the address model to exist).
- **PR2 — `/etc/hosts` injection.** Daemon writes per-network `/etc/hosts`; turns `reach-by-name` green
  *as soon as a data path exists* (and is independently correct for self-name). Daemon-only.
- **PR3 — 2A virtual-switch data path (`container/netbridge.c` + `service.c` range check).** TCP first:
  `bind`/`listen`/`accept`/`connect`/`getsockname`/`getpeername` for `DD_SUBNET` → per-network AF_UNIX
  rendezvous. Turns `reach-by-ip` green and (with PR2) `reach-by-name`. **`docker-net.sh` → 7/7.**
- **PR4 — UDP intra-network + isolation hardening.** AF_UNIX/dgram analog for `SOCK_DGRAM` in the subnet;
  audit cross-network/`--network none` interplay; add a soak test.
- **PR5 — synthetic `eth0` introspection.** Minimal `AF_NETLINK` (`RTM_GETADDR`/`GETLINK`) +
  `SIOCGIFADDR`/`SIOCGIFFLAGS` so apps that read their own interface/IP work under 2A.
- **PR6+ — 2B userspace stack behind `DD_NETSTACK=1`.** Vendored `smoltcp`/lwIP, per-network unixgram
  wire, host-socket NAT shim for egress, embedded gateway resolver, ICMP-local. Optional `utun` fast-path
  gated on the network-extension entitlement. This is the "real TCP/IP stack" milestone; only needed for
  guests that 2A's L4 emulation can't serve.

---

## 7. The first PR (smallest useful slice)

**PR1 — "containers get an IP": daemon-side IPAM + identity, zero JIT changes.**

Why this and not "egress NAT shim first" (the PLAN's suggested slice): egress already works via host-socket
passthrough (§0), so a NAT shim moves no test and adds risk; the address model, by contrast, is the
prerequisite for *every* later phase and immediately turns 2 of the 4 red assertions green.

Scope (all in `dd-daemon`):

1. `model.rs:122` — add `subnet`,`gateway` to `Net`; add `Endpoint{cid,name,aliases,ip,mac}` and replace
   `Net.containers: Vec<String>` with `endpoints: Vec<Endpoint>` (keep a `containers()` helper returning
   the cid list so existing call-sites in `networks.rs` compile).
2. `networks.rs:49 networks_create` — allocate a free `/16` from `172.18.0.0/12`; set `gateway = .1`.
   `default_networks()` (`:116`) gives `bridge` = `172.17.0.0/16`.
3. New `ipam.rs` (or in `networks.rs`): `alloc_ip(net) -> String` / `free_ip(net, ip)` (next-free host
   address, `.1` reserved).
4. Join path — call from **both** `containers.rs:111` (when `network_mode` names a user network) and
   `networks.rs:82 network_connect`: push an `Endpoint` with a freshly allocated IP. Remove on
   stop/`network_disconnect`/rm.
5. `networks.rs:34 net_json` — real `IPAM.Config` (`[{Subnet,Gateway}]`) + populated `Containers` map.
6. `containers.rs:474`-`486` — report `IPAddress`/`Gateway`/`MacAddress`/`NetworkID` per joined network
   and at top-level `.NetworkSettings.IPAddress`.

Acceptance: `bash dd-tests/scenarios/docker-net.sh` advances from **3/7 to 5/7** (`server-has-ip`,
`network-inspect-lists-member` flip green); `docker-full.sh` network assertions unaffected; no JIT rebuild
required. State round-trips through `save_state`/load.

Out of scope for PR1: any `service.c` change, any data path, `/etc/hosts`, DNS. Those are PR2/PR3.

---

## 8. Risks + macOS-primitive constraints

- **No TUN / no root (hard).** Everything L3 is userspace; every external packet leaves on a host socket.
  The `utun` fast-path is entitlement-gated and must never be on the required path (§1). This bounds 2B:
  it can never present a fully kernel-routed bridge by default.
- **2A is L4 emulation, not L3.** No ICMP `ping`, no raw sockets, `eth0` is synthetic. Acceptable for the
  overwhelming majority of container workloads (TCP/UDP services), explicitly not for net-diagnostic
  images. 2B is the escape hatch; keep PR1-PR5 from baking in assumptions that block it (hence the shared
  IPAM model).
- **fd-table size.** netns.c's `g_lo_port[1024]`/`g_sock_stream[1024]` are fixed 1024-fd arrays
  (`netns.c:110`-`112`); a container with >1024 fds silently bypasses redirect. The netbridge tables
  inherit this cap — document it, or move to a dynamic map. (Pre-existing limitation, not introduced here.)
- **AF_UNIX path length.** `sun_path` is 104 bytes on macOS; `/tmp/.ddbr-<netid>/<ip>:<port>` must fit
  (use a short hashed `<netid>`, as `DD_NETNS` already truncates the cid to 40 chars,
  `targets/linux_aarch64.c:192`).
- **`bind(:0)` / ephemeral ports** in the subnet need the same round-trippable allocation the loopback
  path already solves (`netns.c lo_alloc_ephemeral`, `:123`); reuse that logic per-network.
- **UDP semantics over AF_UNIX (2A, PR4).** Connected vs unconnected datagram sockets, peer addressing in
  `recvfrom` (the loopback path sidesteps this by being stream-only, `service.c:2099`). Design UDP as
  AF_UNIX/`SOCK_DGRAM` with an addressing convention, or punt UDP intra-network to 2B.
- **Lifecycle / IP leaks.** Endpoint IPs must be freed on every exit path (stop, crash, rm, daemon
  restart re-derive). A leak exhausts the `/16`. Reconcile on load (`main.rs:64`).
- **getsockname/getpeername truthfulness.** Under 2A redirect, `getsockname` must report the *virtual*
  `AF_INET` address (`fill_inet_lo`'s analog for the subnet), not the AF_UNIX path, or apps that log their
  peer break (cf. the existing `g_fd_cport` getsockname patch at `service.c:2163`).
- **Interaction with port-map.** `-p` publishes on the host; a subnet-redirected `bind` must still let
  `-p` work (a service both reachable intra-network at its private IP *and* published on the host). Order
  the case-200 checks: published-port handling and subnet redirect are not mutually exclusive — a
  container can want both. Define precedence explicitly in PR3.

---

## 9. File/seam index (for the implementer)

JIT (C):
- `dd-jit/src/runtime/os/linux/service.c` — socket cases **198** socket, **199** socketpair, **200** bind,
  **201** listen, **202/242** accept/accept4, **203** connect (`DD_NET_ISOLATE` at `:2124`), **204**
  getsockname, **205** getpeername, **206** sendto, **207** recvfrom, **208/209** set/getsockopt, **210**
  shutdown, **211/212** sendmsg/recvmsg, **243/269** sendmmsg/recvmmsg; ioctl block at `:432`.
- `dd-jit/src/runtime/os/linux/container/netns.c` — loopback `AF_UNIX` redirect (`lo_on/lo_is/lo_swap/`
  `lo_path/lo_alloc_ephemeral/fill_inet_lo`, `g_netns/g_lo_port/g_sock_stream`). **Template for
  `netbridge.c`.**
- `dd-jit/src/runtime/os/linux/container/state.c` — port-map (`g_portmap/g_nportmap/pm_host/`
  `parse_publish/g_fd_cport`); add `parse_subnet`/endpoint-IP parse here.
- `dd-jit/src/runtime/targets/linux_aarch64.c` (`:179` publish, `:190` netns) and `linux_x86_64.c`
  (`:93`,`:116`,`:249`) — env/flag ingestion; add `DD_NETBR/DD_IP/DD_SUBNET/DD_GW`.

Daemon (Rust):
- `dd-daemon/src/networks.rs` — network CRUD + `net_json`/IPAM (PR1).
- `dd-daemon/src/model.rs:122` — `Net` / new `Endpoint`.
- `dd-daemon/src/containers.rs:51,:111,:474` — `network_mode` parse, join, inspect reporting.
- `dd-daemon/src/runtime.rs:45`-`64`,`:98` — SpawnConfig wiring + `/etc/hosts` write.
- `dd-jit/src/lib.rs:94` (`SpawnConfig`), `:163`-`191` (linux `script()` env/flags).

Tests:
- `dd-tests/scenarios/docker-net.sh` — the 7-assertion spec (PR1 → 5/7, PR3 → 7/7).
- `docs/PLAN.md:59`,`:213` — the Phase-2b entry + the gap table row this closes.
