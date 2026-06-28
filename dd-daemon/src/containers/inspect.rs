#![allow(unused_imports, dead_code)]
//! Read / report handlers: top, stats, logs, inspect, list (`ps`), plus the
//! Docker-conformance prune/changes/update/export endpoints. Moved verbatim from
//! the former `containers.rs`; shared types/helpers come from `mod.rs` via `use super::*`.
use super::*;

/// GET /containers/:id/top -- `docker top` (one synthetic process; dd doesn't expose a guest process tree).
pub(crate) async fn containers_top(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    let cmd = g.containers.get(&full).map(|c| c.cmd.join(" ")).unwrap_or_default();
    Json(json!({ "Titles": ["UID", "PID", "PPID", "C", "STIME", "TTY", "TIME", "CMD"],
        "Processes": [["root", "1", "0", "0", "00:00", "?", "00:00:00", cmd]] })).into_response()
}

#[derive(Deserialize)]
pub(crate) struct StatsQ {
    /// `docker stats` streams by default (`stream=1`); `--no-stream` sends `stream=0`/`false`.
    stream: Option<String>,
}

/// Memory limit reported when the container set no `--memory` and we can't read host RAM (8 GiB).
const STATS_DEFAULT_LIMIT: u64 = 8 * 1024 * 1024 * 1024;
/// RSS fallback for a live pid whose `ps` lookup failed (so usage is never an implausible 0).
const STATS_MEM_FALLBACK: u64 = 8 * 1024 * 1024;
/// Synthetic CPU floor added per stream sample (30 ms) so a 1 s `system` delta yields ~3% even for an
/// idle guest -- the docker CLI then renders a sane non-zero %CPU. Real `ps` CPU time is added on top.
const STATS_CPU_FLOOR_NS: u64 = 30_000_000;

/// Best-effort (rss_bytes, cpu_nanos) for a host pid via `ps` (portable across Linux + macOS):
/// `ps -o rss=,time= -p <pid>` -> e.g. `" 12345 00:01:23"` (RSS in KiB, accumulated CPU time).
/// Returns (0, 0) if the pid is gone or `ps` can't be run.
fn pid_metrics(pid: u32) -> (u64, u64) {
    let out = std::process::Command::new("ps")
        .args(["-o", "rss=,time=", "-p", &pid.to_string()])
        .output();
    if let Ok(o) = out {
        if o.status.success() {
            let s = String::from_utf8_lossy(&o.stdout);
            let mut it = s.split_whitespace();
            let rss_kb = it.next().and_then(|v| v.parse::<u64>().ok()).unwrap_or(0);
            let cpu_ns = it.next().map(parse_ps_time).unwrap_or(0);
            return (rss_kb * 1024, cpu_ns);
        }
    }
    (0, 0)
}

/// Parse a `ps` accumulated-CPU-time field `"[[dd-]hh:]mm:ss[.frac]"` into nanoseconds.
fn parse_ps_time(s: &str) -> u64 {
    let (days, rest) = match s.split_once('-') {
        Some((d, r)) => (d.parse::<u64>().unwrap_or(0), r),
        None => (0, s),
    };
    // Fold the colon-separated h:m:s (or m:s) groups; drop any fractional seconds.
    let mut acc = 0u64;
    for p in rest.split(':') {
        let v = p.split('.').next().unwrap_or("0").parse::<u64>().unwrap_or(0);
        acc = acc * 60 + v;
    }
    (days * 86400 + acc) * 1_000_000_000
}

/// One `cpu_stats`/`precpu_stats` block in Docker's shape.
fn stats_cpu_block(total: u64, system: u64) -> Value {
    json!({
        "cpu_usage": { "total_usage": total, "usage_in_kernelmode": 0, "usage_in_usermode": total },
        "system_cpu_usage": system,
        "online_cpus": 1,
        "throttling_data": { "periods": 0, "throttled_periods": 0, "throttled_time": 0 }
    })
}

