//! A small OCI / Docker registry client — pull and push images from **any** registry, not just Docker
//! Hub. Auth uses the standard `WWW-Authenticate: Bearer` challenge flow, so Docker Hub, GHCR, Quay, ECR
//! and a plain `localhost:5000` dev registry all work the same way.
//!
//! HTTP goes through `curl` and (de)compression through `tar`/`gzip`/`sha256sum`: the daemon's offline
//! build can't pull in an async HTTP+TLS+tar crate stack, and these tools are universally present. The
//! shelling-out is confined to the small [`http`] helpers; everything above them is ordinary typed code.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};
use std::process::Command;

const DOCKER_HUB: &str = "registry-1.docker.io";
const MANIFEST_ACCEPT: &str = "application/vnd.docker.distribution.manifest.list.v2+json,\
application/vnd.oci.image.index.v1+json,\
application/vnd.docker.distribution.manifest.v2+json,\
application/vnd.oci.image.manifest.v1+json";
const MEDIA_MANIFEST: &str = "application/vnd.docker.distribution.manifest.v2+json";
const MEDIA_CONFIG: &str = "application/vnd.docker.container.image.v1+json";
const MEDIA_LAYER: &str = "application/vnd.docker.image.rootfs.diff.tar.gzip";

/// Credentials for a registry, as sent by the CLI in the `X-Registry-Auth` header.
#[derive(Clone, Default)]
pub struct Credentials {
    pub username: String,
    pub password: String,
}
impl Credentials {
    /// Decode docker's base64-encoded `X-Registry-Auth` JSON (`{username,password,...}`).
    pub fn from_x_registry_auth(b64: &str) -> Option<Credentials> {
        let json = base64_decode(b64.trim())?;
        let v: Value = serde_json::from_slice(&json).ok()?;
        Some(Credentials {
            username: v["username"].as_str().unwrap_or_default().to_string(),
            password: v["password"].as_str().unwrap_or_default().to_string(),
        })
    }
    fn is_empty(&self) -> bool { self.username.is_empty() && self.password.is_empty() }
}

/// A parsed image reference: `[registry/]repository[:tag]`.
#[derive(Clone, Debug, PartialEq)]
pub struct ImageRef {
    pub registry: String, // host[:port], e.g. "registry-1.docker.io", "ghcr.io", "localhost:5000"
    pub repository: String, // path, e.g. "library/ubuntu", "owner/app"
    pub tag: String,
}
impl ImageRef {
    /// Parse a reference the way docker does: the leading segment is a registry host if it contains a
    /// `.` or `:` or is `localhost`; otherwise it's a Docker Hub image (and a single-element repository
    /// gets the implicit `library/` namespace).
    pub fn parse(s: &str) -> ImageRef {
        let (path, tag) = split_tag(s.trim());
        match path.split_once('/') {
            Some((host, rest)) if is_registry_host(host) => {
                let registry = if host == "docker.io" { DOCKER_HUB.to_string() } else { host.to_string() };
                ImageRef { registry, repository: rest.to_string(), tag }
            }
            _ => {
                let repository = if path.contains('/') { path.to_string() } else { format!("library/{path}") };
                ImageRef { registry: DOCKER_HUB.to_string(), repository, tag }
            }
        }
    }
    /// `registry/repository:tag`, with Docker Hub abbreviated back to `docker.io`.
    pub fn canonical(&self) -> String {
        let host = if self.registry == DOCKER_HUB { "docker.io" } else { &self.registry };
        format!("{host}/{}:{}", self.repository, self.tag)
    }
    /// The short, docker-style display name (`busybox:latest`, `user/app:1`, `ghcr.io/o/a:v2`): Hub's
    /// implicit `docker.io/library/` is elided, other registries are shown.
    pub fn short(&self) -> String {
        let repo = if self.registry == DOCKER_HUB {
            self.repository.strip_prefix("library/").unwrap_or(&self.repository).to_string()
        } else {
            format!("{}/{}", self.registry, self.repository)
        };
        format!("{repo}:{}", self.tag)
    }
    fn base_url(&self) -> String {
        // local dev registries are plain HTTP; everything else is HTTPS
        let scheme = if is_local_registry(&self.registry) { "http" } else { "https" };
        format!("{scheme}://{}/v2/{}", self.registry, self.repository)
    }
}

