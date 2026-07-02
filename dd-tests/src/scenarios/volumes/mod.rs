//! Filesystem WITH a volume (`-v host:ctr`) — the bind-mount path through the daemon. Verifies two-way
//! visibility (container writes seen on the host and vice-versa), read-only enforcement, delete
//! propagation, multiple/nested mounts, and the regression where `..` out of a nested mountpoint must
//! cross back to the CONTAINER rootfs (not the host parent) — GAPS #118. Host-orchestrated (the harness
//! gives each case a private `$WORK` host dir, `$IMG`, `$PLAT`, and auto-cleanup). alpine, each fast.
//! Owner: volumes agent. Verified on the Real docker oracle. Edit ONLY this folder.

use crate::scenario::{scen, sgroup, ScenGroup};

pub fn group() -> ScenGroup {
    sgroup("volumes", vec![
        // container writes into the mount -> visible on the host
        scen("volumes/write-seen-on-host", "alpine:latest")
            .host("docker run --rm $PLAT -v \"$WORK\":/data $IMG sh -c 'echo CWROTE > /data/f.txt'\ncat \"$WORK/f.txt\"")
            .has("CWROTE"),
        // host writes into the mount -> visible in the container
        scen("volumes/host-seen-in-container", "alpine:latest")
            .host("echo HSEED > \"$WORK/h.txt\"\ndocker run --rm $PLAT -v \"$WORK\":/data $IMG cat /data/h.txt")
            .has("HSEED"),
        // read-only mount rejects writes
        scen("volumes/readonly-rejects-write", "alpine:latest")
            .host("docker run --rm $PLAT -v \"$WORK\":/data:ro $IMG sh -c 'echo x > /data/n 2>/dev/null || echo RO_REJECTED'")
            .has("RO_REJECTED"),
        // delete inside the container removes it on the host
        scen("volumes/delete-propagates", "alpine:latest")
            .host("echo a > \"$WORK/d.txt\"\ndocker run --rm $PLAT -v \"$WORK\":/data $IMG rm /data/d.txt\ntest ! -e \"$WORK/d.txt\" && echo DELETED")
            .has("DELETED"),
        // data persists across two separate runs of the same mount
        scen("volumes/persist-across-runs", "alpine:latest")
            .host("docker run --rm $PLAT -v \"$WORK\":/data $IMG sh -c 'echo persisted > /data/p'\ndocker run --rm $PLAT -v \"$WORK\":/data $IMG cat /data/p")
            .has("persisted"),
        // a subdir mount + listing its contents
        scen("volumes/subdir-mount", "alpine:latest")
            .host("mkdir -p \"$WORK/sub\"\necho inner > \"$WORK/sub/inner.txt\"\ndocker run --rm $PLAT -v \"$WORK/sub\":/mnt $IMG sh -c 'cat /mnt/inner.txt && ls /mnt'")
            .has("inner.txt").has("inner"),
        // two independent mounts in one container
        scen("volumes/two-mounts", "alpine:latest")
            .host("mkdir -p \"$WORK/m1\" \"$WORK/m2\"\necho one > \"$WORK/m1/a\"\necho two > \"$WORK/m2/b\"\ndocker run --rm $PLAT -v \"$WORK/m1\":/x -v \"$WORK/m2\":/y $IMG sh -c 'cat /x/a; cat /y/b'")
            .has("one").has("two"),
        // REGRESSION #118: `..` out of a nested mountpoint crosses to the container rootfs, NOT the host
        // parent dir. /mnt is a bind mount; `ls /mnt/..` must list the container root (has etc/bin), and
        // must NOT show the host sibling file we plant in $WORK next to sub/.
        scen("volumes/nested-dotdot-crosses-boundary", "alpine:latest")
            .host("mkdir -p \"$WORK/sub\"\necho host-sibling > \"$WORK/HOSTMARK\"\ndocker run --rm $PLAT -v \"$WORK/sub\":/mnt $IMG sh -c 'ls /mnt/.. | grep -qx etc && ls /mnt/.. | grep -qx bin && echo PARENT_IS_ROOTFS; ls /mnt/.. | grep -q HOSTMARK && echo LEAKED || echo NO_LEAK'")
            .has("PARENT_IS_ROOTFS").has("NO_LEAK"),

        // ---- basic commands ACROSS a volume: the everyday toolbox must work on bind-mounted files, not
        // just raw read/write. Each seeds files on the host ($WORK), runs a coreutils/busybox command in
        // the container against the mount (/d), and checks the result. These were previously uncovered. ----
        scen("volumes/cmd-cat-grep-wc", "alpine:latest")
            .host("printf 'apple\\nbanana\\ncherry\\n' > \"$WORK/f\"\ndocker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'cat /d/f | grep a | wc -l | tr -d \" \"'")
            .has("2"), // apple+banana contain 'a', cherry does not -> 2
        scen("volumes/cmd-cp-mv-rm", "alpine:latest")
            .host("echo hi > \"$WORK/a\"\ndocker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'cp /d/a /d/b && mv /d/b /d/c && rm /d/a && ls /d | sort | tr \"\\n\" \",\"'")
            .has("c,"),
        scen("volumes/cmd-sed-inplace", "alpine:latest")
            .host("echo 'foo=1' > \"$WORK/cfg\"\ndocker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'sed -i s/foo/bar/ /d/cfg'\ncat \"$WORK/cfg\"")
            .has("bar=1"),
        scen("volumes/cmd-append-redirect", "alpine:latest")
            .host("echo one > \"$WORK/log\"\ndocker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'echo two >> /d/log'\ntr \"\\n\" \" \" < \"$WORK/log\"")
            .has("one two"),
        scen("volumes/cmd-sort-head-tail", "alpine:latest")
            .host("printf '3\\n1\\n2\\n' > \"$WORK/n\"\ndocker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'echo MIN=$(sort /d/n | head -1); echo MAX=$(sort /d/n | tail -1)'")
            .has("MIN=1").has("MAX=3"),
        scen("volumes/cmd-chmod-perms", "alpine:latest")
            .host("echo x > \"$WORK/s\"\ndocker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'chmod 640 /d/s && ls -l /d/s | cut -c1-10'")
            .has("-rw-r-----"),
        scen("volumes/cmd-mkdir-touch-find", "alpine:latest")
            .host("docker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'mkdir -p /d/x/y && touch /d/x/y/z.txt && find /d -name z.txt'")
            .has("/d/x/y/z.txt"),
        scen("volumes/cmd-wc-bytes", "alpine:latest")
            .host("printf 'abcde' > \"$WORK/b\"\ndocker run --rm $PLAT -v \"$WORK\":/d $IMG sh -c 'wc -c < /d/b | tr -d \" \"'")
            .has("5"),
    ])
}
