//! PHP — Zend engine CLI on glibc + musl (alpine), 8.2/8.3. Markers per IMAGE-MANIFEST §2.
//! Note: PHP ints are 64-bit; fib(50)=12586269025 < 2^63 stays exact.

use crate::scenario::{scen, Scenario};

pub fn scenarios() -> Vec<Scenario> {
    vec![
        scen("languages/php-sum-82-cli", "php:8.2-cli")
            .run(&["php", "-r", "echo array_sum(range(1,1000));"])
            .has("500500"),
        scen("languages/php-fib-83-cli", "php:8.3-cli")
            .run(&["php", "-r", "$a=0;$b=1;for($i=0;$i<50;$i++){$t=$a+$b;$a=$b;$b=$t;}echo $a;"])
            .has("12586269025"),
        scen("languages/php-json-83-cli", "php:8.3-cli")
            .run(&["php", "-r", "echo json_encode(['s'=>array_sum(range(1,1000))]);"])
            .has("{\"s\":500500}"),
        scen("languages/php-primes-82-cli", "php:8.2-cli")
            .run(&["php", "-r", "$p=0;for($n=2;$n<10000;$n++){$q=1;for($d=2;$d*$d<=$n;$d++)if($n%$d==0){$q=0;break;}$p+=$q;}echo $p;"])
            .has("1229"),
        scen("languages/php-strlen-repl-82-alpine", "php:8.2-cli-alpine")
            .exec("echo '<?php echo \"LEN\".strlen(\"hello\");' | php")
            .has("LEN5"),
    ]
}