fn is_registry_host(seg: &str) -> bool { seg == "localhost" || seg.contains('.') || seg.contains(':') }
fn is_local_registry(host: &str) -> bool { host.starts_with("localhost") || host.starts_with("127.") }
fn split_tag(s: &str) -> (&str, String) {
    match s.rsplit_once(':') {
        Some((p, t)) if !t.contains('/') => (p, t.to_string()),
        _ => (s, "latest".to_string()),
    }
}

/// What `pull` resolved and unpacked.
pub struct Pulled {
    pub image: ImageRef,
    pub config: Value, // the image config blob (Cmd/Entrypoint/Env/Architecture)
}

/// A live progress event for a single image pull, surfaced per-layer as the download/unpack proceeds.
/// `images_create` formats these into docker's newline-delimited JSON status lines and streams them to
/// the client, so the user sees moving download/extract bars instead of one post-hoc dump. `id` is the
/// docker-style short layer id (first 12 hex of the blob digest). Byte counts are the compressed blob
/// size from the manifest (the same units docker's registry pull reports).
#[derive(Clone, Debug)]
pub enum PullEvent {
    /// A layer was discovered in the manifest (docker's "Pulling fs layer").
    Layer { id: String },
    /// Live download progress for a layer (`current`/`total` compressed bytes).
    Downloading { id: String, current: u64, total: u64 },
    /// A layer finished downloading.
    DownloadComplete { id: String },
    /// A layer's contents are being unpacked into the rootfs.
    Extracting { id: String, current: u64, total: u64 },
    /// A layer is fully pulled + unpacked.
    PullComplete { id: String },
}

/// docker's short layer id: the first 12 hex chars after the `sha256:` prefix.
pub fn layer_short(digest: &str) -> String {
    digest.trim_start_matches("sha256:").chars().take(12).collect()
}

/// A registry session for one image: caches the bearer token across the manifest + blob requests.
pub struct Client {
    image: ImageRef,
    creds: Credentials,
    token: Option<String>,
}
impl Client {
    pub fn new(image: ImageRef, creds: Credentials) -> Client { Client { image, creds, token: None } }

    /// Pull the image and unpack its layers into `rootfs` (created fresh). Picks the `linux/arm64`
    /// variant of a multi-arch index, falling back to `linux/amd64`. `progress` is invoked with a
    /// [`PullEvent`] as each layer downloads/unpacks, so the caller can stream live progress to the
    /// client; pass `&mut |_| {}` to ignore it.
    pub fn pull(&mut self, rootfs: &Path, want_archs: &[&str], progress: &mut dyn FnMut(PullEvent)) -> Result<Pulled, String> {
        let manifest = self.resolve_manifest(want_archs)?;
        let config = self.config_blob(&manifest).unwrap_or_else(|_| json!({}));
        let layers = manifest["layers"].as_array().ok_or("manifest has no layers")?;
        if layers.is_empty() { return Err("manifest has no layers".into()); }
        // Announce every layer up front (docker shows one "Pulling fs layer" line per blob), then pull
        // them in order so each layer's download/extract progress streams live and in id order.
        let metas: Vec<(String, u64, String)> = layers.iter().map(|layer| {
            let digest = layer["digest"].as_str().unwrap_or_default().to_string();
            let size = layer["size"].as_u64().unwrap_or(0);
            let id = layer_short(&digest);
            (digest, size, id)
        }).collect();
        for (_, _, id) in &metas { progress(PullEvent::Layer { id: id.clone() }); }
        reset_dir(rootfs)?;
        for (digest, size, id) in &metas {
            if digest.is_empty() { return Err("layer missing digest".into()); }
            self.unpack_layer(digest, *size, id, rootfs, progress)?;
            progress(PullEvent::PullComplete { id: id.clone() });
        }
        Ok(Pulled { image: self.image.clone(), config })
    }

