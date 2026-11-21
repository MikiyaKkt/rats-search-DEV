#Requires -Version 5.1
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$InputJson)
$ErrorActionPreference = 'Stop'
$data = Get-Content -LiteralPath $InputJson -Raw -Encoding UTF8 | ConvertFrom-Json
if ($data.schema -ne 1 -or $data.runs.Count -ne 36) { throw 'Expected schema=1 with 36 controlled measurements.' }
$summary = foreach ($group in ($data.runs | Group-Object scenario,mode)) {
    if ($group.Count -ne 3) { throw "Expected three repeats: $($group.Name)" }
    $times = @($group.Group.elapsed_ms | Sort-Object)
    $first = $group.Group[0]
    [PSCustomObject]@{
        scenario = $first.scenario
        mode = $first.mode
        min_ms = [Math]::Round($times[0], 3)
        median_ms = [Math]::Round($times[1], 3)
        max_ms = [Math]::Round($times[2], 3)
        sql_queries_per_run = $first.metrics.counters.'db.query_calls'
        sql_writes_per_run = $first.metrics.counters.'db.write_calls'
        detail_log_bytes_per_run = $first.log_bytes
    }
}
$summary | Format-Table -AutoSize
foreach ($scenario in ($summary.scenario | Select-Object -Unique)) {
    $off = $summary | Where-Object { $_.scenario -eq $scenario -and $_.mode -eq 'metrics_off' }
    $on = $summary | Where-Object { $_.scenario -eq $scenario -and $_.mode -eq 'metrics_on' }
    $verbose = $summary | Where-Object { $_.scenario -eq $scenario -and $_.mode -eq 'verbose' }
    [PSCustomObject]@{
        scenario = $scenario
        metrics_on_vs_off_median_percent = [Math]::Round(($on.median_ms / $off.median_ms - 1) * 100, 2)
        verbose_vs_on_median_percent = [Math]::Round(($verbose.median_ms / $on.median_ms - 1) * 100, 2)
    }
}