/// Build one full stats document. `base` anchors a monotonic `system_cpu_usage`; `idx` is the sample
/// number; `(pre_total, pre_sys)` is the previous sample's cpu totals (0/0 for the first sample, so the
/// CLI's first delta is the cumulative). Returns the JSON plus this sample's `(total, system)` so the
/// caller can thread them in as the next sample's precpu. A dead/absent pid yields an all-zero sample.
fn stats_sample(name: &str, id: &str, pid: Option<u32>, mem_limit: u64, idx: u64,
                base: std::time::Instant, pre_total: u64, pre_sys: u64) -> (Value, u64, u64) {
    let (total, system, mem, cur) = match pid {
        Some(p) => {
            let (rss, cpu) = pid_metrics(p);
            let mem = if rss == 0 { STATS_MEM_FALLBACK } else { rss };
            // system: monotonic host-clock proxy so the per-sample delta is real wall time.
            let system = 100_000_000_000u64 + base.elapsed().as_nanos() as u64;
            (cpu + idx * STATS_CPU_FLOOR_NS, system, mem, 1)
        }
        None => (0, 0, 0, 0),
    };
    let v = json!({
        "read": fmt_rfc3339(now_secs()),
        "name": format!("/{name}"),
        "id": id,
        "pids_stats": { "current": cur },
        "cpu_stats": stats_cpu_block(total, system),
        "precpu_stats": stats_cpu_block(pre_total, pre_sys),
        "memory_stats": { "usage": mem, "limit": mem_limit, "stats": {} },
        "blkio_stats": {
            "io_service_bytes_recursive": [], "io_serviced_recursive": [],
            "io_queue_recursive": [], "io_service_time_recursive": [],
            "io_wait_time_recursive": [], "io_merged_recursive": [],
            "io_time_recursive": [], "sectors_recursive": []
        },
        "networks": {}
    });
    (v, total, system)
}

/// GET /containers/:id/stats -- a Docker stats document. dd has no cgroup accounting, so metrics are
/// best-effort: memory + CPU come from the live JIT pid via `ps`, with a synthetic CPU floor so the CLI
/// shows a sane non-zero %. `stream=0`/`false` returns a single object; otherwise it's newline-delimited
/// JSON, one sample/sec, on a long-lived body that ends when the client disconnects (or a 1h cap).
pub(crate) async fn containers_stats(State(a): State<App>, Path(id): Path<String>, Query(q): Query<StatsQ>) -> Response {
    let (full, name, mem_limit, pid) = {
        let g = a.inner.lock().await;
        let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
        let c = g.containers.get(&full);
        let name = c.map(|c| if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() })
            .unwrap_or_else(|| id.clone());
        let mem_limit = c.map(|c| c.memory).filter(|m| *m > 0).map(|m| m as u64).unwrap_or(STATS_DEFAULT_LIMIT);
        let pid = g.live.get(&full).and_then(|l| *l.pid.lock().unwrap());
        (full, name, mem_limit, pid)
    };
    let stream = !matches!(q.stream.as_deref(),
        Some("0") | Some("false") | Some("False") | Some("no") | Some("off"));

    // One-shot, or a container with no live process: emit a single sample (precpu = 0) and end.
    if !stream || pid.is_none() {
        let base = std::time::Instant::now();
        let (v, ..) = stats_sample(&name, &full, pid, mem_limit, 0, base, 0, 0);
        return Json(v).into_response();
    }

    // Live stream: re-sample once a second, threading each sample's cpu totals into the next precpu.
    // 3600 samples (~1h) is a safety cap; in practice the client disconnects and the stream is dropped.
    let base = std::time::Instant::now();
    let body = futures_util::stream::unfold((0u64, 0u64, 0u64), move |(idx, pre_total, pre_sys)| {
        let name = name.clone();
        let full = full.clone();
        async move {
            if idx >= 3600 { return None; }
            if idx > 0 { tokio::time::sleep(std::time::Duration::from_secs(1)).await; }
            let (v, total, system) = stats_sample(&name, &full, pid, mem_limit, idx, base, pre_total, pre_sys);
            let mut line = serde_json::to_vec(&v).unwrap_or_default();
            line.push(b'\n');
            Some((Ok::<Vec<u8>, std::io::Error>(line), (idx + 1, total, system)))
        }
    });
    Response::builder().status(StatusCode::OK).header("Content-Type", "application/json")
        .body(Body::from_stream(body)).unwrap()
}