    /// Push `rootfs` to the registry as a single-layer image under `self.image`. Returns the manifest
    /// digest. Requires credentials for a registry that demands auth.
    pub fn push(&mut self, rootfs: &Path, cmd: &[String], arch: &str, os: &str, work: &Path) -> Result<String, String> {
        reset_dir(work)?;
        let layer = work.join("layer.tar.gz");
        let (layer_digest, layer_size) = tar_gzip(rootfs, &layer)?; // compressed digest = blob id
        let diff_id = gunzip_sha256(&layer)?; // uncompressed digest = rootfs diff_id

        let config = json!({
            "architecture": arch, "os": os, // os=darwin for mac containers; the manifest is os/arch-tagged
            "config": { "Cmd": cmd },
            "rootfs": { "type": "layers", "diff_ids": [diff_id] },
        });
        let config_path = work.join("config.json");
        let config_bytes = serde_json::to_vec(&config).map_err(|e| e.to_string())?;
        std::fs::write(&config_path, &config_bytes).map_err(|e| e.to_string())?;
        let config_digest = sha256_of(&config_path)?;

        self.authenticate("push,pull")?;
        self.upload_blob(&config_digest, &config_path)?;
        self.upload_blob(&layer_digest, &layer)?;

        let manifest = json!({
            "schemaVersion": 2, "mediaType": MEDIA_MANIFEST,
            "config": { "mediaType": MEDIA_CONFIG, "size": config_bytes.len(), "digest": config_digest },
            "layers": [{ "mediaType": MEDIA_LAYER, "size": layer_size, "digest": layer_digest }],
        });
        self.put_manifest(&serde_json::to_vec(&manifest).unwrap())
    }

    // ---- manifest / config / layer ----

    fn resolve_manifest(&mut self, want_archs: &[&str]) -> Result<Value, String> {
        let man = self.get_json(&format!("/manifests/{}", self.image.tag), Some(MANIFEST_ACCEPT))?;
        let Some(list) = man["manifests"].as_array() else { return Ok(man) }; // already a single manifest
        let digest = want_archs.iter()
            .find_map(|arch| select_platform(list, arch))
            .ok_or_else(|| format!("no {} variant in the manifest list", want_archs.join("/")))?;
        self.get_json(&format!("/manifests/{digest}"), Some(MANIFEST_ACCEPT))
    }
    fn config_blob(&mut self, manifest: &Value) -> Result<Value, String> {
        let digest = manifest["config"]["digest"].as_str().ok_or("manifest has no config")?;
        let bytes = self.get_blob_bytes(digest)?;
        serde_json::from_slice(&bytes).map_err(|e| e.to_string())
    }
    /// Download one layer blob to a temp file (emitting live `Downloading` byte progress), then unpack it
    /// into `rootfs` (emitting `Extracting`). Landing the compressed blob on disk first — rather than the
    /// old `curl | tar` pipe — is what lets us poll the byte count and report real progress; the temp
    /// file is removed afterwards regardless of outcome.
    fn unpack_layer(&mut self, digest: &str, size: u64, id: &str, rootfs: &Path, progress: &mut dyn FnMut(PullEvent)) -> Result<(), String> {
        let token = self.authenticate("pull")?;
        let url = format!("{}/blobs/{digest}", self.image.base_url());
        let tmp = std::env::temp_dir().join(format!("dd-layer-{}-{id}.tar.gz", std::process::id()));
        let out = (|| {
            http::download_to_file(&url, token.as_deref(), &tmp, &mut |cur| {
                progress(PullEvent::Downloading { id: id.to_string(), current: cur, total: size });
            })?;
            progress(PullEvent::DownloadComplete { id: id.to_string() });
            progress(PullEvent::Extracting { id: id.to_string(), current: size, total: size });
            http::extract_targz(&tmp, rootfs)
        })();
        let _ = std::fs::remove_file(&tmp);
        out?;
        apply_whiteouts(rootfs)
    }

    // ---- push primitives ----

