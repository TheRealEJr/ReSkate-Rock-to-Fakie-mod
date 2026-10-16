# Run after build-release.bat. Creates one distributable ZIP beside RockToFakie.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$dll = Join-Path $root 'payload\ReSkate.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
    throw 'Missing payload\ReSkate.dll. Run build-release.bat first.'
}
$destination = Join-Path $root 'RockToFakie-v1.4.42-Windows-x64.zip'
$temp = Join-Path ([System.IO.Path]::GetTempPath()) ('RockToFakie_release_' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $temp 'RockToFakie'
try {
    New-Item -ItemType Directory -Path (Join-Path $stage 'scripts') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $stage 'payload') -Force | Out-Null
    foreach ($file in @('install.bat','uninstall.bat','README.md')) {
        Copy-Item -LiteralPath (Join-Path $root $file) -Destination (Join-Path $stage $file)
    }
    Copy-Item -LiteralPath (Join-Path $root 'scripts\manage.ps1') -Destination (Join-Path $stage 'scripts\manage.ps1')
    Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'payload\ReSkate.dll')
    Compress-Archive -Path $stage -DestinationPath $destination -Force
    Write-Host "Release created: $destination"
    Write-Host ('DLL SHA-256: ' + (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash)
} finally {
    if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Recurse -Force }
}
