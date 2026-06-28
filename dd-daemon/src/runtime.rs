#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::build::*;
use crate::archive::*;
use crate::volumes::*;
use crate::networks::*;
use crate::registry::{Client, Credentials, ImageRef};
use axum::body::Body;
use axum::extract::{Path, Query, Request, State};
use axum::http::{StatusCode, Uri, HeaderMap};
use axum::response::{IntoResponse, Response};
use axum::Json;
use serde::{Deserialize, Serialize};
use serde_json::{json, Value};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::Arc;
use std::time::{SystemTime, UNIX_EPOCH};
use std::process::Stdio;
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd, RawFd};
use tokio::io::unix::AsyncFd;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::sync::{broadcast, mpsc, watch, Mutex};
use hyper_util::rt::TokioIo;
use ddjit::{Guest, PortMap, SpawnConfig, Volume};


/// Cap on the retained `docker logs` replay buffer (per container/exec). A chatty or long-lived guest
/// would otherwise grow `live.log_chunks` without bound and OOM the daemon. When a new chunk pushes the
/// buffer over this, the oldest chunks are dropped from the front — standard log-rotation behavior, so
/// `docker logs` shows the most-recent ≤ 8 MiB of output.
const LOG_CHUNKS_CAP_BYTES: usize = 8 * 1024 * 1024;

/// Append one output chunk to a Live's ordered `docker logs` buffer, enforcing [`LOG_CHUNKS_CAP_BYTES`]
/// by draining the oldest chunks from the front. The single place the cap lives, shared by the pipe
/// `pump` and the PTY reader. Locks ONLY `live.log_chunks` (never `inner`), so it preserves the reaper's
/// `inner` → `log_chunks` lock ordering and introduces no deadlock.
async fn push_log(live: &Live, ts: i64, stream: u8, bytes: Vec<u8>) {
    let mut log = live.log_chunks.lock().await;
    log.push((ts, stream, bytes));
    let mut total: usize = log.iter().map(|(_, _, b)| b.len()).sum();
    // Drop oldest chunks until under the cap, but always keep at least the just-pushed chunk.
    let mut drop_to = 0;
    while total > LOG_CHUNKS_CAP_BYTES && drop_to < log.len() - 1 {
        total -= log[drop_to].2.len();
        drop_to += 1;
    }
    if drop_to > 0 { log.drain(..drop_to); }
}


/// Resolve a docker `--user`/`Config.User` spec to a numeric `(uid, gid)` against the container's
/// rootfs. Accepts every docker form: `uid`, `name`, `uid:gid`, `name:group`, `uid:group`, `name:gid`.
/// A numeric component is taken verbatim (no file access needed — keeps the numeric path independent of
/// the rootfs contents); a NAME is looked up in `<rootfs>/etc/passwd` (user) or `<rootfs>/etc/group`
/// (group). When no group is given a NAME uses its primary gid from /etc/passwd, while a numeric uid
/// defaults to gid 0 (docker semantics). Returns `None` if a name component can't be resolved, so the
/// caller leaves the guest's default identity (matching the prior "skip unknown user" behavior).
fn resolve_user(rootfs: &str, spec: &str) -> Option<(u32, u32)> {
    let (us, gs) = spec.split_once(':').map_or((spec, None), |(u, g)| (u, Some(g)));
    // passwd line: name:passwd:uid:gid:gecos:home:shell  — return (uid, primary gid) for a name match.
    let lookup_passwd = |name: &str| -> Option<(u32, u32)> {
        let passwd = std::fs::read_to_string(format!("{rootfs}/etc/passwd")).ok()?;
        passwd.lines().find_map(|l| {
            let f: Vec<&str> = l.split(':').collect();
            (f.len() >= 4 && f[0] == name).then(|| Some((f[2].parse().ok()?, f[3].parse().ok()?)))?
        })
    };
    // group line: name:passwd:gid:members — return the gid for a name match.
    let lookup_group = |name: &str| -> Option<u32> {
        let group = std::fs::read_to_string(format!("{rootfs}/etc/group")).ok()?;
        group.lines().find_map(|l| {
            let f: Vec<&str> = l.split(':').collect();
            (f.len() >= 3 && f[0] == name).then(|| f[2].parse().ok())?
        })
    };
    let (uid, primary_gid) = match us.parse::<u32>() {
        Ok(n) => (n, None),
        Err(_) => { let (u, g) = lookup_passwd(us)?; (u, Some(g)) }
    };
    // A trailing-colon empty group (`"name:"` / `"1000:"`) means "no group" — not a parse failure.
    let gid = match gs.filter(|g| !g.is_empty()) {
        // No `:group`: a NAME uses its passwd primary gid; a numeric uid defaults to gid 0 (docker
        // semantics — `--user 1000` => 1000:0), since `primary_gid` is None on the numeric path.
        None => primary_gid.unwrap_or(0),
        Some(g) => g.parse().ok().or_else(|| lookup_group(g))?,
    };
    Some((uid, gid))
}


