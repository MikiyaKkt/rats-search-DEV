#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateRange(1, 64)][int]$Jobs = 4,
    [switch]$SkipTests,
    [AllowEmptyString()][ValidatePattern('^[A-Za-z0-9-]*$')][string]$PackageSuffix = 's3-fix1',
    [switch]$Offline,
    [switch]$Release
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$tools = Join-Path $repo '.tools'
$qtVersion = '6.10.1'
$qtBase = Join-Path $tools 'Qt'
$qt = Join-Path $qtBase "$qtVersion/mingw_64"
$compiler = Join-Path $qtBase 'Tools/mingw1310_64/bin'
$cmake = Join-Path $qtBase 'Tools/CMake_64/bin/cmake.exe'
$ctest = Join-Path $qtBase 'Tools/CMake_64/bin/ctest.exe'
$aqt = Join-Path $tools 'aqt.exe'
$driver = Join-Path $tools "qmysql-$qtVersion-mingw"
$build = Join-Path $repo 'build-windows-x64'
$dist = Join-Path $repo 'dist'

function Invoke-Checked {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)." }
}
function Get-Download {
    param([string]$Url, [string]$Destination)
    if ($Offline) { throw "Offline mode: missing dependency $Destination" }
    Write-Host "Downloading $Url"
    Invoke-WebRequest -UseBasicParsing -Uri $Url -OutFile "$Destination.part"
    Move-Item -LiteralPath "$Destination.part" -Destination $Destination -Force
}
function Ensure-QtTool {
    param([string]$Executable, [string]$Tool, [string]$Variant)
    if (Test-Path -LiteralPath $Executable) { return }
    if ($Offline) { throw "Offline mode: missing $Executable" }
    Invoke-Checked $aqt @('install-tool', 'windows', 'desktop', $Tool, $Variant, '-O', $qtBase, '--timeout', '30')
}

