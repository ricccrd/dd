#![allow(unused_imports, dead_code)]
use crate::model::*;
use crate::util::*;
use crate::system::*;
use crate::images::*;
use crate::containers::*;
use crate::archive::*;
use crate::volumes::*;
use crate::networks::*;
use crate::runtime::*;
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


// ===================== docker build — a minimal Dockerfile builder =====================
// Not BuildKit: we copy the base image's rootfs, run each RUN in the JIT (writes persist in the new
// rootfs), COPY from the build context, track ENV/WORKDIR/CMD/ENTRYPOINT, and register the result as an
// image. Reuses pull (base must be local), the JIT spawn, the archive path-mapper, and image registration.
#[derive(Deserialize)]
pub(crate) struct BuildQ {
    t: Option<String>,
    dockerfile: Option<String>,
    // `docker build --build-arg K=V` -> a URL-encoded JSON object, e.g. {"VERSION":"1.2"}
    buildargs: Option<String>,
    // `docker build --target <stage>` -> stop after this stage in a multi-stage build
    target: Option<String>,
    // `docker build --no-cache` -> "1"/"true"; bypasses the build layer cache entirely (see images_build)
    nocache: Option<String>,
    // `docker build --label K=V` -> a URL-encoded JSON object, e.g. {"team":"infra"}; applied over
    // any `LABEL` instructions in the Dockerfile.
    labels: Option<String>,
}


/// Substitute `${NAME}` / `$NAME` references in a Dockerfile line using the merged ARG map.
/// Unknown `${NAME}` expands to empty (like docker); unknown `$NAME` is left literal.
pub(crate) fn substitute_args(s: &str, map: &HashMap<String, String>) -> String {
    if map.is_empty() || !s.contains('$') { return s.to_string(); }
    let mut out = String::with_capacity(s.len());
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c != '$' { out.push(c); continue; }
        match chars.peek().copied() {
            Some('{') => {
                chars.next(); // consume '{'
                let mut name = String::new();
                while let Some(&nc) = chars.peek() {
                    chars.next();
                    if nc == '}' { break; }
                    name.push(nc);
                }
                if let Some(v) = map.get(&name) { out.push_str(v); }
            }
            Some(nc) if nc.is_ascii_alphanumeric() || nc == '_' => {
                let mut name = String::new();
                while let Some(&nc) = chars.peek() {
                    if nc.is_ascii_alphanumeric() || nc == '_' { name.push(nc); chars.next(); } else { break; }
                }
                match map.get(&name) {
                    Some(v) => out.push_str(v),
                    None => { out.push('$'); out.push_str(&name); }
                }
            }
            _ => out.push('$'),
        }
    }
    out
}


/// Parse a Dockerfile into (INSTRUCTION, args) pairs, honoring `\` line-continuations and `#` comments.
pub(crate) fn parse_dockerfile(text: &str) -> Vec<(String, String)> {
    let (mut out, mut acc) = (Vec::new(), String::new());
    for line in text.lines() {
        let l = line.trim_end();
        let t = l.trim_start();
        if acc.is_empty() && (t.is_empty() || t.starts_with('#')) { continue; }
        if let Some(s) = l.strip_suffix('\\') { acc.push_str(s.trim_start()); acc.push(' '); continue; }
        acc.push_str(t);
        if let Some((inst, args)) = acc.trim().split_once(char::is_whitespace) {
            out.push((inst.to_uppercase(), args.trim().to_string()));
        }
        acc.clear();
    }
    out
}


/// A `CMD`/`ENTRYPOINT` value: JSON-array exec form `["a","b"]` or a shell string (wrapped in sh -c).
pub(crate) fn parse_exec_form(args: &str) -> Vec<String> {
    let a = args.trim();
    if a.starts_with('[') {
        if let Ok(Value::Array(v)) = serde_json::from_str::<Value>(a) {
            return v.into_iter().filter_map(|x| x.as_str().map(String::from)).collect();
        }
    }
    vec!["/bin/sh".into(), "-c".into(), a.to_string()]
}


/// Parse a `LABEL` instruction's args into key/value pairs.
/// Modern form: `LABEL k=v k2="v 2" "com.x"="ACME Inc"` (one or more `key=value` pairs, values may be
/// quoted and contain spaces). Legacy form: `LABEL key the rest is the value` (no `=`, a single pair).
pub(crate) fn parse_labels(args: &str) -> Vec<(String, String)> {
    // tokenize on whitespace, honoring single/double quotes and backslash escapes.
    let (mut toks, mut cur, mut quote, mut had) = (Vec::<String>::new(), String::new(), '\0', false);
    let mut chars = args.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '\\' => { if let Some(&n) = chars.peek() { cur.push(n); chars.next(); had = true; } }
            '"' | '\'' => {
                if quote == c { quote = '\0'; } else if quote == '\0' { quote = c; } else { cur.push(c); }
                had = true;
            }
            c if c.is_whitespace() && quote == '\0' => { if had { toks.push(std::mem::take(&mut cur)); had = false; } }
            c => { cur.push(c); had = true; }
        }
    }
    if had { toks.push(cur); }

    // legacy single-pair form: no token carries an '='.
    if !toks.is_empty() && toks.iter().all(|t| !t.contains('=')) {
        let key = toks[0].clone();
        return if key.is_empty() { vec![] } else { vec![(key, toks[1..].join(" "))] };
    }
    // modern form: each token is `key=value`.
    toks.into_iter().filter_map(|t| t.split_once('=').and_then(|(k, v)|
        (!k.is_empty()).then(|| (k.to_string(), v.to_string())))).collect()
}


