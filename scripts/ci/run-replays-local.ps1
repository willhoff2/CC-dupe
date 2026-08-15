<#
.SYNOPSIS
Run the replay determinism check locally, against retail game data in a local directory, with no
bucket and no CI involved.

.DESCRIPTION
This is the manual equivalent of the `Replay Check GeneralsMD` job in `GenCI`
(`.github/workflows/check-replays.yml`). That job is skipped in a checkout without hosted game
data, so this script is the only way the gate gets run there. It does the same five things the
workflow does:

  1. stages a copy of the build next to the Zero Hour data files,
  2. stages the Generals data files and points the `InstallPath` registry value at them, because
     Zero Hour reads Generals files,
  3. puts the repository's replays and the maps they need where the game looks for them,
  4. runs `generalszh.exe -headless -replay`, and
  5. reports the exit code, which is the verdict.

Nothing is written to the retail installs or to the build directory: the game runs out of a staging
copy. The registry value and any files added to the user data folder are restored on exit,
including after a failure.

**Windows only.** The executable is a 32-bit x86 Windows binary, so it cannot run on macOS:
neither on Apple Silicon (Rosetta 2 translates x86-64 only and macOS has had no i386 loader since
10.15) nor through the Wine/VC6 container, which is amd64. There is no native build to substitute:
see docs/porting/native-build.md. A Windows machine or VM is required.

The build must be a VC6 build with optimisations and `RTS_BUILD_OPTION_DEBUG=OFF`, per TESTING.md;
any other configuration is not retail-compatible by construction, so its failures mean nothing.
The script checks this when the build directory has a CMake cache to check.