if (-not [Environment]::Is64BitOperatingSystem) { throw 'Windows x64 is required.' }
if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Install Git for Windows first.' }
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
New-Item -ItemType Directory -Force $tools, $dist | Out-Null
$previousPath = $env:PATH
$previousPluginPath = $env:QT_PLUGIN_PATH
Push-Location $repo
try {
    $packageVersion = if ($Release) { (Get-Content -LiteralPath VERSION -Raw).Trim() } else { '' }
    if ($Release -and $packageVersion -notmatch '^\d+\.\d+\.\d+$') {
        throw 'VERSION must contain a numeric major.minor.patch version.'
    }
    # Do not update an existing submodule: it may contain local fixes.
    foreach ($module in @('src/librats', 'imports')) {
        $marker = if ($module -eq 'imports') { 'imports/win/x64/searchd.exe' } else { 'src/librats/CMakeLists.txt' }
        if (-not (Test-Path $marker)) {
            if ($Offline) { throw "Offline mode: initialize submodule $module first." }
            Invoke-Checked 'git' @('submodule', 'update', '--init', '--recursive', '--', $module)
        }
    }
    # The pinned librats fork already contains the download and UTF-8 fixes.
    # Refuse a stale checkout instead of silently building an unpatched engine.
    & git -C src/librats merge-base --is-ancestor b9214ff5 HEAD
    if ($LASTEXITCODE -ne 0) { throw 'The librats submodule is outdated. Update it to the commit pinned by this repository.' }
    if (-not (Test-Path $aqt) -and (-not (Test-Path $cmake) -or -not (Test-Path "$compiler/g++.exe") -or -not (Test-Path "$qt/bin/windeployqt.exe"))) {
        Get-Download 'https://github.com/miurahr/aqtinstall/releases/download/v3.3.0/aqt_x64.exe' $aqt
    }
    Ensure-QtTool "$compiler/g++.exe" 'tools_mingw1310' 'qt.tools.win64_mingw1310'
    Ensure-QtTool $cmake 'tools_cmake' 'qt.tools.cmake'
    if (-not (Test-Path "$qt/lib/cmake/Qt6WebSockets/Qt6WebSocketsConfig.cmake") -or -not (Test-Path "$qt/bin/lrelease.exe")) {
        if ($Offline) { throw "Offline mode: Qt $qtVersion is incomplete." }
        Invoke-Checked $aqt @('install-qt', 'windows', 'desktop', $qtVersion, 'win64_mingw', '-O', $qtBase,
            '--archives', 'qtbase', 'qttools', 'qttranslations', 'qtsvg', '-m', 'qtwebsockets', '--timeout', '30')
    }
    if (-not (Test-Path "$driver/sqldrivers/qsqlmysql.dll") -or -not (Test-Path "$driver/libmysql.dll")) {
        $archive = Join-Path $tools "qmysql-$qtVersion-mingw.zip"
        if (-not (Test-Path $archive)) {
            Get-Download "https://github.com/thecodemonkey86/qt_mysql_driver/releases/download/qmysql_$qtVersion/qsqlmysql.dll_Qt_SQL_driver_${qtVersion}_MinGW_13.1_64-bit.zip" $archive
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $driver -Force
    }
    if (-not (Test-Path "$driver/sqldrivers/qsqlmysql.dll") -or -not (Test-Path "$driver/libmysql.dll")) {
        throw 'The MySQL driver archive has an unexpected layout.'
    }
    Copy-Item "$driver/sqldrivers/*.dll" "$qt/plugins/sqldrivers" -Force
    Copy-Item "$driver/*.dll" "$qt/bin" -Force
    $env:PATH = "$compiler;$qt/bin;$(Split-Path $cmake);$previousPath"
    $env:QT_PLUGIN_PATH = "$qt/plugins"
    $tests = if ($SkipTests) { 'OFF' } else { 'ON' }
    $gtest = Join-Path $tools 'googletest-1.17.0'
    if (-not $SkipTests -and -not (Test-Path "$gtest/CMakeLists.txt")) {
        $archive = Join-Path $tools 'googletest-v1.17.0.zip'
        if (-not (Test-Path $archive)) {
            Get-Download 'https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip' $archive
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $tools -Force
    }
    Invoke-Checked $cmake @('-S', $repo, '-B', $build, '-G', 'MinGW Makefiles',
        '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$qt", '-DRATS_SEARCH_USE_SYSTEM_LIBRATS=OFF',
        "-DRATS_SEARCH_BUILD_TESTS=$tests", "-DRATS_SEARCH_BUILD_ENGINE_TESTS=$tests",
        "-DRATS_SEARCH_GTEST_SOURCE_DIR=$gtest")
    Invoke-Checked $cmake @('--build', $build, '--parallel', "$Jobs")
    if (-not $SkipTests) {
        Invoke-Checked $ctest @('--test-dir', $build, '--output-on-failure', '--parallel', "$Jobs")
    }

    # Fresh staging prevents stale DLLs from leaking into subsequent packages.
    $stage = Join-Path $build ('package-' + [Guid]::NewGuid().ToString('N'))
    $package = Join-Path $stage 'RatsSearch-Windows-x64'
    New-Item -ItemType Directory -Force $package | Out-Null
    Copy-Item "$build/bin/*" $package -Recurse -Force
    Copy-Item 'imports/win/x64/*' $package -Recurse -Force
    Invoke-Checked "$qt/bin/windeployqt.exe" @('--release', '--compiler-runtime', '--dir', $package, "$package/RatsSearch.exe")
    # Manticore's libmysql is incompatible with QMYSQL: overwrite it LAST.
    New-Item -ItemType Directory -Force "$package/sqldrivers" | Out-Null
    Copy-Item "$driver/sqldrivers/*.dll" "$package/sqldrivers" -Force
    Copy-Item "$driver/*.dll" $package -Force
    # libmysql and Manticore use MSVC even though the application uses MinGW.
    # Deploy these app-local so the destination needs no redistributable install.
    foreach ($dll in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        $runtime = Join-Path $env:SystemRoot "System32/$dll"
        if (-not (Test-Path $runtime)) {
            throw "Missing $runtime. Install Microsoft's Visual C++ 2015-2022 x64 Redistributable on the build machine."
        }
        Copy-Item -LiteralPath $runtime -Destination $package -Force
    }
    foreach ($required in @('RatsSearch.exe', 'Qt6Core.dll', 'Qt6WebSockets.dll', 'platforms/qwindows.dll',
        'sqldrivers/qsqlmysql.dll', 'libmysql.dll', 'searchd.exe', 'indexer.exe', 'libstdc++-6.dll', 'libwinpthread-1.dll')) {
        if (-not (Test-Path "$package/$required")) { throw "Package missing $required" }
    }
    # Test the staged executable using only app-local and Windows runtime paths.
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = Join-Path $package 'RatsSearch.exe'
    $process.StartInfo.Arguments = '--console --version'
    $process.StartInfo.WorkingDirectory = $package
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.EnvironmentVariables['PATH'] = "$package;$env:SystemRoot/System32;$env:SystemRoot"
    $process.StartInfo.EnvironmentVariables['QT_PLUGIN_PATH'] = $package
    try {
        [void]$process.Start()
        if (-not $process.WaitForExit(15000)) { $process.Kill(); throw 'Package startup timed out.' }
        $versionOutput = $process.StandardOutput.ReadToEnd().Trim()
        $startupError = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne 0 -or $versionOutput -notmatch '^Rats Search ') {
            throw "Package startup failed (exit $($process.ExitCode)): $startupError"
        }
        if ($Release -and $versionOutput -notmatch "^Rats Search $([regex]::Escape($packageVersion))(?:\s|$)") {
            throw "Package executable version does not match VERSION: $versionOutput"
        }
        Write-Host "Package startup verified: $versionOutput"
    } finally { $process.Dispose() }
    Copy-Item 'LICENSE' "$package/LICENSE.txt"
    $revision = (& git rev-parse --short HEAD).Trim()
    $info = [ordered]@{
        builtAtUtc = [DateTime]::UtcNow.ToString('o'); revision = $revision
        stage = if ($Release) { 'release' } elseif ($PackageSuffix) { "$PackageSuffix-awaiting-human-validation" } else { 'awaiting-human-validation' }
        branch = (& git branch --show-current).Trim()
        sourceModified = [bool](& git status --porcelain)
        libratsRevision = (& git -C src/librats rev-parse HEAD).Trim()
        libratsModified = [bool](& git -C src/librats status --porcelain)
        qt = $qtVersion; compiler = 'MinGW 13.1 x64'; configuration = 'Release'; testsRun = (-not $SkipTests)
    }
    $info | ConvertTo-Json | Set-Content "$package/build-info.json" -Encoding UTF8
    @'
Run RatsSearch.exe directly; no installation is required.
For application data stored beside the executable, start from a terminal:
  .\RatsSearch.exe --data-dir .\data --no-remember-data-dir
Existing incorrectly encoded download folders are not renamed automatically.
Qt is dynamically linked. Qt sources and license information:
  https://download.qt.io/archive/qt/6.10/6.10.1/
  https://www.qt.io/licensing/open-source-lgpl-obligations
'@ | Set-Content "$package/README-portable.txt" -Encoding UTF8
    $packageName = 'RatsSearch-Windows-x64' + $(if ($Release) { "-v$packageVersion" } elseif ($PackageSuffix) { "-$PackageSuffix" } else { '' }) + '.zip'
    $zip = Join-Path $dist $packageName
    Compress-Archive -LiteralPath $package -DestinationPath $zip -Force -CompressionLevel Optimal
    if (-not $SkipTests) {
        # Verify the actual ZIP in a fresh Unicode path. The child process uses
        # app-local/Windows DLLs and an isolated ASCII DB directory; payload paths
        # and names are Unicode. No remembered user data directory is changed.
        $extracted = Join-Path $stage (([string][char]0x89e3) + [char]0x538b + '-S3')
        Expand-Archive -LiteralPath $zip -DestinationPath $extracted
        $smokeExecutable = Join-Path $extracted 'RatsSearch-Windows-x64/RatsSearch.exe'
        Invoke-Checked (Join-Path $build 'tests/download_package_smoke.exe') @($smokeExecutable)
    }
    $hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $packageName" | Set-Content "$zip.sha256" -Encoding ASCII
    Write-Host "Package: $zip"
    Write-Host "SHA256: $hash"
} finally {
    Pop-Location
    $env:PATH = $previousPath
    $env:QT_PLUGIN_PATH = $previousPluginPath
}
