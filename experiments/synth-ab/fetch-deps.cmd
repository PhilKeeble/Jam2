@echo off
setlocal EnableExtensions
set "HERE=%~dp0"

if not exist "%HERE%.deps" mkdir "%HERE%.deps"

if not exist "%HERE%.deps\DaisySP\.git" (
    git clone https://github.com/electro-smith/DaisySP.git "%HERE%.deps\DaisySP"
    if errorlevel 1 exit /b 1
)
git -C "%HERE%.deps\DaisySP" checkout --detach 599511b740f8f3a9b8db72a0642aa45b8a23c3a3
if errorlevel 1 exit /b 1

echo Dependencies are pinned and ready.