    fn upload_blob(&self, digest: &str, file: &Path) -> Result<(), String> {
        let base = self.image.base_url();
        // already present?
        if http::head(&format!("{base}/blobs/{digest}"), self.token.as_deref())? == 200 { return Ok(()); }
        // start an upload session -> Location, then monolithic PUT with ?digest=
        let start = http::post(&format!("{base}/blobs/uploads/"), self.token.as_deref())?;
        if start.status == 401 || start.status == 403 {
            return Err("denied: requested access to the resource is denied (run `docker login`)".into());
        }
        if start.status != 202 { return Err(format!("blob upload not accepted ({})", start.status)); }
        let location = header(&start.headers, "location").ok_or("upload returned no Location")?;
        let sep = if location.contains('?') { '&' } else { '?' };
        let put = format!("{}{sep}digest={digest}", absolute(&location, &base));
        let r = http::put_file(&put, file, "application/octet-stream", self.token.as_deref())?;
        if r.status == 201 || r.status == 202 { Ok(()) } else { Err(format!("blob PUT -> {}", r.status)) }
    }
    fn put_manifest(&self, body: &[u8]) -> Result<String, String> {
        let url = format!("{}/manifests/{}", self.image.base_url(), self.image.tag);
        let r = http::put_bytes(&url, body, MEDIA_MANIFEST, self.token.as_deref())?;
        if r.status == 201 { Ok(header(&r.headers, "docker-content-digest").unwrap_or_default()) }
        else { Err(format!("manifest PUT -> {} {}", r.status, String::from_utf8_lossy(&r.body))) }
    }

    // ---- authenticated GET with the bearer-challenge dance ----

    fn get_json(&mut self, path: &str, accept: Option<&str>) -> Result<Value, String> {
        let bytes = self.authed_get(path, accept)?;
        serde_json::from_slice(&bytes).map_err(|e| format!("bad JSON from {path}: {e}"))
    }
    fn get_blob_bytes(&mut self, digest: &str) -> Result<Vec<u8>, String> {
        self.authed_get(&format!("/blobs/{digest}"), None)
    }
    fn authed_get(&mut self, path: &str, accept: Option<&str>) -> Result<Vec<u8>, String> {
        let url = format!("{}{path}", self.image.base_url());
        let first = http::get(&url, accept, self.token.as_deref())?;
        if first.status == 200 { return Ok(first.body); }
        if first.status == 401 {
            let token = self.token_from_challenge(&first.headers)?;
            self.token = Some(token.clone());
            let retry = http::get(&url, accept, Some(&token))?;
            if retry.status == 200 { return Ok(retry.body); }
            return Err(format!("GET {url} -> {} after auth", retry.status));
        }
        Err(format!("GET {url} -> {}", first.status))
    }
    /// Ensure we hold a token for `scope`; returns it (None for anonymous registries that 401-less).
    fn authenticate(&mut self, scope_action: &str) -> Result<Option<String>, String> {
        if self.token.is_some() { return Ok(self.token.clone()); }
        // probe to discover the auth realm
        let probe = http::get(&format!("{}/manifests/{}", self.image.base_url(), self.image.tag),
            Some(MANIFEST_ACCEPT), None)?;
        if probe.status == 200 { return Ok(None); } // open registry
        let scope = format!("repository:{}:{scope_action}", self.image.repository);
        let token = self.token_from_challenge_scoped(&probe.headers, Some(&scope))?;
        self.token = Some(token.clone());
        Ok(Some(token))
    }
    fn token_from_challenge(&self, headers: &str) -> Result<String, String> {
        self.token_from_challenge_scoped(headers, None)
    }
    fn token_from_challenge_scoped(&self, headers: &str, scope: Option<&str>) -> Result<String, String> {
        let ch = BearerChallenge::parse(headers).ok_or("registry gave no Bearer challenge")?;
        let scope = scope.unwrap_or(&ch.scope);
        let url = format!("{}?service={}&scope={}", ch.realm, ch.service, scope);
        let creds = if self.creds.is_empty() { None } else { Some(&self.creds) };
        let resp = http::get_with_basic(&url, creds)?;
        if resp.status != 200 {
            // A registry that refuses to mint a *push*-scoped token for an unauthenticated client is
            // denying the push -- exactly `docker push` without `docker login`. Surface the conformant
            // "denied" message whether the denial lands here at the token endpoint (401/403) or later at
            // the blob-upload POST (see upload_blob), so callers get one stable error either way.
            let action = scope.rsplit(':').next().unwrap_or("");
            if action.contains("push") && (resp.status == 401 || resp.status == 403) {
                return Err("denied: requested access to the resource is denied (run `docker login`)".into());
            }
            return Err(format!("token endpoint -> {}", resp.status));
        }
        let v: Value = serde_json::from_slice(&resp.body).map_err(|e| e.to_string())?;
        v["token"].as_str().or_else(|| v["access_token"].as_str())
            .map(str::to_string).ok_or_else(|| "token response had no token".to_string())
    }
}