/// Translate the container into a typed [`SpawnConfig`] and run it in the matching guest's JIT.
/// Named-volume binds (`name:/path`, no leading `/`) are resolved against `volumes_dir`.
/// Build the (program, args) that launches this container in the matching guest's JIT. `None` if no JIT
/// was built for the image's arch.
pub(crate) fn spawn_cfg(c: &Container, volumes_dir: &str, vols: &[Vol], bridge: Option<(String, String)>) -> Option<(String, Vec<String>)> {
    let guest = c.arch.unwrap_or(Guest::LinuxAarch64);
    let mut cfg = SpawnConfig::new(String::new(), c.rootfs.clone()); // work_dir is the HOST cwd; leave empty
    cfg.argv = c.cmd.clone();
    // `-w DIR` (WorkingDir): the guest's initial cwd, read as DD_CWD by the runtime at startup.
    if !c.working_dir.is_empty() { cfg.env.push(("DD_CWD".into(), c.working_dir.clone())); }
    // container env (image ENV + `docker run -e`) -> one DD_GUEST_ENV var so the JIT forwards EXACTLY
    // these to the guest, never the daemon/host environment.
    if !c.env.is_empty() { cfg.env.push(("DD_GUEST_ENV".into(), c.env.join("\n"))); }
    cfg.hostname = (!c.hostname.is_empty()).then(|| c.hostname.clone());
    cfg.mem_max = c.memory.max(0) as u64;
    cfg.pids_max = c.pids_limit.max(0) as u32;
    // ---- JIT performance: developer-optimal defaults (see dd-daemon/PERFORMANCE.md) ----
    // The transparent codegen/syscall speedups (addressing, lazy flags, traces, tier-up, inline
    // clock_gettime/sigprocmask, epoll batching, path/openat caches, SSE->NEON, rep-string) are ON by
    // default inside the JIT itself -- every container gets them, zero config, byte-identical output.
    // Here the daemon enables the one CONTAINER-managed win: the persistent translated-code cache, so the
    // 2nd+ run of an image skips translation (~-40% cold start). It is self-invalidating (keyed by image
    // hash + engine version) and graceful-miss safe -> it can NEVER serve stale/wrong code, so the
    // everyday developer needs no knowledge of it. Operators can disable it daemon-wide with DD_PCACHE=0;
    // the cache lives under the dd home and is reported by `docker system df` / cleared by `system prune`.
    if std::env::var("DD_PCACHE").as_deref() != Ok("0") {
        let pdir = crate::util::dd_home().join("pcache");
        let _ = std::fs::create_dir_all(&pdir);
        cfg.env.push(("DDJIT_PCACHE".into(), "1".into()));
        cfg.env.push(("DDJIT_PCACHE_DIR".into(), pdir.to_string_lossy().into_owned()));
    }
    // `--network host` shares the host network (no per-container netns); otherwise isolate in one.
    cfg.netns = (c.network_mode != "host").then(|| c.id[..c.id.len().min(40)].to_string());
    // `--network none`: no external egress -- the JIT refuses non-loopback connects (DD_NET_ISOLATE).
    if c.network_mode == "none" { cfg.env.push(("DD_NET_ISOLATE".into(), "1".into())); }
    // netstack PR2 — per-network AF_UNIX virtual switch: container<->container TCP for in-subnet peers.
    if let Some((netid, ip)) = bridge { cfg.env.push(("DD_NETBR".into(), netid)); cfg.env.push(("DD_IP".into(), ip)); }
    // `docker run --user U[:G]` / `docker exec -u U[:G]` (and an image's `Config.User`): surface the
    // requested uid/gid to the JIT, which makes the guest observe them via getuid/getgid/setuid. A NAME
    // (e.g. `postgres`) is resolved against the rootfs's /etc/passwd|/etc/group; an unresolvable name is
    // skipped (the guest keeps its default identity). This is what lets the postgres entrypoint see
    // `id -u != 0` and skip its `gosu` re-exec (B4 — avoids the non-PIE gosu binary entirely).
    if !c.user.is_empty() {
        if let Some((uid, gid)) = resolve_user(&c.rootfs, &c.user) {
            cfg.env.push(("DD_UID".into(), uid.to_string()));
            cfg.env.push(("DD_GID".into(), gid.to_string()));
        }
    }
    // `--security-opt sandbox`/`untrusted` (or daemon-wide DD_SANDBOX=1) -> run this guest under the JIT's
    // untrusted-guest sentry: the worker drops to a deny-default OS sandbox (macOS Seatbelt) and the guest's
    // syscalls are vetted/forwarded over the sentry ring instead of hitting the host directly. We request it
    // by exporting the JIT's own gates (DDJIT_UNTRUSTED + DDJIT_SANDBOX); default OFF inside the JIT.
    //
    // CAVEAT: the JIT sentry is currently a FIRST PR that only forwards read/write/open(at)/close/lseek over
    // its ring. A real image will hit un-forwarded syscalls under the worker's deny-default sandbox until the
    // sentry's full fs/net/proc syscall set lands (a parallel JIT-side change is extending it). So this
    // wiring makes the daemon ABLE to request the sandbox; full untrusted-image support depends on that JIT
    // completion. Simple images + the end-to-end opt-in path work now.
    let sandbox = c.security_opt.iter().any(|o| {
        let o = o.to_ascii_lowercase();
        o.contains("sandbox") || o.contains("untrusted")
    }) || std::env::var("DD_SANDBOX").as_deref() == Ok("1");
    if sandbox {
        cfg.env.push(("DDJIT_UNTRUSTED".into(), "1".into()));
        cfg.env.push(("DDJIT_SANDBOX".into(), "1".into()));
    }
    // Resolve a mount source to a host path: an absolute path is a bind; anything else is a named
    // volume, resolved to its registered mountpoint or a dir under `volumes_dir`. Shared by `-v`/Binds
    // and `--mount` so both go through the SAME (single) volume mechanism.
    let resolve_src = |src: &str| -> String {
        if src.starts_with('/') {
            src.to_string()
        } else if let Some(v) = vols.iter().find(|v| v.name == src) {
            v.mountpoint.clone()
        } else {
            PathBuf::from(volumes_dir).join(src).to_string_lossy().into_owned()
        }
    };
    // `-v src:dst[:opts]` / Binds: parse off the mount options so the container path is the bare `dst`
    // (the old `split_once(':')` left `dst:ro` as the literal mount target — a bug). `ro` marks the
    // mount read-only; we now thread that through to `Volume.ro` so the JIT fails write-intent syscalls
    // under the mount with EROFS (matching what `docker inspect` already reports as RW:false).
    cfg.volumes = c.binds.iter().filter_map(|b| parse_bind(b).map(|(host, dst, ro)| {
        Volume { container: dst.into(), host: resolve_src(host), ro }
    })).collect();
    // `--mount` / HostConfig.Mounts: wire bind/volume mounts into the rootfs via the same Volume list.
    // type=bind -> Source is a host path; type=volume -> Source is a named volume (resolved like `-v`).
    // `ReadOnly` is honored the same as `-v …:ro` (JIT enforces write-intent EROFS under the mount).
    for m in &c.mounts {
        if m.target.is_empty() { continue; }
        let host = if m.typ == "bind" { m.source.clone() } else { resolve_src(&m.source) };
        if host.is_empty() { continue; }
        cfg.volumes.push(Volume { container: m.target.clone(), host, ro: m.read_only });
    }
    cfg.publish = c.publish.split(',').filter(|s| !s.is_empty()).filter_map(|p| p.split_once(':'))
        .filter_map(|(h, cc)| Some(PortMap { host: h.parse().ok()?, container: cc.parse().ok()? })).collect();
    // macOS containers (darwinjail): the userland (nix arm64 tools) is on PATH at /profile/bin, and the
    // host filesystem is the read-only lower so native binaries find their /nix deps; writes land in the
    // rootfs + volumes. The entry argv[0] is run by the mac shell, so it must be a real host path -- we
    // exec it through the profile's bash, which resolves the command via the in-jail PATH (so a bare
    // `bash`/`uname`/… is found at /profile/bin and execve()'d into the jail).
    if guest.os() == "darwin" {
        cfg.lowers = vec!["/".into()];
        // Forward the image ENV (TLS cert bundle, LANG, HOME, …) as real process env so the native
        // jailed binaries see it (the darwin jail inherits the spawn env; DD_GUEST_ENV is Linux-only).
        for kv in &c.env {
            if let Some((k, v)) = kv.split_once('=') {
                if k != "PATH" { cfg.env.push((k.to_string(), v.to_string())); }
            }
        }
        cfg.env.push(("PATH".into(), "/profile/bin".into()));
        // `-c` (not `-lc`): a login shell would source the host's /etc/profile via the lower and exec
        // arm64e system tools (which the arm64 jail can't inject into); the container has its own env.
        let wrapper = format!("{}/profile/bin/bash", c.rootfs);
        // A pulled darwin image may still carry the generic `/bin/sh` default (which doesn't exist in
        // the mac userland) — fall back to a bare `bash`, resolved via the in-jail PATH (/profile/bin).
        let mut argv = std::mem::take(&mut cfg.argv);
        if argv.is_empty() || argv == ["/bin/sh"] { argv = vec!["bash".into()]; }
        let mut wrapped = vec![wrapper, "-c".into(), "exec \"$@\"".into(), "dd-mac".into()];
        wrapped.extend(argv);
        cfg.argv = wrapped;
    }
    cfg.command(guest)
}