/// sha256 (lowercase hex, no `sha256:` prefix) of arbitrary bytes via the `sha256sum` CLI — already a
/// runtime dependency of the daemon (see registry.rs). Returns "" on failure (caller falls back).
fn sha256_hex(data: &[u8]) -> String {
    use std::io::Write;
    let mut child = match std::process::Command::new("sha256sum")
        .stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null()).spawn() {
        Ok(c) => c, Err(_) => return String::new(),
    };
    if let Some(mut si) = child.stdin.take() { let _ = si.write_all(data); }
    match child.wait_with_output() {
        Ok(o) => String::from_utf8_lossy(&o.stdout).split_whitespace().next().unwrap_or("").to_string(),
        Err(_) => String::new(),
    }
}

/// A deterministic content digest of an assembled rootfs: hash of a sorted (type,size,path) listing
/// combined with the sha256 of every regular file's contents. Same tree -> same hash, independent of
/// filesystem iteration order. Returns "" on failure.
fn rootfs_digest(rootfs: &std::path::Path) -> String {
    let script = format!(
        "cd '{}' 2>/dev/null || exit 0; \
         {{ find . -printf '%y %s %p\\n' 2>/dev/null | LC_ALL=C sort; \
            find . -type f -print0 2>/dev/null | LC_ALL=C sort -z | xargs -0 sha256sum 2>/dev/null; \
         }} | sha256sum",
        rootfs.display());
    match std::process::Command::new("sh").arg("-c").arg(&script).output() {
        Ok(o) => String::from_utf8_lossy(&o.stdout).split_whitespace().next().unwrap_or("").to_string(),
        Err(_) => String::new(),
    }
}


// ===================== build layer cache =====================
// A conservative, content-addressed reimplementation of Docker's classic build cache. Each step gets a
// `cache id` = sha256(parent step's cache id + a normalized descriptor of the instruction). For COPY/ADD
// the descriptor folds in a content+metadata digest of the source files, so changed context invalidates;
// for everything else it is the (ARG-substituted) instruction text. The rootfs produced AFTER a step is
// snapshotted under ~/.dd/buildcache/layers/<cache-id>/rootfs (filesystem-mutating steps only) alongside a
// meta.json capturing the cumulative image config, so a future rebuild can REUSE the snapshot+config
// instead of re-running. CORRECTNESS RULE: a hit replays the exact rootfs a prior run of the identical
// (parent+instruction[+context]) step recorded — bit-identical to that run; anything we cannot prove
// identical misses and re-runs. The first miss invalidates the cache for the rest of the stage (Docker
// semantics — and the content-chained ids enforce it automatically).

/// Instructions that mutate the rootfs (so their cache layer needs a full snapshot). Everything else is
/// config-only (ENV/CMD/ENTRYPOINT/LABEL/EXPOSE/USER/...) and stores just metadata.
fn is_fs_inst(inst: &str) -> bool { matches!(inst, "RUN" | "COPY" | "ADD" | "WORKDIR") }

fn bc_layer_dir(cache_id: &str) -> PathBuf { crate::util::buildcache_dir().join("layers").join(cache_id) }

/// Deterministic content+metadata digest of a file or directory subtree at `p` (absolute host path):
/// type, mode and size of every entry plus the sha256 of each regular file's contents, sorted so it is
/// independent of fs iteration order. Used to make COPY/ADD cache keys content-addressed. Returns "" on
/// failure (the caller then forces a miss rather than risk serving a stale layer).
fn path_digest(p: &std::path::Path) -> String {
    let script = format!(
        "p='{}'; if [ -d \"$p\" ]; then cd \"$p\" 2>/dev/null || exit 0; \
            {{ find . -printf '%y %m %s %p\\n' 2>/dev/null | LC_ALL=C sort; \
               find . -type f -print0 2>/dev/null | LC_ALL=C sort -z | xargs -0 sha256sum 2>/dev/null; }} | sha256sum; \
         elif [ -e \"$p\" ]; then {{ stat -c '%F %a %s' \"$p\" 2>/dev/null; sha256sum \"$p\" 2>/dev/null; }} | sha256sum; \
         else echo missing; fi",
        p.display());
    match std::process::Command::new("sh").arg("-c").arg(&script).output() {
        Ok(o) => String::from_utf8_lossy(&o.stdout).split_whitespace().next().unwrap_or("").to_string(),
        Err(_) => String::new(),
    }
}

/// Chain hash for a step's cache id: sha256(parent + descriptor), falling back to a stable non-crypto id
/// if `sha256sum` is unavailable so the cache still keys deterministically (never an empty/colliding id).
fn cache_id(parent: &str, descriptor: &str) -> String {
    let seed = format!("{parent}\n{descriptor}");
    let h = sha256_hex(seed.as_bytes());
    if h.len() == 64 { h } else { fake_id(&seed) }
}

