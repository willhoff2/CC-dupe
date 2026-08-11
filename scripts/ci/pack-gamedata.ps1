<#
.SYNOPSIS
Pack the trimmed Generals 1.08 and Zero Hour 1.04 data the replay check needs, from local
retail installs, and print the SHA256 of each archive.

.DESCRIPTION
`.github/workflows/check-replays.yml` runs `generalszh.exe -headless -replay` against retail
game data it downloads from a bucket. The archives hold only what a headless replay run reads:
the INI/map/W3D `.big` files, the two DLLs the executable links against, and the script files.
No textures, audio or GUI data. Nothing here is redistributable, which is why the archives live
in a private bucket and CI verifies their hash.

Run this once against retail installs, upload the two archives, then set the bucket secrets and
the hash variables this prints. See docs/porting/replay-check-gamedata.md.

.EXAMPLE
pwsh scripts/ci/pack-gamedata.ps1 `
    -GeneralsPath  "C:\Program Files (x86)\EA Games\Command & Conquer Generals" `
    -GeneralsMDPath "C:\Program Files (x86)\EA Games\Command & Conquer Generals Zero Hour" `
    -OutputDir .\gamedata-out
#>

[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string] $GeneralsPath,
    [Parameter(Mandatory = $true)] [string] $GeneralsMDPath,
    [string] $OutputDir = "gamedata-out"
)

$ErrorActionPreference = "Stop"

# Kept in step with the file lists documented in check-replays.yml.
$generalsFiles = @(
    "BINKW32.DLL",
    "English.big",
    "INI.big",
    "Maps.big",
    "mss32.dll",
    "W3D.big",
    "Data\Scripts\MultiplayerScripts.scb",
    "Data\Scripts\SkirmishScripts.scb"
)

$generalsMDFiles = @(
    "BINKW32.DLL",
    "INIZH.big",
    "MapsZH.big",
    "mss32.dll",
    "W3DZH.big",
    "Data\Scripts\MultiplayerScripts.scb",
    "Data\Scripts\Scripts.ini",
    "Data\Scripts\SkirmishScripts.scb"
)

function Resolve-SevenZip {
    foreach ($candidate in @("7z", "7z.exe", "$env:ProgramFiles\7-Zip\7z.exe", "${env:ProgramFiles(x86)}\7-Zip\7z.exe")) {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
        if (Test-Path $candidate) { return $candidate }
    }
    throw "7z was not found. Install 7-Zip (winget install 7zip.7zip) and re-run."
}

function New-GameDataArchive {
    param(
        [string]   $Label,
        [string]   $InstallPath,
        [string[]] $Files,
        [string]   $ArchivePath,
        [string]   $SevenZip
    )

    if (-not (Test-Path -LiteralPath $InstallPath)) {
        throw "$Label install path does not exist: $InstallPath"
    }

    # Case-insensitively locate each wanted file, so an install that spells BINKW32.DLL
    # differently still packs. Staged into a clean tree to keep the archive paths relative.
    $stage = Join-Path ([System.IO.Path]::GetTempPath()) ("packgamedata-" + [guid]::NewGuid())
    New-Item -ItemType Directory -Path $stage -Force | Out-Null

    try {
        $missing = @()
        foreach ($relative in $Files) {
            $source = Join-Path $InstallPath $relative
            if (-not (Test-Path -LiteralPath $source)) {
                $parent = Split-Path (Join-Path $InstallPath $relative) -Parent
                $leaf = Split-Path $relative -Leaf
                $found = if (Test-Path -LiteralPath $parent) {
                    Get-ChildItem -LiteralPath $parent -File |
                        Where-Object { $_.Name -ieq $leaf } |
                        Select-Object -First 1
                } else { $null }
                if (-not $found) { $missing += $relative; continue }
                $source = $found.FullName
            }

            $target = Join-Path $stage $relative
            New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $target -Force
        }

        if ($missing.Count -gt 0) {
            throw "$Label install is missing these files, which the replay check needs:`n  " + ($missing -join "`n  ")
        }

        if (Test-Path -LiteralPath $ArchivePath) { Remove-Item -LiteralPath $ArchivePath -Force }

        Push-Location $stage
        try {
            & $SevenZip a -t7z -mx=9 $ArchivePath "." | Out-Null
            if ($LASTEXITCODE -ne 0) { throw "7z failed packing $Label (exit $LASTEXITCODE)" }
        } finally {
            Pop-Location
        }

        return (Get-FileHash -LiteralPath $ArchivePath -Algorithm SHA256).Hash
    } finally {
        Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    }
}

$sevenZip = Resolve-SevenZip
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDir).Path

$generalsArchive = Join-Path $outputRoot "generals108_gamedata_trimmed.7z"
$generalsMDArchive = Join-Path $outputRoot "zerohour104_gamedata_trimmed.7z"

Write-Host "Packing Generals 1.08 data" -ForegroundColor Cyan
$generalsHash = New-GameDataArchive -Label "Generals" -InstallPath $GeneralsPath `
    -Files $generalsFiles -ArchivePath $generalsArchive -SevenZip $sevenZip

Write-Host "Packing Zero Hour 1.04 data" -ForegroundColor Cyan
$generalsMDHash = New-GameDataArchive -Label "Zero Hour" -InstallPath $GeneralsMDPath `
    -Files $generalsMDFiles -ArchivePath $generalsMDArchive -SevenZip $sevenZip

Write-Host ""
Write-Host "Archives written to $outputRoot" -ForegroundColor Green
Write-Host "  generals108_gamedata_trimmed.7z  $generalsHash"
Write-Host "  zerohour104_gamedata_trimmed.7z  $generalsMDHash"
Write-Host ""
Write-Host "Upload both to your bucket, then set these repository variables:" -ForegroundColor Green
Write-Host "  GAMEDATA_GENERALS_SHA256    = $generalsHash"
Write-Host "  GAMEDATA_GENERALSMD_SHA256  = $generalsMDHash"
Write-Host ""
Write-Host "and the bucket secrets described in docs/porting/replay-check-gamedata.md."
