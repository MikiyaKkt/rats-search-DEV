#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateRange(1,7200)][int]$Seconds = 900,
    [ValidateRange(0,3600)][int]$WarmupSeconds = 300,
    [ValidateRange(1,10)][int]$Runs = 3,
    [string]$BuildDirectory = 'build-windows-x64',
    [string]$OutputDirectory = 'benchmarks/crawler/results/live'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDirectory))
$output = [IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
$probeExe = Join-Path $build 'tests/crawler_live_probe.exe'
if (-not (Test-Path -LiteralPath $probeExe)) { throw "Build crawler_live_probe first: $probeExe" }
New-Item -ItemType Directory -Path $output -Force | Out-Null
$qtBin = Join-Path $repo '.tools/Qt/6.10.1/mingw_64/bin'
$compilerBin = Join-Path $repo '.tools/Qt/Tools/mingw1310_64/bin'
$env:PATH = "$qtBin;$compilerBin;" + $env:PATH
for ($run = 1; $run -le $Runs; $run++) {
    $prefix = Join-Path $output ('run-{0}-{1}' -f $run, (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $resultPath = "$prefix.json"
    $samples = [IO.StreamWriter]::new("$prefix.resources.jsonl", $false, [Text.UTF8Encoding]::new($false))
    $probe = $null
    try {
        $probe = Start-Process -FilePath $probeExe -WorkingDirectory $repo -WindowStyle Hidden -PassThru `
            -ArgumentList @('--output', ('"{0}"' -f $resultPath), '--seconds', $Seconds, '--warmup', $WarmupSeconds) `
            -RedirectStandardOutput "$prefix.stdout.log" -RedirectStandardError "$prefix.stderr.log"
        # Open the handle before polling: Windows PowerShell can otherwise
        # lose the exit code when a refreshed process exits between samples.
        $null = $probe.Handle
        $clock = [Diagnostics.Stopwatch]::StartNew()
        $previousSeconds = 0.0
        $previousCpu = 0.0
        while (-not $probe.HasExited) {
            $probe.Refresh()
            try {
                $now = $clock.Elapsed.TotalSeconds
                $cpu = $probe.TotalProcessorTime.TotalSeconds
                $corePercent = if ($now -gt $previousSeconds) { ($cpu - $previousCpu) * 100 / ($now - $previousSeconds) } else { 0 }
                $row = [ordered]@{
                    utc = [DateTime]::UtcNow.ToString('o')
                    elapsed_seconds = $now
                    phase_estimate = $(if ($now -lt $WarmupSeconds) { 'warmup' } else { 'measurement' })
                    pid = $probe.Id
                    scope = 'probe process only; excludes searchd and operating system'
                    working_set_bytes = $probe.WorkingSet64
                    peak_working_set_bytes = $probe.PeakWorkingSet64
                    private_bytes = $probe.PrivateMemorySize64
                    threads = $probe.Threads.Count
                    cpu_seconds = $cpu
                    cpu_core_percent = $corePercent
                    logical_processors = [Environment]::ProcessorCount
                    sample_interval_target_ms = 1000
                }
                $samples.WriteLine(($row | ConvertTo-Json -Compress))
                $previousCpu = $cpu
                $previousSeconds = $now
            } catch {
                if (-not $probe.HasExited) { throw }
            }
            Start-Sleep -Milliseconds 1000
        }
        $probe.WaitForExit()
        if ($probe.ExitCode -ne 0) { throw "Probe exited $($probe.ExitCode); inspect $prefix.stderr.log" }
        if (-not (Test-Path -LiteralPath $resultPath)) { throw "Probe produced no result: $resultPath" }
        Write-Output "Completed live run $run : $resultPath"
    } finally {
        $samples.Dispose()
        if ($null -ne $probe) { $probe.Dispose() }
    }
}