/// Load a cache layer's metadata iff it is present AND complete (an fs layer's rootfs snapshot must
/// exist). Returns None on any miss so a partial/corrupt layer is never served as a hit.
fn load_layer(id: &str) -> Option<Value> {
    let dir = bc_layer_dir(id);
    let meta: Value = serde_json::from_slice(&std::fs::read(dir.join("meta.json")).ok()?).ok()?;
    if meta.get("fs").and_then(|v| v.as_bool()).unwrap_or(false) && !dir.join("rootfs").is_dir() { return None; }
    Some(meta)
}

/// Materialize a cached fs layer's rootfs snapshot into `dst` (the live stage rootfs), replacing it.
/// Returns false on failure — the caller aborts the build rather than continue on a wrong rootfs.
fn materialize(id: &str, dst: &std::path::Path) -> bool {
    let src = bc_layer_dir(id).join("rootfs");
    if !src.is_dir() { return false; }
    let _ = std::fs::remove_dir_all(dst);
    if let Some(parent) = dst.parent() { let _ = std::fs::create_dir_all(parent); }
    matches!(std::process::Command::new("cp").arg("-a").arg(&src).arg(dst).status(), Ok(s) if s.success())
}

/// Persist a freshly executed step as a cache layer: a full rootfs snapshot for filesystem-mutating
/// instructions, plus a meta.json sidecar capturing the cumulative image config so a future hit can
/// restore it without re-running. Atomic & best-effort: the snapshot is written first and meta.json LAST,
/// so a layer only becomes loadable once complete; a failed snapshot leaves no (false-hit) layer behind.
#[allow(clippy::too_many_arguments)]
fn store_layer(id: &str, parent: &str, inst: &str, args: &str, rootfs: &std::path::Path,
               cmd: &[String], entrypoint: &[String], workdir: &str, env: &[String],
               labels: &HashMap<String, String>) {
    let dir = bc_layer_dir(id);
    let _ = std::fs::remove_dir_all(&dir);
    if std::fs::create_dir_all(&dir).is_err() { return; }
    let fs = is_fs_inst(inst);
    if fs {
        let lr = dir.join("rootfs");
        if !matches!(std::process::Command::new("cp").arg("-a").arg(rootfs).arg(&lr).status(), Ok(s) if s.success()) {
            let _ = std::fs::remove_dir_all(&dir); return;
        }
    }
    let meta = json!({"v": 1, "parent": parent, "inst": inst, "args": args, "fs": fs,
        "created": now_secs(), "cmd": cmd, "entrypoint": entrypoint, "workdir": workdir,
        "env": env, "labels": labels});
    let tmp = dir.join(".meta.json.tmp");
    if std::fs::write(&tmp, meta.to_string()).is_ok() && std::fs::rename(&tmp, dir.join("meta.json")).is_ok() {
        return;
    }
    let _ = std::fs::remove_dir_all(&dir);
}


pub(crate) fn build_stream(lines: Vec<String>) -> Response {
    (StatusCode::OK, [("Content-Type", "application/json")], lines.join("\n") + "\n").into_response()
}

pub(crate) fn build_err(mut lines: Vec<String>, msg: String) -> Response {
    lines.push(json!({"errorDetail": {"message": msg.clone()}, "error": msg}).to_string());
    build_stream(lines)
}


