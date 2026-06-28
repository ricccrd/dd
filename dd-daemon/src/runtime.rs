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
    // `--network host` shares the host network (no per-container netns); otherwise isolate in one.
    cfg.netns = (c.network_mode != "host").then(|| c.id[..c.id.len().min(40)].to_string());
    // `--network none`: no external egress -- the JIT refuses non-loopback connects (DD_NET_ISOLATE).
    if c.network_mode == "none" { cfg.env.push(("DD_NET_ISOLATE".into(), "1".into())); }
    // netstack PR2 — per-network AF_UNIX virtual switch: container<->container TCP for in-subnet peers.
    if let Some((netid, ip)) = bridge { cfg.env.push(("DD_NETBR".into(), netid)); cfg.env.push(("DD_IP".into(), ip)); }
    // `docker run --user U[:G]` / `docker exec -u U[:G]`: surface the requested uid/gid to the JIT, which
    // makes the guest observe them via getuid/getgid/setuid. Only numeric ids are honored; a `name` we
    // can't resolve (no passwd lookup here) is skipped (the guest then keeps its default identity). When
    // no group is given, the gid mirrors the uid (the common rootless case for this DBT).
    if !c.user.is_empty() {
        let (us, gs) = c.user.split_once(':').map_or((c.user.as_str(), None), |(u, g)| (u, Some(g)));
        if let Ok(uid) = us.parse::<u32>() {
            let gid = gs.and_then(|g| g.parse::<u32>().ok()).unwrap_or(uid);
            cfg.env.push(("DD_UID".into(), uid.to_string()));
            cfg.env.push(("DD_GID".into(), gid.to_string()));
        }
    }
    cfg.volumes = c.binds.iter().filter_map(|b| b.split_once(':').map(|(host, dst)| {
        // A bind whose source isn't an absolute path is a named volume.
        let host = if host.starts_with('/') {
            host.to_string()
        } else if let Some(v) = vols.iter().find(|v| v.name == host) {
            v.mountpoint.clone()
        } else {
            PathBuf::from(volumes_dir).join(host).to_string_lossy().into_owned()
        };
        Volume { container: dst.into(), host }
    })).collect();
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
    let Some((prog, args)) = spawn_cfg(c, &app.volumes_dir, vols, bridge) else { return false };
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
                    let b = if kind == 1 { &live.stdout_buf } else { &live.stderr_buf };
                    b.lock().await.extend_from_slice(&chunk);
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
                        lr.stdout_buf.lock().await.extend_from_slice(&chunk);
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
    let dbg = std::env::var("DD_DEBUG").is_ok();
    tokio::spawn(async move {
        let code = child.wait().await.ok().and_then(|s| s.code()).unwrap_or(-1) as i64;
        if dbg { eprintln!("[live] {} exited code={code}", &cid[..12]); }
        for h in io_handles { let _ = h.await; }
        *live.pty_master.lock().unwrap() = None;
        {
            let mut g = app.inner.lock().await;
            if let Some(cc) = g.containers.get_mut(&cid) {
                cc.status = "exited".into();
                cc.exit_code = code;
                cc.finished_at = now_secs();
                cc.stdout = std::mem::take(&mut *live.stdout_buf.lock().await);
                cc.stderr = std::mem::take(&mut *live.stderr_buf.lock().await);
            }
            crate::events::emit_event(&app.events, "container", "die", &cid, serde_json::json!({"exitCode": code.to_string()}));
            save_state(&g, &app.state_path);
        }
        let _ = live.exit.send(Some(code));
    });
    true
}


/// Record the failure on a Live and finalize the container as exit 127. Returns false (spawn failed).
pub(crate) async fn live_fail(app: &App, cid: &str, live: &Arc<Live>, msg: String) -> bool {
    let _ = live.out.send((2, format!("{msg}\n").into_bytes()));
    *live.stderr_buf.lock().await = format!("{msg}\n").into_bytes();
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
