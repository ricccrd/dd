//! Node.js — V8 JIT-in-JIT (PRIME engine stress: V8 emits machine code the dd JIT must in turn
//! translate), 18/20/22 on glibc (slim) + musl (alpine). Markers per IMAGE-MANIFEST §2.
//! Known gap: node:20-slim → jit86 UNIMPL opcode 0x1c on amd64 (GAPS jit86-opcode-1c) → xfail AmdLinux.

use crate::scenario::{scen, Scenario, Target};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        scen("languages/node-sum-18-slim", "node:18-slim")
            .run(&["node", "-e", "console.log([...Array(1000)].reduce((a,_,i)=>a+i+1,0))"])
            .has("500500"),
        scen("languages/node-bigint-20-slim", "node:20-slim")
            .run(&["node", "-e", "let a=0n,b=1n;for(let i=0;i<50;i++){[a,b]=[b,a+b]}console.log(a.toString())"])
            .has("12586269025")
            .xfail(&[Target::AmdLinux]), // GAPS jit86-opcode-1c: node:20-slim UNIMPL 0x1c on amd64
        scen("languages/node-json-22-slim", "node:22-slim")
            .run(&["node", "-e", "console.log(JSON.stringify({s:[...Array(1000)].reduce((a,_,i)=>a+i+1,0)}))"])
            .has("{\"s\":500500}"),
        scen("languages/node-add-18-alpine", "node:18-alpine")
            .run(&["node", "-e", "console.log(1+2+3)"])
            .has("6"),
        // #252: libuv's event loop must exit cleanly. setTimeout drives one epoll_wait(-1) cycle then the
        // loop drains -- a spurious 0-return from epoll_wait(-1) trips `assert(timeout != -1)` in uv__io_poll
        // and node aborts. Must print TICK and exit 0.
        scen("languages/node-settimeout-22-alpine", "node:22-alpine")
            .run(&["node", "-e", "setTimeout(()=>console.log('TICK'),50)"])
            .has("TICK"),
        // #252: even a purely-synchronous script tripped the same assert at loop teardown. Must exit 0.
        scen("languages/node-sync-22-alpine", "node:22-alpine")
            .run(&["node", "-e", "console.log(1+1)"])
            .has("2"),
        scen("languages/node-sort-20-alpine", "node:20-alpine")
            .run(&["node", "-e", "console.log([3,1,2].sort().join(','))"])
            .has("1,2,3"),
        scen("languages/node-version-repl-22-alpine", "node:22-alpine")
            .exec("echo \"console.log('NODEMAJOR', process.version[0])\" | node")
            .has("NODEMAJOR v"),
        // V8 hot loop: primes < 10000 = 1229 (tier-up to optimizing compiler under the dd JIT)
        scen("languages/node-primes-20-alpine", "node:20-alpine")
            .exec("node -e \"let p=0;for(let n=2;n<10000;n++){let q=1;for(let d=2;d*d<=n;d++)if(n%d===0){q=0;break}p+=q}console.log('PRIMES',p)\"")
            .has("PRIMES 1229"),
        scen("languages/node-fib-22-alpine", "node:22-alpine")
            .run(&["node", "-e", "let a=0n,b=1n;for(let i=0;i<50;i++){[a,b]=[b,a+b]}console.log(a.toString())"])
            .has("12586269025"),

        // ---- interactive REPL (`node -i` reading from a pipe): the eval loop, not the one-shot -e path.
        // Feeds statements on stdin, each evaluated + printed; distinct code path (readline + repl.eval).
        scen("languages/node-repl-eval-22-alpine", "node:22-alpine")
            .exec("printf 'console.log(\"R\"+(6*7))\\n.exit\\n' | node -i 2>&1 | grep -q 'R42' && echo REPL_OK")
            .has("REPL_OK"),
        // PERF GATE — the REPL must START + evaluate promptly. A pathological indirect-dispatch/JIT
        // slowdown (regression of #166 node-V8-IBTC) blows the tight timeout -> exit 124 -> FAILS on
        // the Dd backend while the Real oracle finishes in ~1s. Workload is trivial (1+1) so ONLY
        // startup+dispatch speed is measured. If this reddens, node interactive is "too slow" again.
        scen("languages/node-repl-perf-20-alpine", "node:20-alpine")
            .exec("printf '1+1\\n.exit\\n' | node -i >/dev/null 2>&1 && echo REPL_FAST")
            .has("REPL_FAST")
            .timeout(25),
    ]
}
