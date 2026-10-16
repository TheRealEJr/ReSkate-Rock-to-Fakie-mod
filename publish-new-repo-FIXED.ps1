# Publish this source-only mod as a NEW GitHub repository owned by TheRealEJR.
# Requires Git + authenticated GitHub CLI. Never updates an existing GitHub repo.
[CmdletBinding()]
param(
    [ValidateSet('Public','Private')]
    [string]$Visibility = 'Private'
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$owner = 'TheRealEJR'
$slug = 'ReSkate-Rock-to-Fakie-mod'
$fullName = "$owner/$slug"
$root = $PSScriptRoot
Push-Location -LiteralPath $root
try {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { throw 'Git is not installed.' }
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { throw 'GitHub CLI (gh) is not installed. See https://cli.github.com/' }
    & gh auth status
    if ($LASTEXITCODE -ne 0) { throw 'GitHub CLI is not logged in. Run gh auth login.' }
    $login = (& gh api user --jq '.login' 2>$null).Trim()
    if ($LASTEXITCODE -ne 0 -or $login -ne $owner) {
        throw "GitHub CLI is authenticated as '$login', expected '$owner'. Run gh auth switch or gh auth login."
    }
    # Do not query a repository that does not exist: PowerShell 5.1 treats gh stderr as a terminating error.
    # `gh repo create` below refuses existing names, so this preflight check is unnecessary.
    if (Test-Path -LiteralPath (Join-Path $root '.git')) {
        throw 'This folder already has a .git repository. Verify the target manually rather than rerunning the first-publish script.'
    }
    & git init -b main
    if ($LASTEXITCODE -ne 0) { throw 'git init failed.' }
    & git add --all
    if ($LASTEXITCODE -ne 0) { throw 'git add failed.' }
    & git commit -m 'Initial release: persistent Rock to Fakie ticker mod v1.4.42'
    if ($LASTEXITCODE -ne 0) { throw 'Git commit failed. Configure git user.name and user.email, then commit manually.' }
    $flag = if ($Visibility -eq 'Public') { '--public' } else { '--private' }
    & gh repo create $fullName $flag --source '.' --remote origin --push --description 'Rock to Fakie trick ticker mod for skate. using ReSkate, with backup-safe install and uninstall'
    if ($LASTEXITCODE -ne 0) { throw 'Repository creation/push failed; inspect git status and gh output before retrying.' }
    Write-Host "Published: https://github.com/$fullName"
} finally {
    Pop-Location
}
