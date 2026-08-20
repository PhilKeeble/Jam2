@echo off
setlocal EnableExtensions

if /I "%~1"=="--in-dev-shell" (
    shift /1
    goto build
)

start "Jam2 MSVC Build" "%ComSpec%" /k call "%~f0" --in-dev-shell %*
exit /b 0

:build
set "JAM2_BUILD_TESTING=OFF"
set "JAM2_TEST_SUITE="
set "JAM2_TEST_TARGET="
set "JAM2_TEST_LABEL="
set "JAM2_TEST_SHOW_GUI=0"
set "JAM2_TEST_NAME="
set "JAM2_COVERAGE_MODE=0"
set "JAM2_TEST_SELECTION_EXPLICIT=0"
set "JAM2_COVERAGE_RESULT=0"
set "JAM2_CTEST_LOG_ARGS="
set "JAM2_HARDWARE_PROFILE="

:parse_arguments
if "%~1"=="" goto arguments_ready
if /I "%~1"=="--tests-full" (
    set "JAM2_BUILD_TESTING=ON"
    set "JAM2_TEST_SUITE=full"
    set "JAM2_TEST_SELECTION_EXPLICIT=1"
    shift /1
    goto parse_arguments
)
if /I "%~1"=="--coverage" (
    set "JAM2_BUILD_TESTING=ON"
    set "JAM2_TEST_SUITE=full"
    set "JAM2_COVERAGE_MODE=1"
    shift /1
    goto parse_arguments
)
if /I "%~1"=="--tests" (
    if "%~2"=="" (
        echo ERROR: --tests requires a suite name.
        exit /b 2
    )
    set "JAM2_BUILD_TESTING=ON"
    set "JAM2_TEST_SUITE=%~2"
    set "JAM2_TEST_SELECTION_EXPLICIT=1"
    shift /1
    shift /1
    goto parse_arguments
)
if /I "%~1"=="--show-gui" (
    set "JAM2_TEST_SHOW_GUI=1"
    shift /1
    goto parse_arguments
)
if /I "%~1"=="--test-name" (
    if "%~2"=="" (
        echo ERROR: --test-name requires one exact CTest name.
        exit /b 2
    )
    set "JAM2_TEST_NAME=%~2"
    shift /1
    shift /1
    goto parse_arguments
)
if /I "%~1"=="--hardware-profile" (
    if "%~2"=="" (
        echo ERROR: --hardware-profile requires a JSON profile path.
        exit /b 2
    )
    set "JAM2_HARDWARE_PROFILE=%~f2"
    shift /1
    shift /1
    goto parse_arguments
)
echo ERROR: Unknown compile option: %~1
echo Supported options: --tests unit, --tests plugin, --tests hardware, --tests gui, --tests jam-sync, --tests shared-content, --tests performance, --tests network, --tests full, --tests-full, --coverage, --test-name NAME, --show-gui, --hardware-profile PATH
exit /b 2

