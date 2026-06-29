//! Java — JVM (HotSpot C2) is PRIME JIT-in-JIT stress: javac emits bytecode, then the JIT-compiling
//! VM emits native code the dd engine must itself translate, and it mmaps RWX code cache.
//! eclipse-temurin JDKs (glibc + musl alpine), 17/21. J-prog/J-fib written to /Main.java then
//! `javac … && java …`. Markers per IMAGE-MANIFEST §2.

use crate::scenario::{scen, Scenario};

const J_SUM: &str = "cat > /Main.java <<'EOF'\npublic class Main { public static void main(String[] a){ long s=0; for(int i=1;i<=1000;i++) s+=i; System.out.println(s);} }\nEOF\njavac /Main.java -d /out && java -cp /out Main";
const J_FIB: &str = "cat > /Main.java <<'EOF'\npublic class Main { public static void main(String[] a){ long x=0,y=1; for(int i=0;i<50;i++){long t=x+y;x=y;y=t;} System.out.println(x);} }\nEOF\njavac /Main.java -d /out && java -cp /out Main";

pub fn scenarios() -> Vec<Scenario> {
    vec![
        // openjdk:*-slim tags were removed from Docker Hub (repo deprecated) → use eclipse-temurin JDKs.
        scen("languages/java-sum-17", "eclipse-temurin:17")
            .exec(J_SUM).has("500500").long(),
        scen("languages/java-fib-21", "eclipse-temurin:21")
            .exec(J_FIB).has("12586269025").long(),
        scen("languages/java-version-temurin17", "eclipse-temurin:17")
            .exec("java -version 2>&1 | grep -o 'openjdk version \"17'")
            .has("openjdk version \"17"),
        scen("languages/java-sum-temurin21", "eclipse-temurin:21")
            .exec(J_SUM).has("500500").long(),
        scen("languages/java-sum-temurin21-alpine", "eclipse-temurin:21-alpine")
            .exec(J_SUM).has("500500").long(),
    ]
}
