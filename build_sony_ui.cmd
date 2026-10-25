@echo off
setlocal EnableExtensions
chcp 65001 >nul
set "ROOT=%~dp0"
set "GXX=C:\Users\liuar\Desktop\tools\mingw64\bin\g++.exe"
if not exist "%GXX%" set "GXX=g++"
echo ============================================================
echo  Sony SoundConnect Premium UI - ULT WEAR Build and Test
echo ============================================================
"%GXX%" --version
if errorlevel 1 goto :no_compiler
echo.
echo [1/3] Building protocol simulator tests...
"%GXX%" -std=c++17 -pthread -finput-charset=UTF-8 -fexec-charset=UTF-8 -O2 -Wall -Wextra "%ROOT%sony-soundconnect-cross-fixed.cpp" -o "%ROOT%SonySoundConnectTest.exe" -lbthprops
if errorlevel 1 goto :failed
echo.
echo [2/3] Running simulator regression tests...
"%ROOT%SonySoundConnectTest.exe" --test-sim
if errorlevel 1 goto :failed
echo.
echo [3/3] Building Windows native UI...
"%GXX%" -std=c++17 -pthread -DUNICODE -D_UNICODE -DENABLE_NATIVE_BT -finput-charset=UTF-8 -fexec-charset=UTF-8 -O2 -Wall -Wextra "%ROOT%sony-soundconnect-ui.cpp" -o "%ROOT%SonySoundConnectUI.exe" -mwindows -lws2_32 -lgdi32 -luser32 -lbthprops
if errorlevel 1 goto :failed
echo.
echo [SUCCESS] Built: "%ROOT%SonySoundConnectUI.exe"
echo Run SonySoundConnectUI.exe to open the window.
pause
exit /b 0
:no_compiler
echo [ERROR] g++ not found. Edit GXX in this script to your MinGW compiler path.
pause
exit /b 1
:failed
echo.
echo [FAILED] Build/test failed. Copy all output above for diagnosis.
pause
exit /b 1
