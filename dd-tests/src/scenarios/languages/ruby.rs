//! Ruby — MRI (YARV bytecode VM) on musl (alpine) + glibc (slim), 3.2/3.3. Markers per MANIFEST §2.

use crate::scenario::{scen, Scenario};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        scen("languages/ruby-sum-33-alpine", "ruby:3.3-alpine")
            .run(&["ruby", "-e", "puts (1..1000).sum"])
            .has("500500"),
        scen("languages/ruby-fib-32-alpine", "ruby:3.2-alpine")
            .run(&["ruby", "-e", "a,b=0,1;50.times{a,b=b,a+b};puts a"])
            .has("12586269025"),
        scen("languages/ruby-json-33-alpine", "ruby:3.3-alpine")
            .run(&["ruby", "-e", "require 'json';puts({s:(1..1000).sum}.to_json)"])
            .has("{\"s\":500500}"),
        scen("languages/ruby-primes-33-alpine", "ruby:3.3-alpine")
            .run(&["ruby", "-e", "puts (2...10000).count{|n| (2..Integer.sqrt(n)).all?{|d| n%d!=0}}"])
            .has("1229"),
        scen("languages/ruby-sort-repl-33-slim", "ruby:3.3-slim")
            .exec("echo 'puts [3,1,2].sort.join(\",\")' | ruby")
            .has("1,2,3"),
    ]
}
