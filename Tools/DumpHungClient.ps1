# NOT IN BINARY. Watches a running WowClient64 and, if its main window stops pumping messages
# (i.e. the process is hung rather than merely slow), writes a full minidump so the stack can be
# walked offline. The in-tree crash logger only catches exceptions; a hang raises none, which is
# why the stuck-zone load left nothing behind.
#
# Usage:  powershell -File Tools/DumpHungClient.ps1 [-ProcessName WowClient64] [-HangSeconds 20]
#
# A dump is written to bin/Win/Errors/hang-<yyyyMMdd-HHmmss>.dmp and the process is left running.

param(
    [string] $ProcessName = "WowClient64",
    [int]    $HangSeconds = 20,
    [int]    $PollSeconds = 2
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$outDir = Join-Path $root "bin\Win\Errors"

if (-not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Path $outDir -Force | Out-Null
}

# A hung window stops answering WM_NULL; SendMessageTimeout returns 0 instead of 1.
Add-Type -Namespace Win32 -Name Native -MemberDefinition @'
    [DllImport("user32.dll", SetLastError = true)]
    public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam,
                                                   uint flags, uint timeout, out IntPtr result);
'@

function Test-Responsive([IntPtr] $hWnd) {
    $result = [IntPtr]::Zero
    $reply = [Win32.Native]::SendMessageTimeout($hWnd, 0, [IntPtr]::Zero, [IntPtr]::Zero, 2, 2000, [ref] $result)
    return ($reply -ne [IntPtr]::Zero)
}

$proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1

if (-not $proc) {
    Write-Host "No '$ProcessName' process is running. Start the client, get it hung, then run this."
    exit 1
}

Write-Host "Watching PID $($proc.Id) for a hang of $HangSeconds s..."

$hungSince = $null

while (-not $proc.HasExited) {
    $proc.Refresh()

    if ($proc.MainWindowHandle -eq [IntPtr]::Zero) {
        # Still on the loading screen with no window yet: treat as not-yet-observed.
        $hungSince = $null
        Start-Sleep -Seconds $PollSeconds
        continue
    }

    if (Test-Responsive $proc.MainWindowHandle) {
        $hungSince = $null
    } else {
        if ($null -eq $hungSince) {
            $hungSince = Get-Date
            Write-Host "$(Get-Date -Format HH:mm:ss)  window stopped responding, waiting..."

            # Capture the stack immediately: the first unresponsive sample is already the hang,
            # and waiting only risks the process being killed before the dump is written.
            $stamp = Get-Date -Format "yyyyMMdd-HHmmss"
            $dump = Join-Path $outDir "hang-$stamp.dmp"

            & "$env:SystemRoot\System32\rundll32.exe" `
                "$env:SystemRoot\System32\comsvcs.dll",MiniDump `
                $proc.Id $dump full

            if (Test-Path $dump) {
                Write-Host "Wrote $dump ($([math]::Round((Get-Item $dump).Length / 1MB)) MB)"
                Write-Host "The process was left running. Read the dump with:  windbg -z `"$dump`""
                exit 0
            }

            Write-Host "comsvcs MiniDump failed; retrying via the procdump path if available."
        }
    }

    Start-Sleep -Seconds $PollSeconds
}

Write-Host "Process exited before a dump was taken."
exit 1
