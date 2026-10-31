# Times the workloads of each module on wasmtime: bench-wasmtime.ps1 a.wasm b.wasm ...
# Each run includes startup and compilation, so a run with 0 repetitions is
# timed too and subtracted.
param([Parameter(ValueFromRemainingArguments)][string[]]$Files)
$ErrorActionPreference = 'Stop'
$wasmtime = 'C:\Program Files\Wasmtime\bin\wasmtime.exe'
# Large enough that a run takes seconds, so startup noise stays small.
$work = [ordered]@{
    walk = @{ reps = 1500; ops = 65536; unit = 'node' }
    walk1 = @{ reps = 1500; ops = 65536; unit = 'node' }
    scan = @{ reps = 5000; ops = 65536; unit = 'load' }
    copy = @{ reps = 4000000; ops = 1; unit = '4KiB copy' }
}

function Time-Run($file, $fn, $reps) {
    $best = [double]::MaxValue
    for ($i = 0; $i -lt 3; $i++) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        # stderr only carries --invoke's "experimental" warning.
        $out = & $wasmtime run -W multi-memory=y --invoke $fn $file $reps 2>$null
        $sw.Stop()
        if ($LASTEXITCODE) { throw "wasmtime failed: $out" }
        $best = [Math]::Min($best, $sw.Elapsed.TotalMilliseconds)
    }
    return @{ ms = $best; out = ($out | Select-Object -Last 1) }
}

foreach ($file in $Files) {
    $row = @((Split-Path -LeafBase $file).PadRight(14))
    foreach ($name in $work.Keys) {
        $w = $work[$name]
        $base = Time-Run $file $name 0
        $run = Time-Run $file $name $w.reps
        $ns = ($run.ms - $base.ms) * 1e6 / ($w.reps * $w.ops)
        $row += ('{0} {1,7:F2} ns/{2} ({3})' -f $name, $ns, $w.unit, $run.out)
    }
    $row -join '  '
}
