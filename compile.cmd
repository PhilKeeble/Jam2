@echo off
setlocal EnableExtensions

if /I "%~1"=="--in-dev-shell" goto build

start "Jam2 MSVC Build" "%ComSpec%" /k call "%~f0" --in-dev-shell
exit /b 0

:build
title Jam2 MSVC Build
set "REPO_DIR=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

echo.
echo Checking Jam2 build prerequisites...

if not exist "%VSWHERE%" (
    call :print_visual_studio_error "Visual Studio Installer's vswhere.exe was not found."
    exit /b 1
)

for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"

if not defined VS_INSTALL (
    call :print_visual_studio_error "No Visual Studio installation with the C++ toolchain was found."
    exit /b 1
)

set "VSDEVCMD=%VS_INSTALL%\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEVCMD%" (
    echo.
    echo ERROR: The Visual Studio developer environment was not found at:
    echo        "%VSDEVCMD%"
    echo.
    echo Repair the Visual Studio installation, then run compile.cmd again.
    exit /b 1
)

call "%VSDEVCMD%" -arch=x64 -host_arch=x64
if errorlevel 1 (
    echo.
    echo ERROR: Visual Studio could not initialize its x64 developer environment.
    echo Repair the Visual Studio installation, then run compile.cmd again.
    exit /b 1
)

set "PREREQ_FAILED=0"

call :check_msvc
if errorlevel 1 set "PREREQ_FAILED=1"

call :check_windows_sdk
if errorlevel 1 set "PREREQ_FAILED=1"

call :check_cmake
if errorlevel 1 set "PREREQ_FAILED=1"

call :check_ninja
if errorlevel 1 set "PREREQ_FAILED=1"

call :resolve_qt
if errorlevel 1 set "PREREQ_FAILED=1"

call :resolve_asio
if errorlevel 1 set "PREREQ_FAILED=1"

if "%PREREQ_FAILED%"=="1" (
    echo.
    echo PREREQUISITE CHECK FAILED. Install or configure the items above, then run compile.cmd again.
    echo Full Windows setup instructions: "%REPO_DIR%docs\Building.md"
    exit /b 1
)

echo.
echo Prerequisites found:
echo   Visual Studio: "%VS_INSTALL%"
echo   MSVC compiler: "%CL_PATH%"
echo   Windows SDK:   "%RC_PATH%"
echo   CMake %CMAKE_VERSION%: "%CMAKE_PATH%"
echo   Ninja %NINJA_VERSION%: "%NINJA_PATH%"
echo   Qt 6:          "%QT_DIR%"
echo   ASIO SDK:      "%ASIO_SDK_DIR%"

cd /d "%REPO_DIR%"
if errorlevel 1 (
    echo.
    echo ERROR: Could not change to the repository directory:
    echo        "%REPO_DIR%"
    exit /b 1
)

echo.
echo Configuring Jam2...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" -DASIO_SDK_DIR="%ASIO_SDK_DIR%"
if errorlevel 1 (
    echo.
    echo CONFIGURE FAILED.
    exit /b 1
)

echo.
echo Building Jam2...
cmake --build build
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    exit /b 1
)

echo.
echo BUILD SUCCEEDED.
exit /b 0

:check_msvc
where cl.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo ERROR: The MSVC x64 compiler was not found after loading Visual Studio.
    echo Open Visual Studio Installer, modify the installation, and add:
    echo   Desktop development with C++
    exit /b 1
)
for /f "delims=" %%I in ('where cl.exe 2^>nul') do if not defined CL_PATH set "CL_PATH=%%I"
exit /b 0

:check_windows_sdk
where rc.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo ERROR: A Windows SDK resource compiler was not found.
    echo Open Visual Studio Installer, modify the C++ workload, and add a Windows 10 or Windows 11 SDK.
    exit /b 1
)
for /f "delims=" %%I in ('where rc.exe 2^>nul') do if not defined RC_PATH set "RC_PATH=%%I"
exit /b 0

:check_cmake
where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo ERROR: CMake 3.24 or newer was not found.
    echo Install it from an Administrator PowerShell with:
    echo   winget install --id Kitware.CMake -e
    echo Or add "C++ CMake tools for Windows" in Visual Studio Installer.
    exit /b 1
)
for /f "delims=" %%I in ('where cmake.exe 2^>nul') do if not defined CMAKE_PATH set "CMAKE_PATH=%%I"
for /f "tokens=3" %%V in ('cmake --version 2^>nul ^| findstr /b /c:"cmake version"') do if not defined CMAKE_VERSION set "CMAKE_VERSION=%%V"
if not defined CMAKE_VERSION (
    echo.
    echo ERROR: CMake was found, but its version could not be determined.
    echo Found executable: "%CMAKE_PATH%"
    exit /b 1
)
call :cmake_version_supported "%CMAKE_VERSION%"
if errorlevel 1 (
    echo.
    echo ERROR: CMake %CMAKE_VERSION% is too old. Jam2 requires CMake 3.24 or newer.
    echo Upgrade it from an Administrator PowerShell with:
    echo   winget upgrade --id Kitware.CMake -e
    exit /b 1
)
exit /b 0

:cmake_version_supported
for /f "tokens=1,2 delims=." %%A in ("%~1") do call :cmake_version_numbers %%A %%B
exit /b %errorlevel%

