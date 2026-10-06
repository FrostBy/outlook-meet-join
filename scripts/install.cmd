@echo off
setlocal
set "ACTION=%~1"
if "%ACTION%"=="" set "ACTION=Install"
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -ArgumentList '%ACTION%' -Verb RunAs"
    exit /b
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Action %ACTION%
echo.
pause
