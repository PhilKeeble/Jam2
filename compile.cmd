@echo off
setlocal EnableExtensions

if /I "%~1"=="--in-dev-shell" goto build

start "Jam2 MSVC Build" "%ComSpec%" /k call "%~f0" --in-dev-shell
exit /b 0

:build
title Jam2 MSVC Build
set "REPO_DIR=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "QT_DIR=C:/Qt/6.10.3/msvc2022_64"
set "ASIO_DIR=C:/Tools/ASIO-SDK_2.3.4_2025-10-15/ASIOSDK"

if not exist "%VSWHERE%" (
    echo ERROR: Visual Studio Installer's vswhere.exe was not found.
    exit /b 1
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"

if not defined VS_INSTALL (
    echo ERROR: No Visual Studio installation with the C++ toolchain was found.
    exit /b 1
)

set "VSDEVCMD=%VS_INSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
    echo ERROR: Visual Studio developer environment was not found at:
    echo        %VSDEVCMD%
    exit /b 1
)

call "%VSDEVCMD%" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b %errorlevel%

cd /d "%REPO_DIR%"
if errorlevel 1 exit /b %errorlevel%

echo.
echo Configuring Jam2...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" -DASIO_SDK_DIR="%ASIO_DIR%"
if errorlevel 1 (
    echo.
    echo CONFIGURE FAILED.
    exit /b %errorlevel%
)

echo.
echo Building Jam2...
cmake --build build
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    exit /b %errorlevel%
)

echo.
echo BUILD SUCCEEDED.
