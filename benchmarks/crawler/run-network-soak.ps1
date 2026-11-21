#Requires -Version 5.1
[CmdletBinding()]
param([ValidateRange(3,7200)][int]$Seconds=7200,[ValidateRange(0,1000)][int]$Cycles=100,
    [string]$BuildDirectory='build-windows-x64',[string]$OutputDirectory='build-windows-x64/p5-network-two-hour')
$ErrorActionPreference='Stop'
$repo=(Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build=[IO.Path]::GetFullPath((Join-Path $repo $BuildDirectory))
$output=[IO.Path]::GetFullPath((Join-Path $repo $OutputDirectory))
New-Item -ItemType Directory -Path $output -Force|Out-Null
$result=Join-Path $output 'live.json';$stopFile=Join-Path $output 'STOP'
if(Test-Path -LiteralPath "$result.resources.jsonl"){throw 'Choose a fresh OutputDirectory'}
$exe=Join-Path $build 'tests/crawler_live_probe_p5_under_test.exe'
$env:PATH="$repo/.tools/Qt/6.10.1/mingw_64/bin;$repo/.tools/Qt/Tools/mingw1310_64/bin;"+$env:PATH
$manifest=[ordered]@{kind='network-only';started_utc=[DateTime]::UtcNow.ToString('o');seconds=$Seconds;cycles=$Cycles;
    live_sha256=(Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash.ToLowerInvariant();live=$result;completed=$false}
$manifestPath=Join-Path $output 'run.json'
$process=Start-Process -FilePath $exe -WorkingDirectory $repo -WindowStyle Hidden -PassThru `
    -ArgumentList @('--seconds',$Seconds,'--warmup','0','--concurrency','10','--restart-cycles',$Cycles,'--resource-interval','1000',
        '--stop-file',('"{0}"' -f $stopFile),'--output',('"{0}"' -f $result)) `
    -RedirectStandardOutput (Join-Path $output 'live.stdout.log') -RedirectStandardError (Join-Path $output 'live.stderr.log')
$null=$process.Handle;$manifest['live_pid']=$process.Id
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
$watch=[Diagnostics.Stopwatch]::StartNew();$lastReport=-60
while(-not $process.HasExited){
    if($watch.Elapsed.TotalSeconds-$lastReport -ge 60){$lastReport=$watch.Elapsed.TotalSeconds;Write-Output("P5 live network soak: {0:N0}/{1} s" -f $lastReport,$Seconds)}
    Start-Sleep -Milliseconds 1000
}
$process.WaitForExit();$manifest['ended_utc']=[DateTime]::UtcNow.ToString('o');$manifest['live_exit_code']=$process.ExitCode
$manifest['cancelled']=Test-Path -LiteralPath $stopFile
$manifest['completed']=$process.ExitCode -eq 0 -and -not $manifest.cancelled
[IO.File]::WriteAllText($manifestPath,($manifest|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
if(-not $manifest.completed){throw 'Network soak did not complete normally'}
Write-Output "Completed P5 network soak: $output"
