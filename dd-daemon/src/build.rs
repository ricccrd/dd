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
    // `docker build --no-cache` -> "1"/"true"; dd has no layer cache so this is a parsed no-op
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
    // nocache: dd has no layer cache yet, every build is already from-scratch — parse & ignore.
    let _nocache = matches!(q.nocache.as_deref(), Some("1") | Some("true"));

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

    for (i, (inst, args)) in steps.iter().enumerate() {
        // expand ${ARG}/$ARG using the merged map before logging or executing the step.
        let args = substitute_args(args, &args_map);
        log.push(json!({"stream": format!("Step {}/{} : {} {}\n", i + 1, total, inst, args)}).to_string());
        match inst.as_str() {
            "FROM" => {
                // --target: the target stage is fully built; don't start any later stage.
                if target_built { break; }
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
    }
    if !from_done { cleanup(&ctx); return build_err(log, "Dockerfile had no FROM".into()); }
    cleanup(&ctx);

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

/// `POST /build/prune` — `docker builder prune`. dd has no build cache; report nothing reclaimed.
pub(crate) async fn build_prune() -> axum::Json<serde_json::Value> {
    // `docker builder prune` / the build-cache portion of `docker system prune`. dd has no layer build
    // cache, but the persistent JIT translated-code cache (~/.dd/pcache) lives in this slot (see
    // system_df) — reclaim it here. It's safe to drop wholesale: every entry rebuilds on demand, keyed by
    // binary hash, so this only forces a one-time re-translation on the next run of each image.
    let dir = crate::util::dd_home().join("pcache");
    let (mut deleted, mut reclaimed) = (Vec::new(), 0i64);
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