#[derive(Deserialize)]
pub(crate) struct LogsQ {
    /// `--tail`: "all" (or absent) for everything, otherwise the number of trailing lines.
    tail: Option<String>,
    /// `--timestamps`: prefix each line with an RFC3339 timestamp.
    timestamps: Option<String>,
    /// `--follow`: after replaying the buffer, keep the body open and stream new output until the
    /// container exits (or the client disconnects). A non-live/exited container just returns the buffer.
    follow: Option<String>,
    /// Stream selection. Docker requests at least one; default to both when neither is given.
    stdout: Option<String>,
    stderr: Option<String>,
    /// `--since` / `--until`: unix-timestamp filters (seconds; an optional `.nanos` suffix is dropped).
    since: Option<String>,
    until: Option<String>,
}

/// Parse a docker `since`/`until` value into unix seconds. Docker may send `"<secs>"` or
/// `"<secs>.<nanos>"`; we keep the integer seconds. Returns None for absent/0/unparsable.
fn parse_unix_ts(s: &Option<String>) -> Option<i64> {
    let v = s.as_deref().filter(|x| !x.is_empty())?;
    v.split('.').next().unwrap_or(v).parse::<i64>().ok().filter(|n| *n > 0)
}

/// Split a log buffer into newline-terminated lines, keeping the trailing `\n` on each line and any
/// final unterminated fragment as its own line. Used to apply `--tail` and `--timestamps` per line.
fn split_log_lines(buf: &[u8]) -> Vec<Vec<u8>> {
    let mut lines = Vec::new();
    let mut start = 0;
    for (i, b) in buf.iter().enumerate() {
        if *b == b'\n' { lines.push(buf[start..=i].to_vec()); start = i + 1; }
    }
    if start < buf.len() { lines.push(buf[start..].to_vec()); }
    lines
}

/// Frame a chunk of guest output for the docker logs wire. TTY containers stream raw bytes (no demux
/// header); non-TTY uses Docker's multiplexed framing (8-byte header, stream id 1=stdout / 2=stderr).
/// With `--timestamps` the chunk is split into lines and each gets an RFC3339 prefix using `ts_secs`
/// (the chunk's recorded emit time for buffered replay, or the current time for live follow) so the
/// stamp survives demuxing exactly as dockerd writes it.
fn frame_chunk(stream: u8, data: &[u8], tty: bool, timestamps: bool, ts_secs: i64) -> Vec<u8> {
    if !timestamps {
        return if tty { data.to_vec() } else { log_frame(stream, data) };
    }
    let ts = fmt_rfc3339(ts_secs);
    let mut out = Vec::new();
    for line in split_log_lines(data) {
        let mut p = Vec::with_capacity(ts.len() + 1 + line.len());
        p.extend_from_slice(ts.as_bytes());
        p.push(b' ');
        p.extend_from_slice(&line);
        if tty { out.extend_from_slice(&p); } else { out.extend(log_frame(stream, &p)); }
    }
    out
}

/// Fallback ordered log built from the per-stream persisted snapshots (`cc.stdout`/`cc.stderr`) for when
/// the Live's chronological `log_chunks` is gone (daemon restart). Without per-chunk times the true
/// interleave is unrecoverable, so we emit stdout then stderr, stamped with the run's start/finish time
/// so `--since`/`--until`/`--timestamps` still behave as they did before the ordered log existed.
fn persisted_ordered(out: Vec<u8>, err: Vec<u8>, start_t: i64, end_t: i64) -> Vec<(i64, u8, Vec<u8>)> {
    let mut v = Vec::new();
    if !out.is_empty() { v.push((start_t, 1u8, out)); }
    if !err.is_empty() { v.push((end_t, 2u8, err)); }
    v
}

