@echo off
setlocal EnableExtensions
set "HERE=%~dp0"
set "PORT=48727"

if not exist "%HERE%build\jam2_sound_lab.exe" (
    echo The native sound renderer has not been built.
    echo Run build.cmd before opening the workbench.
    pause
    exit /b 1
)

if not defined QT_DIR (
    for /f "delims=" %%V in ('dir /b /ad /o-n "%SystemDrive%\Qt\6.*" 2^>nul') do if not defined QT_DIR if exist "%SystemDrive%\Qt\%%V\msvc2022_64\bin\Qt6Core.dll" set "QT_DIR=%SystemDrive%\Qt\%%V\msvc2022_64"
)
if not defined QT_DIR (
    echo Set QT_DIR to a Qt 6 MSVC x64 kit.
    pause
    exit /b 1
)

where py >nul 2>nul
if not errorlevel 1 (
    for /f "usebackq delims=" %%I in (`py -3 -c "import sys; print(sys.executable)"`) do set "PYTHON_EXE=%%I"
) else (
    where python >nul 2>nul
    if errorlevel 1 (
        echo Python 3 is required to run the local sound workbench.
        echo The generated WAV files are present, but browsers will not pass
        echo file:// audio through Web Audio reliably.
        pause
        exit /b 1
    )
    for /f "usebackq delims=" %%I in (`python -c "import sys; print(sys.executable)"`) do set "PYTHON_EXE=%%I"
)
if not defined PYTHON_EXE (
    echo Python 3 could not be resolved to an executable.
    pause
    exit /b 1
)

start "Jam2 Sound Workbench Server" /D "%HERE%" "%PYTHON_EXE%" tools\workbench_server.py --site site --renderer build\jam2_sound_lab.exe --qt-bin "%QT_DIR%\bin" --port %PORT%
powershell.exe -NoProfile -Command "Start-Sleep -Milliseconds 800"
start "" "http://127.0.0.1:%PORT%/index.html"

echo Jam2 sound workbench opened at:
echo   http://127.0.0.1:%PORT%/index.html
echo.
echo Close the separate server window when you finish listening.
exit /b 0