:arguments_ready
if "%JAM2_COVERAGE_MODE%"=="1" if "%JAM2_TEST_SELECTION_EXPLICIT%"=="1" (
    echo ERROR: --coverage is a standalone full-catalogue mode; do not combine it with --tests or --tests-full.
    exit /b 2
)
if /I "%JAM2_TEST_SUITE%"=="unit" (
    set "JAM2_TEST_TARGET=jam2_tests_unit"
    set "JAM2_TEST_LABEL=unit"
) else if /I "%JAM2_TEST_SUITE%"=="plugin" (
    set "JAM2_TEST_TARGET=jam2_tests_plugin"
    set "JAM2_TEST_LABEL=plugin"
) else if /I "%JAM2_TEST_SUITE%"=="hardware" (
    set "JAM2_TEST_TARGET=jam2_tests_hardware"
    set "JAM2_TEST_LABEL=hardware"
) else if /I "%JAM2_TEST_SUITE%"=="gui" (
    set "JAM2_TEST_TARGET=jam2_tests_gui"
    set "JAM2_TEST_LABEL=gui"
) else if /I "%JAM2_TEST_SUITE%"=="jam-sync" (
    set "JAM2_TEST_TARGET=jam2_tests_jam_sync"
    set "JAM2_TEST_LABEL=jam-sync"
) else if /I "%JAM2_TEST_SUITE%"=="shared-content" (
    set "JAM2_TEST_TARGET=jam2_tests_shared_content"
    set "JAM2_TEST_LABEL=shared-content"
) else if /I "%JAM2_TEST_SUITE%"=="performance" (
    set "JAM2_TEST_TARGET=jam2_tests_performance"
    set "JAM2_TEST_LABEL=performance"
) else if /I "%JAM2_TEST_SUITE%"=="network" (
    set "JAM2_TEST_TARGET=jam2_tests_network"
    set "JAM2_TEST_LABEL=^^network$"
) else if /I "%JAM2_TEST_SUITE%"=="full" (
    set "JAM2_TEST_TARGET=jam2_tests_all"
) else if not "%JAM2_TEST_SUITE%"=="" (
    echo ERROR: Test suite "%JAM2_TEST_SUITE%" is not implemented yet.
    echo Available suites: unit, plugin, hardware, gui, jam-sync, shared-content, performance, network, full
    exit /b 2
)
if "%JAM2_TEST_SHOW_GUI%"=="1" (
    if /I not "%JAM2_TEST_SUITE%"=="gui" if /I not "%JAM2_TEST_SUITE%"=="full" (
        echo ERROR: --show-gui requires the gui or full test suite.
        exit /b 2
    )
)
if defined JAM2_TEST_NAME if not defined JAM2_TEST_TARGET (
    echo ERROR: --test-name requires --tests SUITE, --tests-full, or --coverage.
    exit /b 2
)
if /I "%JAM2_TEST_SUITE%"=="hardware" if not defined JAM2_HARDWARE_PROFILE (
    echo ERROR: --tests hardware requires --hardware-profile PATH.
    exit /b 2
)
if defined JAM2_HARDWARE_PROFILE if /I not "%JAM2_TEST_SUITE%"=="hardware" if /I not "%JAM2_TEST_SUITE%"=="full" (
    echo ERROR: --hardware-profile requires --tests hardware, --tests-full, or --coverage.
    exit /b 2
)
if defined JAM2_HARDWARE_PROFILE if not exist "%JAM2_HARDWARE_PROFILE%" (
    echo ERROR: Hardware profile was not found: "%JAM2_HARDWARE_PROFILE%"
    exit /b 2
)

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

if defined JAM2_TEST_TARGET (
    call :resolve_test_jobs
    if errorlevel 1 exit /b 2
    set "JAM2_TEST_ARTIFACT_ROOT=%REPO_DIR%build\test-artifacts"
    if exist "%REPO_DIR%build\test-artifacts" cmake -E remove_directory "%REPO_DIR%build\test-artifacts"
    if errorlevel 1 (
        echo.
        echo ERROR: Could not clear the previous test artifact workspace:
        echo        "%REPO_DIR%build\test-artifacts"
        exit /b 1
    )
    cmake -E make_directory "%REPO_DIR%build\test-artifacts"
    if errorlevel 1 (
        echo.
        echo ERROR: Could not create the test artifact workspace:
        echo        "%REPO_DIR%build\test-artifacts"
        exit /b 1
    )
)

if "%JAM2_COVERAGE_MODE%"=="1" (
    if not exist "%REPO_DIR%build\coverage" mkdir "%REPO_DIR%build\coverage"
    if errorlevel 1 (
        echo.
        echo ERROR: Could not create the coverage report directory.
        exit /b 1
    )
    set "JAM2_COVERAGE_TEST_ARTIFACT_ROOT=%REPO_DIR%build\coverage\test-artifacts"
    if exist "%REPO_DIR%build\coverage\test-artifacts" cmake -E remove_directory "%REPO_DIR%build\coverage\test-artifacts"
    if errorlevel 1 (
        echo.
        echo ERROR: Could not clear the previous coverage test artifacts.
        exit /b 1
    )
)

