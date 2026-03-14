param(
    [string]$Tag = "3.25"
)

$submodulePath = Join-Path $PSScriptRoot "external\\bullet3"
$safeDirectory = $PSScriptRoot.Replace('\', '/')

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

if (!(Test-Path (Join-Path $PSScriptRoot ".gitmodules"))) {
    throw ".gitmodules was not found. Run this script from the repository root."
}

Write-Host "Initializing bullet3 submodule"
Invoke-CheckedGit @("submodule", "update", "--init", "--recursive", "external/bullet3")

Write-Host "Switching bullet3 to tag $Tag"
Invoke-CheckedGit @("-C", $submodulePath, "fetch", "--tags", "origin")
Invoke-CheckedGit @("-C", $submodulePath, "checkout", $Tag)

Write-Host "Bullet source is ready: $submodulePath"
Write-Host "Note: commit the updated submodule pointer if you changed the tag."
