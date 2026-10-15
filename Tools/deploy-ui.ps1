# Copies the repo-tracked UI overrides from Data/ into the run directory.
#
# bin/ is gitignored, so the copies of VideoOptionsPanels.{lua,xml} that actually get loaded live in
# bin/Win/Data/custom.MPQ/ and are not version-controlled. The editable originals live in
# Data/custom.MPQ/Interface/ and this script puts them in place. Run it after editing them, before
# launching the game.
#
# The client must be closed: the loader holds these files open while running.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repoRoot "Data/custom.MPQ"
$target = Join-Path $repoRoot "bin/Win/Data/custom.MPQ"

if (-not (Test-Path $source)) {
    throw "No repo-tracked UI found at $source"
}

if (Get-Process -Name WowClient64 -ErrorAction SilentlyContinue) {
    throw "WowClient64.exe is running; close it first or the files cannot be replaced."
}

$files = Get-ChildItem -Path $source -Recurse -File
$copied = 0

foreach ($file in $files) {
    $relative = $file.FullName.Substring($source.Length).TrimStart("\")
    $destination = Join-Path $target $relative

    $destinationDirectory = Split-Path -Parent $destination

    if (-not (Test-Path $destinationDirectory)) {
        New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
    }

    Copy-Item $file.FullName $destination -Force
    Write-Host "  $relative"
    $copied++
}

Write-Host "Deployed $copied UI file(s) to $target"