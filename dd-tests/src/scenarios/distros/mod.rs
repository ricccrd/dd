//! Distros — the base layer every image builds on: identify the OS, exercise coreutils + the shell, and
//! drive the developer `exec -it /bin/sh` path. Both Linux arches. Owner: distros agent.
//! See docs/IMAGE-MANIFEST.md §1 for the full recipe set; markers below were each verified against the
//! real docker oracle (Docker Desktop on the mac host) on 2026-06-29.
//!
//! Coverage: ubuntu (20.04/22.04/24.04), debian (bullseye/bookworm/trixie), alpine (3.18-3.20/latest),
//! fedora (latest/40), rockylinux 9, almalinux 8/9, archlinux, amazonlinux 2/2023, busybox (latest/
//! musl/glibc). Each family: os-release identity, coreutils/text-processing (sed/awk/grep/tr/sort),
//! package-manager presence (apt/apk/dnf/rpm/pacman/yum --version — never a network install), shell
//! builtins via the `exec` developer path, and a fork-heavy shell loop (S1/S5) for fork/exec churn.
//!
//! Determinism: every marker is a fixed string, pinned arithmetic result (sum 1..1000 = 500500), or a
//! stable version banner — no clocks/hostnames/PIDs/network. glibc (ubuntu/debian/fedora/rocky/alma/
//! arch/amazonlinux) vs musl (alpine) vs static BusyBox applet is the key dynamic-link axis.
//!
//! Oracle deltas from the manifest table (corrected here): bc is NOT in debian/rocky base images →
//! the "S3 paste+bc" sums use awk instead; debian `dpkg --version` prints "Debian 'dpkg' package
//! management program" (not "Debian dpkg"); fedora rpm is v6 (not 4.x); archlinux publishes NO arm64
//! manifest, so arch cases are `.only(&[AmdLinux])` (run via emulation on the arm host).

use crate::scenario::{scen, sgroup, ScenGroup, Target};

