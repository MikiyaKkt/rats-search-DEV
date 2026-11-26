# Windows x64 portable build

Install Git for Windows and Microsoft's Visual C++ 2015-2022 x64 Redistributable
on the build machine, then double-click [`build-windows.cmd`](../build-windows.cmd).
The first run downloads a portable toolchain into `.tools/`: aqtinstall 3.3.0,
Qt 6.10.1, MinGW 13.1 x64, CMake, and the matching QMYSQL plugin/runtime.
It also initializes missing Git submodules. Existing submodules and local
changes are preserved. The pinned librats fork includes the Windows UTF-8 and
download fixes. The build rejects a submodule checkout that predates those fixes.

The script builds Release, runs the application tests, deploys Qt and compiler
runtime DLLs, copies Manticore, verifies the staged executable starts without
the toolchain on PATH, and creates:

- `dist/RatsSearch-Windows-x64-Adv-s3-fix1.zip`
- `dist/RatsSearch-Windows-x64-Adv-s3-fix1.zip.sha256`

The S3 fix1 validation build uses a separate filename and preserves the previously
delivered stable and S0–S3 ZIPs. It fixes remembered task settings, startup progress,
selection-change latency and download-settings localization.
See [fix1 validation](DOWNLOAD_S3_FIX1_REPORT.md).
Set `-PackageSuffix <name>` to label another local package.
Default builds also run the focused librats regression suite, using pinned
GoogleTest v1.17.0 sources cached in `.tools/googletest-1.17.0`.

The patches in `patches/` are retained as historical references; they are not
applied during builds because the pinned fork already contains the changes.

The MSVC runtime required by MySQL/Manticore is copied app-local from the build
machine's System32 directory, alongside the MinGW runtime deployed by Qt.
Extract the ZIP and run `RatsSearch.exe`. No Qt or compiler installation is
needed on the destination machine. To keep application data in the extracted
folder, launch `RatsSearch.exe --data-dir .\data` from that folder.
Existing incorrectly encoded download folders are not migrated automatically.

## Command-line options

```powershell
# Default: build and test, downloading only missing dependencies
.\build-windows.cmd

# Reuse the cached toolchain and initialized submodules without downloading
.\build-windows.cmd -Offline

# Change build/test parallelism (default: 4)
.\build-windows.cmd -Jobs 8

# Explicitly omit tests
.\build-windows.cmd -SkipTests
```

The PowerShell implementation is [`scripts/build-windows.ps1`](../scripts/build-windows.ps1).
It requires Windows PowerShell 5.1 or newer. Download or command failures stop
the build. Build products remain under `build-windows-x64/` for incremental
rebuilds; each packaging run uses a fresh staging directory. A ZIP is replaced
only after a successful build and the requested test run.

The package uses the `libmysql.dll` supplied with QMYSQL, copied after Manticore
and Qt deployment. Do not substitute the DLL in `imports/`: its ABI can differ
and cause incorrect database field decoding.

The UTF-8 filesystem changes live in the forked `src/librats` Git submodule.
Commit and push further dependency changes to the fork before updating the
submodule pointer in the parent repository.

Default builds also extract the actual ZIP into a Unicode directory and run the local Tracker/Seeder application smoke helper. The child uses only package/Windows runtime paths, a fresh ASCII database directory, and Unicode payload names/paths; it verifies exact downloaded bytes and policy persistence after a forced process restart. The helper owns only its test process tree and uses --no-remember-data-dir. -SkipTests skips both CTest and this smoke.
