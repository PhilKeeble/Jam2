param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$BuildDirectory,

    [string]$TestName = '',

    [string]$CTestPath = ''
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$repoPath = (Resolve-Path -LiteralPath $RepoRoot).Path
$buildPath = (Resolve-Path -LiteralPath $BuildDirectory).Path
$coveragePath = Join-Path $buildPath 'coverage'
[IO.Directory]::CreateDirectory($coveragePath) | Out-Null
$toolsObjectPath = Join-Path $coveragePath 'tools-obj'
[IO.Directory]::CreateDirectory($toolsObjectPath) | Out-Null

$toolsProject = Join-Path $repoPath 'tests\coverage\WindowsCoverageTools.csproj'
Write-Host "Restoring the pinned Microsoft native coverage tool..."
$toolsObjectArgument = $toolsObjectPath.TrimEnd('\', '/') + `
    [IO.Path]::DirectorySeparatorChar
& dotnet restore $toolsProject --nologo --verbosity minimal `
    ('-p:BaseIntermediateOutputPath=' + $toolsObjectArgument) `
    ('-p:MSBuildProjectExtensionsPath=' + $toolsObjectArgument)
if ($LASTEXITCODE -ne 0) {
    throw "Microsoft.CodeCoverage restore failed with exit code $LASTEXITCODE."
}

$assetsPath = Join-Path $toolsObjectPath 'project.assets.json'
if (-not (Test-Path -LiteralPath $assetsPath -PathType Leaf)) {
    throw "Coverage-tool restore metadata was not written at $assetsPath"
}
$assets = Get-Content -LiteralPath $assetsPath -Raw | ConvertFrom-Json
$collector = $null
foreach ($packageRoot in @($assets.packageFolders.PSObject.Properties.Name)) {
    $candidate = Join-Path $packageRoot `
        'microsoft.codecoverage\17.3.0\build\netstandard1.0\CodeCoverage\amd64\CodeCoverage.exe'
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        $collector = $candidate
        break
    }
}
if ($null -eq $collector -or
    -not (Test-Path -LiteralPath $collector -PathType Leaf)) {
    throw "Pinned Microsoft.CodeCoverage collector was absent from all restored package folders."
}

$config = Join-Path $repoPath 'tests\coverage\WindowsCodeCoverage.config'
$manifest = Join-Path $repoPath 'tests\coverage\CoverageManifest.json'
$cachePath = Join-Path $buildPath 'CMakeCache.txt'
$hardwareProfileConfigured = $false
$midiInstrumentProfileConfigured = $false
$profileEntry = Get-Content -LiteralPath $cachePath |
    Where-Object { $_ -like 'JAM2_HARDWARE_PROFILE:FILEPATH=*' } |
    Select-Object -First 1
if ($null -ne $profileEntry) {
    $profilePath = $profileEntry.Substring($profileEntry.IndexOf('=') + 1).Trim()
    if (-not [string]::IsNullOrWhiteSpace($profilePath) -and
        (Test-Path -LiteralPath $profilePath -PathType Leaf)) {
        $hardwareProfileConfigured = $true
        $profile = Get-Content -LiteralPath $profilePath -Raw | ConvertFrom-Json
        $midiInstrumentProfileConfigured =
            [string]$profile.schema -eq 'jam2-hardware-profile-v2'
    }
}
Write-Host (
    "Coverage profile: hardware=$hardwareProfileConfigured " +
    "midi-instrument=$midiInstrumentProfileConfigured")
$focused = -not [string]::IsNullOrWhiteSpace($TestName)
$reportPath = if ($focused) {
    Join-Path $coveragePath 'focused'
} else {
    $coveragePath
}
[IO.Directory]::CreateDirectory($reportPath) | Out-Null
$coverageStem = if ($focused) { 'windows-focused' } else { 'windows-full' }
$rawCoverage = Join-Path $reportPath ($coverageStem + '.coverage')
$xmlCoverage = Join-Path $reportPath ($coverageStem + '.xml')
$ctestLogName = if ($focused) {
    'windows-focused-instrumented-ctest.log'
} else {
    'windows-instrumented-ctest.log'
}
$ctestExitName = if ($focused) {
    'windows-focused-instrumented-ctest-exit.txt'
} else {
    'windows-instrumented-ctest-exit.txt'
}
$ctestLog = Join-Path $reportPath $ctestLogName
$ctestExitReport = Join-Path $reportPath $ctestExitName

foreach ($generatedFile in @($rawCoverage, $xmlCoverage, $ctestLog, $ctestExitReport)) {
    $candidate = [IO.Path]::GetFullPath($generatedFile)
    $prefix = $reportPath.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $candidate.StartsWith(
            $prefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to replace coverage output outside $reportPath"
    }
    if (Test-Path -LiteralPath $candidate -PathType Leaf) {
        Remove-Item -LiteralPath $candidate -Force
    }
}

if ([string]::IsNullOrWhiteSpace($CTestPath)) {
    $ctestCommand = Get-Command ctest.exe -ErrorAction SilentlyContinue
    if ($null -ne $ctestCommand) {
        $CTestPath = $ctestCommand.Source
    }
    else {
        $cachePath = Join-Path $buildPath 'CMakeCache.txt'
        $cmakeEntry = Get-Content -LiteralPath $cachePath |
            Where-Object { $_ -like 'CMAKE_COMMAND:INTERNAL=*' } |
            Select-Object -First 1
        if ($null -ne $cmakeEntry) {
            $cmakePath = $cmakeEntry.Substring($cmakeEntry.IndexOf('=') + 1)
            $CTestPath = Join-Path ([IO.Path]::GetDirectoryName($cmakePath)) 'ctest.exe'
        }
    }
}
if (-not (Test-Path -LiteralPath $CTestPath -PathType Leaf)) {
    throw "CTest was not found. Pass -CTestPath or run through compile.cmd --tests-full."
}
$ctest = (Resolve-Path -LiteralPath $CTestPath).Path
$ctestArguments = @(
    '--test-dir', $buildPath,
    '--output-on-failure',
    '--output-log', $ctestLog)
# Native instrumentation adds one collector to every process and makes two
# four-peer workflows contend inside the same coverage session even on a
# high-core host. Keep CTest parallel, but cap this Windows-only instrumented
# capacity at one four-peer workload. The normal Release gate retains its
# default capacity of eight and all test assertions remain identical.
$parallelLevel = [Math]::Max(1, [Math]::Min(4, [Environment]::ProcessorCount - 1))
if (-not [string]::IsNullOrWhiteSpace($env:JAM2_TEST_JOBS)) {
    $parsedParallelLevel = 0
    if (-not [int]::TryParse($env:JAM2_TEST_JOBS, [ref]$parsedParallelLevel) -or
        $parsedParallelLevel -lt 1) {
        throw 'JAM2_TEST_JOBS must be a positive integer.'
    }
    $parallelLevel = $parsedParallelLevel
}
$ctestArguments += @('--parallel', $parallelLevel.ToString())
if (-not [string]::IsNullOrWhiteSpace($TestName)) {
    $ctestArguments += @('--no-tests=error', '-R', ('^' + [regex]::Escape($TestName) + '$'))
}
else {
    # The reactive session-command coordinator intentionally launches Jam2 with
    # an exact Windows inherited-handle allowlist. Microsoft.CodeCoverage's
    # injected child-process handles cannot pass that boundary, so the
    # instrumented child never receives its automation pipes. Instrumentation
    # also makes the burst-loss leader-audio callback path too slow to preserve
    # real-time tone continuity. The normal optimized Release gate below still
    # requires both exact four-peer behavioral tests.
    $ctestArguments += @(
        '-E',
        '^(jam2_four_session_command_integration|jam2_metronome_leader_audio_burst_loss)$')
}

Write-Host "Running the instrumented CTest catalogue under native coverage..."
& $collector collect `
    ('/config:' + $config) `
    ('/output:' + $rawCoverage) `
    $ctest @ctestArguments
$testExit = $LASTEXITCODE
[IO.File]::WriteAllText(
    $ctestExitReport,
    "instrumented_ctest_exit=$testExit$([Environment]::NewLine)",
    [Text.UTF8Encoding]::new($false))
if ($testExit -ne 0) {
    Write-Warning (
        "Instrumented CTest returned $testExit. Coverage export and the " +
        "maintained-function audit will continue for diagnostics, but the full " +
        "native gate will fail.")
}

Write-Host "Exporting native coverage XML..."
& $collector analyze `
    /include_skipped_functions `
    /include_skipped_modules `
    ('/output:' + $xmlCoverage) `
    $rawCoverage
$analysisExit = $LASTEXITCODE
if ($analysisExit -ne 0) {
    Write-Error "Coverage XML export failed with exit code $analysisExit."
    exit $analysisExit
}

& (Join-Path $repoPath 'tests\coverage\CheckWindowsCoverage.ps1') `
    -RepoRoot $repoPath `
    -CoverageXml $xmlCoverage `
    -ManifestPath $manifest `
    -ReportDirectory $reportPath `
    -HardwareProfileConfigured:$hardwareProfileConfigured `
    -MidiInstrumentProfileConfigured:$midiInstrumentProfileConfigured
$coverageAuditExit = $LASTEXITCODE
if ($testExit -ne 0) {
    exit $testExit
}
exit $coverageAuditExit