/// Spawn the container's guest process live (piped stdio) and wire its IO into `live`: stdout/stderr fan
/// out to attached clients + the log buffers; on exit, the container's status/exit-code are finalized.
/// Idempotent per container (start is a no-op if already running). Returns false if no JIT for the arch.
pub(crate) async fn spawn_live(app: &App, c: &Container, vols: &[Vol], live: Arc<Live>) -> bool {
    use std::sync::atomic::Ordering;
    if live.started.swap(true, Ordering::SeqCst) {
        return true; // already started
    }
    // netstack PR2: this container's (network-id, assigned-ip) from PR1's per-network endpoints map,
    // plus the /etc/hosts reach-by-name table (this container + same-network peers, name -> ip).
    let (bridge, hosts) = {
        let g = app.inner.lock().await;
        let bridge = g.networks.iter().find_map(|n| n.endpoints.get(&c.id).map(|e| (n.id.clone(), e.ip.clone())));
        // netstack reach-by-name: a guest's getaddrinfo("peer") must resolve to the peer's network IP so
        // the per-network br_* switch (PR2) can carry the connect. Docker drives this via embedded DNS;
        // the equivalent here is to seed /etc/hosts with every endpoint on the network(s) this container
        // is attached to. musl/glibc read /etc/hosts before any nameserver, so the resolve is local.
        let mut hosts = String::from("127.0.0.1\tlocalhost\n");
        // own entry (once): own-ip  own-name [hostname]. Absent for --network host/none (no endpoint).
        if let Some(own) = g.networks.iter().find_map(|n| n.endpoints.get(&c.id)) {
            let mut names = own.name.clone();
            if !c.hostname.is_empty() && c.hostname != own.name { names.push(' '); names.push_str(&c.hostname); }
            hosts.push_str(&format!("{}\t{}\n", own.ip, names));
        }
        // peers: every OTHER endpoint on any network this container is a member of.
        for n in &g.networks {
            if !n.endpoints.contains_key(&c.id) { continue; }
            for (cid, e) in &n.endpoints {
                if cid != &c.id { hosts.push_str(&format!("{}\t{}\n", e.ip, e.name)); }
            }
        }
        (bridge, hosts)
    };
    // Write the table best-effort — Docker manages /etc/hosts, so overwriting with the generated
    // reach-by-name content is correct; never fail the spawn on an I/O error.
    {
        let etc = format!("{}/etc", c.rootfs);
        let _ = std::fs::create_dir_all(&etc);
        if let Err(e) = std::fs::write(format!("{etc}/hosts"), &hosts) {
            if std::env::var("DD_DEBUG").is_ok() { eprintln!("[live] {} write /etc/hosts failed: {e}", &c.id[..c.id.len().min(12)]); }
        }
    }
    // No launch command means the JIT engine for this guest arch isn't bundled (e.g. a darwin-only build
    // shipped without ddjit-linux_*). Surface a CLEAN error (exit 127, like every other spawn failure) so an
    // interactive `docker run -it` exits with a message instead of hanging forever on a stream that never
    // opens -- the missing-engine hang that looked like a frozen, Ctrl-C-deaf shell.
    let Some((prog, args)) = spawn_cfg(c, &app.volumes_dir, vols, bridge) else {
        let guest = c.arch.unwrap_or(Guest::LinuxAarch64);
        return live_fail(app, &c.id, &live,
            format!("dd: no JIT engine for {} guests in this build (ddjit-{} missing) -- cannot start container",
                guest.target(), guest.target())).await;
    };
    let mut cmd = tokio::process::Command::new(prog);
    cmd.args(args);

    // pump one piped stream: broadcast each chunk to attached clients + accumulate for `docker logs`.
    async fn pump(mut r: impl AsyncReadExt + Unpin, kind: u8, live: Arc<Live>) {
        let mut buf = [0u8; 8192];
        loop {
            match r.read(&mut buf).await {
                Ok(0) | Err(_) => break,
                Ok(n) => {
                    let chunk = buf[..n].to_vec();
                    let _ = live.out.send((kind, chunk.clone()));
                    // Append to the single ordered log (arrival order) so the buffered replay interleaves;
                    // push_log enforces the LOG_CHUNKS_CAP_BYTES rotation so this can't grow unbounded.
                    push_log(&live, now_secs(), kind, chunk).await;
                }
            }
        }
    }

    // tty=true gives the guest a real PTY -- an interactive shell sees a terminal (prompt, line editing),
    // and stdout/stderr merge into one raw stream. Otherwise stdio is piped (the multiplexed-frame path).
    let (mut child, io_handles): (tokio::process::Child, Vec<tokio::task::JoinHandle<()>>) = if c.tty {
        let (master, slave) = match open_pty() {
            Ok(p) => p,
            Err(e) => return live_fail(app, &c.id, &live, format!("openpty: {e}")).await,
        };
        let slave_fd = slave.as_raw_fd();
        cmd.stdin(Stdio::null()).stdout(Stdio::null()).stderr(Stdio::null());
        // SAFETY: in the forked child, login_tty makes the slave the controlling terminal + stdin/out/err.
        unsafe {
            cmd.pre_exec(move || {
                if libc::login_tty(slave_fd) != 0 { return Err(std::io::Error::last_os_error()); }
                Ok(())
            });
        }
        let child = match cmd.spawn() {
            Ok(ch) => ch,
            Err(e) => return live_fail(app, &c.id, &live, format!("jit exec failed: {e}")).await,
        };
        drop(slave); // the child dup'd it via login_tty; close the parent's copy
        set_nonblocking(master.as_raw_fd());
        *live.pty_master.lock().unwrap() = Some(master.as_raw_fd());
        let afd = match AsyncFd::new(master) {
            Ok(a) => Arc::new(a),
            Err(e) => return live_fail(app, &c.id, &live, format!("asyncfd: {e}")).await,
        };
        // client stdin -> PTY master
        if let Some(mut rx) = live.stdin_rx.lock().await.take() {
            let w = afd.clone();
            tokio::spawn(async move {
                while let Some(chunk) = rx.recv().await {
                    if chunk.is_empty() { break; }
                    let mut off = 0;
                    while off < chunk.len() {
                        let Ok(mut g) = w.writable().await else { return };
                        match g.try_io(|i| pty_write(i.as_raw_fd(), &chunk[off..])) {
                            Ok(Ok(n)) => off += n,
                            Ok(Err(_)) => return,
                            Err(_would_block) => continue,
                        }
                    }
                }
            });
        }
        // PTY master -> broadcast (kind 1) + log buffer
        let r = afd.clone();
        let lr = live.clone();
        let reader = tokio::spawn(async move {
            loop {
                let Ok(mut g) = r.readable().await else { break };
                let mut buf = [0u8; 8192];
                match g.try_io(|i| pty_read(i.as_raw_fd(), &mut buf)) {
                    Ok(Ok(0)) | Ok(Err(_)) => break, // EOF / EIO when the guest exits
                    Ok(Ok(n)) => {
                        let chunk = buf[..n].to_vec();
                        let _ = lr.out.send((1, chunk.clone()));
                        // TTY: one merged raw stream, all stdout (kind 1), recorded in arrival order;
                        // push_log enforces the LOG_CHUNKS_CAP_BYTES rotation (most-recent ≤ 8 MiB).
                        push_log(&lr, now_secs(), 1, chunk).await;
                    }
                    Err(_would_block) => continue,
                }
            }
        });
        (child, vec![reader])
    } else {
        cmd.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::piped());
        let mut child = match cmd.spawn() {
            Ok(ch) => ch,
            Err(e) => return live_fail(app, &c.id, &live, format!("jit exec failed: {e}")).await,
        };
        let stdout = child.stdout.take().unwrap();
        let stderr = child.stderr.take().unwrap();
        // Feed the guest's stdin from the channel attach writes to (buffered until now). An empty Vec is
        // the stdin-EOF sentinel: close the guest's stdin so a shell reading to EOF can finish.
        if let Some(mut child_in) = child.stdin.take() {
            if let Some(mut rx) = live.stdin_rx.lock().await.take() {
                tokio::spawn(async move {
                    while let Some(chunk) = rx.recv().await {
                        if chunk.is_empty() { break; }
                        if child_in.write_all(&chunk).await.is_err() { break; }
                        let _ = child_in.flush().await;
                    }
                    drop(child_in); // EOF to the guest
                });
            }
        }
        let lo = live.clone();
        let h_out = tokio::spawn(async move { pump(stdout, 1, lo).await; });
        let le = live.clone();
        let h_err = tokio::spawn(async move { pump(stderr, 2, le).await; });
        (child, vec![h_out, h_err])
    };

    *live.pid.lock().unwrap() = child.id(); // remember the pid so pause can SIGSTOP/SIGCONT it
    let app = app.clone();
    let cid = c.id.clone();
    let auto_remove = c.auto_remove; // `--rm`: drop the container from state once it exits (see below)
    let dbg = std::env::var("DD_DEBUG").is_ok();
    tokio::spawn(async move {
        let code = child.wait().await.ok().and_then(|s| s.code()).unwrap_or(-1) as i64;
        if dbg { eprintln!("[live] {} exited code={code}", &cid[..12]); }
        *live.pty_master.lock().unwrap() = None;
        // Signal the natural exit IMMEDIATELY — flip status + fire the exit watch the instant the guest's
        // own process dies, so an interactive `docker run`/`ddcli mac` returns at once when the user types
        // `exit`. CRITICAL: this must NOT be gated on draining the PTY/pipe reader tasks. A stray
        // grandchild that inherited the slave/pipe fds can keep those readers from ever hitting EOF, and
        // the previous code awaited them BEFORE flipping status — which made `exit` hang for as long as
        // such a child lived (the daemon never told `docker run`/`/wait` the container was done). We drain
        // the readers AFTER, with a bounded grace, purely to finalize the log snapshot.
        {
            let mut g = app.inner.lock().await;
            if let Some(cc) = g.containers.get_mut(&cid) {
                cc.status = "exited".into();
                cc.exit_code = code;
                cc.finished_at = now_secs();
            }
            let (cname, cimage) = g.containers.get(&cid).map(|c| (c.name.clone(), c.image.clone())).unwrap_or_default();
            crate::events::emit_event(&app.events, "container", "die", &cid, serde_json::json!({"exitCode": code.to_string(), "name": cname, "image": cimage}));
            save_state(&g, &app.state_path);
        }
        let _ = live.exit.send(Some(code));
        // Drain the reader tasks so the final output lands in the log buffer, but never block the reaper
        // forever: a lingering child still holding the fds open would keep a reader from EOFing, so cap
        // the wait. In the normal case (the guest's process tree fully exited) the slave/pipes close at
        // once and each reader returns in well under this grace, so the snapshot below is complete.
        for h in io_handles {
            let _ = tokio::time::timeout(std::time::Duration::from_millis(500), h).await;
        }
        {
            let mut g = app.inner.lock().await;
            if let Some(cc) = g.containers.get_mut(&cid) {
                // Finalize the per-stream snapshots downstream code reads (cc.stdout/cc.stderr) by
                // filtering the ordered log by stream. The ordered log itself is LEFT INTACT on the
                // retained Live so `docker logs` can still replay stdout/stderr interleaved after exit
                // (the Live stays in the map for non-`--rm` containers).
                let (mut so, mut se) = (Vec::new(), Vec::new());
                for (_, kind, data) in live.log_chunks.lock().await.iter() {
                    if *kind == 1 { so.extend_from_slice(data); } else { se.extend_from_slice(data); }
                }
                cc.stdout = so;
                cc.stderr = se;
            }
        }
        // Exec cleanup: a `docker exec` runs through spawn_live with the EXEC id as `cid` (never a
        // `containers` entry). Record the exit code on the Exec, then drop ONLY its Live now that it has
        // exited — the Live's 8 MiB log buffer + channels are the real leak; without this every exec
        // leaks one. We KEEP the (tiny) Exec record so a post-exit `docker exec inspect` still returns
        // the real code (it reads `exec.exit_code` once the Live is gone). Independent of `--rm`, which
        // governs the parent CONTAINER, not the exec. Return: AutoRemove/RestartPolicy below are
        // container-only (and would no-op on an exec id anyway).
        {
            let mut g = app.inner.lock().await;
            if let Some(e) = g.execs.get_mut(&cid) {
                e.exit_code = code;
                g.live.remove(&cid);
                return;
            }
        }
        // `--rm` (AutoRemove): drop the container from state now that it has exited and its final
        // status/logs are recorded. AutoRemove and RestartPolicy are mutually exclusive in docker, so a
        // removed container is never a restart candidate — return before the supervisor runs. Anything
        // waiting on the exit watch (the `docker run --rm` foreground client) already saw Some(code).
        if auto_remove {
            let mut g = app.inner.lock().await;
            if let Some(dc) = g.containers.remove(&cid) {
                crate::events::emit_event(&app.events, "container", "destroy", &cid, serde_json::json!({"name": dc.name, "image": dc.image}));
                for n in g.networks.iter_mut() { leave_network(n, &cid); }
            }
            g.live.remove(&cid);
            save_state(&g, &app.state_path);
            return;
        }
        // RestartPolicy supervisor: re-run the container per `--restart` unless it was deliberately
        // stopped (stop/kill/rm set stop_requested). A no-op for the default `no`/empty policy, so the
        // common `docker run` path is untouched.
        if !live.stop_requested.load(std::sync::atomic::Ordering::SeqCst) {
            maybe_restart(&app, &cid, code).await;
        }
    });
    true
}


