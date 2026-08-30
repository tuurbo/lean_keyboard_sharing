@echo off
cd /d "%~dp0"
echo Building Keyboard Switcher with MinGW...

set "OUTPUT_DIR=dist"
if not exist "%OUTPUT_DIR%" mkdir "%OUTPUT_DIR%"

gcc -O3 -static -o "%OUTPUT_DIR%\sender.exe" sender.cpp -lws2_32 -municode -mwindows -lstdc++

if %ERRORLEVEL% NEQ 0 (
    echo Failed to build sender.exe
    exit /b 1
)

windres receiver.rc receiver_res.o
if %ERRORLEVEL% NEQ 0 (
    echo Failed to compile receiver resources
    exit /b 1
)

gcc -O3 -static -mwindows -o "%OUTPUT_DIR%\receiver.exe" receiver.cpp receiver_res.o -lws2_32 -luser32 -lstdc++
if %ERRORLEVEL% NEQ 0 (
    echo Failed to build receiver.exe
    exit /b 1
)

echo.
echo Build successful!
echo.
echo Files created:
echo   - %OUTPUT_DIR%\sender.exe   (run on gaming PC)
echo   - %OUTPUT_DIR%\receiver.exe (run on receiving PC)
echo.
