param(
    [Parameter(Mandatory = $true)][string]$ToolsetDirectory,
    [string]$InstalledDirectory = "$env:SystemRoot/System32",
    [version]$MinimumVersion = '14.40.0.0'
)
$ErrorActionPreference = 'Stop'

# ASIO drivers share the host's already-loaded runtime. In particular, drivers
# built with VS 17.10's constexpr mutex cannot use the old 14.36 runtime, even
# when Jam2 itself was built with that older compiler.
$required = @('msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'concrt140.dll')
$selected = $null
$selectedVersion = [version]'0.0.0.0'
foreach ($directory in @($ToolsetDirectory, $InstalledDirectory)) {
    $files = @()
    $setVersion = $null
    $names = $required
    # Newer runtimes may split additional support into this DLL.
    if (Test-Path -LiteralPath (Join-Path $directory 'vcruntime140_threads.dll') -PathType Leaf) {
        $names += 'vcruntime140_threads.dll'
    }
    foreach ($name in $names) {
        $path = Join-Path $directory $name
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { break }
        $info = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        $version = [version]::new($info.FileMajorPart, $info.FileMinorPart,
            $info.FileBuildPart, $info.FilePrivatePart)
        if ($version -lt $MinimumVersion -or ($setVersion -and $version -ne $setVersion)) { break }
        # Do not select an x86 installation from a redirected System32 path.
        $bytes = [IO.File]::ReadAllBytes($path)
        if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { break }
        $pe = [BitConverter]::ToInt32($bytes, 0x3c)
        if ($pe -lt 0 -or $pe -gt $bytes.Length - 6 -or
            [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
            [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664) { break }
        $setVersion = $version
        $files += [IO.Path]::GetFullPath($path)
    }
    if ($files.Count -eq $names.Count -and $setVersion -gt $selectedVersion) {
        $selected = $files
        $selectedVersion = $setVersion
    }
}
if (-not $selected) {
    throw "A complete x64 Visual C++ runtime >= $MinimumVersion is required. Install the latest Microsoft x64 Visual C++ Redistributable (https://aka.ms/vc14/vc_redist.x64.exe), then rerun compile.cmd."
}
ConvertTo-Json -InputObject @($selected) -Compress
