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
    ])
}