pub(crate) async fn images_build(State(a): State<App>, Query(q): Query<BuildQ>, body: axum::body::Bytes) -> Response {
    let raw_tag = q.t.clone().filter(|t| !t.is_empty()).unwrap_or_else(|| "built:latest".into());
    let name = ref_name(&raw_tag);
    let dfname = q.dockerfile.filter(|d| !d.is_empty()).unwrap_or_else(|| "Dockerfile".into());
    let mut log: Vec<String> = Vec::new();

    // --build-arg: decode the JSON object (values may be null) into a name->value map.
    let buildargs: HashMap<String, String> = q.buildargs.as_deref()
        .filter(|s| !s.is_empty())
        .and_then(|s| serde_json::from_str::<HashMap<String, Option<String>>>(s).ok())
        .map(|m| m.into_iter().filter_map(|(k, v)| v.map(|v| (k, v))).collect())
        .unwrap_or_default();
    // --target: name of the stage to stop at (empty = build every stage, as before).
    let target = q.target.clone().unwrap_or_default();
    // --no-cache: bypass the build layer cache entirely (never read, never write) — a from-scratch build
    // identical to the pre-cache behavior. Otherwise the per-step layer cache is active (see below).
    let nocache = matches!(q.nocache.as_deref(), Some("1") | Some("true"));
    let use_cache = !nocache;

    // unpack the build context (a tar in the request body)
    let ctx = std::path::PathBuf::from(format!("{}/.build-ctx-{}", a.images_dir, std::process::id()));
    let _ = std::fs::remove_dir_all(&ctx);
    if std::fs::create_dir_all(&ctx).is_err() { return build_err(log, "cannot create build dir".into()); }
    let ctar = ctx.join(".context.tar");
    let cleanup = |ctx: &std::path::Path| { let _ = std::fs::remove_dir_all(ctx); };
    if std::fs::write(&ctar, &body).is_err() { cleanup(&ctx); return build_err(log, "cannot write context".into()); }
    if !matches!(std::process::Command::new("tar").arg("xf").arg(&ctar).arg("-C").arg(&ctx).status(), Ok(s) if s.success()) {
        cleanup(&ctx); return build_err(log, "cannot unpack build context".into());
    }
    let dockerfile = match std::fs::read_to_string(ctx.join(&dfname)) {
        Ok(d) => d, Err(_) => { cleanup(&ctx); return build_err(log, format!("Cannot locate specified Dockerfile: {dfname}")); }
    };
    let steps = parse_dockerfile(&dockerfile);
    let total = steps.len();

    // the new image's rootfs dir under DD_IMAGES
    let safe: String = name.chars().map(|c| if c.is_alphanumeric() || "._-".contains(c) { c } else { '_' }).collect();
    let img_dir = std::path::PathBuf::from(format!("{}/{}", a.images_dir, safe));
    let _ = std::fs::remove_dir_all(&img_dir);
    let mut rootfs = img_dir.join("rootfs"); // the CURRENT stage's rootfs (reassigned at each FROM)
    let mut stages: Vec<std::path::PathBuf> = Vec::new();      // stage index -> its rootfs (multi-stage)
    let mut stage_names: HashMap<String, usize> = HashMap::new(); // name/index -> stage index

    // image config built up across the instructions (inherited from the base at FROM, then mutated)
    let (mut arch, mut cmd, mut entrypoint, mut workdir, mut env, mut from_done) =
        (Guest::LinuxAarch64, Vec::<String>::new(), Vec::<String>::new(), String::new(), Vec::<String>::new(), false);

    // merged ARG map: Dockerfile `ARG` defaults, overridden by --build-arg values (filled as ARG steps run).
    let mut args_map: HashMap<String, String> = HashMap::new();
    // image labels accumulated from `LABEL` instructions (per-stage; cleared at each FROM). The
    // `--label` build option is merged on top after the loop.
    let mut labels: HashMap<String, String> = HashMap::new();
    // set once the --target stage has been fully built, so the next FROM stops the build.
    let mut target_built = false;

    // --- build layer cache chain state (reset at each FROM) ---
    let mut parent_id = String::new();        // cache id of the previous step (seeded from the base at FROM)
    let mut cache_ok = false;                 // false after the first miss: no more hits this stage (Docker rule)
    let mut pending_fs: Option<String> = None; // a cache-hit fs layer whose rootfs restore is deferred (lazy)
    // a per-build nonce, mixed into a COPY/ADD key only when its source digest is unavailable, so we force
    // a miss instead of risking a stale layer we cannot prove identical.
    let nonce = format!("nonce:{}", SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos()).unwrap_or(0));

    for (i, (inst, args)) in steps.iter().enumerate() {
        // expand ${ARG}/$ARG using the merged map before logging or executing the step.
        let args = substitute_args(args, &args_map);
        log.push(json!({"stream": format!("Step {}/{} : {} {}\n", i + 1, total, inst, args)}).to_string());

        // ----- build layer cache: try to reuse this step's recorded layer -----
        // `current_cid` is set when we are going to EXECUTE the step (a miss) and must store the result.
        let mut current_cid: Option<String> = None;
        if use_cache && from_done && inst != "FROM" {
            // descriptor = normalized instruction; COPY/ADD fold in a content digest of each source so a
            // changed build context invalidates; ARG folds in its *resolved* value so --build-arg changes
            // invalidate the rest of the build even when the arg is unreferenced.
            let desc = match inst.as_str() {
                "COPY" | "ADD" => {
                    let from_stage = args.split_whitespace().find_map(|p| p.strip_prefix("--from="));
                    let parts: Vec<&str> = args.split_whitespace().filter(|p| !p.starts_with("--")).collect();
                    let mut d = format!("{inst} {args}");
                    if parts.len() >= 2 {
                        let src_root = match from_stage {
                            Some(s) => stage_names.get(s).map(|&idx| stages[idx].clone()),
                            None => Some(ctx.clone()),
                        };
                        match src_root {
                            Some(root) => for src in &parts[..parts.len() - 1] {
                                let sp = if from_stage.is_some() {
                                    archive_host_path(&root.to_string_lossy(), &[], "", src)
                                } else { root.join(src) };
                                let dg = path_digest(&sp);
                                d.push('\n');
                                d.push_str(if dg.is_empty() { &nonce } else { &dg });
                            },
                            None => d.push_str("\n?unknown-stage"),
                        }
                    }
                    d
                }
                "ARG" => {
                    let spec = args.split_whitespace().next().unwrap_or("");
                    let kv = match spec.split_once('=') {
                        Some((k, v)) => format!("{k}={}", buildargs.get(k).cloned().unwrap_or_else(|| v.to_string())),
                        None => match buildargs.get(spec) { Some(v) => format!("{spec}={v}"), None => spec.to_string() },
                    };
                    format!("ARG {kv}")
                }
                _ => format!("{inst} {args}"),
            };
            let cid = cache_id(&parent_id, &desc);
            if inst == "ARG" {
                // ARG is transparent to the fs/config cache: it advances the chain but always runs (so
                // args_map stays live for downstream substitution) and stores no layer of its own.
                parent_id = cid;
            } else {
                let hit = if cache_ok { load_layer(&cid) } else { None };
                if let Some(meta) = hit {
                    // HIT — replay the recorded config now; defer the rootfs restore (a run of consecutive
                    // hits costs zero copies). The rootfs is materialized on the first miss / stage finalize
                    // from `pending_fs` (the latest hit fs layer, whose snapshot is cumulative).
                    log.push(json!({"stream": " ---> Using cache\n"}).to_string());
                    let arr = |k: &str| meta.get(k).and_then(|v| v.as_array())
                        .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect::<Vec<_>>()).unwrap_or_default();
                    cmd = arr("cmd"); entrypoint = arr("entrypoint"); env = arr("env");
                    workdir = meta.get("workdir").and_then(|v| v.as_str()).unwrap_or("").to_string();
                    labels = meta.get("labels").and_then(|v| v.as_object())
                        .map(|o| o.iter().filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string()))).collect())
                        .unwrap_or_default();
                    if is_fs_inst(inst) { pending_fs = Some(cid.clone()); }
                    parent_id = cid;
                    continue; // skip executing the instruction
                }
                // MISS — invalidate the cache for the rest of the stage and restore the real rootfs (if a
                // prior hit deferred it) before executing this step.
                cache_ok = false;
                if let Some(fsid) = pending_fs.take() {
                    if !materialize(&fsid, &rootfs) {
                        cleanup(&ctx); return build_err(log, "build cache: failed to restore a cached layer".into());
                    }
                }
                current_cid = Some(cid);
            }
        }

        match inst.as_str() {
            "FROM" => {
                // --target: the target stage is fully built; don't start any later stage.
                if target_built { break; }
                // finalize the previous stage's rootfs from any deferred cache layer before starting a new
                // one, so a later COPY --from=<that stage> sees its complete contents.
                if use_cache { if let Some(fsid) = pending_fs.take() {
                    if !materialize(&fsid, &rootfs) {
                        cleanup(&ctx); return build_err(log, "build cache: failed to restore a stage layer".into());
                    }
                }}
                let base = args.split_whitespace().next().unwrap_or("").to_string();
                let pick = |im: &Image| (im.rootfs.clone(), im.arch, im.cmd.clone(), im.entrypoint.clone(), im.env.clone(), im.workdir.clone());
                let mut found = { let g = a.inner.lock().await;
                    g.images.iter().find(|im| ref_name(&im.name) == ref_name(&base)).map(&pick) };
                if found.is_none() {
                    // not local -> auto-pull the base like real docker build (reuses the registry pull)
                    log.push(json!({"stream": format!("Unable to find image '{base}' locally; pulling\n")}).to_string());
                    let (n, t) = match base.rsplit_once(':') {
                        Some((n, t)) if !t.contains('/') => (n.to_string(), t.to_string()),
                        _ => (base.clone(), "latest".to_string()),
                    };
                    let (dir, archs) = (a.images_dir.clone(), platform_archs(None));
                    match tokio::task::spawn_blocking(move || pull_image(&dir, &n, &t, Credentials::default(), &archs)).await
                        .unwrap_or_else(|e| Err(format!("pull task crashed: {e}"))) {
                        Ok(img) => { found = Some(pick(&img)); a.inner.lock().await.images.push(img); }
                        Err(e) => { cleanup(&ctx); return build_err(log, format!("pull of base image '{base}' failed: {e}")); }
                    }
                }
                let Some((base_rootfs, base_arch, base_cmd, base_ep, base_env, base_wd)) = found else {
                    cleanup(&ctx); return build_err(log, format!("base image '{base}' unavailable")); };
                arch = base_arch; cmd = base_cmd; entrypoint = base_ep; env = base_env; workdir = base_wd; // inherit base config
                labels.clear(); // labels are per-stage; base-image label inheritance is not modeled
                // start a new build stage (its own rootfs); `FROM <base> AS <name>` names it.
                let sidx = stages.len();
                rootfs = img_dir.join(format!("_s{sidx}")).join("rootfs");
                stages.push(rootfs.clone());
                stage_names.insert(sidx.to_string(), sidx);
                let words: Vec<&str> = args.split_whitespace().collect();
                if let Some(nm) = words.iter().position(|w| w.eq_ignore_ascii_case("AS")).and_then(|i| words.get(i + 1)) {
                    stage_names.insert(nm.to_string(), sidx);
                    // mark this stage so the next FROM stops the build (--target reached).
                    if !target.is_empty() && *nm == target.as_str() { target_built = true; }
                }
                std::fs::create_dir_all(rootfs.parent().unwrap_or(&img_dir)).ok();
                if !matches!(std::process::Command::new("cp").arg("-a").arg(&base_rootfs).arg(&rootfs).status(), Ok(s) if s.success()) {
                    cleanup(&ctx); return build_err(log, "failed to copy base image rootfs".into()); }
                from_done = true;
                // (re)seed the per-stage cache chain from the base image's *content* digest, so a changed
                // base (re-pulled/rebuilt) invalidates the whole stage. cache_ok re-enables hits.
                if use_cache {
                    let seed = format!("FROM {base}\n{}", rootfs_digest(std::path::Path::new(&base_rootfs)));
                    parent_id = cache_id("", &seed);
                    cache_ok = true;
                    pending_fs = None;
                }
            }
            "ARG" => {
                // `ARG NAME` or `ARG NAME=default`; --build-arg overrides the default. Allowed before FROM.
                let spec = args.split_whitespace().next().unwrap_or("");
                if let Some((k, v)) = spec.split_once('=') {
                    let val = buildargs.get(k).cloned().unwrap_or_else(|| v.to_string());
                    if !k.is_empty() { args_map.insert(k.to_string(), val); }
                } else if !spec.is_empty() {
                    if let Some(v) = buildargs.get(spec) { args_map.insert(spec.to_string(), v.clone()); }
                }
            }
            _ if !from_done => { cleanup(&ctx); return build_err(log, "no FROM before the first instruction".into()); }
            "RUN" => {
                let mut cfg = SpawnConfig::new(workdir.clone(), rootfs.to_string_lossy().into_owned());
                cfg.env = env.iter().filter_map(|e| e.split_once('=').map(|(k, v)| (k.to_string(), v.to_string()))).collect();
                cfg.argv = vec!["/bin/sh".into(), "-c".into(), args.clone()];
                let Some((prog, cargs)) = cfg.command(arch) else { cleanup(&ctx); return build_err(log, "JIT not available for this arch".into()); };
                match tokio::process::Command::new(prog).args(cargs).output().await {
                    Ok(o) => {
                        if !o.stdout.is_empty() { log.push(json!({"stream": String::from_utf8_lossy(&o.stdout)}).to_string()); }
                        if !o.stderr.is_empty() { log.push(json!({"stream": String::from_utf8_lossy(&o.stderr)}).to_string()); }
                        if !o.status.success() {
                            cleanup(&ctx); return build_err(log, format!("The command '/bin/sh -c {}' returned a non-zero code: {}", args, o.status.code().unwrap_or(-1))); }
                    }
                    Err(e) => { cleanup(&ctx); return build_err(log, format!("RUN failed to start: {e}")); }
                }
            }
            "COPY" | "ADD" => {
                let from_stage = args.split_whitespace().find_map(|p| p.strip_prefix("--from="));
                let parts: Vec<&str> = args.split_whitespace().filter(|p| !p.starts_with("--")).collect();
                if parts.len() < 2 { cleanup(&ctx); return build_err(log, format!("{inst} needs a source and destination")); }
                let dst = parts[parts.len() - 1];
                let dst_guest = if dst.starts_with('/') { dst.to_string() } else { format!("{}/{}", workdir.trim_end_matches('/'), dst) };
                let dst_host = archive_host_path(&rootfs.to_string_lossy(), &[], "", &dst_guest);
                let into_dir = dst.ends_with('/') || parts.len() > 2;
                if into_dir { std::fs::create_dir_all(&dst_host).ok(); } else if let Some(p) = dst_host.parent() { std::fs::create_dir_all(p).ok(); }
                // COPY --from=<stage>: source is a path inside that stage's rootfs; else the build context.
                let src_root = match from_stage {
                    Some(s) => match stage_names.get(s) { Some(&idx) => stages[idx].clone(),
                        None => { cleanup(&ctx); return build_err(log, format!("COPY --from: unknown stage '{s}'")); } },
                    None => ctx.clone(),
                };
                for src in &parts[..parts.len() - 1] {
                    let src_host = if from_stage.is_some() { archive_host_path(&src_root.to_string_lossy(), &[], "", src) } else { src_root.join(src) };
                    if !matches!(std::process::Command::new("cp").arg("-a").arg(&src_host).arg(&dst_host).status(), Ok(s) if s.success()) {
                        cleanup(&ctx); return build_err(log, format!("{inst} {src}: not found")); }
                }
            }
            "ENV" => {
                // `ENV K V` or `ENV K=V`; stored as "K=V"
                let kv = if let Some((k, v)) = args.split_once('=') {
                    format!("{}={}", k.trim(), v.split_whitespace().next().unwrap_or("").trim_matches('"'))
                } else if let Some((k, v)) = args.split_once(char::is_whitespace) {
                    format!("{}={}", k.trim(), v.trim().trim_matches('"'))
                } else { String::new() };
                if !kv.is_empty() { env.retain(|e| e.split_once('=').map(|(k, _)| k) != kv.split_once('=').map(|(k, _)| k)); env.push(kv); }
            }
            "WORKDIR" => {
                workdir = if args.starts_with('/') { args.clone() } else { format!("{}/{}", workdir.trim_end_matches('/'), args) };
                let wh = archive_host_path(&rootfs.to_string_lossy(), &[], "", &workdir);
                std::fs::create_dir_all(&wh).ok();
            }
            "CMD" => cmd = parse_exec_form(&args),
            "ENTRYPOINT" => entrypoint = parse_exec_form(&args),
            "LABEL" => for (k, v) in parse_labels(&args) { labels.insert(k, v); },
            _ => {} // EXPOSE/MAINTAINER/USER/VOLUME/HEALTHCHECK — no rootfs effect in this builder
        }

        // Step executed (a cache miss): record its result as a layer for future rebuilds and advance the
        // chain. fs-mutating steps snapshot the live rootfs; config-only steps store just their metadata.
        if let Some(cid) = current_cid.take() {
            store_layer(&cid, &parent_id, inst, &args, &rootfs, &cmd, &entrypoint, &workdir, &env, &labels);
            parent_id = cid;
        }
    }
    if !from_done { cleanup(&ctx); return build_err(log, "Dockerfile had no FROM".into()); }
    cleanup(&ctx);
    // finalize the final stage's rootfs from any deferred cache layer (a build that ended on a run of
    // cache hits never materialized it).
    if use_cache { if let Some(fsid) = pending_fs.take() {
        if !materialize(&fsid, &rootfs) { return build_err(log, "build cache: failed to restore the final layer".into()); }
    }}

    // the LAST stage is the final image: move its rootfs to <img>/rootfs, drop the intermediate stages.
    let final_rootfs = stages.last().cloned().unwrap_or_else(|| rootfs.clone());
    let image_rootfs = img_dir.join("rootfs");
    if final_rootfs != image_rootfs {
        let _ = std::fs::remove_dir_all(&image_rootfs);
        if std::fs::rename(&final_rootfs, &image_rootfs).is_err() {
            let _ = std::process::Command::new("cp").arg("-a").arg(&final_rootfs).arg(&image_rootfs).status();
        }
    }
    for s in &stages { if let Some(p) = s.parent() { if p != img_dir { let _ = std::fs::remove_dir_all(p); } } }
    let rootfs = image_rootfs; // the registered image's rootfs

    // `docker build --label K=V` -> the `labels` query param (a JSON object), merged on top of any
    // `LABEL` instructions so a CLI flag wins over the Dockerfile, matching docker.
    if let Some(extra) = q.labels.as_deref().filter(|s| !s.is_empty())
        .and_then(|s| serde_json::from_str::<HashMap<String, String>>(s).ok()) {
        for (k, v) in extra { labels.insert(k, v); }
    }

    // register the built image (persist the full config so it survives a daemon restart)
    if cmd.is_empty() && entrypoint.is_empty() { cmd = default_shell(&rootfs); }
    std::fs::write(img_dir.join("dd-image.json"),
        json!({"name": name, "cmd": cmd, "entrypoint": entrypoint, "env": env, "workdir": workdir, "labels": labels}).to_string()).ok();

    // a real content digest for the image ID: sha256 over the image's defining content — the Dockerfile,
    // a deterministic content hash of the assembled rootfs, and the final config (incl. sorted labels, so
    // the digest is reproducible: HashMap iteration order must not leak in). Same inputs -> same ID.
    let id = {
        let mut lbl: Vec<(&String, &String)> = labels.iter().collect();
        lbl.sort();
        let labels_str = lbl.iter().map(|(k, v)| format!("{k}={v}")).collect::<Vec<_>>().join("\n");
        let manifest = format!(
            "dockerfile:\n{dockerfile}\nrootfs:{}\ncmd:{}\nentrypoint:{}\nenv:{}\nworkdir:{workdir}\nlabels:\n{labels_str}",
            rootfs_digest(&rootfs), cmd.join("\u{1}"), entrypoint.join("\u{1}"), env.join("\u{1}"));
        let h = sha256_hex(manifest.as_bytes());
        if h.len() == 64 { h } else { fake_id(&manifest) } // fallback keeps a deterministic id if sha256sum is missing
    };
    {
        let mut g = a.inner.lock().await;
        g.images.retain(|im| ref_name(&im.name) != name);
        g.images.push(Image { name: name.to_string(), rootfs: rootfs.to_string_lossy().into_owned(), arch, cmd, entrypoint, env, workdir, labels, created: now_secs() });
    }
    log.push(json!({"stream": format!("Successfully built {}\n", &id[..12.min(id.len())])}).to_string());
    log.push(json!({"stream": format!("Successfully tagged {raw_tag}\n")}).to_string());
    log.push(json!({"aux": {"ID": format!("sha256:{id}")}}).to_string());
    build_stream(log)
}