:cmake_version_numbers
if %1 GTR 3 exit /b 0
if %1 LSS 3 exit /b 1
if %2 GEQ 24 exit /b 0
exit /b 1

:check_ninja
where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo ERROR: Ninja was not found.
    echo Install it from an Administrator PowerShell with:
    echo   winget install --id Ninja-build.Ninja -e
    echo Or add "C++ CMake tools for Windows" in Visual Studio Installer.
    exit /b 1
)
for /f "delims=" %%I in ('where ninja.exe 2^>nul') do if not defined NINJA_PATH set "NINJA_PATH=%%I"
for /f "delims=" %%V in ('ninja --version 2^>nul') do if not defined NINJA_VERSION set "NINJA_VERSION=%%V"
if not defined NINJA_VERSION set "NINJA_VERSION=unknown version"
exit /b 0

:resolve_qt
if defined QT_DIR goto validate_qt_override
for /f "delims=" %%V in ('dir /b /ad /o-n "%SystemDrive%\Qt\6.*" 2^>nul') do call :scan_qt_version "%SystemDrive%\Qt\%%V"
if defined QT_DIR exit /b 0
call :print_missing_qt
exit /b 1

:validate_qt_override
call :validate_qt "%QT_DIR%"
if not errorlevel 1 exit /b 0
echo.
echo ERROR: QT_DIR does not point to a complete Qt 6 MSVC x64 kit:
echo        "%QT_DIR%"
call :print_qt_help
exit /b 1

:scan_qt_version
if defined QT_DIR exit /b 0
if exist "%~1\msvc2022_64" call :consider_qt "%~1\msvc2022_64"
for /f "delims=" %%Q in ('dir /b /ad /o-n "%~1\msvc*_64" 2^>nul') do call :consider_qt "%~1\%%Q"
exit /b 0

:consider_qt
if defined QT_DIR exit /b 0
call :validate_qt "%~1"
if errorlevel 1 exit /b 0
set "QT_DIR=%~1"
exit /b 0

:validate_qt
if not exist "%~1\lib\cmake\Qt6\Qt6Config.cmake" exit /b 1
if not exist "%~1\lib\cmake\Qt6Core\Qt6CoreConfig.cmake" exit /b 1
if not exist "%~1\lib\cmake\Qt6Gui\Qt6GuiConfig.cmake" exit /b 1
if not exist "%~1\lib\cmake\Qt6Widgets\Qt6WidgetsConfig.cmake" exit /b 1
if not exist "%~1\lib\cmake\Qt6Network\Qt6NetworkConfig.cmake" exit /b 1
exit /b 0

:print_missing_qt
echo.
echo ERROR: A complete Qt 6 MSVC x64 installation was not found under:
echo        "%SystemDrive%\Qt"
call :print_qt_help
exit /b 0

:print_qt_help
echo Jam2 needs the Qt 6 Core, Gui, Widgets, and Network development packages.
echo Install Qt with the Qt Online Installer and select an MSVC 2022 64-bit desktop kit.
echo Do not select a MinGW-only kit for this MSVC build.
echo   https://doc.qt.io/qt-6/get-and-install-qt.html
echo.
echo For a custom installation, set QT_DIR to the kit directory before running compile.cmd:
echo   set "QT_DIR=D:\SDKs\Qt\6.x\msvc2022_64"
exit /b 0

:resolve_asio
if defined ASIO_SDK_DIR goto validate_asio_override
for /f "delims=" %%A in ('dir /b /ad /o-n "%SystemDrive%\Tools\ASIO*" 2^>nul') do call :consider_asio "%SystemDrive%\Tools\%%A\ASIOSDK"
if defined ASIO_SDK_DIR exit /b 0
call :print_missing_asio
exit /b 1

:validate_asio_override
call :validate_asio "%ASIO_SDK_DIR%"
if not errorlevel 1 exit /b 0
echo.
echo ERROR: ASIO_SDK_DIR does not point to a complete Steinberg ASIO SDK:
echo        "%ASIO_SDK_DIR%"
call :print_asio_help
exit /b 1

:consider_asio
if defined ASIO_SDK_DIR exit /b 0
call :validate_asio "%~1"
if errorlevel 1 exit /b 0
set "ASIO_SDK_DIR=%~1"
exit /b 0

:validate_asio
if not exist "%~1\common\asio.h" exit /b 1
if not exist "%~1\common\iasiodrv.h" exit /b 1
exit /b 0

:print_missing_asio
echo.
echo ERROR: The Steinberg ASIO SDK was not found under:
echo        "%SystemDrive%\Tools"
call :print_asio_help
exit /b 0

:print_asio_help
echo Download and extract the ASIO SDK from:
echo   https://www.steinberg.net/developers/asiosdk-open/
echo The ZIP file must be extracted; the selected directory must contain common\asio.h.
echo.
echo For a custom location, set ASIO_SDK_DIR before running compile.cmd:
echo   set "ASIO_SDK_DIR=D:\SDKs\ASIO-SDK\ASIOSDK"
exit /b 0

:print_visual_studio_error
echo.
echo ERROR: %~1
echo Install Visual Studio 2022 Build Tools with the "Desktop development with C++" workload.
echo In Visual Studio Installer, include MSVC x64 tools, a Windows SDK, and C++ CMake tools.
echo   https://visualstudio.microsoft.com/downloads/
echo.
echo From an Administrator PowerShell, the core workload can be installed with:
echo   winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
exit /b 0
