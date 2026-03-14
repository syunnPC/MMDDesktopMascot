param(
    [string]$Tag = "3.25"
)

$submodulePath = Join-Path $PSScriptRoot "external\\bullet3"
$submoduleParentPath = Split-Path $submodulePath -Parent
$safeDirectory = $PSScriptRoot.Replace('\', '/')
$bulletRepoUrl = "https://github.com/bulletphysics/bullet3.git"

function Invoke-CheckedGit {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments
    )

    & git "-c" "safe.directory=$safeDirectory" @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "git command failed: git $($Arguments -join ' ')"
    }
}

function Test-GitSubmoduleRegistered {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $output = & git "-c" "safe.directory=$safeDirectory" "ls-tree" "HEAD" $Path
    return $LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace(($output | Out-String))
}

if (!(Test-Path (Join-Path $PSScriptRoot ".gitmodules"))) {
    throw ".gitmodules was not found. Run this script from the repository root."
}

if (!(Test-Path $submoduleParentPath)) {
    New-Item -ItemType Directory -Path $submoduleParentPath | Out-Null
}

$submoduleRegistered = Test-GitSubmoduleRegistered -Path "external/bullet3"

if ($submoduleRegistered) {
    Write-Host "Initializing bullet3 submodule"
    Invoke-CheckedGit @("submodule", "update", "--init", "--recursive", "external/bullet3")
} elseif (Test-Path (Join-Path $submodulePath ".git")) {
    Write-Host "Using existing bullet3 clone at $submodulePath"
} else {
    if (Test-Path $submodulePath) {
        throw "Found $submodulePath, but it is not a git repository. Remove it or convert it into a bullet3 clone."
    }

    Write-Host "Submodule is not registered in Git history. Cloning bullet3 into $submodulePath"
    Invoke-CheckedGit @("clone", "--recursive", $bulletRepoUrl, $submodulePath)
}

Write-Host "Switching bullet3 to tag $Tag"
Invoke-CheckedGit @("-C", $submodulePath, "fetch", "--tags", "origin")
Invoke-CheckedGit @("-C", $submodulePath, "checkout", $Tag)

Write-Host "Bullet source is ready: $submodulePath"
Write-Host "Note: commit the updated submodule pointer if you changed the tag."
