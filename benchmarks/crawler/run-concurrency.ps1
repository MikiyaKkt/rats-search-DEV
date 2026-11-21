#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateRange(3,900)][int]$LiveSeconds = 10,
    [ValidateRange(1,3)][int]$Runs = 3,
    [switch]$ControlledOnly,
    [string]$BuildDirectory = 'build-windows-x64',
    [string]$OutputDirectory = 'benchmarks/crawler/results/p4'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$env:PATH = "$repo/.tools/Qt/6.10.1/mingw_64/bin;$repo/.tools/Qt/Tools/mingw1310_64/bin;" + $env:PATH

function Invoke-Probe([string]$Name, [string]$Executable, [string[]]$ProbeArguments) {
    $prefix = Join-Path $output $Name
    $resultPath = "$prefix.json"
    $samples = [IO.StreamWriter]::new("$prefix.resources.jsonl",$false,[Text.UTF8Encoding]::new($false))
    $process = $null
    try {
        $process = Start-Process -FilePath (Join-Path $build "tests/$Executable") -WorkingDirectory $repo -WindowStyle Hidden -PassThru `
            -ArgumentList (@('--output',('"{0}"' -f $resultPath)) + $ProbeArguments) `
            -RedirectStandardOutput "$prefix.stdout.log" -RedirectStandardError "$prefix.stderr.log"
        $null = $process.Handle
        $watch = [Diagnostics.Stopwatch]::StartNew()
        while (-not $process.HasExited) {
            $process.Refresh()
            try {
                $samples.WriteLine((([ordered]@{elapsed_ms=$watch.ElapsedMilliseconds;working_set_bytes=$process.WorkingSet64;
                    private_bytes=$process.PrivateMemorySize64;threads=$process.Threads.Count;cpu_seconds=$process.TotalProcessorTime.TotalSeconds;
                    scope='probe only; excludes searchd';network_bandwidth=$null}) | ConvertTo-Json -Compress))
            } catch { if (-not $process.HasExited) { throw } }
            Start-Sleep -Milliseconds 100
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$Name failed with $($process.ExitCode); inspect $prefix.stderr.log" }
        if (-not (Test-Path -LiteralPath $resultPath)) { throw "$Name produced no JSON" }
        Write-Output "Completed $Name"
    } finally { $samples.Dispose(); if ($null -ne $process) { $process.Dispose() } }
}

# Sequential: every instance owns a fresh temporary database; no port sharing.
for ($run=1; $run -le $Runs; ++$run) {
    foreach ($limit in @(10,20,40)) {
        Invoke-Probe "controlled-$limit-$run" 'crawler_concurrency_probe.exe' @('--concurrency',"$limit",'--count','300')
    }
}
if ($ControlledOnly) { return }
for ($run=1; $run -le $Runs; ++$run) {
    Invoke-Probe "live-p3-10-$run" 'crawler_live_probe_p3_baseline.exe' @('--seconds',"$LiveSeconds",'--warmup','0')
    foreach ($limit in @(10,20,40)) {
        Invoke-Probe "live-p4-$limit-$run" 'crawler_live_probe.exe' @('--concurrency',"$limit",'--seconds',"$LiveSeconds",'--warmup','0')
    }
}
foreach ($count in @(10,20,40)) {
    Invoke-Probe "waiters-$count" 'crawler_live_probe.exe' @('--profile-waiters',"$count",'--seconds','3','--warmup','0')
}
