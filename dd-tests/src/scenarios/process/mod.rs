//! Process basics — the daemon's container-launch contract: env-var passthrough (`-e`), working dir
//! (`-w`), exit-code propagation, stdout/stderr separation, clean SIGTERM stop (`docker stop`), and
//! `docker exec` into a running container. A few one-shot `run` cases cover PID-namespace init (pid 1)
//! and uid. Host-orchestrated where a docker flag is the thing under test. alpine, each sub-second.
//! Owner: process agent. Verified on the Real docker oracle. Edit ONLY this folder.

use crate::scenario::{scen, sgroup, ScenGroup};

pub fn group() -> ScenGroup {
    sgroup("process", vec![
        // ---- env-var passthrough (-e) ---------------------------------------------------------------
        scen("process/env-passthrough", "alpine:latest")
            .host("docker run --rm $PLAT -e DD_ENV=hello123 $IMG printenv DD_ENV").has("hello123"),
        scen("process/env-multiple", "alpine:latest")
            .host("docker run --rm $PLAT -e A=1 -e B=2 $IMG sh -c 'echo \"$A-$B\"'").has("1-2"),

        // ---- working directory (-w) -----------------------------------------------------------------
        scen("process/workdir", "alpine:latest")
            .host("docker run --rm $PLAT -w /etc $IMG pwd").has("/etc"),
        scen("process/workdir-created", "alpine:latest")
            .host("docker run --rm $PLAT -w /made/here $IMG pwd").has("/made/here"),

        // ---- exit-code propagation ------------------------------------------------------------------
        scen("process/exit-zero", "alpine:latest")
            .host("docker run --rm $PLAT $IMG true; echo \"rc=$?\"").has("rc=0"),
        scen("process/exit-nonzero", "alpine:latest")
            .host("docker run --rm $PLAT $IMG sh -c 'exit 7'; echo \"rc=$?\"").has("rc=7"),
        // exit code via the runner's own rc check (Run step → script rc is the container rc)
        scen("process/exit-rc-check", "alpine:latest")
            .run(&["sh", "-c", "exit 5"]).rc(5),

        // ---- stdout vs stderr separation ------------------------------------------------------------
        scen("process/stdout-stderr-split", "alpine:latest")
            .host("docker run --rm $PLAT $IMG sh -c 'echo OUTLINE; echo ERRLINE >&2' 1>\"$WORK/o\" 2>\"$WORK/e\"\necho \"O=$(cat \"$WORK/o\") E=$(cat \"$WORK/e\")\"")
            .has("O=OUTLINE").has("E=ERRLINE"),

        // ---- signals: docker stop sends SIGTERM, the container traps it and exits cleanly (0) --------
        scen("process/sigterm-clean-stop", "alpine:latest")
            .host("docker run -d --name ${C} $PLAT $IMG sh -c 'trap \"echo GOT_TERM; exit 0\" TERM; while true; do sleep 0.2; done' >/dev/null\n\
                   sleep 0.6\n\
                   docker stop -t 5 ${C} >/dev/null 2>&1\n\
                   echo \"EXIT=$(docker inspect -f '{{.State.ExitCode}}' ${C})\"\n\
                   docker logs ${C} 2>&1")
            .has("GOT_TERM").has("EXIT=0").timeout(30),

        // ---- docker exec into a running container ---------------------------------------------------
        scen("process/exec-into-running", "alpine:latest")
            .host("docker run -d --rm --name ${C} $PLAT $IMG sleep 30 >/dev/null\nsleep 0.3\ndocker exec ${C} echo EXEC_OK")
            .has("EXEC_OK").timeout(30),
        scen("process/exec-env", "alpine:latest")
            .host("docker run -d --rm --name ${C} $PLAT $IMG sleep 30 >/dev/null\nsleep 0.3\ndocker exec -e EE=zz ${C} printenv EE")
            .has("zz").timeout(30),
        scen("process/exec-sees-shared-fs", "alpine:latest")
            .host("docker run -d --rm --name ${C} $PLAT $IMG sleep 30 >/dev/null\nsleep 0.3\ndocker exec ${C} sh -c 'echo shared > /tmp/x'\ndocker exec ${C} cat /tmp/x")
            .has("shared").timeout(30),

        // ---- container identity ---------------------------------------------------------------------
        scen("process/hostname-flag", "alpine:latest")
            .host("docker run --rm $PLAT --hostname ddbox $IMG hostname").has("ddbox"),
        scen("process/pid1-is-init", "alpine:latest")
            .run(&["sh", "-c", "echo PID=$$"]).has("PID=1"),
        scen("process/uid-root", "alpine:latest")
            .run(&["id", "-u"]).eq_("0"),
    ])
}
