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
    ]
}
