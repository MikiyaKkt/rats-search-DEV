#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$InputDirectory,
    [Parameter(Mandatory=$true)][string]$OutputJson,
    [ValidateRange(1,10)][int]$ExpectedRuns = 3
)
$ErrorActionPreference = 'Stop'
function Counter($snapshot, [string]$name) {
    $property = $snapshot.counters.PSObject.Properties[$name]
    if ($null -eq $property) { return [long]0 }
    return [long]$property.Value
}
function Percentile($values, [double]$fraction) {
    $sorted = @($values | Sort-Object)
    if ($sorted.Count -eq 0) { return $null }
    return $sorted[[Math]::Max(0, [int][Math]::Ceiling($sorted.Count * $fraction) - 1)]
}
$files = @(Get-ChildItem -LiteralPath $InputDirectory -Filter 'run-*.json' -File | Sort-Object Name)
if ($files.Count -ne $ExpectedRuns) { throw "Expected $ExpectedRuns completed probe results, found $($files.Count)." }
$runs = @(
foreach ($file in $files) {
    $probe = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($probe.schema -ne 1 -or -not $probe.isolated_database -or -not $probe.peer_ingestion_disabled -or -not $probe.crawler_running `
        -or -not $probe.start_snapshot.enabled -or -not $probe.end_snapshot.enabled) {
        throw "Invalid or unisolated probe: $($file.Name)"
    }
    $startUtc = [DateTimeOffset]::Parse($probe.measurement_start_utc)
    $endUtc = [DateTimeOffset]::Parse($probe.measurement_end_utc)
    $elapsed = ($endUtc - $startUtc).TotalSeconds
    if ([Math]::Abs($elapsed - $probe.measurement_seconds) -gt [Math]::Max(10, $probe.measurement_seconds * 0.02)) {
        throw "Measurement duration outside tolerance: $($file.Name) : $elapsed seconds"
    }
    $resourceFile = Join-Path $file.DirectoryName ($file.BaseName + '.resources.jsonl')
    $resources = @(
        Get-Content -LiteralPath $resourceFile -Encoding UTF8 | ForEach-Object {
            $row = $_ | ConvertFrom-Json
            $utc = [DateTimeOffset]::Parse($row.utc)
            if ($utc -ge $startUtc -and $utc -le $endUtc) { $row }
        }
    )
    if ($resources.Count -lt 2) { throw "Insufficient resources in measurement window: $($file.Name)" }
    $coveredSeconds = ([DateTimeOffset]::Parse($resources[-1].utc) - [DateTimeOffset]::Parse($resources[0].utc)).TotalSeconds
    if ($coveredSeconds -lt $elapsed * 0.95) { throw "Less than 95% resource coverage: $($file.Name)" }
    if ($resources.Count -lt [Math]::Floor($elapsed * 0.90)) { throw "Resource sampling density below 90%: $($file.Name)" }
    $maxGapSeconds = 0.0
    for ($i = 1; $i -lt $resources.Count; $i++) {
        $gap = ([DateTimeOffset]::Parse($resources[$i].utc) - [DateTimeOffset]::Parse($resources[$i-1].utc)).TotalSeconds
        if ($gap -le 0) { throw "Non-monotonic resource timestamps: $($file.Name)" }
        $maxGapSeconds = [Math]::Max($maxGapSeconds, $gap)
    }
    if ($maxGapSeconds -gt 10) { throw "Resource sampling gap exceeds ten seconds: $($file.Name)" }
    $deltas = [ordered]@{}
    $keys = @(@($probe.start_snapshot.counters.PSObject.Properties.Name) + @($probe.end_snapshot.counters.PSObject.Properties.Name) | Sort-Object -Unique)
    foreach ($key in $keys) {
        $delta = (Counter $probe.end_snapshot $key) - (Counter $probe.start_snapshot $key)
        if ($delta -lt 0) { throw "Counter reset in measurement window: $key" }
        $deltas[$key] = $delta
    }
    $snapshots = @(
        Get-Content -LiteralPath (Join-Path $file.DirectoryName ($file.BaseName + '.stderr.log')) -Encoding UTF8 | ForEach-Object {
            $marker = $_.IndexOf('[CrawlerMetrics] ')
            if ($marker -ge 0) {
                $snapshot = $_.Substring($marker + '[CrawlerMetrics] '.Length) | ConvertFrom-Json
                if ($snapshot.elapsed_ms -ge $probe.start_snapshot.elapsed_ms -and $snapshot.elapsed_ms -le $probe.end_snapshot.elapsed_ms) { $snapshot }
            }
        }
    )
    if ($snapshots.Count -lt [Math]::Floor($elapsed / 10)) { throw "Too few periodic snapshots: $($file.Name)" }
    $windowGauges = [ordered]@{}
    $gaugeNames = @($snapshots | ForEach-Object { $_.gauges.PSObject.Properties.Name } | Sort-Object -Unique)
    foreach ($name in $gaugeNames) {
        $values = @($snapshots | ForEach-Object {
            $property = $_.gauges.PSObject.Properties[$name]
            if ($null -ne $property) { $property.Value.current }
        })
        $windowGauges[$name] = [ordered]@{ min = ($values | Measure-Object -Minimum).Minimum; max = ($values | Measure-Object -Maximum).Maximum }
    }
    $success = (Counter $probe.end_snapshot 'crawler.metadata_succeeded') - (Counter $probe.start_snapshot 'crawler.metadata_succeeded')
    $failed = (Counter $probe.end_snapshot 'crawler.metadata_failed') - (Counter $probe.start_snapshot 'crawler.metadata_failed')
    $timeouts = (Counter $probe.end_snapshot 'crawler.metadata_timeouts') - (Counter $probe.start_snapshot 'crawler.metadata_timeouts')
    $inserted = (Counter $probe.end_snapshot 'crawler.index_inserted') - (Counter $probe.start_snapshot 'crawler.index_inserted')
    $cpu = [double]$resources[-1].cpu_seconds - [double]$resources[0].cpu_seconds
    # Retain only content-free telemetry, never peer/torrent diagnostic lines.
    $metricsFileName = $file.BaseName + '.metrics.jsonl'
    $snapshots | ForEach-Object { $_ | ConvertTo-Json -Depth 15 -Compress } |
        Set-Content -LiteralPath (Join-Path $file.DirectoryName $metricsFileName) -Encoding UTF8
    [ordered]@{
        file = $file.Name
        measurement_metrics_file = $metricsFileName
        source = $probe.source
        warmup_seconds = $probe.warmup_seconds
        requested_seconds = $probe.measurement_seconds
        actual_seconds = $elapsed
        measurement_start_utc = $probe.measurement_start_utc
        measurement_end_utc = $probe.measurement_end_utc
        counter_deltas = $deltas
        observed_announcements = [long]$deltas['crawler.announcements']
        metadata_attempts = [long]$deltas['crawler.metadata_attempts']
        metadata_succeeded = $success
        metadata_failed = $failed
        crawler_inserted = $inserted
        crawler_inserted_per_minute = $inserted * 60.0 / $elapsed
        metadata_success_ratio = $(if ($success + $failed -gt 0) { $success / [double]($success + $failed) } else { $null })
        metadata_timeout_ratio = $(if ($success + $failed -gt 0) { $timeouts / [double]($success + $failed) } else { $null })
        dht_nodes_at_end = $probe.dht_nodes_at_end
        periodic_snapshots = $snapshots.Count
        sampled_window_gauges = $windowGauges
        final_recent_samples = $probe.end_snapshot.samples
        resource_scope = 'probe process only; excludes searchd; UTC-filtered measurement samples'
        resource_sample_count = $resources.Count
        resource_covered_seconds = $coveredSeconds
        resource_max_gap_seconds = $maxGapSeconds
        cpu_seconds_in_covered_window = $cpu
        average_cpu_core_percent = $cpu * 100 / $coveredSeconds
        rss_median_bytes = (Percentile $resources.working_set_bytes 0.5)
        rss_p95_bytes = (Percentile $resources.working_set_bytes 0.95)
        rss_max_bytes = ($resources.working_set_bytes | Measure-Object -Maximum).Maximum
        private_max_bytes = ($resources.private_bytes | Measure-Object -Maximum).Maximum
        threads_min = ($resources.threads | Measure-Object -Minimum).Minimum
        threads_max = ($resources.threads | Measure-Object -Maximum).Maximum
        udp_address_error_10049_lines = @(
            Select-String -LiteralPath @(
                (Join-Path $file.DirectoryName ($file.BaseName + '.stdout.log')),
                (Join-Path $file.DirectoryName ($file.BaseName + '.stderr.log'))
            ) -Pattern 'Failed to send UDP.*10049'
        ).Count
        diagnostic_scope = 'entire probe process, including warmup and shutdown; not measurement-window error rates'
    }
})
if (@($runs.requested_seconds | Select-Object -Unique).Count -ne 1 `
    -or @($runs.warmup_seconds | Select-Object -Unique).Count -ne 1 `
    -or @($runs.source | Select-Object -Unique).Count -ne 1) {
    throw 'Probe duration, warmup or source differs between runs.'
}
$summary = [ordered]@{
    schema = 1
    generated_utc = [DateTime]::UtcNow.ToString('o')
    expected_runs = $ExpectedRuns
    notes = @('Counter deltas exclude warmup.', 'Sample percentiles are final recent windows, not full-window quantiles.', 'Gauge extrema are sampled every five seconds and can miss intervening peaks.', 'Resource samples exclude startup, warmup and shutdown; CPU coverage is reported separately.', 'No completed metadata requests means ratios are null, not zero success rate.')
    runs = $runs
}
$summary | ConvertTo-Json -Depth 15 | Set-Content -LiteralPath $OutputJson -Encoding UTF8
Write-Output "Summarized $($runs.Count) live runs to $OutputJson"