/// Apply the container's `--restart` policy after an exit. Restarts on `always`/`unless-stopped`
/// (any exit) or `on-failure` (non-zero exit, up to MaximumRetryCount). `no`/empty never restarts.
/// A short backoff avoids a tight crash-loop. Spawns a fresh [`Live`] (the old one is spent) and
/// re-enters [`spawn_live`], whose reaper re-applies this policy on the next exit.
fn maybe_restart<'a>(app: &'a App, cid: &'a str, code: i64)
    -> std::pin::Pin<Box<dyn std::future::Future<Output = ()> + Send + 'a>> { Box::pin(async move {
    let (name, max_retry, count, c, vols) = {
        let g = app.inner.lock().await;
        let Some(c) = g.containers.get(cid) else { return };
        // Don't restart a container that's already been removed or re-started elsewhere.
        if c.status != "exited" { return; }
        (c.restart_policy.name.clone(), c.restart_policy.max_retry, c.restart_count, c.clone(), g.volumes.clone())
    };
    let should = match name.as_str() {
        "always" | "unless-stopped" => true,
        "on-failure" => code != 0 && (max_retry <= 0 || count < max_retry),
        _ => false, // "no" / "" / unknown
    };
    if !should { return; }
    // Backoff (capped) so a container that exits immediately doesn't spin the daemon.
    let backoff = (100u64 << (count.clamp(0, 6) as u32)).min(10_000);
    tokio::time::sleep(std::time::Duration::from_millis(backoff)).await;
    // Install a fresh Live (the prior one is "started"/spent), mark running, bump the restart count.
    let live = Live::new(c.tty);
    {
        let mut g = app.inner.lock().await;
        match g.containers.get(cid) {
            // Re-check: a stop/rm may have raced in during the backoff.
            Some(cc) if cc.status == "exited" => {}
            _ => return,
        }
        // A deliberate `docker stop`/`kill`/`rm` during the backoff sets `stop_requested` on the OLD,
        // spent Live (still the `g.live[cid]` entry until we replace it below) but leaves status
        // "exited" — so the status check above can't see it. Re-read that flag and abort the restart,
        // otherwise the container the user just stopped would respawn.
        if g.live.get(cid).map_or(false, |l| l.stop_requested.load(std::sync::atomic::Ordering::SeqCst)) {
            return;
        }
        g.live.insert(cid.to_string(), live.clone());
        if let Some(cc) = g.containers.get_mut(cid) {
            cc.status = "running".into();
            cc.started_at = now_secs();
            cc.restart_count += 1;
        }
        save_state(&g, &app.state_path);
    }
    crate::events::emit_event(&app.events, "container", "restart", cid, serde_json::json!({"name": c.name}));
    spawn_live(app, &c, &vols, live).await;
    }) }