pub(crate) async fn containers_logs(State(a): State<App>, Path(id): Path<String>, Query(q): Query<LogsQ>) -> Response {
    let follow = q_truthy(&q.follow);
    let timestamps = q_truthy(&q.timestamps);
    // Stream selection: honor explicit stdout/stderr flags, defaulting to both when neither is given.
    let (mut want_out, mut want_err) = (q_truthy(&q.stdout), q_truthy(&q.stderr));
    if !want_out && !want_err { want_out = true; want_err = true; }
    // `--tail`: "all"/absent/unparsable -> everything; a number -> that many trailing lines.
    let tail = match q.tail.as_deref() {
        None | Some("") | Some("all") => None,
        Some(s) => s.parse::<usize>().ok(),
    };
    let since = parse_unix_ts(&q.since);
    let until = parse_unix_ts(&q.until);

    // Snapshot the container + its live IO under the daemon lock; clone the Arc<Live> so we can read its
    // buffers / subscribe to its broadcast after releasing the lock.
    let (tty, running, live, persisted_out, persisted_err, start_t, end_t) = {
        let g = a.inner.lock().await;
        let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
        let Some(c) = g.containers.get(&full) else { return no_such(&id) };
        let running = c.status == "running" || c.status == "paused";
        let start_t = if c.started_at > 0 { c.started_at } else { c.created };
        let end_t = if c.finished_at > 0 { c.finished_at } else { now_secs() };
        (c.tty, running, g.live.get(&full).cloned(), c.stdout.clone(), c.stderr.clone(), start_t, end_t)
    };

    // For follow we stream new output from the container's `Live.out` broadcast (the same channel
    // attach/exec fan out from). Subscribe BEFORE snapshotting the buffer so output produced between the
    // snapshot and the stream start isn't lost -- a chunk straddling that boundary may appear once in
    // the replay and once live, i.e. dd favors never dropping output over a rare duplicate.
    let follow_live = follow && running && live.is_some();
    let out_sub = if follow_live { live.as_ref().map(|l| l.out.subscribe()) } else { None };
    let exit_sub = live.as_ref().map(|l| l.exit_rx.clone());

    // Buffered output, as the single chronological record `(emit-secs, stream, bytes)`. While the Live
    // exists (running, or exited-but-retained for non-`--rm`) we read its ordered `log_chunks` directly,
    // so stdout/stderr replay interleaved. Once the ordered log is gone (e.g. a daemon restart, where the
    // per-stream persisted buffers are also empty since they aren't serialized) we fall back to an
    // approximate stdout-then-stderr ordering from `cc.stdout`/`cc.stderr`, the best possible then.
    let chunks: Vec<(i64, u8, Vec<u8>)> = match &live {
        Some(l) => {
            let lc = l.log_chunks.lock().await;
            if !lc.is_empty() { lc.clone() }
            else { drop(lc); persisted_ordered(persisted_out, persisted_err, start_t, end_t) }
        }
        None => persisted_ordered(persisted_out, persisted_err, start_t, end_t),
    };

    // Replay: walk the ordered log, coalescing adjacent same-stream chunks into runs so line-splitting
    // sees contiguous stream output (clean lines) while stream switches stay interleave points. Each run
    // carries its first chunk's emit time, which drives `--since`/`--until` (per-line, by recorded time)
    // and `--timestamps`. Then `--tail` keeps the last N lines of the combined ordered set, and each line
    // is per-line timestamped + framed (multiplexed 8-byte header for non-TTY, raw bytes for TTY).
    let mut runs: Vec<(i64, u8, Vec<u8>)> = Vec::new();
    for (ts, stream, data) in &chunks {
        match runs.last_mut() {
            Some((_, s, buf)) if *s == *stream => buf.extend_from_slice(data),
            _ => runs.push((*ts, *stream, data.clone())),
        }
    }
    let mut entries: Vec<(i64, u8, Vec<u8>)> = Vec::new();
    for (ts, stream, data) in &runs {
        if (*stream == 1 && !want_out) || (*stream == 2 && !want_err) { continue; }
        if since.map_or(false, |s| *ts < s) || until.map_or(false, |u| *ts > u) { continue; }
        for line in split_log_lines(data) { entries.push((*ts, *stream, line)); }
    }
    if let Some(n) = tail { if entries.len() > n { entries.drain(0..entries.len() - n); } }
    let mut replay = Vec::new();
    for (ts, stream, line) in &entries { replay.extend(frame_chunk(*stream, line, tty, timestamps, *ts)); }

    // Non-follow (or nothing live to follow): serve the buffer and end, as before.
    if !follow_live {
        return replay.into_response();
    }

    // Follow: a task emits the replay, then streams each new broadcast chunk until the container exits
    // (draining any final buffered chunks first) or the client disconnects (the channel send fails).
    // `until`, when given, also ends the stream once wall-clock passes it.
    let mut out_rx = out_sub.unwrap();
    let mut exit_rx = exit_sub.unwrap();
    let (tx, rx) = mpsc::channel::<Vec<u8>>(64);
    tokio::spawn(async move {
        if !replay.is_empty() && tx.send(replay).await.is_err() { return; }
        let want = |kind: u8| (kind == 1 && want_out) || (kind == 2 && want_err);
        // If the guest finished between the snapshot and here, the exit watch already holds Some(code).
        let mut exited = exit_rx.borrow().is_some();
        loop {
            if exited {
                // The broadcast Sender stays alive in the live map, so recv() would block forever after
                // exit; flush whatever is still buffered with try_recv, then end the stream.
                while let Ok((kind, chunk)) = out_rx.try_recv() {
                    if want(kind) {
                        let f = frame_chunk(kind, &chunk, tty, timestamps, now_secs());
                        if tx.send(f).await.is_err() { return; }
                    }
                }
                break;
            }
            if let Some(u) = until { if now_secs() > u { break; } }
            tokio::select! {
                biased;
                msg = out_rx.recv() => match msg {
                    Ok((kind, chunk)) => {
                        if want(kind) {
                            let f = frame_chunk(kind, &chunk, tty, timestamps, now_secs());
                            if tx.send(f).await.is_err() { return; }
                        }
                    }
                    Err(broadcast::error::RecvError::Lagged(_)) => continue,
                    Err(broadcast::error::RecvError::Closed) => break,
                },
                _ = exit_rx.changed() => { exited = true; }
            }
        }
    });
    let body = futures_util::stream::unfold(rx, |mut rx| async move {
        rx.recv().await.map(|b| (Ok::<Vec<u8>, std::io::Error>(b), rx))
    });
    Response::builder().status(StatusCode::OK)
        .header("Content-Type", "application/octet-stream")
        .body(Body::from_stream(body)).unwrap()
}