/// The `realm`/`service`/`scope` of a `WWW-Authenticate: Bearer …` header.
struct BearerChallenge { realm: String, service: String, scope: String }
impl BearerChallenge {
    fn parse(headers: &str) -> Option<BearerChallenge> {
        let line = headers.lines().find(|l| l.to_ascii_lowercase().starts_with("www-authenticate:"))?;
        let params = line.splitn(2, "Bearer").nth(1)?;
        let get = |k: &str| params.split(',')
            .find_map(|kv| kv.trim().strip_prefix(&format!("{k}="))).map(|v| v.trim_matches('"').to_string());
        Some(BearerChallenge {
            realm: get("realm")?,
            service: get("service").unwrap_or_default(),
            scope: get("scope").unwrap_or_default(),
        })
    }
}

/// Pick the digest of the `linux/<arch>` entry of a manifest list/index.
fn select_platform(list: &[Value], arch: &str) -> Option<String> {
    list.iter()
        .find(|m| m["platform"]["architecture"] == arch && m["platform"]["os"] == "linux")
        .and_then(|m| m["digest"].as_str().map(str::to_string))
}

// ---- filesystem helpers (tar / gzip / sha256 / whiteouts) -------------------

fn reset_dir(p: &Path) -> Result<(), String> {
    let _ = std::fs::remove_dir_all(p);
    std::fs::create_dir_all(p).map_err(|e| format!("mkdir {}: {e}", p.display()))
}

const WH_PREFIX: &str = ".wh.";
const WH_OPAQUE: &str = ".wh..wh..opq";

/// Apply OCI whiteouts left by a just-extracted layer: a `.wh.<name>` marker deletes the sibling
/// `<name>`, and `.wh..wh..opq` clears the directory's lower contents (we just drop the marker — the
/// layers are already flattened). Done with a plain filesystem walk rather than a `find | while …
/// dirname/basename/rm` pipeline: a degenerate marker name can't make a shell utility error out
/// ("sh failed: …") nor, worse, delete the wrong path (a bare `.wh.` made the old script run
/// `rm -rf "$dir/"`, wiping the parent directory).
fn apply_whiteouts(rootfs: &Path) -> Result<(), String> {
    // Enumerate every marker first, then apply: a deletion can remove a whole subtree that itself
    // holds further markers, so we must not mutate the tree while still walking it.
    let mut markers = Vec::new();
    collect_whiteouts(rootfs, &mut markers);
    for marker in &markers {
        let name = marker.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        // The opaque marker has no sibling to delete; any other `.wh.<name>` hides the sibling `<name>`.
        // A marker that is *only* the `.wh.` prefix (empty target) is malformed — drop it without
        // deleting anything rather than removing its parent directory.
        if name != WH_OPAQUE {
            if let Some(target) = name.strip_prefix(WH_PREFIX).filter(|t| !t.is_empty()) {
                if let Some(parent) = marker.parent() { remove_path(&parent.join(target)); }
            }
        }
        let _ = std::fs::remove_file(marker);
    }
    Ok(())
}

/// Collect every `.wh.*` marker under `dir`, recursing into real subdirectories only (symlinks are not
/// followed, so a layer can't redirect the walk outside the rootfs).
fn collect_whiteouts(dir: &Path, out: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        if entry.file_name().to_string_lossy().starts_with(WH_PREFIX) { out.push(entry.path()); }
        if entry.file_type().map(|t| t.is_dir()).unwrap_or(false) { collect_whiteouts(&entry.path(), out); }
    }
}

