$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoDir = $PSScriptRoot

function Invoke-Checked {
    param(
        [Parameter(Mandatory)]
        [string] $FilePath,

        [Parameter(Mandatory)]
        [string[]] $ArgumentList
    )

    & $FilePath @ArgumentList
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath $($ArgumentList -join ' ')"
    }
}

function Update-ProcessPath {
    $machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    $knownGitPaths = @(
        (Join-Path $env:ProgramFiles 'Git\cmd')
        (Join-Path $env:LOCALAPPDATA 'Programs\Git\cmd')
    )
    $env:Path = (@($knownGitPaths) + @($machinePath, $userPath, $env:Path)) -join ';'
}

function Install-GitForWindows {
    param(
        [switch] $Force
    )

    if (-not (Get-Command winget.exe -ErrorAction SilentlyContinue)) {
        throw @'
Git or Git LFS is missing, and winget is unavailable.
Install Git for Windows from https://git-scm.com/download/win, including its
Git LFS component, then run update.ps1 again.
'@
    }

    Write-Host ''
    Write-Host 'Installing Git for Windows (which includes Git LFS)...'
    $arguments = @(
        'install', '--id', 'Git.Git', '--exact', '--source', 'winget',
        '--accept-package-agreements', '--accept-source-agreements'
    )
    if ($Force) {
        $arguments += '--force'
    }
    Invoke-Checked -FilePath 'winget.exe' -ArgumentList $arguments
    Update-ProcessPath
}

try {
    Write-Host ''
    Write-Host 'Checking Jam2 update prerequisites...'

    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
        Install-GitForWindows
    }
    if (-not (Get-Command git.exe -ErrorAction SilentlyContinue)) {
        throw 'Git was installed, but it is not available in this terminal. Open a new PowerShell window and run update.ps1 again.'
    }

    & git.exe lfs version
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'Git LFS is missing from the current Git installation; repairing Git for Windows...'
        Install-GitForWindows -Force
        & git.exe lfs version
        if ($LASTEXITCODE -ne 0) {
            throw 'Git LFS is still unavailable. Repair Git for Windows and enable its Git LFS component.'
        }
    }

    Invoke-Checked -FilePath 'git.exe' -ArgumentList @('-C', $repoDir, 'rev-parse', '--is-inside-work-tree')

    Write-Host ''
    Write-Host 'Configuring Git LFS...'
    Invoke-Checked -FilePath 'git.exe' -ArgumentList @('-C', $repoDir, 'lfs', 'install')

    Write-Host ''
    Write-Host 'Updating the current branch...'
    Invoke-Checked -FilePath 'git.exe' -ArgumentList @('-C', $repoDir, 'pull', '--ff-only')

    Write-Host ''
    Write-Host 'Downloading Git LFS files...'
    Invoke-Checked -FilePath 'git.exe' -ArgumentList @('-C', $repoDir, 'lfs', 'pull')
    Invoke-Checked -FilePath 'git.exe' -ArgumentList @('-C', $repoDir, 'lfs', 'fsck')

    $lfsFiles = & git.exe -C $repoDir lfs ls-files
    if ($LASTEXITCODE -ne 0) {
        throw 'Could not inspect the Git LFS working tree.'
    }
    $pointerFiles = @($lfsFiles | Where-Object { $_ -match '^[0-9a-f]+\s+-\s+' })
    if ($pointerFiles.Count -ne 0) {
        throw "Some Git LFS files are still pointers rather than downloaded content:`n$($pointerFiles -join "`n")"
    }

    Write-Host ''
    Write-Host 'Repository status:'
    Invoke-Checked -FilePath 'git.exe' -ArgumentList @('-C', $repoDir, 'status', '--short', '--branch')

    Write-Host ''
    Write-Host 'UPDATE SUCCEEDED. Jam2 is ready to compile with .\compile.cmd'
    exit 0
}
catch {
    Write-Host ''
    Write-Error $_.Exception.Message -ErrorAction Continue
    Write-Host 'UPDATE FAILED. Existing local changes have not been discarded.'
    exit 1
}