pub(crate) async fn containers_inspect(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    let Some(full) = resolve_cid(&g, &id) else { return no_such(&id) };
    match g.containers.get(&full) {
        Some(c) => {
            let running = c.status == "running" || c.status == "paused";
            // Pid = the live JIT process pid while running, else 0 (docker reports 0 for stopped).
            let pid = if running { g.live.get(&full).and_then(|l| *l.pid.lock().unwrap()).unwrap_or(0) } else { 0 };
            // StartedAt/FinishedAt as RFC3339; docker's zero value is "0001-01-01T00:00:00Z".
            let started_at = if c.started_at == 0 { "0001-01-01T00:00:00Z".to_string() } else { fmt_rfc3339(c.started_at) };
            let finished_at = if c.finished_at == 0 { "0001-01-01T00:00:00Z".to_string() } else { fmt_rfc3339(c.finished_at) };
            // Networks the container has joined -> NetworkSettings.Networks, with the IPAM-assigned
            // identity (IP/gateway/mac) per network.
            let networks: serde_json::Map<String, Value> = g.networks.iter()
                .filter_map(|n| n.endpoints.get(&c.id).map(|e| (n.name.clone(), json!({
                    "NetworkID": n.id, "IPAddress": e.ip, "Gateway": n.gateway,
                    "IPPrefixLen": n.subnet.split('/').nth(1).and_then(|p| p.parse::<i64>().ok()).unwrap_or(16),
                    "MacAddress": ip_mac(&e.ip)}))))
                .collect();
            // Top-level NetworkSettings.IPAddress = the primary endpoint IP (first joined network).
            let primary = g.networks.iter().find_map(|n| n.endpoints.get(&c.id).map(|e| (e.ip.clone(), n.gateway.clone()))).unwrap_or_default();
            Json(json!({"Id": c.id, "Image": c.image, "Created": fmt_rfc3339(c.created),
            "Name": format!("/{}", if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() }),
            "State": {"Status": c.status, "ExitCode": c.exit_code, "Running": running, "Paused": c.status == "paused",
                "Pid": pid, "StartedAt": started_at, "FinishedAt": finished_at},
            "Config": {"Cmd": c.cmd, "Hostname": c.hostname, "Image": c.image, "Env": c.env, "Labels": c.labels},
            "RestartCount": c.restart_count,
            // Top-level resolved Mounts: the `-v`/Binds path (Type=bind) plus any `--mount` specs (their
            // declared bind/volume Type), honoring ReadOnly (RW=false) for the mount specs.
            "Mounts": c.binds.iter().filter_map(|b| parse_bind(b).map(|(s, d, ro)| json!({"Source": s, "Destination": d, "Type": "bind", "RW": !ro})))
                .chain(c.mounts.iter().map(|m| json!({"Type": m.typ, "Source": m.source, "Destination": m.target, "RW": !m.read_only})))
                .collect::<Vec<_>>(),
            // HostConfig round-trips the fidelity extras verbatim so docker clients diff them cleanly.
            "HostConfig": {"Binds": c.binds, "Memory": c.memory, "PidsLimit": c.pids_limit,
                "RestartPolicy": {"Name": c.restart_policy.name, "MaximumRetryCount": c.restart_policy.max_retry},
                "CapAdd": c.cap_add, "CapDrop": c.cap_drop, "Devices": c.devices, "Mounts": c.mounts,
                "Privileged": c.privileged, "SecurityOpt": c.security_opt},
            // NetworkSettings present so `docker port` (reads .NetworkSettings.Ports) doesn't panic.
            "NetworkSettings": {"Ports": ports_map_json(&c.publish), "IPAddress": primary.0, "Gateway": primary.1,
                "Networks": Value::Object(networks)}})).into_response()
        }
        None => no_such(&id) }
}