/// Remove a path whether it's a file, a symlink, or a directory subtree; missing is success.
fn remove_path(p: &Path) {
    let _ = match std::fs::symlink_metadata(p) {
        Ok(m) if m.is_dir() => std::fs::remove_dir_all(p),
        Ok(_) => std::fs::remove_file(p),
        Err(_) => Ok(()),
    };
}

/// `tar | gzip` a rootfs into `out`; returns (compressed digest, compressed size).
fn tar_gzip(rootfs: &Path, out: &Path) -> Result<(String, u64), String> {
    let cmd = format!("tar cf - -C '{}' . | gzip -n > '{}'", rootfs.display(), out.display());
    run("sh", &["-c", &cmd])?;
    let size = std::fs::metadata(out).map_err(|e| e.to_string())?.len();
    Ok((sha256_of(out)?, size))
}
/// sha256 of the *decompressed* contents of a gzip file (an OCI `diff_id`).
fn gunzip_sha256(gz: &Path) -> Result<String, String> {
    let out = run("sh", &["-c", &format!("gzip -dc '{}' | sha256sum", gz.display())])?;
    parse_sha256(&out)
}
fn sha256_of(file: &Path) -> Result<String, String> {
    parse_sha256(&run("sha256sum", &[&file.to_string_lossy()])?)
}
fn parse_sha256(sha256sum_out: &str) -> Result<String, String> {
    sha256sum_out.split_whitespace().next().map(|h| format!("sha256:{h}"))
        .ok_or_else(|| "sha256sum gave no output".to_string())
}

fn run(prog: &str, args: &[&str]) -> Result<String, String> {
    let out = Command::new(prog).args(args).output().map_err(|e| format!("{prog}: {e}"))?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let detail = if stderr.trim().is_empty() { format!("exited with {}", out.status) } else { stderr.trim().to_string() };
        return Err(format!("{prog} {args:?} failed: {detail}"));
    }
    Ok(String::from_utf8_lossy(&out.stdout).into_owned())
}

fn header<'a>(headers: &'a str, name: &str) -> Option<String> {
    let want = format!("{}:", name.to_ascii_lowercase());
    headers.lines().find(|l| l.to_ascii_lowercase().starts_with(&want))
        .and_then(|l| l.split_once(':')).map(|(_, v)| v.trim().to_string())
}
/// Resolve a possibly-relative `Location` against the registry base origin.
fn absolute(location: &str, base_v2: &str) -> String {
    if location.starts_with("http") { return location.to_string(); }
    let origin = base_v2.split("/v2/").next().unwrap_or(base_v2);
    format!("{origin}{location}")
}

fn base64_decode(s: &str) -> Option<Vec<u8>> {
    // docker uses standard or URL-safe base64; do it without a crate
    const A: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut table = [255u8; 256];
    for (i, &c) in A.iter().enumerate() { table[c as usize] = i as u8; }
    table[b'-' as usize] = 62; table[b'_' as usize] = 63;
    let mut bits = 0u32; let mut nbits = 0; let mut out = Vec::new();
    for &c in s.as_bytes() {
        if c == b'=' || c == b'\n' || c == b'\r' { continue; }
        let v = table[c as usize];
        if v == 255 { return None; }
        bits = (bits << 6) | v as u32; nbits += 6;
        if nbits >= 8 { nbits -= 8; out.push((bits >> nbits) as u8); }
    }
    Some(out)
}

/// Thin `curl` wrappers. Headers are captured to a temp file (`-D`); the body goes to stdout (or a tar).
mod http {
    use super::{run, Credentials};
    use std::path::Path;
    use std::process::Command;
    use std::sync::atomic::{AtomicU64, Ordering};

    pub struct Resp { pub status: u16, pub headers: String, pub body: Vec<u8> }

    static SEQ: AtomicU64 = AtomicU64::new(0);
    fn tmp_headers() -> std::path::PathBuf {
        let n = SEQ.fetch_add(1, Ordering::Relaxed);
        std::env::temp_dir().join(format!("dd-reg-{}-{n}.hdr", std::process::id()))
    }

