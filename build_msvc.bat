@echo off
setlocal EnableExtensions
cd /d "%~dp0"

echo Building Keyboard Switcher with Microsoft C++...

if defined VSCMD_VER goto build

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto tools_missing

for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%I"
if not defined VSINSTALL goto tools_missing

set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1

:build

set "OUTPUT_DIR=dist"
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

rc.exe /nologo /fo receiver_res.res receiver.rc
if errorlevel 1 exit /b 1

cl.exe /nologo /O2 /EHsc /MT /DUNICODE /D_UNICODE /Fe:"%OUTPUT_DIR%\sender.exe" sender.cpp /link /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup ws2_32.lib user32.lib
if errorlevel 1 exit /b 1

cl.exe /nologo /O2 /EHsc /MT /DUNICODE /D_UNICODE /Fe:"%OUTPUT_DIR%\receiver.exe" receiver.cpp receiver_res.res /link /SUBSYSTEM:WINDOWS ws2_32.lib user32.lib
if errorlevel 1 exit /b 1

del /q sender.obj receiver.obj receiver_res.res >nul 2>nul

echo.
echo Build successful:
echo   %OUTPUT_DIR%\sender.exe
echo   %OUTPUT_DIR%\receiver.exe
exit /b 0

:tools_missing
echo Microsoft C++ Build Tools were not found.
exit /b 1