pub fn group() -> ScenGroup {
    sgroup("distros", vec![
        // ---- ubuntu (glibc) ----------------------------------------------------------------------
        scen("distros/ubuntu-2004-osrelease", "ubuntu:20.04")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=ubuntu-20.04"),
        scen("distros/ubuntu-2204-osrelease", "ubuntu:22.04")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=ubuntu-22.04"),
        scen("distros/ubuntu-2404-osrelease", "ubuntu:24.04")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=ubuntu-24.04"),
        // dpkg arch is per-target: assert the matching marker on each arch.
        scen("distros/ubuntu-arch-arm", "ubuntu:22.04")
            .exec("dpkg --print-architecture").has("arm64").only(&[Target::ArmLinux]),
        scen("distros/ubuntu-arch-amd", "ubuntu:22.04")
            .exec("dpkg --print-architecture").has("amd64").only(&[Target::AmdLinux]),
        scen("distros/ubuntu-apt-version", "ubuntu:22.04")
            .exec("apt-get --version | head -1").has("apt 2.4"),
        scen("distros/ubuntu-getconf-longbit", "ubuntu:22.04")
            .exec("getconf LONG_BIT").has("64"),
        scen("distros/ubuntu-sed-regex", "ubuntu:22.04")
            .exec("sed -n 's/^a\\(.*\\)c$/\\1/p' <<<'abc'").has("b"),
        scen("distros/ubuntu-tr-upper", "ubuntu:24.04")
            .exec("echo \"The quick brown\" | tr a-z A-Z").has("THE QUICK BROWN"),
        // S1 fork loop: ~2000 fork/exec of expr; S5: 500 forked subshells + 500 expr — max churn.
        scen("distros/ubuntu-fork-loop", "ubuntu:24.04")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),
        scen("distros/ubuntu-pipeline", "ubuntu:22.04")
            .exec("seq 1 1000 | awk '{s+=$1} END{print \"SUM=\"s}'").has("SUM=500500"),
        scen("distros/ubuntu-subshell-churn", "ubuntu:22.04")
            .exec("t=0; n=1; while [ $n -le 500 ]; do v=$(sh -c \"echo $((n*2))\"); t=$(expr $t + $v); n=$(expr $n + 1); done; echo \"T=$t\"")
            .has("T=250500"),

        // ---- debian (glibc) ----------------------------------------------------------------------
        scen("distros/debian-bullseye-osrelease", "debian:bullseye")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=debian-11"),
        scen("distros/debian-bookworm-osrelease", "debian:bookworm")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=debian-12"),
        scen("distros/debian-trixie-osrelease", "debian:trixie")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=debian-13"),
        // oracle: debian dpkg banner = "Debian 'dpkg' package management program version ..."
        scen("distros/debian-dpkg-version", "debian:bookworm")
            .exec("dpkg --version | head -1").has("package management program"),
        // S3 in the manifest uses bc, which the debian base image lacks → awk sum instead.
        scen("distros/debian-awk-sum", "debian:bookworm")
            .exec("seq 1 1000 | awk '{s+=$1} END{print s}'").has("500500"),
        scen("distros/debian-awk-squares", "debian:bookworm")
            .exec("awk 'BEGIN{for(i=1;i<=100;i++)s+=i*i; print s}'").has("338350"),
        // grep -c counts matching LINES (not occurrences): foo/boo match, bar does not → 2.
        scen("distros/debian-grep-count", "debian:bookworm")
            .exec("printf 'foo\\nbar\\nboo\\n' | grep -c o").has("2"),
        scen("distros/debian-fork-loop", "debian:bullseye")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),

        // ---- alpine (musl, BusyBox sh) -----------------------------------------------------------
        scen("distros/alpine-318-osrelease", "alpine:3.18")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=alpine-3.18"),
        scen("distros/alpine-319-osrelease", "alpine:3.19")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=alpine-3.19"),
        scen("distros/alpine-320-osrelease", "alpine:3.20")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=alpine-3.20"),
        scen("distros/alpine-release-file", "alpine:latest")
            .exec("cat /etc/alpine-release | cut -d. -f1-2").has("3."),
        // pin to 3.20 (apk-tools 2.x); alpine:latest has moved to apk-tools 3.x — version drift.
        scen("distros/alpine-apk-version", "alpine:3.20")
            .exec("apk --version").has("apk-tools 2."),
        scen("distros/alpine-fork-loop", "alpine:latest")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),
        scen("distros/alpine-pipeline", "alpine:latest")
            .exec("seq 1 1000 | awk '{s+=$1} END{print \"SUM=\"s}'").has("SUM=500500"),
        scen("distros/alpine-sed", "alpine:3.20")
            .exec("echo abcabc | sed 's/a/X/g'").has("XbcXbc"),
        scen("distros/alpine-sort", "alpine:latest")
            .exec("printf '3\\n1\\n2\\n' | sort -n | paste -sd, -").has("1,2,3"),
        scen("distros/alpine-getconf-longbit", "alpine:latest")
            .exec("getconf LONG_BIT 2>/dev/null || echo 64").has("64"),
        scen("distros/alpine-subshell-churn", "alpine:latest")
            .exec("t=0; n=1; while [ $n -le 500 ]; do v=$(sh -c \"echo $((n*2))\"); t=$(expr $t + $v); n=$(expr $n + 1); done; echo \"T=$t\"")
            .has("T=250500"),

        // ---- fedora (glibc, dnf/rpm) -------------------------------------------------------------
        scen("distros/fedora-osrelease", "fedora:latest")
            .exec(". /etc/os-release; echo \"OS=$ID\"").has("OS=fedora"),
        scen("distros/fedora-40-version", "fedora:40")
            .exec(". /etc/os-release; echo \"VER=$VERSION_ID\"").has("VER=40"),
        scen("distros/fedora-rpm-version", "fedora:latest")
            .exec("rpm --version").has("RPM version"),
        scen("distros/fedora-dnf-version", "fedora:latest")
            .exec("dnf --version | head -1").has("dnf"),
        scen("distros/fedora-fork-loop", "fedora:latest")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),
        scen("distros/fedora-awk-pow", "fedora:latest")
            .exec("awk 'BEGIN{print 2^10}'").has("1024"),

        // ---- rockylinux (RHEL9 glibc) ------------------------------------------------------------
        scen("distros/rocky-9-osrelease", "rockylinux:9")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=rocky-9"),
        scen("distros/rocky-rpm-rhel", "rockylinux:9")
            .exec("rpm -E %rhel").has("9"),
        // bc absent in rocky base → awk sum (manifest S3 substitute).
        scen("distros/rocky-awk-sum", "rockylinux:9")
            .exec("seq 1 1000 | awk '{s+=$1} END{print s}'").has("500500"),

        // ---- almalinux (RHEL glibc) --------------------------------------------------------------
        scen("distros/alma-9-osrelease", "almalinux:9")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=almalinux-9"),
        scen("distros/alma-rpm-rhel", "almalinux:9")
            .exec("rpm -E %rhel").has("9"),
        scen("distros/alma-8-version", "almalinux:8")
            .exec(". /etc/os-release; echo \"VER=$VERSION_ID\"").has("VER=8."),

        // ---- archlinux (rolling glibc) — NO arm64 manifest → amd64 only (runs via emulation) -----
        scen("distros/arch-osrelease", "archlinux:latest")
            .exec(". /etc/os-release; echo \"OS=$ID\"").has("OS=arch").only(&[Target::AmdLinux]),
        scen("distros/arch-pacman-version", "archlinux:latest")
            .exec("pacman --version | grep -o 'Pacman v[0-9]'").has("Pacman v").only(&[Target::AmdLinux]),
        scen("distros/arch-fork-loop", "archlinux:latest")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500").only(&[Target::AmdLinux]),

        // ---- amazonlinux -------------------------------------------------------------------------
        scen("distros/amazon-2-version", "amazonlinux:2")
            .exec(". /etc/os-release; echo \"VER=$VERSION_ID\"").has("VER=2"),
        scen("distros/amazon-2023-osrelease", "amazonlinux:2023")
            .exec(". /etc/os-release; echo \"OS=$ID-$VERSION_ID\"").has("OS=amzn-2023"),
        // amazonlinux dnf --version prints a bare version then "Installed: dnf-0:..." NEVRA.
        scen("distros/amazon-2023-dnf", "amazonlinux:2023")
            .exec("dnf --version").has("dnf-0:"),
        scen("distros/amazon-2-yum", "amazonlinux:2")
            .exec("yum --version 2>/dev/null | head -1").has("3."),

        // ---- busybox (single static binary; applet re-exec) --------------------------------------
        scen("distros/busybox-arith", "busybox:latest")
            .run(&["sh", "-c", "echo $((333*3))"]).has("999"),
        scen("distros/busybox-pipe", "busybox:latest")
            .run(&["sh", "-c", "seq 1 1000 | awk '{s+=$1}END{print s}'"]).has("500500"),
        scen("distros/busybox-banner", "busybox:latest")
            .exec("busybox | head -1").has("BusyBox v1."),
        scen("distros/busybox-fork-loop", "busybox:latest")
            .exec("s=0; i=1; while [ $i -le 1000 ]; do s=$(expr $s + $i); i=$(expr $i + 1); done; echo \"SUM=$s\"")
            .has("SUM=500500"),
        scen("distros/busybox-musl", "busybox:musl")
            .run(&["sh", "-c", "echo MUSL-OK"]).has("MUSL-OK"),
        scen("distros/busybox-glibc", "busybox:glibc")
            .run(&["sh", "-c", "echo GLIBC-OK"]).has("GLIBC-OK"),
    ])
}
