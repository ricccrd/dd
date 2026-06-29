//! Python — CPython on glibc (slim) and musl (alpine), 3.11/3.12/3.13. Deterministic compute
//! (sum/fib/primes/dict/json) via one-shot `run` + a stdin REPL `exec`. Markers per IMAGE-MANIFEST §2.
//! Known gap: python:3.12-slim silently exits 255 on amd64 (GAPS jit86-opcode-1c) → xfail AmdLinux.

use crate::scenario::{scen, Scenario, Target};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        // --- one-shot compute (run = container ENTRYPOINT + argv) ---
        scen("languages/python-sum-311-slim", "python:3.11-slim")
            .run(&["python", "-c", "print(sum(range(1,1001)))"])
            .has("500500"),
        scen("languages/python-fib-312-slim", "python:3.12-slim")
            .run(&["python", "-c", "a,b=0,1\nfor _ in range(50):a,b=b,a+b\nprint(a)"])
            .has("12586269025")
            .xfail(&[Target::AmdLinux]), // GAPS jit86-opcode-1c: 3.12-slim silent 255 on amd64
        scen("languages/python-primes-313-slim", "python:3.13-slim")
            .run(&["python", "-c", "print(sum(1 for n in range(2,10000) if all(n%d for d in range(2,int(n**.5)+1))))"])
            .has("1229"),
        scen("languages/python-sum-312-alpine", "python:3.12-alpine")
            .run(&["python", "-c", "print(sum(range(1,1001)))"])
            .has("500500"),
        scen("languages/python-json-311-alpine", "python:3.11-alpine")
            .run(&["python", "-c", "import json;print(json.dumps({'s':sum(range(1,1001))},sort_keys=True))"])
            .has("{\"s\": 500500}"),
        scen("languages/python-listcomp-313-alpine", "python:3.13-alpine")
            .run(&["python", "-c", "print(len([x*x for x in range(1000)]))"])
            .has("1000"),
        scen("languages/python-fib-313-alpine", "python:3.13-alpine")
            .run(&["python", "-c", "a,b=0,1\nfor _ in range(50):a,b=b,a+b\nprint(a)"])
            .has("12586269025"),
        scen("languages/python-sort-312-alpine", "python:3.12-alpine")
            .run(&["python", "-c", "print(','.join(map(str,sorted([3,1,2]))))"])
            .has("1,2,3"),
        // --- developer REPL / stdin path (exec -i) ---
        scen("languages/python-bigint-repl-312-alpine", "python:3.12-alpine")
            .exec("echo 'print(2**100)' | python3")
            .has("1267650600228229401496703205376"),
        // dict/hash workload — run form (no shell) so the dict-comprehension braces survive intact.
        scen("languages/python-dictsum-312-slim", "python:3.12-slim")
            .run(&["python", "-c", "d={i:i for i in range(1,1001)}\nprint('DICT', sum(d.values()))"])
            .has("DICT 500500")
            .xfail(&[Target::AmdLinux]), // GAPS jit86-opcode-1c
    ]
}