#[derive(Deserialize)]
pub(crate) struct PsQ { all: Option<String>, filters: Option<String>, size: Option<String> }

/// `docker ps --size` -> (SizeRw, SizeRootFs). dd runs a container directly in the (shared) image
/// rootfs with NO copy-on-write upper layer, so there is no isolated writable diff to measure: like
/// `docker diff` (see `containers_changes`, which reports no rootfs changes), SizeRw is 0. SizeRootFs is
/// the full rootfs `du`-style walk. The host-fs `macos` image (rootfs "/") is skipped -- walking it
/// would be catastrophic, exactly as `image_size` guards against.
fn container_sizes(c: &Container) -> (i64, i64) {
    if c.image == "macos" || c.rootfs.is_empty() || c.rootfs == "/" { return (0, 0); }
    (0, dir_size(std::path::Path::new(&c.rootfs)))
}

/// Apply `docker ps --filter`. `f` is the decoded `filters` map (`{"status":[..],"name":[..],"label":[..]}`).
/// Within a filter type the values are OR'd; `label` entries are AND'd (each must match). `name` is a
/// substring match against the container's effective name; `label` matches `key` or `key=value`.
/// `before_ts`/`since_ts` are the `created` timestamps of the containers named by `before=`/`since=`
/// (resolved by the caller, which holds the full container map); `None` => that key is absent/unresolved.
fn ps_match(c: &Container, name: &str, f: &HashMap<String, Vec<String>>, before_ts: Option<i64>, since_ts: Option<i64>) -> bool {
    if let Some(vals) = f.get("status") { if !vals.iter().any(|v| v == &c.status) { return false; } }
    if let Some(vals) = f.get("name") { if !vals.iter().any(|v| name.contains(v.as_str())) { return false; } }
    if let Some(vals) = f.get("label") {
        for v in vals {
            let ok = match v.split_once('=') {
                Some((k, val)) => c.labels.get(k).map(|cv| cv == val).unwrap_or(false),
                None => c.labels.contains_key(v),
            };
            if !ok { return false; }
        }
    }
    // `id=`: full-or-prefix match on the container id (docker accepts a leading prefix).
    if let Some(vals) = f.get("id") { if !vals.iter().any(|v| c.id.starts_with(v.as_str())) { return false; } }
    // `ancestor=`: the image the container was created from (repo[:tag] or a raw image ref).
    if let Some(vals) = f.get("ancestor") {
        if !vals.iter().any(|v| c.image == *v || ref_name(&c.image) == ref_name(v)) { return false; }
    }
    // `exited=N`: containers that exited with code N (only meaningful for the exited state).
    if let Some(vals) = f.get("exited") {
        if !vals.iter().any(|v| v.parse::<i64>().map_or(false, |n| c.status == "exited" && c.exit_code == n)) { return false; }
    }
    // `health=`: dd models no healthcheck, so every container is effectively `none`; any other value
    // (starting/healthy/unhealthy) matches nothing.
    if let Some(vals) = f.get("health") { if !vals.iter().any(|v| v == "none") { return false; } }
    // `before=`/`since=`: created strictly before / after the referenced container (by create time).
    if let Some(ts) = before_ts { if c.created >= ts { return false; } }
    if let Some(ts) = since_ts { if c.created <= ts { return false; } }
    true
}