.EXAMPLE
pwsh scripts/ci/run-replays-local.ps1 `
    -BuildDir       .\build\vc6 `
    -GeneralsPath   "C:\Program Files (x86)\EA Games\Command & Conquer Generals" `
    -GeneralsMDPath "C:\Program Files (x86)\EA Games\Command & Conquer Generals Zero Hour"
#>

[CmdletBinding()]
param(
    # Directory holding the built generalszh.exe (a VC6 optimised build).
    [Parameter(Mandatory = $true)] [string] $BuildDir,
    # Retail Generals 1.08 install, or a directory holding its trimmed data files.
    [Parameter(Mandatory = $true)] [string] $GeneralsPath,
    # Retail Zero Hour 1.04 install, or a directory holding its trimmed data files.
    [Parameter(Mandatory = $true)] [string] $GeneralsMDPath,
    # Folder with the Replays and Maps subfolders to test. Defaults to the repository's own set,
    # which is what CI runs.
    [string] $UserData = (Join-Path $PSScriptRoot "..\..\GeneralsReplays\GeneralsZH\1.04"),
    [int]    $Jobs = 4,
    [int]    $TimeoutMinutes = 10,
    [string] $StageDir = (Join-Path ([System.IO.Path]::GetTempPath()) "cc-replay-check"),
    # Skip the RTS_BUILD_OPTION_DEBUG check for a build whose configuration you have verified
    # by other means.
    [switch] $SkipBuildOptionCheck
)

$ErrorActionPreference = "Stop"

if ($PSVersionTable.PSVersion.Major -ge 6 -and -not $IsWindows) {
    throw "This script only runs on Windows: generalszh.exe is a 32-bit x86 Windows binary. See the notes in this file and docs/porting/replay-check-gamedata.md."
}

# The subfolder the replays are copied into, so that the run cannot pick up replays the user
# already had and so that cleanup can delete exactly what it created.
$replaySubfolder = "cc-replay-check"

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

function Find-GameFile {
    param([string] $Root, [string] $RelativePath)

    # Case-insensitively locate a wanted file, so an install that spells BINKW32.DLL differently
    # still works. Matches the lookup scripts/ci/pack-gamedata.ps1 does.
    $direct = Join-Path $Root $RelativePath
    if (Test-Path -LiteralPath $direct) { return (Resolve-Path -LiteralPath $direct).Path }

    $leaf = Split-Path $RelativePath -Leaf
    $parent = Split-Path $RelativePath -Parent
    $searchRoot = if ($parent) { Join-Path $Root $parent } else { $Root }
    if (-not (Test-Path -LiteralPath $searchRoot)) { return $null }

    $match = Get-ChildItem -LiteralPath $searchRoot -File |
        Where-Object { $_.Name -ieq $leaf } |
        Select-Object -First 1
    if ($match) { return $match.FullName }
    return $null
}

function Copy-GameData {
    param([string] $Label, [string] $Root, [string[]] $Files, [string] $Destination)

    if (-not (Test-Path -LiteralPath $Root)) {
        throw "$Label path does not exist: $Root"
    }

    $missing = @()
    foreach ($file in $Files) {
        $found = Find-GameFile -Root $Root -RelativePath $file
        if (-not $found) { $missing += $file; continue }

        $target = Join-Path $Destination $file
        New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
        Copy-Item -LiteralPath $found -Destination $target -Force
    }

    if ($missing.Count -gt 0) {
        throw "$Label data is incomplete in $Root - missing: $($missing -join ', ')"
    }
    Write-Host "Staged $($Files.Count) $Label files into $Destination"
}

function Assert-RetailCompatibleBuild {
    param([string] $Directory)

    # TESTING.md: only a VC6 build with optimisations and RTS_BUILD_OPTION_DEBUG=OFF is
    # retail-compatible, so a failure from any other configuration says nothing.
    $cache = Join-Path $Directory "CMakeCache.txt"
    if (-not (Test-Path -LiteralPath $cache)) {
        Write-Warning "No CMakeCache.txt in $Directory, so the build configuration could not be verified. The replay check is only meaningful for a VC6 build with RTS_BUILD_OPTION_DEBUG=OFF (see TESTING.md)."
        return
    }

    $debugOption = Select-String -LiteralPath $cache -Pattern '^RTS_BUILD_OPTION_DEBUG:BOOL=(.*)$' |
        Select-Object -First 1
    if (-not $debugOption) {
        Write-Warning "RTS_BUILD_OPTION_DEBUG is not in $cache; cannot verify the build is retail-compatible."
        return
    }

    $value = $debugOption.Matches[0].Groups[1].Value.Trim()
    if ($value -ieq "ON" -or $value -eq "1" -or $value -ieq "TRUE") {
        throw "This build has RTS_BUILD_OPTION_DEBUG=$value. Replays from it are not retail-compatible by construction, so the result would be meaningless. Build a VC6 optimised configuration (see TESTING.md), or pass -SkipBuildOptionCheck if you know better."
    }
    Write-Host "Build option check: RTS_BUILD_OPTION_DEBUG=$value"
}

$exeName = "generalszh.exe"
$sourceExe = Find-GameFile -Root $BuildDir -RelativePath $exeName
if (-not $sourceExe) {
    throw "$exeName was not found in $BuildDir. Build the vc6 preset first (see docs/porting/replay-check-gamedata.md)."
}
if (-not $SkipBuildOptionCheck) { Assert-RetailCompatibleBuild -Directory $BuildDir }

if (-not (Test-Path -LiteralPath (Join-Path $UserData "Replays"))) {
    throw "No Replays folder under $UserData."
}
if (-not (Test-Path -LiteralPath (Join-Path $UserData "Maps"))) {
    throw "No Maps folder under $UserData."
}

# --- Stage the build and the game data ------------------------------------------------------
# The game is run from a copy so that the build directory and the retail installs are left
# untouched by the data staging and by the log files the run writes next to the executable.

$gameStage = Join-Path $StageDir "GeneralsMD"
$generalsStage = Join-Path $StageDir "Generals"

if (Test-Path -LiteralPath $StageDir) { Remove-Item -LiteralPath $StageDir -Recurse -Force }
New-Item -ItemType Directory -Path $gameStage -Force | Out-Null
New-Item -ItemType Directory -Path $generalsStage -Force | Out-Null

Write-Host "Staging build from $BuildDir" -ForegroundColor Cyan
Copy-Item -Path (Join-Path $BuildDir "*") -Destination $gameStage -Recurse -Force

Copy-GameData -Label "Zero Hour 1.04" -Root $GeneralsMDPath -Files $generalsMDFiles -Destination $gameStage
Copy-GameData -Label "Generals 1.08" -Root $GeneralsPath -Files $generalsFiles -Destination $generalsStage

# --- Point the game at the staged Generals data, and place the replays ----------------------

$regPath = "HKCU:\SOFTWARE\Electronic Arts\EA Games\Generals"
$regKeyExisted = Test-Path $regPath
$previousInstallPath = if ($regKeyExisted) {
    (Get-ItemProperty -Path $regPath -Name InstallPath -ErrorAction SilentlyContinue).InstallPath
} else { $null }

$userDataRoot = Join-Path ([Environment]::GetFolderPath('MyDocuments')) "Command and Conquer Generals Zero Hour Data"
$replayDestination = Join-Path (Join-Path $userDataRoot "Replays") $replaySubfolder
$mapsDestination = Join-Path $userDataRoot "Maps"
$addedMaps = @()

try {
    if (-not $regKeyExisted) { New-Item -Path $regPath -Force | Out-Null }
    Set-ItemProperty -Path $regPath -Name InstallPath -Value "$generalsStage\" -Type String
    Write-Host "Registry: $regPath -> InstallPath = $generalsStage\"

    New-Item -ItemType Directory -Path $replayDestination -Force | Out-Null
    Copy-Item -Path (Join-Path $UserData "Replays\*") -Destination $replayDestination -Recurse -Force
    Write-Host "Copied replays to $replayDestination"

    # Maps are not read from subfolders, so they have to go next to any the user already has.
    # Only the ones that are not already there are copied, and only those are removed afterwards.
    New-Item -ItemType Directory -Path $mapsDestination -Force | Out-Null
    foreach ($map in Get-ChildItem -LiteralPath (Join-Path $UserData "Maps")) {
        $target = Join-Path $mapsDestination $map.Name
        if (Test-Path -LiteralPath $target) {
            Write-Host "Map already present, left alone: $($map.Name)"
            continue
        }
        Copy-Item -LiteralPath $map.FullName -Destination $target -Recurse
        $addedMaps += $target
    }
    Write-Host "Copied $($addedMaps.Count) maps to $mapsDestination"

    # --- Run ---------------------------------------------------------------------------------

    $exePath = Join-Path $gameStage $exeName
    $arguments = "-jobs $Jobs -headless -replay $replaySubfolder/*.rep"
    $stdoutPath = Join-Path $StageDir "stdout.log"
    $stderrPath = Join-Path $StageDir "stderr.log"

    # The game is a GUI application, so its console output only exists if it is redirected.
    Write-Host "Run $exePath $arguments" -ForegroundColor Cyan
    $process = Start-Process -FilePath $exePath `
        -ArgumentList $arguments `
        -WorkingDirectory $gameStage `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -PassThru

    $exited = $process.WaitForExit($TimeoutMinutes * 60 * 1000)
    if (-not $exited) {
        Write-Host "ERROR: still running after $TimeoutMinutes minutes. Killing process." -ForegroundColor Red
        Stop-Process -Id $process.Id -Force
    }

    Write-Host "=== STDOUT ==="
    if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath }
    if ((Test-Path -LiteralPath $stderrPath) -and (Get-Item -LiteralPath $stderrPath).Length -gt 0) {
        Write-Host "`n=== STDERR ==="
        Get-Content -LiteralPath $stderrPath
    }

    $debugLogs = Get-ChildItem -LiteralPath $gameStage -Filter "DebugLogFile*.txt" -ErrorAction SilentlyContinue
    if ($debugLogs) {
        Write-Host "`nDebug logs (say which replay diverged):"
        $debugLogs | ForEach-Object { Write-Host "  $($_.FullName)" }
    }

    if (-not $exited) { exit 1 }

    $exitCode = $process.ExitCode
    if ($exitCode -ne 0) {
        Write-Host "`nFAILED: replay check exited with $exitCode. The replays diverged, or the run crashed." -ForegroundColor Red
        exit $exitCode
    }

    Write-Host "`nSuccess: replays are deterministic for this build." -ForegroundColor Green
}
finally {
    Write-Host "`nRestoring the environment" -ForegroundColor Cyan

    if (Test-Path -LiteralPath $replayDestination) {
        Remove-Item -LiteralPath $replayDestination -Recurse -Force
    }
    foreach ($map in $addedMaps) {
        if (Test-Path -LiteralPath $map) { Remove-Item -LiteralPath $map -Recurse -Force }
    }

    if (-not $regKeyExisted) {
        Remove-Item -Path $regPath -Recurse -Force -ErrorAction SilentlyContinue
    } elseif ($null -eq $previousInstallPath) {
        Remove-ItemProperty -Path $regPath -Name InstallPath -ErrorAction SilentlyContinue
    } else {
        Set-ItemProperty -Path $regPath -Name InstallPath -Value $previousInstallPath -Type String
    }

    Write-Host "The staged copy, its logs and the replay output are left in $StageDir"
}
