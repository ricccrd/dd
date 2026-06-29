//! .NET — CoreCLR RyuJIT is PRIME JIT-in-JIT stress: Roslyn compiles C# to IL, the runtime JITs IL to
//! native that the dd engine must itself translate, with RWX code heaps. SDK 8/9 do a full
//! restore+build+JIT (heaviest case here); runtime image for the host banner. Markers per MANIFEST §2.
//! The SDK compile cases are heavy → `.long()`.

use crate::scenario::{scen, Scenario};

// NOTE: the exec harness shell-processes the heredoc body (strips `$`, brace-expands `{a,b}`), so the
// C# avoids `$"…"` interpolation and comma-bearing `{…}` — use string concatenation instead.
const DN_SUM: &str = "mkdir -p /app && cd /app && dotnet new console -o . >/dev/null 2>&1 && cat > Program.cs <<'CSEOF'\nusing System;\nusing System.Linq;\nConsole.WriteLine(\"NET \" + Enumerable.Range(1,1000).Sum());\nCSEOF\ndotnet run -c Release 2>/dev/null";
const DN_FIB: &str = "mkdir -p /app && cd /app && dotnet new console -o . >/dev/null 2>&1 && cat > Program.cs <<'CSEOF'\nusing System;\nulong a=0,b=1; for(int i=0;i<50;i++){ulong t=a+b;a=b;b=t;} Console.WriteLine(\"NETFIB \" + a);\nCSEOF\ndotnet run -c Release 2>/dev/null";

pub fn scenarios() -> Vec<Scenario> {
    vec![
        scen("languages/dotnet-version-sdk8", "mcr.microsoft.com/dotnet/sdk:8.0")
            .exec("echo \"SDK $(dotnet --version | cut -d. -f1)\"")
            .has("SDK 8")
            .long(),
        scen("languages/dotnet-sum-sdk8", "mcr.microsoft.com/dotnet/sdk:8.0")
            .exec(DN_SUM).has("NET 500500").long(),
        scen("languages/dotnet-fib-sdk9", "mcr.microsoft.com/dotnet/sdk:9.0")
            .exec(DN_FIB).has("NETFIB 12586269025").long(),
        scen("languages/dotnet-runtime-info-8", "mcr.microsoft.com/dotnet/runtime:8.0")
            .exec("dotnet --info | grep -o 'Microsoft.NETCore.App 8'")
            .has("Microsoft.NETCore.App 8")
            .long(),
    ]
}
