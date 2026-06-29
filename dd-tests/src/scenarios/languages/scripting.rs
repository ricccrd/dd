//! Perl + Elixir — Perl interp (glibc/slim) and the BEAM VM (Elixir/Erlang, scheduler threads, musl
//! alpine + glibc). Markers per IMAGE-MANIFEST §2.

use crate::scenario::{scen, Scenario};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        // --- Perl ---
        scen("languages/perl-sum-538", "perl:5.38")
            .run(&["perl", "-e", "$s+=$_ for 1..1000;print $s"])
            .has("500500"),
        scen("languages/perl-sort-538-slim", "perl:5.38-slim")
            .run(&["perl", "-e", "print join(',',sort{$a<=>$b}(3,1,2))"])
            .has("1,2,3"),
        scen("languages/perl-primes-538", "perl:5.38")
            .run(&["perl", "-e", "$c=0;for $n (2..9999){$p=1;for $d (2..int(sqrt($n))){if($n%$d==0){$p=0;last}}$c+=$p}print $c"])
            .has("1229"),
        scen("languages/perl-mult-repl-540", "perl:5.40")
            .exec("echo 'print 6*7' | perl")
            .has("42"),
        // --- Elixir (BEAM VM) ---
        scen("languages/elixir-sum-116-alpine", "elixir:1.16-alpine")
            .run(&["elixir", "-e", "IO.puts Enum.sum(1..1000)"])
            .has("500500")
            .long(),
        scen("languages/elixir-reduce-117", "elixir:1.17-otp-27")
            .run(&["elixir", "-e", "IO.puts(Enum.reduce(1..1000,0,&+/2))"])
            .has("500500")
            .long(),
        scen("languages/elixir-version-116", "elixir:1.16")
            .exec("elixir --version | grep -o 'Elixir 1.16'")
            .has("Elixir 1.16")
            .long(),
    ]
}