if not "%JAM2_COVERAGE_MODE%"=="1" goto normal_build
call :run_windows_coverage
set "JAM2_COVERAGE_RESULT=%errorlevel%"
if not "%JAM2_COVERAGE_RESULT%"=="0" (
    cmake -E rename "%JAM2_TEST_ARTIFACT_ROOT%" "%JAM2_COVERAGE_TEST_ARTIFACT_ROOT%"
    if errorlevel 1 (
        echo ERROR: Could not retain failed coverage test artifacts under build\coverage.
        exit /b 1
    )
) else (
    cmake -E remove_directory "%JAM2_TEST_ARTIFACT_ROOT%"
    if errorlevel 1 exit /b 1
)
cmake -E make_directory "%JAM2_TEST_ARTIFACT_ROOT%"
if errorlevel 1 exit /b 1
set "JAM2_BUILD_TESTING=OFF"
set "JAM2_TEST_SUITE="
set "JAM2_TEST_TARGET="
set "JAM2_TEST_LABEL="

:normal_build

echo.
echo Configuring normal Jam2 Release build...
cmake -U CMAKE_C_FLAGS_RELEASE -U CMAKE_CXX_FLAGS_RELEASE -U CMAKE_EXE_LINKER_FLAGS_RELEASE -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" -DASIO_SDK_DIR="%ASIO_SDK_DIR%" -DBUILD_TESTING=%JAM2_BUILD_TESTING% -DJAM2_ENABLE_COVERAGE=OFF -DJAM2_HARDWARE_PROFILE:FILEPATH="%JAM2_HARDWARE_PROFILE%"
if errorlevel 1 (
    echo.
    echo CONFIGURE FAILED.
    exit /b 1
)

findstr /B /C:"CMAKE_CXX_FLAGS_RELEASE:STRING=" build\CMakeCache.txt | findstr /C:"/O2" >nul
if errorlevel 1 (
    echo.
    echo ERROR: The normal Release build is not configured with MSVC /O2 optimization.
    exit /b 1
)
findstr /B /C:"CMAKE_CXX_FLAGS_RELEASE:STRING=" build\CMakeCache.txt | findstr /C:"/Od" >nul
if not errorlevel 1 (
    echo.
    echo ERROR: Coverage /Od flags leaked into the normal Release build.
    exit /b 1
)
findstr /B /C:"CMAKE_EXE_LINKER_FLAGS_RELEASE:STRING=" build\CMakeCache.txt | findstr /C:"/PROFILE" >nul
if not errorlevel 1 (
    echo.
    echo ERROR: Coverage /PROFILE flags leaked into the normal Release build.
    exit /b 1
)

echo.
echo Building Jam2...
if defined JAM2_TEST_TARGET (
    cmake --build build --target %JAM2_TEST_TARGET%
) else (
    cmake --build build
)
if errorlevel 1 (
    echo.
    echo BUILD FAILED.
    exit /b 1
)

if defined JAM2_TEST_TARGET (
    echo.
    echo Running Jam2 %JAM2_TEST_SUITE% tests...
    echo CTest parallel capacity: %JAM2_CTEST_JOBS% ^(override with JAM2_TEST_JOBS^)
    if defined JAM2_TEST_NAME (
        ctest --test-dir build --output-on-failure --parallel %JAM2_CTEST_JOBS% %JAM2_CTEST_LOG_ARGS% --no-tests=error -R "^%JAM2_TEST_NAME%$"
    ) else if defined JAM2_TEST_LABEL (
        ctest --test-dir build --output-on-failure --parallel %JAM2_CTEST_JOBS% %JAM2_CTEST_LOG_ARGS% -L "%JAM2_TEST_LABEL%"
    ) else (
        ctest --test-dir build --output-on-failure --parallel %JAM2_CTEST_JOBS% %JAM2_CTEST_LOG_ARGS%
    )
    if errorlevel 1 (
        echo.
        echo TESTS FAILED.
        exit /b 1
    )
)

