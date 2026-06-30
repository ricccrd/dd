//! Terminal / interactive-shell coverage — the developer `docker exec -it /bin/bash` path with a REAL
//! PTY (`.tty()` → `docker run/exec -t`), the dimension every other category misses. Exercises the JIT's
//! pty/termios/ioctl/job-control syscalls: isatty, termios tcgetattr/tcsetattr, ioctl(TIOCGWINSZ/
//! TIOCSWINSZ), openpty, and job control (setpgrp, SIGTTIN/SIGTTOU, process groups). ubuntu:latest +
//! bash/zsh/dash/busybox + python pty. Both Linux arches. Every marker verified on the Real oracle.

use crate::scenario::{scen, sgroup, ScenGroup};

pub fn group() -> ScenGroup {
    sgroup("terminal", vec![
        // ---- isatty / TTY detection (the core `-it` assertion + the no-PTY contrast) --------------
        scen("terminal/ubuntu-isatty", "ubuntu:latest").tty()
            .exec("[ -t 0 ] && [ -t 1 ] && [ -t 2 ] && echo TTY-ALL").has("TTY-ALL").timeout(60),
        scen("terminal/ubuntu-notty", "ubuntu:latest")
            .exec("[ -t 1 ] && echo HAS || echo NO-TTY").has("NO-TTY").timeout(60),
        scen("terminal/ubuntu-tty-dev", "ubuntu:latest").tty()
            .exec("tty").has("/dev/pts/").timeout(60),
        scen("terminal/ubuntu-notty-dev", "ubuntu:latest")
            .exec("tty || true").has("not a tty").timeout(60),
        scen("terminal/debian-isatty", "debian:bookworm").tty()
            .exec("[ -t 1 ] && echo TTY-OK").has("TTY-OK").timeout(60),
        scen("terminal/alpine-isatty", "alpine").tty()
            .exec("[ -t 1 ] && echo TTY-OK").has("TTY-OK").timeout(60),
        scen("terminal/bash52-isatty", "bash:5.2").tty()
            .run(&["bash", "-c", "[ -t 1 ] && echo TTY-OK"]).has("TTY-OK").timeout(60),

        // ---- window size: ioctl TIOCGWINSZ (read) / TIOCSWINSZ (set) ------------------------------
        scen("terminal/ubuntu-tput-cols", "ubuntu:latest").tty()
            .exec("tput cols").has("80").timeout(60),
        scen("terminal/ubuntu-stty-setsize", "ubuntu:latest").tty()
            .exec("stty rows 40 cols 100; stty size").has("40 100").timeout(60),
        scen("terminal/alpine-stty-setsize", "alpine").tty()
            .exec("stty rows 24 cols 80; stty size").has("24 80").timeout(60),

        // ---- termios: tcgetattr / tcsetattr round-trips -------------------------------------------
        scen("terminal/ubuntu-stty-echo", "ubuntu:latest").tty()
            .exec("stty -echo; stty -a | grep -q -- '-echo' && echo ECHO-OFF").has("ECHO-OFF").timeout(60),
        scen("terminal/ubuntu-stty-sane", "ubuntu:latest").tty()
            .exec("stty sane && echo SANE-OK").has("SANE-OK").timeout(60),

        // ---- python pty / termios / winsize (stdlib, exact) --------------------------------------
        scen("terminal/py-isatty", "python:alpine").tty()
            .exec("python3 -c 'import sys; print(\"PYTTY\", sys.stdin.isatty(), sys.stdout.isatty())'")
            .has("PYTTY True True").timeout(60),
        scen("terminal/py-termsize", "python:alpine").tty()
            .exec("stty rows 30 cols 90; python3 -c 'import os; s=os.get_terminal_size(); print(\"SIZE\", s.columns, s.lines)'")
            .has("SIZE 90 30").timeout(60),
        scen("terminal/py-termios", "python:alpine").tty()
            .exec("python3 -c 'import sys,termios; a=termios.tcgetattr(sys.stdin.fileno()); print(\"TERMIOS\", len(a))'")
            .has("TERMIOS 7").timeout(60),
        // openpty/forkpty don't need an outer PTY — they create their own.
        scen("terminal/py-openpty", "python:alpine")
            .exec("python3 -c 'import os,pty; m,s=pty.openpty(); os.write(s,b\"hi\\n\"); print(\"OPENPTY\", os.read(m,3).decode().strip())'")
            .has("OPENPTY hi").timeout(60),

        // ---- real interactive shells (bash -i / dash / busybox ash / zsh) -------------------------
        scen("terminal/bash-interactive", "ubuntu:latest").tty()
            .exec("printf 'echo hello-bash-i\\nexit\\n' | bash -i 2>&1 | grep hello").has("hello-bash-i").timeout(60),
        scen("terminal/bash-dollar-flags", "ubuntu:latest").tty()
            .exec("bash -ic 'case $- in *i*) echo INTERACTIVE;; esac' 2>&1 | tail -1").has("INTERACTIVE").timeout(60),
        scen("terminal/dash-interactive", "ubuntu:latest").tty()
            .exec("printf 'echo hello-dash\\nexit\\n' | dash -i 2>&1 | grep hello").has("hello-dash").timeout(60),
        scen("terminal/busybox-interactive", "alpine").tty()
            .exec("printf 'echo hello-ash\\nexit\\n' | /bin/sh -i 2>&1 | grep hello").has("hello-ash").timeout(60),
        scen("terminal/zsh-interactive", "ubuntu:latest").tty()
            .exec("export DEBIAN_FRONTEND=noninteractive; apt-get update >/dev/null 2>&1; apt-get install -y zsh >/dev/null 2>&1; \
                   printf 'echo hello-zsh\\nexit\\n' | zsh -i 2>&1 | grep hello").has("hello-zsh").timeout(240).long(),
        scen("terminal/zsh-version", "ubuntu:latest")
            .exec("export DEBIAN_FRONTEND=noninteractive; apt-get update >/dev/null 2>&1; apt-get install -y zsh >/dev/null 2>&1; zsh --version")
            .has("zsh 5").timeout(240).long(),

        // ---- job control (the highest dd-risk path: pgrp / SIGTTIN-TTOU / TIOCSPGRP) --------------
        scen("terminal/jobctl-jobs", "ubuntu:latest").tty()
            .exec("set -m; sleep 0.4 & jobs | grep -c Running").has("1").timeout(60),
        scen("terminal/jobctl-wait-rc", "ubuntu:latest").tty()
            .exec("set -m; (exit 7) & wait $!; echo rc=$?").has("rc=7").timeout(60),
        scen("terminal/jobctl-fgbg", "ubuntu:latest").tty()
            .exec("set -m; sleep 0.5 & disown -h %1 2>/dev/null; jobs -p | grep -q '[0-9]' && echo JOB-PID-OK").has("JOB-PID-OK").timeout(60),
        // Foreground PIPELINE under interactive job control: the pipeline's group leader briefly sits in a
        // background process group (between its setpgid and the shell's tcsetpgrp) and calls tcsetpgrp to grab
        // the terminal -- it must NOT be SIGTTOU-stopped mid-handoff. Regression for the x86 "[1]+ Stopped
        // ls | cat" hang where the leader froze before it could exec. `bash -ic` enables real job control on
        // the PTY; the pipeline must complete and print its output (no "Stopped", no hang).
        scen("terminal/jobctl-fg-pipeline", "ubuntu:latest").tty()
            .exec("bash -ic 'ls / | cat >/dev/null && echo PIPE-OK' 2>&1 | tail -1").has("PIPE-OK").timeout(60),
        scen("terminal/setsid-newsession", "ubuntu:latest")
            .exec("setsid sh -c 'echo SETSID-OK' 2>&1 | head -1").has("SETSID-OK").timeout(60),

        // ---- terminal control / terminfo (escape-sequence emission) ------------------------------
        scen("terminal/tput-colors", "ubuntu:latest").tty()
            .exec("export TERM=xterm; tput colors").has("8").timeout(60),
        scen("terminal/tput-setaf-escape", "ubuntu:latest").tty()
            .exec("export TERM=xterm; tput setaf 1 | od -An -tx1 | tr -d ' \\n'").has("1b5b").timeout(60), // ESC[ = 0x1b 0x5b
        scen("terminal/clear-escape", "ubuntu:latest").tty()
            .exec("export TERM=xterm; clear | od -An -tx1 | tr -d ' \\n' | grep -q 1b5b && echo CLEAR-ESC").has("CLEAR-ESC").timeout(60),
    ])
}
