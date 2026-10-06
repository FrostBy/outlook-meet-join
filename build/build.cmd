@echo off
setlocal EnableDelayedExpansion
set "ROOT=%~dp0.."
set "SDK=%ROOT%\build\sdk"
set "OUT=%ROOT%\dist"

if not exist "%SDK%\include\WebView2.h" (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0get-sdk.ps1" || exit /b 1
)

set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "!VSWHERE!" (
    for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not defined VCVARS (
    echo Visual Studio 2022 Build Tools with the C++ workload are required.
    exit /b 1
)

call "%VCVARS%" >nul 2>&1 || exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%ROOT%\build\obj" mkdir "%ROOT%\build\obj"
cl /nologo /LD /O2 /MT /EHsc /W4 /DUNICODE /D_UNICODE /I"%SDK%\include" "%ROOT%\src\loader\OutlookMeetJoin.cpp" /Fo"%ROOT%\build\obj\\" /Fe:"%OUT%\OutlookMeetJoin.dll" /link /DLL || exit /b 1
copy /y "%ROOT%\src\payload\inject.js" "%OUT%\inject.js" >nul
copy /y "%ROOT%\scripts\install.ps1" "%OUT%\install.ps1" >nul
copy /y "%ROOT%\scripts\install.cmd" "%OUT%\install.cmd" >nul
echo Built: %OUT%\OutlookMeetJoin.dll
