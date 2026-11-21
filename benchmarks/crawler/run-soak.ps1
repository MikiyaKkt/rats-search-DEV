#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateRange(3,7200)][int]$Seconds = 7200,
    [ValidateRange(1,1000)][int]$Cycles = 100,
    [string]$BuildDirectory = 'build-windows-x64',
    [string]$OutputDirectory = 'build-windows-x64/p5-soak'
)
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build=[IO.Path]::GetFullPath((Join-Path $repo $BuildDirectory))
$output=[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$env:PATH="$repo/.tools/Qt/6.10.1/mingw_64/bin;$repo/.tools/Qt/Tools/mingw1310_64/bin;"+$env:PATH
$controlledExe=Join-Path $build 'tests/crawler_soak_probe_p5_under_test.exe'
$liveExe=Join-Path $build 'tests/crawler_live_probe_p5_under_test.exe'
Copy-Item -LiteralPath (Join-Path $build 'tests/crawler_soak_probe.exe') -Destination $controlledExe
Copy-Item -LiteralPath (Join-Path $build 'tests/crawler_live_probe.exe') -Destination $liveExe
$controlledResult=Join-Path $output 'controlled.json'
$liveResult=Join-Path $output 'live.json'
$stopFile=Join-Path $output 'STOP'
if(Test-Path -LiteralPath "$controlledResult.resources.jsonl"){throw 'Choose a fresh OutputDirectory to preserve previous results'}
$manifest=[ordered]@{started_utc=[DateTime]::UtcNow.ToString('o');seconds=$Seconds;cycles=$Cycles;
    controlled_sha256=(Get-FileHash -LiteralPath $controlledExe -Algorithm SHA256).Hash.ToLowerInvariant();
    live_sha256=(Get-FileHash -LiteralPath $liveExe -Algorithm SHA256).Hash.ToLowerInvariant();
    controlled=$controlledResult;live=$liveResult;completed=$false}
$manifestPath=Join-Path $output 'run.json'
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
$controlled=Start-Process -FilePath $controlledExe -WorkingDirectory $repo -WindowStyle Hidden -PassThru `
    -ArgumentList @('--seconds',$Seconds,'--cycles',$Cycles,'--stop-file',('"{0}"' -f $stopFile),'--output',('"{0}"' -f $controlledResult)) `
    -RedirectStandardOutput (Join-Path $output 'controlled.stdout.log') -RedirectStandardError (Join-Path $output 'controlled.stderr.log')
$null=$controlled.Handle
# Wait for its database to be listening before launching the second temporary
# instance, avoiding a race in automatic port selection.
$startup=[Diagnostics.Stopwatch]::StartNew()
while(-not(Test-Path -LiteralPath "$controlledResult.resources.jsonl")) {
    if($controlled.HasExited -or $startup.Elapsed.TotalSeconds -gt 60){throw 'Controlled probe did not start; inspect its stderr log'}
    Start-Sleep -Milliseconds 100
}
$live=Start-Process -FilePath $liveExe -WorkingDirectory $repo -WindowStyle Hidden -PassThru `
    -ArgumentList @('--seconds',$Seconds,'--warmup','0','--concurrency','10','--restart-cycles',$Cycles,'--resource-interval','1000','--stop-file',('"{0}"' -f $stopFile),'--output',('"{0}"' -f $liveResult)) `
    -RedirectStandardOutput (Join-Path $output 'live.stdout.log') -RedirectStandardError (Join-Path $output 'live.stderr.log')
$null=$live.Handle
$manifest['controlled_pid']=$controlled.Id;$manifest['live_pid']=$live.Id
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
$watch=[Diagnostics.Stopwatch]::StartNew();$lastReport=-60
while(-not $controlled.HasExited -or -not $live.HasExited) {
    if(($controlled.HasExited -and $controlled.ExitCode -ne 0) -or ($live.HasExited -and $live.ExitCode -ne 0)) {
        if(-not(Test-Path -LiteralPath $stopFile)){[IO.File]::WriteAllText($stopFile,'Peer probe failed; stop cleanly')}
    }
    if($watch.Elapsed.TotalSeconds-$lastReport -ge 60) {
        $lastReport=$watch.Elapsed.TotalSeconds
        Write-Output ("P5 soak: {0:N0}/{1} s; controlled running={2}; live running={3}" -f $lastReport,$Seconds,(-not $controlled.HasExited),(-not $live.HasExited))
    }
    Start-Sleep -Milliseconds 1000
}
$controlled.WaitForExit();$live.WaitForExit()
$manifest['ended_utc']=[DateTime]::UtcNow.ToString('o')
$manifest['controlled_exit_code']=$controlled.ExitCode;$manifest['live_exit_code']=$live.ExitCode
$manifest['completed']=$controlled.ExitCode -eq 0 -and $live.ExitCode -eq 0
$manifest['cancelled']=Test-Path -LiteralPath $stopFile
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
if(-not $manifest.completed){throw 'Soak probe failed; inspect results and logs'}
Write-Output "Completed both P5 soak probes: $output"