/// Record the failure on a Live and finalize the container as exit 127. Returns false (spawn failed).
pub(crate) async fn live_fail(app: &App, cid: &str, live: &Arc<Live>, msg: String) -> bool {
    let _ = live.out.send((2, format!("{msg}\n").into_bytes()));
    live.log_chunks.lock().await.push((now_secs(), 2, format!("{msg}\n").into_bytes()));
    let _ = live.exit.send(Some(127));
    if let Some(cc) = app.inner.lock().await.containers.get_mut(cid) { cc.status = "exited".into(); cc.exit_code = 127; }
    false
}


/// Allocate a pseudo-terminal; returns (master, slave) owned fds.
pub(crate) fn open_pty() -> std::io::Result<(OwnedFd, OwnedFd)> {
    let (mut m, mut s): (RawFd, RawFd) = (-1, -1);
    // termios/winsize are *mut on macOS, *const on linux; null_mut() coerces to both.
    let r = unsafe { libc::openpty(&mut m, &mut s, std::ptr::null_mut(), std::ptr::null_mut(), std::ptr::null_mut()) };
    if r != 0 { return Err(std::io::Error::last_os_error()); }
    Ok(unsafe { (OwnedFd::from_raw_fd(m), OwnedFd::from_raw_fd(s)) })
}

pub(crate) fn set_nonblocking(fd: RawFd) {
    unsafe {
        let fl = libc::fcntl(fd, libc::F_GETFL);
        libc::fcntl(fd, libc::F_SETFL, fl | libc::O_NONBLOCK);
    }
}

pub(crate) fn pty_read(fd: RawFd, buf: &mut [u8]) -> std::io::Result<usize> {
    let n = unsafe { libc::read(fd, buf.as_mut_ptr().cast(), buf.len()) };
    if n < 0 { Err(std::io::Error::last_os_error()) } else { Ok(n as usize) }
}

pub(crate) fn pty_write(fd: RawFd, buf: &[u8]) -> std::io::Result<usize> {
    let n = unsafe { libc::write(fd, buf.as_ptr().cast(), buf.len()) };
    if n < 0 { Err(std::io::Error::last_os_error()) } else { Ok(n as usize) }
}