/// Render a container's `docker ps` Status column the way docker does: "Up 3 minutes" while
/// running/paused, "Exited (0) 5 minutes ago" otherwise. The elapsed time is measured from the
/// container's `created` unix timestamp and humanized coarsely (seconds/minutes/hours/days).
fn human_status(c: &Container) -> String {
    let secs = (now_secs() - c.created).max(0);
    let dur = if secs < 60 { format!("{secs} seconds") }
        else if secs < 3600 { format!("{} minutes", secs / 60) }
        else if secs < 86400 { format!("{} hours", secs / 3600) }
        else { format!("{} days", secs / 86400) };
    if c.status == "running" || c.status == "paused" {
        format!("Up {dur}")
    } else {
        format!("Exited ({}) {dur} ago", c.exit_code)
    }
}

pub(crate) async fn containers_json(State(a): State<App>, Query(q): Query<PsQ>) -> Json<Value> {
    let all = matches!(q.all.as_deref(), Some("1") | Some("true") | Some("True"));
    // `filters` arrives URL-encoded JSON; axum has already percent-decoded it. Bad JSON => no filters.
    // Docker encodes it as map[key]->{value:true} (e.g. {"name":{"web":true}}), older clients as
    // map[key]->[value]. Accept BOTH: decode to a generic Value, normalize to key -> [values].
    let filters: HashMap<String, Vec<String>> = q.filters.as_deref()
        .and_then(|s| serde_json::from_str::<Value>(s).ok())
        .and_then(|v| v.as_object().map(|m| m.iter().map(|(k, val)| {
            let vals = match val {
                Value::Object(set) => set.keys().cloned().collect(),                 // {"web":true}
                Value::Array(a) => a.iter().filter_map(|x| x.as_str().map(String::from)).collect(), // ["web"]
                _ => vec![],
            };
            (k.clone(), vals)
        }).collect()))
        .unwrap_or_default();
    // A `status` filter implies "show all matching" (like `docker ps --filter status=exited`).
    let status_filter = filters.contains_key("status");
    // `--size`: docker only computes SizeRw/SizeRootFs on demand (a per-container rootfs walk is
    // expensive). Gather the matching containers, release the lock, THEN walk the disk so the daemon
    // lock isn't held across the (synchronous) `du`.
    let want_size = q_truthy(&q.size);
    let matched: Vec<Container> = {
        let g = a.inner.lock().await;
        // `before=`/`since=` name a reference container (by id-prefix or name); resolve each to that
        // container's `created` time so ps_match can compare create-order against it.
        let resolve = |key: &str| -> Option<i64> {
            filters.get(key).and_then(|vals| vals.first()).and_then(|r| {
                g.containers.values()
                    .find(|c| c.id.starts_with(r.as_str()) || &c.name == r)
                    .map(|c| c.created)
            })
        };
        let before_ts = resolve("before");
        let since_ts = resolve("since");
        // A before/since (like status) filter implies "show all matching", not just running.
        let order_filter = filters.contains_key("before") || filters.contains_key("since");
        g.containers.values()
            .filter(|c| all || status_filter || order_filter || c.status == "running")
            .filter(|c| {
                let name = if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() };
                ps_match(c, &name, &filters, before_ts, since_ts)
            })
            .cloned().collect()
    };
    let v: Vec<Value> = matched.iter().map(|c| {
        let mut entry = json!({
        "Id": c.id, "Image": c.image, "Command": c.cmd.join(" "), "Created": c.created,
        "State": c.status, "Status": human_status(c), "ExitCode": c.exit_code, "Ports": ports_json(&c.publish),
        "Labels": c.labels,
        "Mounts": c.binds.iter().filter_map(|b| parse_bind(b).map(|(s, d, ro)| json!({"Source": s, "Destination": d, "Type": "bind", "RW": !ro}))).collect::<Vec<_>>(),
        "Names": [format!("/{}", if c.name.is_empty() { c.id[..12.min(c.id.len())].to_string() } else { c.name.clone() })]});
        // Only emit the size keys when requested -- docker omits them otherwise.
        if want_size {
            let (rw, rootfs) = container_sizes(c);
            entry["SizeRw"] = json!(rw);
            entry["SizeRootFs"] = json!(rootfs);
        }
        entry
    }).collect();
    Json(json!(v))
}

