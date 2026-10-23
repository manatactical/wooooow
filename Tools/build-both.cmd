@echo off
rem Double-click to build both launchable clients: WowClient64.exe and Wow64.exe.
rem Stops at the first failure and keeps the window open so the result can be read.
cd /d "%~dp0.."

powershell -NoProfile -ExecutionPolicy Bypass -File Tools\build.ps1 -Target WowClient
if errorlevel 1 goto failed

powershell -NoProfile -ExecutionPolicy Bypass -File Tools\build.ps1 -Target Wow
if errorlevel 1 goto failed

echo.
echo BUILD OK: bin\Win\WowClient64.exe and bin\Win\Wow64.exe
pause
exit /b 0

:failed
echo.
echo BUILD FAILED (exit code %errorlevel%). The full log is in build\x64\*-build.log
pause
exit /b 1
