$ErrorActionPreference = 'Stop'
$repo = $PSScriptRoot
$version = ''
$includeModels = 'OFF'
$publish = $false

try {
    for ($i = 0; $i -lt $args.Count; ++$i) {
        switch -CaseSensitive ($args[$i]) {
            '--version' {
                if (++$i -ge $args.Count) { throw '--version requires major.minor.patch.' }
                $version = $args[$i]
                if ($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or
                    @($version.Split('.') | Where-Object { $_.Length -gt 5 -or [int]$_ -gt 65535 }).Count) {
                    throw 'Version must be major.minor.patch, with components between 0 and 65535 and no leading zeroes.'
                }
            }
            '--include-models' { $includeModels = 'ON' }
            '--publish' { $publish = $true }
            '--help' {
                Write-Host 'Usage: .\build-release.ps1 [--version 0.8.1] [--include-models] [--publish]'
                Write-Host 'Builds Windows x64, writes dist/Jam2-windows.zip, and optionally a separate Jam2-models.zip.'
                Write-Host '--publish requires an explicit version and GitHub CLI authentication (gh auth login).'
                exit 0
            }
            default { throw "Unknown release option: $($args[$i])" }
        }
    }
    if ($publish) {
        if (-not $version) { throw '--publish requires an explicit --version.' }
        if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
            throw 'Publishing requires GitHub CLI. Install gh, then run: gh auth login'
        }
        & gh auth status --hostname github.com
        if ($LASTEXITCODE -ne 0) { throw 'GitHub authentication failed. Run: gh auth login' }
    }
    $previousVersion = $env:JAM2_RELEASE_VERSION
    try {
        $env:JAM2_RELEASE_VERSION = $version
        Push-Location -LiteralPath $repo
        try {
            & cmd.exe /d /c 'call compile.cmd --in-dev-shell'
            if ($LASTEXITCODE -ne 0) { throw "Release compilation failed (exit $LASTEXITCODE)." }
            $cache = Get-Content -LiteralPath (Join-Path $repo 'build/CMakeCache.txt')
            $cmakeLine = @($cache | Where-Object { $_ -match '^CMAKE_COMMAND:INTERNAL=' })
            if ($cmakeLine.Count -ne 1) { throw 'Could not locate CMake in the completed build cache.' }
            $cmake = $cmakeLine[0].Substring('CMAKE_COMMAND:INTERNAL='.Length)
            & $cmake "-DJAM2_PACKAGE_CONFIG=$repo/build/release-package.cmake" `
                "-DJAM2_INCLUDE_MODELS=$includeModels" -P "$repo/tools/release/PackageRelease.cmake"
            if ($LASTEXITCODE -ne 0) { throw "Release packaging failed (exit $LASTEXITCODE)." }
            if ($publish) {
                & $cmake "-DJAM2_PACKAGE_CONFIG=$repo/build/release-package.cmake" `
                    "-DJAM2_INCLUDE_MODELS=$includeModels" -P "$repo/tools/release/PublishRelease.cmake"
                if ($LASTEXITCODE -ne 0) { throw "Release publishing failed (exit $LASTEXITCODE)." }
            }
        }
        finally { Pop-Location }
    }
    finally { $env:JAM2_RELEASE_VERSION = $previousVersion }
}
catch {
    Write-Error $_.Exception.Message -ErrorAction Continue
    exit 1
}