/// `POST /build/prune` — `docker builder prune` / the build-cache portion of `docker system prune`.
/// Reclaims BOTH dd build-cache slots: the new per-step layer cache (~/.dd/buildcache, populated by
/// `docker build`) and the persistent JIT translated-code cache (~/.dd/pcache, surfaced as `system df`
/// BuilderSize). Both are fully reclaimable — layers re-snapshot on the next build, pcache re-translates
/// on demand — so a wholesale drop only forces a one-time recompute.
pub(crate) async fn build_prune() -> axum::Json<serde_json::Value> {
    let (mut deleted, mut reclaimed) = (Vec::new(), 0i64);
    // 1) the build layer cache: one dir per cached step under ~/.dd/buildcache/layers.
    let layers = crate::util::buildcache_dir().join("layers");
    if let Ok(rd) = std::fs::read_dir(&layers) {
        for e in rd.filter_map(|e| e.ok()) {
            let sz = crate::util::dir_size(&e.path());
            if std::fs::remove_dir_all(e.path()).is_ok() {
                reclaimed += sz;
                deleted.push(format!("buildcache:{}", e.file_name().to_string_lossy()));
            }
        }
    }
    // 2) the persistent JIT translated-code cache: one <binid>.pcache file per guest binary.
    let dir = crate::util::dd_home().join("pcache");
    if let Ok(rd) = std::fs::read_dir(&dir) {
        for e in rd.filter_map(|e| e.ok()) {
            let sz = e.metadata().ok().filter(|m| m.is_file()).map(|m| m.len() as i64);
            if let Some(sz) = sz {
                if std::fs::remove_file(e.path()).is_ok() {
                    reclaimed += sz;
                    deleted.push(e.file_name().to_string_lossy().into_owned());
                }
            }
        }
    }
    axum::Json(serde_json::json!({"CachesDeleted": deleted, "SpaceReclaimed": reclaimed}))
}


