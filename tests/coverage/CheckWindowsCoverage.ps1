param(
    [Parameter(Mandatory = $true)]
    [string]$RepoRoot,

    [Parameter(Mandatory = $true)]
    [string]$CoverageXml,

    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,

    [Parameter(Mandatory = $true)]
    [string]$ReportDirectory,

    [switch]$HardwareProfileConfigured,

    [switch]$MidiInstrumentProfileConfigured,

    [switch]$ReportOnly
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0

$repoPath = (Resolve-Path -LiteralPath $RepoRoot).Path.TrimEnd('\', '/')
$coveragePath = (Resolve-Path -LiteralPath $CoverageXml).Path
$manifestFile = (Resolve-Path -LiteralPath $ManifestPath).Path
$reportPath = [IO.Path]::GetFullPath($ReportDirectory)
[IO.Directory]::CreateDirectory($reportPath) | Out-Null

function Get-RelativeSourcePath([string]$path) {
    if ([string]::IsNullOrWhiteSpace($path)) {
        return $null
    }

    $fullPath = [IO.Path]::GetFullPath($path)
    $prefix = $repoPath + [IO.Path]::DirectorySeparatorChar
    if (-not $fullPath.StartsWith(
            $prefix,
            [StringComparison]::OrdinalIgnoreCase)) {
        return $null
    }

    return $fullPath.Substring($prefix.Length).Replace('\', '/')
}

function Test-Pattern([string]$value, [string]$pattern) {
    return [regex]::IsMatch(
        $value,
        $pattern,
        [Text.RegularExpressions.RegexOptions]::IgnoreCase)
}

function Get-XmlAttribute($node, [string]$name) {
    $attribute = $node.Attributes[$name]
    if ($null -eq $attribute) {
        return ''
    }
    return [string]$attribute.Value
}

function Test-Rule([string]$source, $rule) {
    if ($rule.PSObject.Properties.Name -contains 'path') {
        if (-not $source.Equals(
                [string]$rule.path,
                [StringComparison]::OrdinalIgnoreCase)) {
            return $false
        }
    }
    elseif ($rule.PSObject.Properties.Name -contains 'pathPattern') {
        if (-not (Test-Pattern $source ([string]$rule.pathPattern))) {
            return $false
        }
    }
    else {
        return $false
    }

    return $true
}

function Test-RuleEnabled($rule) {
    if (-not ($rule.PSObject.Properties.Name -contains 'condition')) {
        return $true
    }
    switch ([string]$rule.condition) {
        'without-hardware-profile' { return -not $HardwareProfileConfigured }
        'without-midi-instrument-profile' {
            return -not $MidiInstrumentProfileConfigured
        }
        default { throw "Unknown coverage rule condition: $($rule.condition)" }
    }
}

function Get-FileExemption([string]$source, $rules) {
    foreach ($rule in @($rules)) {
        if ((Test-RuleEnabled $rule) -and (Test-Rule $source $rule)) {
            return $rule
        }
    }
    return $null
}

function Get-FunctionExemption([string]$source, [string]$functionName, $rules) {
    foreach ($rule in @($rules)) {
        if (-not (Test-RuleEnabled $rule)) {
            continue
        }
        $sourceMatches = $true
        if ($rule.PSObject.Properties.Name -contains 'path') {
            $sourceMatches = $source.Equals(
                [string]$rule.path,
                [StringComparison]::OrdinalIgnoreCase)
        }
        elseif ($rule.PSObject.Properties.Name -contains 'pathPattern') {
            $sourceMatches = Test-Pattern $source ([string]$rule.pathPattern)
        }

        if (-not $sourceMatches) {
            continue
        }

        $nameMatches = $false
        if ($rule.PSObject.Properties.Name -contains 'function') {
            $nameMatches = $functionName.Equals(
                [string]$rule.function,
                [StringComparison]::Ordinal)
        }
        elseif ($rule.PSObject.Properties.Name -contains 'functionPattern') {
            $nameMatches = Test-Pattern $functionName ([string]$rule.functionPattern)
        }

        if ($nameMatches) {
            return $rule
        }
    }
    return $null
}

function Export-CsvReport([object[]]$rows, [string]$path) {
    if ($rows.Count -eq 0) {
        [IO.File]::WriteAllText($path, '', [Text.UTF8Encoding]::new($false))
        return
    }
    $rows | Export-Csv -LiteralPath $path -NoTypeInformation -Encoding UTF8
}

$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
if ([int]$manifest.schemaVersion -ne 1) {
    throw "Unsupported coverage manifest schema: $($manifest.schemaVersion)"
}

$allRules = @($manifest.pathExclusions) +
    @($manifest.fileExemptions) +
    @($manifest.functionExemptions)
foreach ($rule in $allRules) {
    if (-not ($rule.PSObject.Properties.Name -contains 'reason') -or
        [string]::IsNullOrWhiteSpace([string]$rule.reason)) {
        throw "Every coverage exclusion or exemption requires a reason."
    }
    if ($rule.PSObject.Properties.Name -contains 'condition') {
        $condition = [string]$rule.condition
        if ($condition -notin @(
                'without-hardware-profile',
                'without-midi-instrument-profile')) {
            throw "Unknown coverage rule condition: $condition"
        }
    }
}

function Test-MaintainedSourcePath([string]$relative) {
    if ([string]::IsNullOrWhiteSpace($relative)) {
        return $false
    }

    $underRoot = $false
    foreach ($sourceRoot in @($manifest.sourceRoots)) {
        $normalizedRoot = ([string]$sourceRoot).Replace('\', '/').TrimEnd('/')
        if ($relative.StartsWith(
                $normalizedRoot + '/',
                [StringComparison]::OrdinalIgnoreCase)) {
            $underRoot = $true
            break
        }
    }
    if (-not $underRoot) {
        return $false
    }

    foreach ($rule in @($manifest.pathExclusions)) {
        if (Test-Rule $relative $rule) {
            return $false
        }
    }
    return $true
}

$extensions = @{}
foreach ($extension in @($manifest.sourceExtensions)) {
    $extensions[[string]$extension.ToLowerInvariant()] = $true
}

$maintainedFiles = @{}
foreach ($sourceRoot in @($manifest.sourceRoots)) {
    $rootPath = Join-Path $repoPath ([string]$sourceRoot)
    foreach ($file in Get-ChildItem -LiteralPath $rootPath -Recurse -File) {
        if (-not $extensions.ContainsKey($file.Extension.ToLowerInvariant())) {
            continue
        }

        $relative = Get-RelativeSourcePath $file.FullName
        if (Test-MaintainedSourcePath $relative) {
            $maintainedFiles[$relative.ToLowerInvariant()] = $relative
        }
    }
}

[xml]$coverage = Get-Content -LiteralPath $coveragePath -Raw
$observedFiles = @{}
$functions = @{}
$skipped = New-Object System.Collections.Generic.List[object]

foreach ($module in @($coverage.SelectNodes('/results/modules/module'))) {
    $sourceById = @{}
    foreach ($sourceFile in @($module.SelectNodes('source_files/source_file'))) {
        $relative = Get-RelativeSourcePath (Get-XmlAttribute $sourceFile 'path')
        if ($null -eq $relative) {
            continue
        }
        $sourceById[(Get-XmlAttribute $sourceFile 'id')] = $relative
        if ($maintainedFiles.ContainsKey($relative.ToLowerInvariant())) {
            $observedFiles[$relative.ToLowerInvariant()] = $relative
        }
    }

    foreach ($function in @($module.SelectNodes('functions/function'))) {
        $qualifiedName = Get-XmlAttribute $function 'name'
        $typeName = Get-XmlAttribute $function 'type_name'
        $namespaceName = Get-XmlAttribute $function 'namespace'
        if (-not [string]::IsNullOrWhiteSpace($typeName)) {
            $qualifiedName = $typeName + '::' + $qualifiedName
        }
        elseif (-not [string]::IsNullOrWhiteSpace($namespaceName)) {
            $qualifiedName = $namespaceName + '::' + $qualifiedName
        }

        $functionSources = @{}
        foreach ($range in @($function.SelectNodes('ranges/range'))) {
            $sourceId = Get-XmlAttribute $range 'source_id'
            if ($sourceById.ContainsKey($sourceId)) {
                $relative = $sourceById[$sourceId]
                # Compiler-emitted inline/template functions can be attributed
                # to maintained headers even though the file-presence inventory
                # intentionally contains implementation units only.
                if (Test-MaintainedSourcePath $relative) {
                    $functionSources[$relative.ToLowerInvariant()] = $relative
                }
            }
        }

        foreach ($relative in $functionSources.Values) {
            $key = $relative.ToLowerInvariant() + '|' + $qualifiedName
            $coveredBlocks = [int](Get-XmlAttribute $function 'blocks_covered')
            $uncoveredBlocks = [int](Get-XmlAttribute $function 'blocks_not_covered')
            if (-not $functions.ContainsKey($key)) {
                $functions[$key] = @{
                    Source = $relative
                    Function = $qualifiedName
                    CoveredBlocks = $coveredBlocks
                    UncoveredBlocks = $uncoveredBlocks
                    Modules = New-Object System.Collections.Generic.List[string]
                }
            }
            else {
                $functions[$key].CoveredBlocks = [Math]::Max(
                    [int]$functions[$key].CoveredBlocks,
                    $coveredBlocks)
                $functions[$key].UncoveredBlocks = [Math]::Min(
                    [int]$functions[$key].UncoveredBlocks,
                    $uncoveredBlocks)
            }
            $functions[$key].Modules.Add((Get-XmlAttribute $module 'name'))
        }
    }

    foreach ($skippedFunction in @($module.SelectNodes('skipped_functions/skipped_function'))) {
        $reason = Get-XmlAttribute $skippedFunction 'reason'
        if ($reason -eq 'source_excluded' -or $reason -eq 'name_excluded') {
            continue
        }
        $name = Get-XmlAttribute $skippedFunction 'name'
        $typeName = Get-XmlAttribute $skippedFunction 'type_name'
        if (-not [string]::IsNullOrWhiteSpace($typeName)) {
            $name = $typeName + '::' + $name
        }
        $skipped.Add([pscustomobject]@{
            Module = Get-XmlAttribute $module 'name'
            Function = $name
            Reason = $reason
        })
    }
}

$missingFiles = New-Object System.Collections.Generic.List[string]
foreach ($entry in $maintainedFiles.GetEnumerator()) {
    if ($observedFiles.ContainsKey($entry.Key)) {
        continue
    }
    if ($null -eq (Get-FileExemption $entry.Value $manifest.fileExemptions)) {
        $missingFiles.Add($entry.Value)
    }
}

$functionRows = New-Object System.Collections.Generic.List[object]
$uncoveredFunctions = New-Object System.Collections.Generic.List[object]
$partialFunctions = New-Object System.Collections.Generic.List[object]
foreach ($record in $functions.Values) {
    $fileExemption = Get-FileExemption $record.Source $manifest.fileExemptions
    $functionExemption = Get-FunctionExemption `
        $record.Source $record.Function $manifest.functionExemptions
    $exemption = if ($null -ne $fileExemption) {
        $fileExemption
    } else {
        $functionExemption
    }

    $status = 'covered'
    if ($null -ne $exemption) {
        $status = 'exempt'
    }
    elseif ([int]$record.CoveredBlocks -eq 0) {
        $status = 'uncovered'
    }
    elseif ([int]$record.UncoveredBlocks -gt 0) {
        $status = 'partial'
    }

    $row = [pscustomobject]@{
        Source = $record.Source
        Function = $record.Function
        Status = $status
        CoveredBlocks = [int]$record.CoveredBlocks
        UncoveredBlocks = [int]$record.UncoveredBlocks
        Modules = (($record.Modules | Sort-Object -Unique) -join ';')
        Exemption = if ($null -ne $exemption) { [string]$exemption.reason } else { '' }
    }
    $functionRows.Add($row)
    if ($status -eq 'uncovered') {
        $uncoveredFunctions.Add($row)
    }
    elseif ($status -eq 'partial') {
        $partialFunctions.Add($row)
    }
}

$unreviewedSkipped = New-Object System.Collections.Generic.List[object]
foreach ($row in $skipped) {
    $exemption = Get-FunctionExemption `
        '<unknown>' $row.Function $manifest.functionExemptions
    if ($null -eq $exemption) {
        $unreviewedSkipped.Add($row)
    }
}

Export-CsvReport `
    @($functionRows | Sort-Object Source, Function) `
    (Join-Path $reportPath 'windows-functions.csv')
Export-CsvReport `
    @($uncoveredFunctions | Sort-Object Source, Function) `
    (Join-Path $reportPath 'windows-uncovered-functions.csv')
Export-CsvReport `
    @($partialFunctions | Sort-Object Source, Function) `
    (Join-Path $reportPath 'windows-partial-functions.csv')
Export-CsvReport `
    @($skipped | Sort-Object Module, Function) `
    (Join-Path $reportPath 'windows-skipped-functions.csv')
$missingFilePath = Join-Path $reportPath 'windows-missing-files.txt'
$missingFileLines = @($missingFiles | Sort-Object)
if ($missingFileLines.Count -eq 0) {
    [IO.File]::WriteAllText(
        $missingFilePath, '', [Text.UTF8Encoding]::new($false))
}
else {
    [IO.File]::WriteAllLines($missingFilePath, [string[]]$missingFileLines)
}

$coveredCount = @($functionRows | Where-Object Status -eq 'covered').Count
$partialCount = $partialFunctions.Count
$exemptCount = @($functionRows | Where-Object Status -eq 'exempt').Count
$summary = @(
    "Hardware profile configured: $([bool]$HardwareProfileConfigured)",
    "MIDI instrument profile configured: $([bool]$MidiInstrumentProfileConfigured)",
    "Maintained source files: $($maintainedFiles.Count)",
    "Observed maintained source files: $($observedFiles.Count)",
    "Unreviewed missing source files: $($missingFiles.Count)",
    "Catalogued maintained functions: $($functionRows.Count)",
    "Fully covered maintained functions: $coveredCount",
    "Partially covered maintained functions: $partialCount",
    "Explicitly exempt maintained functions: $exemptCount",
    "Unreviewed wholly uncovered functions: $($uncoveredFunctions.Count)",
    "Unreviewed collector-skipped functions: $($unreviewedSkipped.Count)"
)
[IO.File]::WriteAllLines(
    (Join-Path $reportPath 'windows-summary.txt'),
    [string[]]$summary)
$summary | ForEach-Object { Write-Host $_ }

if ($functionRows.Count -eq 0) {
    Write-Error "Coverage report contained no maintained Jam2 functions."
    exit 1
}
if ($missingFiles.Count -gt 0 -or
    $uncoveredFunctions.Count -gt 0 -or
    $unreviewedSkipped.Count -gt 0) {
    if ($ReportOnly) {
        Write-Host (
            "Focused coverage report completed with expected catalogue gaps. " +
            "Run compile.cmd --coverage without --test-name for the strict full audit.")
        exit 0
    }
    Write-Error (
        "Coverage has unreviewed gaps. Inspect reports under " + $reportPath)
    exit 1
}

if ($ReportOnly) {
    Write-Host "Focused Windows maintained-source/function coverage report completed."
}
else {
    Write-Host "Windows maintained-source/function coverage gate passed."
}
exit 0