// ---- prune / changes / export / update (Docker conformance additions) -------

/// `POST /containers/prune` — `docker container prune`. Removes exited (non-running) containers and
/// reports what was deleted.
pub(crate) async fn containers_prune(State(a): State<App>) -> Json<Value> {
    let mut g = a.inner.lock().await;
    let dead: Vec<String> = g.containers.iter()
        .filter(|(_, c)| c.status != "running" && c.status != "paused")
        .map(|(id, _)| id.clone()).collect();
    for id in &dead { g.containers.remove(id); g.live.remove(id); }
    save_state(&g, &a.state_path);
    Json(json!({"ContainersDeleted": dead, "SpaceReclaimed": 0}))
}

/// `GET /containers/{id}/changes` — `docker diff`. dd does not track rootfs diffs; report no changes
/// (correct shape: an array of `{Path, Kind}`, or `null` for none).
pub(crate) async fn containers_changes(State(a): State<App>, Path(id): Path<String>) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id) {
        Some(_) => Json(json!([])).into_response(),
        None => no_such(&id),
    }
}

/// `POST /containers/{id}/update` — `docker update`. dd does not apply live resource limits; accept
/// the request and return the conformant `{Warnings}` envelope.
pub(crate) async fn containers_update(State(a): State<App>, Path(id): Path<String>, _body: axum::body::Bytes) -> Response {
    let g = a.inner.lock().await;
    match resolve_cid(&g, &id) {
        Some(_) => Json(json!({"Warnings": []})).into_response(),
        None => no_such(&id),
    }
}

/// `GET /containers/{id}/export` — `docker export`. Streams a tar of the container rootfs.
pub(crate) async fn containers_export(State(a): State<App>, Path(id): Path<String>) -> Response {
    let rootfs = {
        let g = a.inner.lock().await;
        match resolve_cid(&g, &id).and_then(|f| g.containers.get(&f)).map(|c| c.rootfs.clone()) {
            Some(r) => r,
            None => return no_such(&id),
        }
    };
    match std::process::Command::new("tar").arg("cf").arg("-").arg("-C").arg(&rootfs).arg(".").output() {
        Ok(o) if o.status.success() => {
            Response::builder().status(StatusCode::OK).header("Content-Type", "application/x-tar")
                .body(Body::from(o.stdout)).unwrap()
        }
        _ => (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": "export failed"}))).into_response(),
    }
}
