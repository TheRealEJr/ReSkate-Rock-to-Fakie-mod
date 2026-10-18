# Rock to Fakie installer - release v2.0.0 (ReSkate 2.1.0). Requires Windows PowerShell 5.1+.
# Only manages ReSkate.dll beside Skate.exe. All backup and manifest files
# live under the game directory; the original DLL is never changed in place.
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)]
    [ValidateSet('Install','Uninstall')]
    [string]$Action,
    [string]$GameDir
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$releaseRoot = Split-Path $PSScriptRoot -Parent
$lastPath = Join-Path $releaseRoot 'last-install-path.txt'

function Get-Sha([string]$path) {
    return (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToUpperInvariant()
}
function Ensure-NotLink([string]$path) {
    if (Test-Path -LiteralPath $path) {
        $item = Get-Item -LiteralPath $path -Force
        if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Refusing symbolic link or reparse point: $path"
        }
    }
}
function Resolve-GameFolder() {
    $candidate = $GameDir
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        if (Test-Path -LiteralPath $lastPath -PathType Leaf) {
            $saved = (Get-Content -LiteralPath $lastPath -Raw).Trim()
            if ($saved -and (Test-Path -LiteralPath (Join-Path $saved 'Skate.exe') -PathType Leaf)) {
                $candidate = $saved
                Write-Host "Using previous game folder: $candidate"
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = Read-Host 'Full path to folder containing Skate.exe'
    }
    $candidate = $candidate.Trim().Trim('"')
    if (-not (Test-Path -LiteralPath $candidate -PathType Container)) {
        throw "Game folder does not exist: $candidate"
    }
    $folder = (Resolve-Path -LiteralPath $candidate).ProviderPath
    if (-not (Test-Path -LiteralPath (Join-Path $folder 'Skate.exe') -PathType Leaf)) {
        throw "Skate.exe not found inside $folder. Choose the game root directory."
    }
    Ensure-NotLink (Join-Path $folder 'Skate.exe')
    return $folder
}
function Require-ClosedGame() {
    if (Get-Process -Name 'Skate' -ErrorAction SilentlyContinue) {
        throw 'Skate.exe is still running. Close the game before installing or uninstalling.'
    }
}

try {
    Require-ClosedGame
    $folder = Resolve-GameFolder
    $target = Join-Path $folder 'ReSkate.dll'
    $backupDir = Join-Path $folder 'RockToFakie_Backup'
    $backup = Join-Path $backupDir 'ReSkate.dll.original'
    $manifestFile = Join-Path $backupDir 'installation.json'
    Ensure-NotLink $target
    Ensure-NotLink $backupDir
    Ensure-NotLink $backup
    Ensure-NotLink $manifestFile
    Write-Host "Game folder: $folder"

    if ($Action -eq 'Install') {
        $source = Join-Path $releaseRoot 'payload\ReSkate.dll'
        if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
            throw 'payload\ReSkate.dll is missing. Build the v2.0.0 DLL (ReSkate 2.1.0 + Rock to Fakie) or download a compiled release. No changes made.'
        }
        $bytes = [System.IO.File]::ReadAllBytes($source)
        if ($bytes.Length -lt 1048576 -or $bytes[0] -ne 0x4D -or $bytes[1] -ne 0x5A) {
            throw 'The supplied DLL is missing a valid PE (MZ) header or is unexpectedly small. No changes made.'
        }
        $newHash = Get-Sha $source
        if (Test-Path -LiteralPath $backupDir) {
            if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf)) {
                throw "An existing backup directory has no installation manifest. Refusing to overwrite: $backupDir"
            }
            $saved = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
            if ([string]$saved.InstalledSha256 -eq $newHash -and
                (Test-Path -LiteralPath $target -PathType Leaf) -and
                (Get-Sha $target) -eq $newHash) {
                Write-Host 'Already installed. Existing original backup is preserved.'
                exit 0
            }
            # Upgrade: an earlier Rock to Fakie build is still installed, unchanged since this
            # installer put it there. Replace it and keep the ORIGINAL backup and its hash.
            if ((Test-Path -LiteralPath $target -PathType Leaf) -and
                (Get-Sha $target) -eq [string]$saved.InstalledSha256) {
                $staging = "$target.r2f-staging"
                Ensure-NotLink $staging
                Copy-Item -LiteralPath $source -Destination $staging -Force -ErrorAction Stop
                if ((Get-Sha $staging) -ne $newHash) {
                    Remove-Item -LiteralPath $staging -Force -ErrorAction SilentlyContinue
                    throw 'The new DLL failed SHA-256 verification while staging. Nothing was changed.'
                }
                Move-Item -LiteralPath $staging -Destination $target -Force -ErrorAction Stop
                if ((Get-Sha $target) -ne $newHash) {
                    throw "Upgraded DLL failed SHA-256 verification. Your original backup is still in $backupDir."
                }
                $previousVersion = [string]$saved.Version
                $saved.InstalledSha256 = $newHash
                $saved.Version = 'v2.0.0'
                $saved | ConvertTo-Json | Set-Content -LiteralPath $manifestFile -Encoding UTF8
                Write-Host "Upgraded Rock to Fakie $previousVersion -> v2.0.0. Original backup preserved: $backupDir"
                Write-Host 'Launch Skate, open the ReSkate console, and enter: rocktofakie on'
                exit 0
            }
            throw "An installation or backup already exists at $backupDir. The installed ReSkate.dll was changed after the last install (for example by a ReSkate update). Run uninstall.bat first, or move that folder aside."
        }
        $existed = Test-Path -LiteralPath $target -PathType Leaf
        $originalHash = $null
        $originalBackedUp = $false
        New-Item -ItemType Directory -Path $backupDir | Out-Null
        try {
            if ($existed) {
                $originalHash = Get-Sha $target
                Copy-Item -LiteralPath $target -Destination $backup -ErrorAction Stop
                if ((Get-Sha $backup) -ne $originalHash) {
                    throw 'The original DLL backup failed SHA-256 verification.'
                }
                $originalBackedUp = $true
                Write-Host "Saved original DLL: $backup"
            } else {
                Write-Host 'No existing ReSkate.dll; uninstall will remove only the DLL installed by this mod.'
            }
            $record = [ordered]@{
                Schema = 1
                Version = 'v2.0.0'
                OriginalExisted = [bool]$existed
                OriginalSha256 = $originalHash
                InstalledSha256 = $newHash
                CreatedLocal = (Get-Date).ToString('o')
            }
            $record | ConvertTo-Json | Set-Content -LiteralPath $manifestFile -Encoding UTF8
            Copy-Item -LiteralPath $source -Destination $target -Force -ErrorAction Stop
            if ((Get-Sha $target) -ne $newHash) {
                throw 'Installed DLL failed SHA-256 verification.'
            }
            Write-Host 'Installed Rock to Fakie v2.0.0 ReSkate.dll successfully.'
            Write-Host "Original backup (do not delete): $backupDir"
            try { Set-Content -LiteralPath $lastPath -Value $folder -Encoding UTF8 } catch {
                Write-Warning 'Could not remember game folder. You can pass -GameDir to uninstall.bat.'
            }
            Write-Host 'Launch Skate, open the ReSkate console, and enter: rocktofakie on'
        } catch {
            $reason = $_
            Write-Warning "Installation failed: $reason. Attempting automatic rollback."
            $restored = $false
            try {
                if ($existed) {
                    if ($originalBackedUp) {
                        Copy-Item -LiteralPath $backup -Destination $target -Force -ErrorAction Stop
                        $restored = (Get-Sha $target) -eq $originalHash
                    } else {
                        $restored = (Test-Path -LiteralPath $target -PathType Leaf) -and ((Get-Sha $target) -eq $originalHash)
                    }
                } else {
                    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force -ErrorAction Stop }
                    $restored = -not (Test-Path -LiteralPath $target)
                }
                if ($restored) {
                    Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction Stop
                    Write-Warning 'Rollback restored the original file state.'
                }
            } catch { Write-Warning "Rollback error: $_" }
            if (-not $restored) {
                Write-Warning "Rollback could not be verified. Preserve the backup in $backupDir."
            }
            throw $reason
        }
    } else {
        if (-not (Test-Path -LiteralPath $manifestFile -PathType Leaf)) {
            throw "No Rock to Fakie installation manifest in $backupDir. Nothing was changed."
        }
        $saved = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
        if ([int]$saved.Schema -ne 1 -or [string]::IsNullOrWhiteSpace([string]$saved.InstalledSha256)) {
            throw 'Unsupported or invalid installation manifest. No changes made.'
        }
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
            throw 'Installed DLL is missing or was moved. Refusing automatic restore; preserve the original backup.'
        }
        if ((Get-Sha $target) -ne [string]$saved.InstalledSha256) {
            throw 'The installed DLL has been modified or updated. Refusing to overwrite those changes. Your original backup is untouched.'
        }
        if ([bool]$saved.OriginalExisted) {
            if (-not (Test-Path -LiteralPath $backup -PathType Leaf)) {
                throw 'Original backup file missing. Refusing uninstall.'
            }
            if ((Get-Sha $backup) -ne [string]$saved.OriginalSha256) {
                throw 'Original backup checksum mismatch. Refusing uninstall.'
            }
            Copy-Item -LiteralPath $backup -Destination $target -Force -ErrorAction Stop
            if ((Get-Sha $target) -ne [string]$saved.OriginalSha256) {
                throw "Could not verify restored DLL. Backup still available at $backupDir"
            }
            Write-Host 'Original ReSkate.dll restored and checksum verified.'
        } else {
            Remove-Item -LiteralPath $target -Force -ErrorAction Stop
            if (Test-Path -LiteralPath $target) {
                throw 'Unable to verify removal of the installed DLL; backup metadata retained.'
            }
            Write-Host 'Mod-installed ReSkate.dll removed (no prior DLL existed).'
        }
        Remove-Item -LiteralPath $backupDir -Recurse -Force -ErrorAction Stop
        Write-Host 'Rock to Fakie uninstall complete. Other game files unchanged.'
    }
    exit 0
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
    exit 1
}