    fn run_curl(args: &[String]) -> Result<Resp, String> {
        let hdr = tmp_headers();
        let mut c = Command::new("curl");
        c.arg("-sS").arg("--max-time").arg("600").arg("-D").arg(&hdr);
        for a in args { c.arg(a); }
        let out = c.output().map_err(|e| format!("curl: {e}"))?;
        let headers = std::fs::read_to_string(&hdr).unwrap_or_default();
        let _ = std::fs::remove_file(&hdr);
        if !out.status.success() && headers.is_empty() {
            return Err(format!("curl failed: {}", String::from_utf8_lossy(&out.stderr)));
        }
        Ok(Resp { status: status_of(&headers), headers, body: out.stdout })
    }
    /// The status of the *last* response (after any redirects curl followed).
    fn status_of(headers: &str) -> u16 {
        headers.lines().rev()
            .find_map(|l| l.strip_prefix("HTTP/").and_then(|r| r.split_whitespace().nth(1)))
            .and_then(|c| c.parse().ok()).unwrap_or(0)
    }

    fn with_auth(mut args: Vec<String>, accept: Option<&str>, token: Option<&str>) -> Vec<String> {
        if let Some(a) = accept { args.push("-H".into()); args.push(format!("Accept: {a}")); }
        if let Some(t) = token { args.push("-H".into()); args.push(format!("Authorization: Bearer {t}")); }
        args
    }

