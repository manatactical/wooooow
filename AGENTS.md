# AGENTS.md

## Build

**Always build with the wrapper. It fails loudly; a raw piped `cmake --build` does not.**

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/build.ps1
# -Target Wow -Config Debug -BuildDir build/win32   (Win32 does not work on this machine)
```

It runs the build, tees the full log to `<buildDir>/<Target>-<Config>-build.log` (inside the build directory,
so x64 and win32 runs cannot overwrite each other's log), then re-reads that log for
`error C####:`/`error D#### :`/`error:`/`Build FAILED`/`fatal error`/`N Error(s)` (a non-zero count only —
MSBuild's success summary ends in `0 Error(s)`) and exits non-zero if any appear or if no binary was
produced. Exit codes: `0` built, `1` cmake failed, `2` error lines in the log, `3` no binary / unknown target.

**Do not pipe the build into `Select-String`/`Where-Object`/`Select-Object`.** Piping replaces
`$LASTEXITCODE` with the filter's status and discards the error lines, so a broken build reads as a green
one — this is exactly how a compile error got missed before. If you must run cmake by hand, check
`$LASTEXITCODE` immediately and never after a pipe:

```powershell
$cmake = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
& $cmake --build build/x64 --config Release --target WowClient --parallel
if ($LASTEXITCODE -ne 0) { Write-Error "build failed: $LASTEXITCODE" }
```

`build/x64` is already configured (VS18 2026 generator, x64). Reuse it; do not re-run `cmake -S` unless
you change CMakeLists or need different options.

Output: `bin/Win/WowClient64.exe`. `Wow` gives `Wow64.exe` (same client, no crash logger).

`cmake` is usually not on PATH here; `Tools/build.ps1` falls back to the VS18 Community one:
`C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe`.

Shell is PowerShell; `&&` does not work. Use `;`.

Build is noisy: MSVC emits C4309/C4369 enum-truncation warnings from `Engine/Source/Event/Types.h`,
`WoW/Source/Glue/Types.h`, `WoW/Source/WorldClient/Types.h`. Pre-existing, ignore (warnings, not errors).

Win32 build needs NASM on PATH and does not work on this machine. x64 only unless asked otherwise.

See README.md for full options (`WOWDECOMP_DISABLE_SIGNATURE_CHECKS`, `WOWCLIENT_CRASH_LOG`) and layout.

## UI files (Lua/XML)

**After editing anything under `Data/custom.MPQ/Interface/`, always run `Tools/deploy-ui.ps1`.** The client loads the
copies in `bin/Win/Data/custom.MPQ/` (gitignored), not the tracked originals, so an edit that is not deployed does
not show up in game. Building the client does not do this. The client must be closed while it runs.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File Tools/deploy-ui.ps1
```
