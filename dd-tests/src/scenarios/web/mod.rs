//! Web servers / proxies — nginx, httpd, caddy, traefik, haproxy, varnish. Start the server, hit it
//! with curl/wget over LOOPBACK, assert a deterministic body/banner. Exercises listen/accept/epoll +
//! fork-worker models (good JIT stress). Hermetic: loopback-only, no external network. Both Linux
//! arches. Owner: web agent. Recipes: docs/IMAGE-MANIFEST.md §4.
//!
//! Every case below is verified against the REAL docker oracle (`--backend real`). Known dd gaps are
//! `.xfail()`-marked so the gate stays green and XPASS fires when the engine lane fixes them:
//!   * httpd (alpine AND glibc): the exec-loader gap (`exec-loader-noent`, GAPS.md) — httpd's entry
//!     binary fails to load under dd; nginx works (binary-link-shape dependent). xfail both arches.

use crate::scenario::{scen, sgroup, ScenGroup, Target};

const BOTH: &[Target] = &[Target::ArmLinux, Target::AmdLinux];

pub fn group() -> ScenGroup {
    sgroup("web", vec![
        // ---- nginx (C, fork-worker, musl + glibc) — works on dd, no xfail -------------------------
        // seed (proven on Real): nginx serves its default page over loopback.
        scen("web/nginx-serve", "nginx:alpine")
            .exec("nginx; sleep 1; wget -qO- http://127.0.0.1/ | head -1")
            .has("<!DOCTYPE html>").timeout(60),
        // full default page banner string.
        scen("web/nginx-welcome", "nginx:alpine")
            .exec("nginx; sleep 1; wget -qO- http://127.0.0.1/")
            .has("Welcome to nginx!").timeout(60),
        // serve a fixed file we write — deterministic body, exercises static file path + sendfile.
        scen("web/nginx-custom-file", "nginx:alpine")
            .exec("echo dd-served-ok > /usr/share/nginx/html/dd.txt; nginx; sleep 1; \
                   wget -qO- http://127.0.0.1/dd.txt")
            .has("dd-served-ok").timeout(60),
        // replace the index served at / with a fixed marker.
        scen("web/nginx-index-replace", "nginx:alpine")
            .exec("echo dd-index-ok > /usr/share/nginx/html/index.html; nginx; sleep 1; \
                   wget -qO- http://127.0.0.1/")
            .has("dd-index-ok").timeout(60),
        // a missing path returns the built-in 404 page (loopback error path).
        scen("web/nginx-404", "nginx:alpine")
            .exec("nginx; sleep 1; wget -qO- http://127.0.0.1/nope-does-not-exist 2>&1; \
                   wget -S -qO /dev/null http://127.0.0.1/nope 2>&1 | grep -i 'HTTP/'")
            .has("404").timeout(60),
        // response headers over loopback (busybox wget -S dumps headers to stderr).
        scen("web/nginx-headers", "nginx:alpine")
            .exec("nginx; sleep 1; wget -S -qO /dev/null http://127.0.0.1/ 2>&1 | grep -i 'Server:'")
            .has("nginx").timeout(60),
        // config syntax check (parses nginx.conf without starting workers).
        scen("web/nginx-config-test", "nginx:alpine")
            .exec("nginx -t 2>&1")
            .has("syntax is ok").has("test is successful").timeout(45),
        // version banners (loose + pinned).
        scen("web/nginx-version", "nginx:alpine")
            .exec("nginx -v 2>&1")
            .has("nginx/1.").timeout(45),
        scen("web/nginx-version-127", "nginx:1.27-alpine")
            .exec("nginx -v 2>&1")
            .has("nginx/1.27").timeout(45),
        // stable-alpine variant: default page + custom file.
        scen("web/nginx-stable-serve", "nginx:stable-alpine")
            .exec("nginx; sleep 1; wget -qO- http://127.0.0.1/")
            .has("Welcome to nginx!").timeout(60),
        scen("web/nginx-stable-custom", "nginx:stable-alpine")
            .exec("echo dd-served-ok > /usr/share/nginx/html/dd.txt; nginx; sleep 1; \
                   wget -qO- http://127.0.0.1/dd.txt")
            .has("dd-served-ok").timeout(60),
        scen("web/nginx-stable-version", "nginx:stable-alpine")
            .exec("nginx -v 2>&1")
            .has("nginx/1.").timeout(45),
        // glibc nginx (debian) — exercises the glibc dynamic-linker worker path. No http client in the
        // debian image, so banner + config-test only (no loopback fetch needed).
        scen("web/nginx-glibc-version", "nginx:1.26")
            .exec("nginx -v 2>&1")
            .has("nginx/1.26").timeout(45),
        scen("web/nginx-glibc-config", "nginx:1.26")
            .exec("nginx -t 2>&1")
            .has("syntax is ok").timeout(45),

        // ---- caddy (Go, goroutine scheduler, musl) ------------------------------------------------
        scen("web/caddy-version", "caddy:2-alpine")
            .exec("caddy version")
            .has("v2.").timeout(45),
        // run form (no shell needed): the image CMD is `caddy`, so pass `caddy version` argv.
        scen("web/caddy-version-run", "caddy:2")
            .run(&["caddy", "version"])
            .has("v2.").timeout(45),
        // adapt a tiny Caddyfile and serve a fixed response over loopback.
        scen("web/caddy-respond", "caddy:2-alpine")
            .exec("printf ':80\\n\\nrespond \"dd-served-ok\"\\n' > /tmp/Caddyfile; \
                   caddy start --config /tmp/Caddyfile --adapter caddyfile >/dev/null 2>&1; sleep 1; \
                   wget -qO- http://127.0.0.1/")
            .has("dd-served-ok").timeout(60),
        // file-server serving a written file.
        scen("web/caddy-file-server", "caddy:2-alpine")
            .exec("mkdir -p /tmp/srv; echo dd-served-ok > /tmp/srv/dd.txt; \
                   caddy file-server --root /tmp/srv --listen :80 >/dev/null 2>&1 & sleep 1; \
                   wget -qO- http://127.0.0.1/dd.txt")
            .has("dd-served-ok").timeout(60),

        // ---- traefik (Go, scratch image — no shell, run form only) --------------------------------
        scen("web/traefik-version", "traefik:v3.1")
            .run(&["version"])
            .has("3.1").timeout(45),
        scen("web/traefik-version-211", "traefik:v2.11")
            .run(&["version"])
            .has("2.11").timeout(45),

        // ---- haproxy (C, event-driven, musl + glibc) ----------------------------------------------
        scen("web/haproxy-version", "haproxy:alpine")
            .exec("haproxy -v 2>&1 | head -1")
            .has("HAProxy").timeout(45),
        scen("web/haproxy-version-lts", "haproxy:lts-alpine")
            .exec("haproxy -v 2>&1 | head -1")
            .has("HAProxy").timeout(45),
        // glibc (debian) variant banner.
        scen("web/haproxy-glibc-version", "haproxy:2.9")
            .exec("haproxy -v 2>&1 | head -1")
            .has("HAProxy version 2.9").timeout(45),
        // config validity check (haproxy -c is silent on success → echo our own marker).
        scen("web/haproxy-config-check", "haproxy:alpine")
            .exec("cat > /tmp/h.cfg <<'CFG'\n\
defaults\n  mode http\n  timeout connect 1s\n  timeout client 1s\n  timeout server 1s\n\
frontend f\n  bind :80\n  http-request return status 200 content-type \"text/plain\" string \"ok\"\n\
CFG\n\
haproxy -c -f /tmp/h.cfg && echo HAPROXY-CFG-VALID")
            .has("HAPROXY-CFG-VALID").timeout(45),
        // self-contained loopback round-trip: a frontend that returns a fixed body (no backend needed).
        scen("web/haproxy-return", "haproxy:alpine")
            .exec("cat > /tmp/h.cfg <<'CFG'\n\
global\n  daemon\n\
defaults\n  mode http\n  timeout connect 1s\n  timeout client 1s\n  timeout server 1s\n\
frontend f\n  bind :80\n  http-request return status 200 content-type \"text/plain\" string \"dd-haproxy-ok\"\n\
CFG\n\
haproxy -f /tmp/h.cfg -D; sleep 1; wget -qO- http://127.0.0.1/")
            .has("dd-haproxy-ok").timeout(60),

        // ---- varnish (C; VCL is JIT-compiled to C and dlopen'd — a real codegen path) -------------
        scen("web/varnish-version", "varnish:7.5")
            .exec("varnishd -V 2>&1 | head -1")
            .has("varnish-7.5").timeout(45),
        scen("web/varnish-version-74", "varnish:7.4")
            .exec("varnishd -V 2>&1 | head -1")
            .has("varnish-7").timeout(45),
        // varnishd -C compiles the VCL to C and dumps it — exercises the VCL→C codegen without a backend.
        scen("web/varnish-vcl-compile", "varnish:7.5")
            .exec("varnishd -C -f /etc/varnish/default.vcl 2>&1 | grep -o VRT_ | head -1")
            .has("VRT_").timeout(45),
        scen("web/varnish-vcl-compile-stable", "varnish:stable")
            .exec("varnishd -C -f /etc/varnish/default.vcl 2>&1 | grep -o VRT_ | head -1")
            .has("VRT_").timeout(45),

        // ---- httpd / apache — xfail both Linux arches (exec-loader-noent gap, GAPS.md) ------------
        // httpd's entry binary fails to load under dd (open: No such file or directory); proven correct
        // on Real. nginx works → binary-link-shape dependent. Same family as fork-exec.
        scen("web/httpd-serve", "httpd:alpine")
            .exec("httpd -k start 2>/dev/null; sleep 1; wget -qO- http://127.0.0.1/")
            .has("It works!").timeout(60)
            .xfail(BOTH),
        scen("web/httpd-version", "httpd:alpine")
            .exec("httpd -v 2>&1")
            .has("Apache/2.4").timeout(45)
            .xfail(BOTH),
        scen("web/httpd-custom", "httpd:alpine")
            .exec("echo dd-served-ok > /usr/local/apache2/htdocs/dd.txt; httpd -k start 2>/dev/null; \
                   sleep 1; wget -qO- http://127.0.0.1/dd.txt")
            .has("dd-served-ok").timeout(60)
            .xfail(BOTH),
        scen("web/httpd-config-test", "httpd:alpine")
            .exec("httpd -t 2>&1")
            .has("Syntax OK").timeout(45)
            .xfail(BOTH),
        // glibc apache (debian).
        scen("web/httpd-glibc-version", "httpd:2.4")
            .exec("httpd -v 2>&1")
            .has("Apache/2.4").timeout(45)
            .xfail(BOTH),
        // glibc apache config-test (debian httpd ships no wget/curl, so no loopback fetch here).
        scen("web/httpd-glibc-config", "httpd:2.4")
            .exec("httpd -t 2>&1")
            .has("Syntax OK").timeout(45)
            .xfail(BOTH),
    ])
}
