//! Utilities / devtools — the everyday developer-at-a-shell surface. busybox, coreutils, sed/awk/grep,
//! tar/gzip, jq, openssl, git, curl, socat, bash. Deterministic, HERMETIC (no network: loopback only,
//! no package installs) workflows that push fork-heavy pipelines, text/crypto codegen, and real tool
//! binaries through the JIT. Both Linux arches. Owner: utilities agent. Recipes: docs/IMAGE-MANIFEST §6.
//!
//! Harness constraint discovered during authoring: single-tool images (jq, alpine/git, alpine/openssl,
//! alpine/socat, curlimages/curl) use the TOOL as their ENTRYPOINT, so the `exec` form (which appends
//! `/bin/sh -c …`) can't drive them — those use `.run(argv)`. Shell workflows use base images
//! (alpine/busybox/debian) or `bitnami/git` (passthrough entrypoint + bash). `bash:5.2` ships bash only
//! at /usr/local/bin (no /bin/bash) → it also uses `.run(&["bash","-c",…])`.
//!
//! All hashes are published vectors or verified-once-and-pinned on the Real oracle (Docker Desktop):
//!   sha256("abc")=ba7816bf… · sha256("")=e3b0c442… · md5("abc")=90015098… · empty git blob=e69de29b…
//!   git blob "dd\n"=f03f6945… · fixed-identity commit SHA=9fba1c3d… (all inputs pinned → reproducible).

use crate::scenario::{scen, sgroup, ScenGroup, Target};

