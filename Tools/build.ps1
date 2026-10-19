# Builds a target and fails loudly. Never pipe the raw cmake command yourself: piping it into
# Select-String/Where-Object replaces $LASTEXITCODE with the filter's status and drops the error lines, so a
# broken build reads as a successful one. This script keeps the full log, scans it for failure markers and
# exits non-zero, which is what makes `; if ($LASTEXITCODE) { ... }` and CI mean anything.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File Tools/build.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File Tools/build.ps1 -Target Wow -Config Debug
#
# Exit codes: 0 built, 1 cmake failed, 2 error markers in the log, 3 no output produced.

[CmdletBinding()]
param(
    [string]$Target = 'WowClient',
    [string]$Config = 'Release',
    [string]$BuildDir = 'build/x64',
    [switch]$Quiet
)

$ErrorActionPreference = 'Stop'

# A -Target typo must not read as success: only these have a known binary to check for.
$knownTargets = @{
    'WowClient' = 'bin/Win/WowClient64.exe'
    'Wow'       = 'bin/Win/Wow64.exe'
}

function Fail([string]$message, [int]$code) {
    Write-Host "==> BUILD FAILED: $message" -ForegroundColor Red
    Write-Host "    full log: $script:log" -ForegroundColor Red
    exit $code
}

$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo

try {
    if (-not $knownTargets.ContainsKey($Target)) {
        Write-Host "==> BUILD FAILED: unknown target '$Target' (known: $($knownTargets.Keys -join ', '))" -ForegroundColor Red
        exit 3
    }

    $cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source

    if (-not $cmake) {
        # VS18 Community's bundled cmake, the path AGENTS.md documents.
        $cmake = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
    }

    if (-not (Test-Path $cmake)) {
        Write-Host "==> BUILD FAILED: cmake not found on PATH or at '$cmake'" -ForegroundColor Red
        exit 1
    }

    if (-not (Test-Path $BuildDir)) {
        Write-Host "==> BUILD FAILED: build directory '$BuildDir' does not exist (configure it first, see AGENTS.md)" -ForegroundColor Red
        exit 1
    }

    # The log lives inside the build directory, so x64 and win32 runs cannot overwrite each other's log and a
    # tag can never collide with a differently-named directory. build/ is gitignored.
    $script:log = Join-Path $BuildDir "$Target-$Config-build.log"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $script:log) | Out-Null

    Write-Host "==> $cmake --build $BuildDir --config $Config --target $Target" -ForegroundColor Cyan

    # Native stderr through the pipeline can surface as a terminating NativeCommandError depending on host and
    # $PSNativeCommandUseErrorActionPreference, which would abort before cmake's own exit code is read. So the
    # preference is relaxed for the invocation only, and every stream is merged with *>&1.
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'

    try {
        & $cmake --build $BuildDir --config $Config --target $Target --parallel *>&1 |
            Tee-Object -FilePath $script:log
        $cmakeExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previous
    }

    if ($cmakeExit -ne 0) {
        Fail "cmake exited $cmakeExit" 1
    }

    if (-not (Test-Path $script:log)) {
        Fail 'cmake produced no log output' 2
    }

    # cmake can exit 0 while MSBuild reported errors (e.g. in a multi-target invocation), so the log is the
    # verdict. Match the failure markers themselves rather than dropping every line containing "warning" --
    # that would hide C2220, "error C2220: the following warning is treated as an error", and report a green
    # build. The error-count alternative is anchored to a non-zero count, because MSBuild's success summary
    # ends in "0 Error(s)", and the space before the colon covers cl.exe option diagnostics like D8021.
    $errors = Select-String -Path $script:log -Pattern 'error [A-Z]+\d+ *:|error *:|Build FAILED|fatal error|[1-9]\d* Error\(s\)'

    if ($errors) {
        Write-Host "==> BUILD FAILED: $($errors.Count) error line(s) in the log" -ForegroundColor Red

        if (-not $Quiet) {
            $errors | Select-Object -First 40 | ForEach-Object { Write-Host "    $($_.Line.Trim())" -ForegroundColor Red }
        }

        Write-Host "    full log: $script:log" -ForegroundColor Red
        exit 2
    }

    $binary = $knownTargets[$Target]

    if (-not (Test-Path $binary)) {
        Fail "no binary at '$binary'" 3
    }

    $info = Get-Item $binary
    Write-Host ("==> OK: {0}  ({1:N0} bytes, {2})" -f $binary, $info.Length, $info.LastWriteTime) -ForegroundColor Green
    Write-Host "    full log: $script:log"
    exit 0
} finally {
    Pop-Location
}