    pub fn get(url: &str, accept: Option<&str>, token: Option<&str>) -> Result<Resp, String> {
        run_curl(&with_auth(vec![url.into()], accept, token))
    }
    pub fn get_with_basic(url: &str, creds: Option<&Credentials>) -> Result<Resp, String> {
        let mut args = vec![url.to_string()];
        if let Some(c) = creds { args.push("-u".into()); args.push(format!("{}:{}", c.username, c.password)); }
        run_curl(&args)
    }
    pub fn head(url: &str, token: Option<&str>) -> Result<u16, String> {
        run_curl(&with_auth(vec!["-I".into(), url.into()], None, token)).map(|r| r.status)
    }
    pub fn post(url: &str, token: Option<&str>) -> Result<Resp, String> {
        run_curl(&with_auth(vec!["-X".into(), "POST".into(), url.into()], None, token))
    }
    pub fn put_file(url: &str, file: &Path, content_type: &str, token: Option<&str>) -> Result<Resp, String> {
        // `-T` (upload-file) STREAMS the body from disk and sets Content-Length from the file size —
        // unlike `--data-binary @file`, which buffers the entire file in memory (OOMs on multi-GB layers).
        let args = with_auth(vec![
            "-X".into(), "PUT".into(), "-H".into(), format!("Content-Type: {content_type}"),
            "-T".into(), file.display().to_string(), url.into()], None, token);
        run_curl(&args)
    }
    pub fn put_bytes(url: &str, body: &[u8], content_type: &str, token: Option<&str>) -> Result<Resp, String> {
        let tmp = std::env::temp_dir().join(format!("dd-reg-body-{}.bin", std::process::id()));
        std::fs::write(&tmp, body).map_err(|e| e.to_string())?;
        let r = put_file(url, &tmp, content_type, token);
        let _ = std::fs::remove_file(&tmp);
        r
    }
    /// Download a blob to `dest`, calling `progress` with the bytes-so-far while curl runs so the caller
    /// can stream a live download bar. curl writes straight to disk (`-o`); we poll the file size every
    /// ~150ms until the process exits, then report the final size. Landing the blob on disk (vs piping)
    /// is what makes the byte count observable.
    pub fn download_to_file(url: &str, token: Option<&str>, dest: &Path, progress: &mut dyn FnMut(u64)) -> Result<(), String> {
        let mut cmd = Command::new("curl");
        cmd.arg("-sSL").arg("--max-time").arg("600");
        if let Some(t) = token { cmd.arg("-H").arg(format!("Authorization: Bearer {t}")); }
        cmd.arg("-o").arg(dest).arg(url);
        let mut child = cmd.spawn().map_err(|e| format!("curl: {e}"))?;
        let file_len = |p: &Path| std::fs::metadata(p).map(|m| m.len()).unwrap_or(0);
        loop {
            match child.try_wait().map_err(|e| e.to_string())? {
                Some(st) => {
                    if !st.success() { return Err(format!("curl blob download failed ({st})")); }
                    progress(file_len(dest)); // final, exact size
                    return Ok(());
                }
                None => {
                    progress(file_len(dest));
                    std::thread::sleep(std::time::Duration::from_millis(150));
                }
            }
        }
    }
    /// Unpack a gzipped-tar layer blob from `src` into `rootfs` (`tar xzf`). tar's stderr is muted because
    /// OCI layers carry entries (device nodes, ownership) tar can't always recreate unprivileged — the
    /// same tolerance the old `curl | tar xz 2>/dev/null` pipe had.
    pub fn extract_targz(src: &Path, rootfs: &Path) -> Result<(), String> {
        let cmd = format!("tar xzf '{}' -C '{}' 2>/dev/null", src.display(), rootfs.display());
        run("sh", &["-c", &cmd]).map(|_| ())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parse_refs() {
        let h = ImageRef::parse("ubuntu");
        assert_eq!((h.registry.as_str(), h.repository.as_str(), h.tag.as_str()),
            (DOCKER_HUB, "library/ubuntu", "latest"));
        assert_eq!(ImageRef::parse("alpine:3.19").tag, "3.19");
        assert_eq!(ImageRef::parse("user/app").repository, "user/app");
        let g = ImageRef::parse("ghcr.io/owner/app:v2");
        assert_eq!((g.registry.as_str(), g.repository.as_str(), g.tag.as_str()), ("ghcr.io", "owner/app", "v2"));
        let l = ImageRef::parse("localhost:5000/img");
        assert_eq!((l.registry.as_str(), l.repository.as_str()), ("localhost:5000", "img"));
        assert!(is_local_registry(&l.registry));
    }
    #[test]
    fn whiteouts() {
        // A just-extracted layer: a normal whiteout, an opaque-dir marker, and two degenerate names
        // that the old `find | … rm` shell mishandled (a bare `.wh.` wiped the parent dir; `.wh..`
        // made `rm` error). After apply_whiteouts: targets gone, all markers gone, parents kept.
        let root = std::env::temp_dir().join(format!("dd-wh-test-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&root);
        let sub = root.join("sub");
        std::fs::create_dir_all(&sub).unwrap();
        std::fs::write(root.join("keep"), b"x").unwrap();
        std::fs::write(sub.join("gone"), b"x").unwrap(); // hidden by sub/.wh.gone
        std::fs::write(sub.join(".wh.gone"), b"").unwrap();
        std::fs::write(sub.join(".wh..wh..opq"), b"").unwrap();
        std::fs::write(sub.join(".wh."), b"").unwrap(); // malformed: must NOT delete sub/
        std::fs::write(sub.join(".wh.."), b"").unwrap(); // malformed: target "." must be ignored

        apply_whiteouts(&root).unwrap();

        assert!(root.join("keep").exists(), "unrelated file preserved");
        assert!(sub.exists(), "parent dir must survive a bare .wh. marker");
        assert!(!sub.join("gone").exists(), "whiteout deleted its target");
        for m in [".wh.gone", ".wh..wh..opq", ".wh.", ".wh.."] {
            assert!(!sub.join(m).exists(), "marker {m} removed");
        }
        let _ = std::fs::remove_dir_all(&root);
    }
    #[test]
    fn challenge() {
        let h = "HTTP/1.1 401 Unauthorized\r\nwww-authenticate: Bearer realm=\"https://auth.docker.io/token\",service=\"registry.docker.io\",scope=\"repository:library/ubuntu:pull\"\r\n";
        let c = BearerChallenge::parse(h).unwrap();
        assert_eq!(c.realm, "https://auth.docker.io/token");
        assert_eq!(c.service, "registry.docker.io");
    }
}