echo.
if defined JAM2_TEST_TARGET (
    cmake -P "%REPO_DIR%build\ResetTestArtifactWorkspace.cmake"
    if errorlevel 1 (
        echo ERROR: Tests passed, but their artifact workspace could not be reset:
        echo        "%JAM2_TEST_ARTIFACT_ROOT%"
        exit /b 1
    )
)
if not "%JAM2_COVERAGE_RESULT%"=="0" (
    echo COVERAGE GATE FAILED. The normal Release binary and tests were restored successfully.
    echo Inspect "%REPO_DIR%build\coverage" for the native coverage reports.
    exit /b 1
)
if "%JAM2_COVERAGE_MODE%"=="1" (
    echo COVERAGE SUCCEEDED. THE NORMAL RELEASE BUILD WAS RESTORED.
) else if defined JAM2_TEST_TARGET (
    echo BUILD AND TESTS SUCCEEDED.
) else (
    echo BUILD SUCCEEDED.
)
exit /b 0

:resolve_test_jobs
if defined JAM2_TEST_JOBS (
    echo(%JAM2_TEST_JOBS%| findstr /r "^[1-9][0-9]*$" >nul
    if errorlevel 1 (
        echo.
        echo ERROR: JAM2_TEST_JOBS must be a positive integer.
        exit /b 1
    )
    set "JAM2_CTEST_JOBS=%JAM2_TEST_JOBS%"
) else (
    set "JAM2_CTEST_JOBS=1"
    if defined NUMBER_OF_PROCESSORS set /a JAM2_CTEST_JOBS=%NUMBER_OF_PROCESSORS%-1
    if defined NUMBER_OF_PROCESSORS if %NUMBER_OF_PROCESSORS% LEQ 2 set "JAM2_CTEST_JOBS=1"
    if defined NUMBER_OF_PROCESSORS if %NUMBER_OF_PROCESSORS% GTR 9 set "JAM2_CTEST_JOBS=8"
)
exit /b 0

:run_windows_coverage
where dotnet.exe >nul 2>nul
if errorlevel 1 (
    echo.
    echo ERROR: dotnet.exe is required only for --coverage tool restore.
    exit /b 1
)

echo.
echo Configuring instrumented Jam2 MSVC coverage build...
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%" -DASIO_SDK_DIR="%ASIO_SDK_DIR%" -DBUILD_TESTING=ON -DJAM2_ENABLE_COVERAGE=ON -DJAM2_HARDWARE_PROFILE:FILEPATH="%JAM2_HARDWARE_PROFILE%"
if errorlevel 1 (
    echo.
    echo COVERAGE CONFIGURE FAILED. Restoring the normal Release build next.
    exit /b 1
)

echo.
echo Building instrumented Jam2 test catalogue...
cmake --build build --target jam2_tests_all
if errorlevel 1 (
    echo.
    echo COVERAGE BUILD FAILED. Restoring the normal Release build next.
    exit /b 1
)

for %%I in ("%CMAKE_PATH%") do set "JAM2_CTEST_PATH=%%~dpIctest.exe"
set "JAM2_REPO_ROOT=%REPO_DIR:~0,-1%"
echo.
if defined JAM2_TEST_NAME (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%REPO_DIR%tests\coverage\RunWindowsCoverage.ps1" -RepoRoot "%JAM2_REPO_ROOT%" -BuildDirectory "%REPO_DIR%build" -CTestPath "%JAM2_CTEST_PATH%" -TestName "%JAM2_TEST_NAME%"
) else (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%REPO_DIR%tests\coverage\RunWindowsCoverage.ps1" -RepoRoot "%JAM2_REPO_ROOT%" -BuildDirectory "%REPO_DIR%build" -CTestPath "%JAM2_CTEST_PATH%"
)
if errorlevel 1 (
    echo.
    echo INSTRUMENTED COVERAGE GATE FAILED. Restoring the normal Release build next.
    exit /b 1
)

echo INSTRUMENTED COVERAGE GATE SUCCEEDED.
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