#[derive(Deserialize)]
pub(crate) struct CommitQ {
    container: Option<String>,
    repo: Option<String>,
    tag: Option<String>,
    comment: Option<String>,
    author: Option<String>,
    pause: Option<String>,
}

/// `POST /commit?container=<id>&repo=<r>&tag=<t>` — `docker commit`. Snapshots the container's CURRENT
/// rootfs into a new image directory under `images_dir` and registers it as `repo:tag` carrying the
/// container's run config (cmd/env/workdir/labels, plus the source image's entrypoint). dd runs a
/// container directly in the (shared) image rootfs with no copy-on-write upper, so the snapshot is a
/// `cp -a` of the live rootfs — it captures every write the container made, matching `docker commit`'s
/// "freeze the current filesystem" semantics. Reuses the same registration path as build/load/import.
pub(crate) async fn commit_container(State(a): State<App>, Query(q): Query<CommitQ>) -> Response {
    let cid = q.container.unwrap_or_default();
    if cid.is_empty() {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "container is required for commit"}))).into_response();
    }
    // Resolve the source container; carry its run config into the new image. The container only stores
    // the resolved cmd/env it ran with, so inherit the source image's ENTRYPOINT (and arch as a fallback).
    let (rootfs, cmd, entrypoint, env, workdir, labels, arch) = {
        let g = a.inner.lock().await;
        let Some(full) = resolve_cid(&g, &cid) else { return no_such(&cid) };
        let Some(c) = g.containers.get(&full) else { return no_such(&cid) };
        let img = g.images.iter().find(|i| ref_name(&i.name) == ref_name(&c.image)).cloned();
        let entrypoint = img.as_ref().map(|i| i.entrypoint.clone()).unwrap_or_default();
        let arch = c.arch.or(img.as_ref().map(|i| i.arch)).unwrap_or(Guest::LinuxAarch64);
        (c.rootfs.clone(), c.cmd.clone(), entrypoint, c.env.clone(), c.working_dir.clone(), c.labels.clone(), arch)
    };
    // The host-fs `macos` image (rootfs "/") can't be snapshotted — copying it would be catastrophic.
    if rootfs.is_empty() || rootfs == "/" {
        return (StatusCode::BAD_REQUEST, Json(json!({"message": "cannot commit a container with a host-filesystem rootfs"}))).into_response();
    }
    // repo[:tag]. Docker defaults the tag to "latest"; an empty repo commits a dangling <none> image.
    let repo = q.repo.unwrap_or_default();
    let tag = q.tag.filter(|t| !t.is_empty()).unwrap_or_else(|| "latest".into());
    let name = if repo.is_empty() { String::new() } else { format!("{}:{}", repo.trim_end_matches(':'), tag) };
    let key = if name.is_empty() { format!("commit-{}", &fake_id(&cid)[..12]) } else { name.clone() };
    // Snapshot the rootfs into a fresh image dir (mirrors image_load/import's <images_dir>/<sanitized>).
    let target = PathBuf::from(format!("{}/{}", a.images_dir, key.replace(['/', ':'], "_")));
    let new_rootfs = target.join("rootfs");
    let _ = std::fs::remove_dir_all(&target);
    if let Err(e) = std::fs::create_dir_all(&target) {
        return (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": format!("commit: {e}")}))).into_response();
    }
    // `cp -a SRC DST` (DST absent) copies the SRC tree to DST, preserving perms/links/timestamps.
    let cp = std::process::Command::new("cp").arg("-a").arg(&rootfs).arg(&new_rootfs).status();
    if !matches!(cp, Ok(s) if s.success()) {
        let _ = std::fs::remove_dir_all(&target);
        return (StatusCode::INTERNAL_SERVER_ERROR, Json(json!({"message": "commit: failed to snapshot rootfs"}))).into_response();
    }
    // A content-ish id (deterministic per source + time so repeated commits get distinct ids).
    let id = {
        let manifest = format!("commit:{cid}\nname:{key}\ncmd:{}\nenv:{}\nworkdir:{workdir}\nat:{}",
            cmd.join("\u{1}"), env.join("\u{1}"), now_secs());
        let h = sha256_hex(manifest.as_bytes());
        if h.len() == 64 { h } else { fake_id(&manifest) }
    };
    // Persist a dd-image.json so the image survives a daemon restart (discover_images reads it).
    let mut dd = json!({"name": key, "cmd": cmd, "entrypoint": entrypoint, "env": env, "workdir": workdir, "labels": labels});
    if let Some(c) = &q.comment { dd["comment"] = json!(c); }
    if let Some(a) = &q.author { dd["author"] = json!(a); }
    let _ = std::fs::write(target.join("dd-image.json"), dd.to_string());
    let _ = &q.pause; // accepted for CLI compatibility; dd's snapshot doesn't need to freeze the guest.
    {
        let mut g = a.inner.lock().await;
        // Replace any existing image sharing this repo:tag (mirrors the build/load re-tag dedupe).
        if !key.is_empty() { g.images.retain(|im| im.name != key); }
        g.images.push(Image {
            name: key.clone(), rootfs: new_rootfs.to_string_lossy().into_owned(), arch,
            cmd, entrypoint, env, workdir, labels, created: now_secs(),
        });
    }
    crate::events::emit_event(&a.events, "image", "commit", &id, json!({"name": key}));
    (StatusCode::CREATED, Json(json!({"Id": format!("sha256:{id}")}))).into_response()
}