pub fn group() -> ScenGroup {
    sgroup("utilities", vec![
        // ---- crypto / digests --------------------------------------------------------------------
        // sha256("abc") = published NIST vector. busybox/coreutils sha256 applet (musl).
        scen("utilities/sha256-abc", "alpine")
            .exec("printf abc | sha256sum | cut -d' ' -f1")
            .has("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        // same vector through glibc coreutils — distinct libc/codegen path.
        scen("utilities/sha256-abc-glibc", "debian:bookworm")
            .exec("printf abc | sha256sum | cut -d' ' -f1")
            .has("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        // md5("abc") = published vector.
        scen("utilities/md5-abc", "alpine")
            .exec("printf abc | md5sum | cut -d' ' -f1")
            .has("900150983cd24fb0d6963f7d28e17f72"),
        // real OpenSSL EVP digest — entrypoint=openssl, so run-form; empty stdin → sha256("") vector.
        scen("utilities/openssl-sha256-empty", "alpine/openssl:latest")
            .run(&["dgst", "-sha256", "-r"])
            .has("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        scen("utilities/openssl-version", "alpine/openssl:latest")
            .run(&["version"])
            .has("OpenSSL 3"),
        // base64 encode / round-trip (deterministic).
        scen("utilities/base64-encode", "alpine")
            .exec("printf dd | base64")
            .has("ZGQ="),
        scen("utilities/base64-roundtrip", "alpine")
            .exec("printf hello | base64 | base64 -d")
            .has("hello"),

        // ---- text processing: sed / awk / grep / sort / coreutils --------------------------------
        scen("utilities/sort-numeric", "alpine")
            .exec("printf '3\\n1\\n2\\n' | sort -n | paste -sd, -")
            .has("1,2,3"),
        // S4 sort|uniq -c counts: a×2,b×2,c×1.
        scen("utilities/sort-uniq-count", "alpine")
            .exec("printf 'b\\na\\nc\\na\\nb\\n' | sort | uniq -c | awk '{print $1$2}' | paste -sd, -")
            .has("2a,2b,1c"),
        scen("utilities/sort-uniq-count-glibc", "debian:bookworm")
            .exec("printf 'b\\na\\nc\\na\\nb\\n' | sort | uniq -c | awk '{print $1$2}' | paste -sd, -")
            .has("2a,2b,1c"),
        // word-frequency pipeline: sort|uniq -c|sort -rn|head|awk — 5-stage fork-heavy pipe.
        scen("utilities/word-frequency", "alpine")
            .exec("printf 'apple\\nbanana\\napple\\ncherry\\nbanana\\napple\\n' | sort | uniq -c | sort -rn | head -1 | awk '{print $2}'")
            .has("apple"),
        scen("utilities/tr-upper", "alpine")
            .exec("echo 'the quick brown' | tr a-z A-Z")
            .has("THE QUICK BROWN"),
        scen("utilities/cut-field", "alpine")
            .exec("echo a:b:c | cut -d: -f2")
            .has("b"),
        scen("utilities/grep-count", "alpine")
            .exec("printf 'foo\\nbar\\nfoo\\n' | grep -c foo")
            .has("2"),
        scen("utilities/sed-substitute", "alpine")
            .exec("echo abcabc | sed 's/a/X/g'")
            .has("XbcXbc"),
        // awk integer compute: sum of squares 1..100 = 338350.
        scen("utilities/awk-squares", "alpine")
            .exec("awk 'BEGIN{for(i=1;i<=100;i++)s+=i*i;print s}'")
            .has("338350"),
        scen("utilities/awk-squares-glibc", "debian:bookworm")
            .exec("awk 'BEGIN{for(i=1;i<=100;i++)s+=i*i;print s}'")
            .has("338350"),
        scen("utilities/head-tail", "alpine")
            .exec("seq 1 10 | head -3 | tail -1")
            .has("3"),
        scen("utilities/wc-lines", "alpine")
            .exec("seq 1 100 | wc -l")
            .has("100"),
        scen("utilities/wc-chars", "alpine")
            .exec("printf abc | wc -c")
            .has("3"),
        // factor: largest prime factor of 500500 = 13 (500500 = 2^2·5^3·7·11·13).
        scen("utilities/factor", "alpine")
            .exec("factor 500500 | tr ' ' '\\n' | tail -n1")
            .has("13"),
        scen("utilities/factor-glibc", "debian:bookworm")
            .exec("factor 500500 | tr ' ' '\\n' | tail -n1")
            .has("13"),

        // ---- arithmetic pipelines ----------------------------------------------------------------
        // S2: two-process pipe, awk sum 1..1000 = 500500.
        scen("utilities/seq-awk-sum", "alpine")
            .exec("seq 1 1000 | awk '{s+=$1} END{print \"SUM=\"s}'")
            .has("SUM=500500"),
        // S3: 3-stage pipe through bc (arbitrary precision).
        scen("utilities/paste-bc", "alpine")
            .exec("seq 1 1000 | paste -sd+ - | bc")
            .has("500500"),
        scen("utilities/paste-bc-bash", "bash:5.2")
            .run(&["bash", "-c", "seq 1 1000 | paste -sd+ - | bc"])
            .has("500500"),

        // ---- fork-heavy shell workflows (the core JIT stressor) ----------------------------------
        // S1: ~2000 fork/exec of expr per iteration. musl applet re-exec.
        scen("utilities/fork-loop", "alpine")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),
        // glibc variant — different fork/exec + dynamic-linker path.
        scen("utilities/fork-loop-glibc", "debian:bookworm")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),
        // S5: 500 forked subshells each spawning sh+expr → maximal process-tree churn. T=250500.
        scen("utilities/fork-subshells", "alpine")
            .exec("t=0; n=1; while [ $n -le 500 ]; do v=$(sh -c \"echo $((n*2))\"); t=$(expr $t + $v); n=$(expr $n + 1); done; echo \"T=$t\"")
            .has("T=250500"),
        scen("utilities/fork-subshells-glibc", "debian:bookworm")
            .exec("t=0; n=1; while [ $n -le 500 ]; do v=$(sh -c \"echo $((n*2))\"); t=$(expr $t + $v); n=$(expr $n + 1); done; echo \"T=$t\"")
            .has("T=250500"),

        // ---- tar / gzip round-trips --------------------------------------------------------------
        scen("utilities/tar-roundtrip", "alpine")
            .exec("cd /tmp && rm -rf d a.tar && mkdir d && echo dd-tar-ok > d/f.txt && tar cf a.tar d && rm -rf d && tar xf a.tar && cat d/f.txt")
            .has("dd-tar-ok"),
        scen("utilities/gzip-roundtrip", "alpine")
            .exec("printf 'dd-gzip-ok\\n' | gzip | gunzip")
            .has("dd-gzip-ok"),
        // tar+gzip combined, content checked via awk sum after extract.
        scen("utilities/targz-roundtrip", "alpine")
            .exec("cd /tmp && rm -rf g g.tgz && mkdir g && seq 1 1000 > g/n.txt && tar czf g.tgz g && rm -rf g && tar xzf g.tgz && awk '{s+=$1}END{print s}' g/n.txt")
            .has("500500"),
        scen("utilities/tar-roundtrip-glibc", "debian:bookworm")
            .exec("cd /tmp && rm -rf d a.tar && mkdir d && echo dd-tar-ok > d/f.txt && tar cf a.tar d && rm -rf d && tar xf a.tar && cat d/f.txt")
            .has("dd-tar-ok"),
        scen("utilities/gzip-roundtrip-glibc", "debian:bookworm")
            .exec("printf 'dd-gzip-ok\\n' | gzip | gunzip")
            .has("dd-gzip-ok"),

        // ---- busybox (single static musl multi-call binary) --------------------------------------
        scen("utilities/busybox-arith", "busybox:latest")
            .run(&["sh", "-c", "echo $((7*6))"])
            .has("42"),
        scen("utilities/busybox-pipe", "busybox:latest")
            .run(&["sh", "-c", "seq 1 1000 | awk '{s+=$1}END{print s}'"])
            .has("500500"),
        scen("utilities/busybox-banner", "busybox:latest")
            .exec("busybox 2>&1 | head -1")
            .has("BusyBox v1.3"),
        scen("utilities/busybox-fork-loop", "busybox:latest")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),
        scen("utilities/busybox-sha256", "busybox:latest")
            .exec("printf abc | sha256sum | cut -d' ' -f1")
            .has("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),

        // ---- bash builtins (run-form: bash:5.2 has bash only under /usr/local/bin) ----------------
        scen("utilities/bash-base-convert", "bash:5.2")
            .run(&["bash", "-c", "echo $((2#1010))"])
            .has("10"),
        scen("utilities/bash-brace-arith", "bash:5.2")
            .run(&["bash", "-c", "for i in {1..1000}; do :; done; echo $((1000*1001/2))"])
            .has("500500"),
        scen("utilities/bash-arrays", "bash:5.2")
            .run(&["bash", "-c", "a=(1 2 3); echo ${#a[@]}"])
            .has("3"),
        scen("utilities/bash-param-expand", "bash:5.2")
            .run(&["bash", "-c", "echo ${x:-default}"])
            .has("default"),
        scen("utilities/bash-version", "bash:5.2")
            .run(&["bash", "--version"])
            .has("version 5.2"),

        // ---- jq (VM over fixed JSON; entrypoint=jq → run-form) -----------------------------------
        scen("utilities/jq-add", "ghcr.io/jqlang/jq:latest")
            .run(&["-n", "[range(1;1001)]|add"])
            .has("500500"),
        scen("utilities/jq-object", "ghcr.io/jqlang/jq:latest")
            .run(&["-n", "{a:1,b:2}|.a+.b"])
            .has("3"),
        scen("utilities/jq-sort", "ghcr.io/jqlang/jq:latest")
            .run(&["-nc", "[3,1,2]|sort"])
            .has("[1,2,3]"),
        scen("utilities/jq-version", "ghcr.io/jqlang/jq:latest")
            .run(&["--version"])
            .has("jq-1."),

        // ---- git (alpine/git entrypoint=git → run banner; bitnami/git passthrough → exec workflow) -
        scen("utilities/git-version", "alpine/git:latest")
            .run(&["--version"])
            .has("git version 2."),
        // canonical empty-blob SHA (well-known, version-independent).
        scen("utilities/git-empty-blob", "bitnami/git:latest")
            .exec("printf '' | git hash-object --stdin")
            .has("e69de29bb2d1d6434b8b29ae775ad8c2e48c5391"),
        // blob of "dd\n" — verified+pinned on the Real oracle.
        scen("utilities/git-hashobject-dd", "bitnami/git:latest")
            .exec("printf 'dd\\n' | git hash-object --stdin")
            .has("f03f6945fbf941fa91cb460eab583c7f36c8cee3"),
        // init + add + commit + log — fixed identity so the message is stable.
        scen("utilities/git-init-commit", "bitnami/git:latest")
            .exec("export GIT_AUTHOR_NAME=dd GIT_AUTHOR_EMAIL=dd@dd GIT_COMMITTER_NAME=dd GIT_COMMITTER_EMAIL=dd@dd; \
                   export GIT_AUTHOR_DATE='2000-01-01T00:00:00Z' GIT_COMMITTER_DATE='2000-01-01T00:00:00Z'; \
                   cd /tmp && rm -rf r && mkdir r && cd r && git init -q && echo dd > f && git add f && \
                   git commit -q -m 'dd: first commit' && git log --format='%s' -1")
            .has("dd: first commit"),
        // fully deterministic commit SHA: every input (tree, identity, dates, message) pinned.
        scen("utilities/git-deterministic-sha", "bitnami/git:latest")
            .exec("export GIT_AUTHOR_NAME=dd GIT_AUTHOR_EMAIL=dd@dd GIT_COMMITTER_NAME=dd GIT_COMMITTER_EMAIL=dd@dd; \
                   export GIT_AUTHOR_DATE='2000-01-01T00:00:00Z' GIT_COMMITTER_DATE='2000-01-01T00:00:00Z'; \
                   cd /tmp && rm -rf r && mkdir r && cd r && git init -q && echo dd > f && git add f && \
                   git commit -q -m 'dd: first commit' && git rev-parse HEAD")
            .has("9fba1c3dda82182611817eab9c713c8f5afbd0c1"),

        // ---- curl / socat / loopback networking --------------------------------------------------
        // hermetic: banner only (no network round-trip).
        scen("utilities/curl-version", "curlimages/curl:latest")
            .run(&["--version"])
            .has("curl 8."),
        scen("utilities/socat-version", "alpine/socat:latest")
            .run(&["-V"])
            .has("socat by Gerhard Rieger"),
        // loopback TCP echo via busybox nc — fork + 127.0.0.1 round-trip, fully hermetic.
        scen("utilities/nc-loopback", "alpine")
            .exec("{ echo dd-echo-ok | nc -l -p 9000; } & sleep 0.4; nc 127.0.0.1 9000 </dev/null")
            .has("dd-echo-ok"),
        // loopback HTTP: busybox httpd serves a file, wget fetches it (server+client fork, no network).
        scen("utilities/wget-loopback", "busybox:latest")
            .exec("mkdir -p /www && echo dd-http-ok > /www/f.txt && httpd -p 127.0.0.1:8080 -h /www && sleep 0.3 && wget -qO- http://127.0.0.1:8080/f.txt")
            .has("dd-http-ok"),

        // ---- tiny static exec path ---------------------------------------------------------------
        // passes on Real; known dd loader gap (exec-loader-noent) on both linux arches → xfail.
        scen("utilities/hello-world", "hello-world:latest")
            .run(&[])
            .has("Hello from Docker!")
            .xfail(&[Target::ArmLinux, Target::AmdLinux]),
    ])
}
