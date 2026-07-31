@echo off
setlocal EnableExtensions
set "HERE=%~dp0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if not exist "%HERE%.deps\DaisySP\.git" call "%HERE%fetch-deps.cmd"
if errorlevel 1 exit /b 1

if not exist "%VSWHERE%" (
    echo Visual Studio vswhere.exe was not found.
    exit /b 1
)
for /f "usebackq tokens=*" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL (
    echo No Visual Studio C++ installation was found.
    exit /b 1
)
call "%VS_INSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
if errorlevel 1 exit /b 1

if not defined QT_DIR (
    for /f "delims=" %%V in ('dir /b /ad /o-n "%SystemDrive%\Qt\6.*" 2^>nul') do if not defined QT_DIR if exist "%SystemDrive%\Qt\%%V\msvc2022_64\lib\cmake\Qt6\Qt6Config.cmake" set "QT_DIR=%SystemDrive%\Qt\%%V\msvc2022_64"
)
if not defined QT_DIR (
    echo Set QT_DIR to a Qt 6 MSVC x64 kit.
    exit /b 1
)

cmake -S "%HERE%." -B "%HERE%build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_DIR%"
if errorlevel 1 exit /b 1
cmake --build "%HERE%build"
if errorlevel 1 exit /b 1

set "PATH=%QT_DIR%\bin;%PATH%"
"%HERE%build\jam2_sound_lab.exe" "%HERE%site" %*
if not "%ERRORLEVEL%"=="0" exit /b 1

echo.
echo Jam2 sound-design workbench:
echo   %HERE%open-workbench.cmd
echo DaisySP showcase:
echo   %HERE%site\showcase.html
exit /b 0
