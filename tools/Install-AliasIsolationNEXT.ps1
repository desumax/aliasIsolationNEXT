<#
.SYNOPSIS
    Installs and manages Alias Isolation NEXT for Alien: Isolation, plus two optional extras:
    skipping the intro videos, and the DLSS 5 + LumeniteFX pack from DLSS5-Feeder.

.DESCRIPTION
    Run it from the extracted release folder (next to the "files" folder), with no arguments for a menu:

        powershell -ExecutionPolicy Bypass -File .\Install-AliasIsolationNEXT.ps1

    Or pass -Action for a single step. The game is found through Steam; -GameDir overrides that.

    Every file this script puts in the game folder is listed in _aliasisolation-files.txt there, which is what
    -Action Uninstall removes. The DLSS 5 pack is not bundled: the script downloads DLSS5-Feeder's own installer
    from its official GitHub release and runs it, so ReShade, LumeniteFX and the NVIDIA runtimes come from their
    original sources under their own licences.

.PARAMETER Action
    Install, Uninstall, SkipIntros, RestoreIntros, InstallDlss5, DisableDlss5, EnableDlss5 or Status.

.PARAMETER Yes
    Don't ask for confirmation.
#>
[CmdletBinding()]
param(
    [ValidateSet('Menu', 'Install', 'Uninstall', 'SkipIntros', 'RestoreIntros', 'InstallDlss5', 'DisableDlss5',
        'EnableDlss5', 'Status')]
    [string]$Action = 'Menu',
    [string]$GameDir,
    [switch]$Yes
)

Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'

$ModVersion = '1.3.0-fixes'
$ManifestName = '_aliasisolation-files.txt'
$IntroVideos = @('AMD_IDENT.USM', 'CA_IDENT.USM', 'FOX_IDENT.USM')
$IntroBackupName = '_intro-videos-backup'
$FeederRepo = 'jlrouzies-fr/DLSS5-Feeder'
$SourceDir = Join-Path $PSScriptRoot 'files'

function Write-Step([string]$Text) { Write-Host "  $Text" }
function Write-Ok([string]$Text) { Write-Host "  [ OK ] $Text" -ForegroundColor Green }
function Write-Warn([string]$Text) { Write-Host "  [WARN] $Text" -ForegroundColor Yellow }
function Write-Fail([string]$Text) { Write-Host "  [FAIL] $Text" -ForegroundColor Red }

function Confirm-Step([string]$Question) {
    if ($Yes) { return $true }
    $answer = Read-Host "  $Question [y/N]"
    return $answer -match '^(y|yes)$'
}

# --- Finding the game -------------------------------------------------------------------------------------------

function Get-SteamLibraries {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $item = Get-ItemProperty -Path $key -ErrorAction Stop
            if ($item.PSObject.Properties['SteamPath']) { $steam = $item.SteamPath }
            elseif ($item.PSObject.Properties['InstallPath']) { $steam = $item.InstallPath }
        } catch { }
        if ($steam) { break }
    }
    if (-not $steam) { return @() }

    $libraries = @($steam -replace '/', '\')
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    if (Test-Path $vdf) {
        foreach ($line in Get-Content $vdf) {
            if ($line -match '"path"\s+"([^"]+)"') { $libraries += ($Matches[1] -replace '\\\\', '\') }
        }
    }
    return $libraries | Select-Object -Unique
}

function Find-GameDir {
    if ($GameDir) {
        if (-not (Test-Path (Join-Path $GameDir 'AI.exe'))) { throw "AI.exe was not found in '$GameDir'." }
        return (Resolve-Path $GameDir).Path
    }
    foreach ($library in Get-SteamLibraries) {
        $candidate = Join-Path $library 'steamapps\common\Alien Isolation'
        if (Test-Path (Join-Path $candidate 'AI.exe')) { return $candidate }
    }
    if ($Action -ne 'Menu') { throw 'Alien: Isolation was not found through Steam. Pass -GameDir "<folder with AI.exe>".' }
    $entered = Read-Host '  Alien: Isolation was not found through Steam. Folder that contains AI.exe'
    $entered = $entered.Trim('"')
    if (-not (Test-Path (Join-Path $entered 'AI.exe'))) { throw "AI.exe was not found in '$entered'." }
    return (Resolve-Path $entered).Path
}

function Assert-GameClosed {
    if (Get-Process -Name 'AI' -ErrorAction SilentlyContinue) {
        throw 'Alien: Isolation is running. Close it first.'
    }
}

# --- File helpers ------------------------------------------------------------------------------------------------

function Get-ProductName([string]$Path) {
    if (-not (Test-Path $Path)) { return $null }
    return (Get-Item $Path).VersionInfo.ProductName
}

function Test-AsiLoader([string]$Path) { return (Get-ProductName $Path) -like 'Ultimate*ASI*Loader*' }
function Test-ReShade([string]$Path) { return (Get-ProductName $Path) -eq 'ReShade' }

function Get-Manifest([string]$Game) {
    $path = Join-Path $Game $ManifestName
    if (Test-Path $path) { return @(Get-Content $path | Where-Object { $_.Trim() }) }
    return @()
}

function Add-ToManifest([string]$Game, [string[]]$Entries) {
    $path = Join-Path $Game $ManifestName
    $all = @(Get-Manifest $Game) + $Entries | Select-Object -Unique
    Set-Content -Path $path -Value $all -Encoding UTF8
}

Add-Type -Namespace AliasIsolationNext -Name Cpu -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("kernel32.dll")]
public static extern bool IsProcessorFeaturePresent(uint feature);
'@

function Get-CpuBuild {
    # PF_AVX2_INSTRUCTIONS_AVAILABLE = 40, PF_AVX_INSTRUCTIONS_AVAILABLE = 39
    if ([AliasIsolationNext.Cpu]::IsProcessorFeaturePresent(40)) { return 'V3' }
    if ([AliasIsolationNext.Cpu]::IsProcessorFeaturePresent(39)) { return 'AVX1' }
    return 'SSE3'
}

# --- Status ------------------------------------------------------------------------------------------------------

function Get-IntroState([string]$Game) {
    $movies = Join-Path $Game 'DATA\UI\MOVIES'
    $backup = Join-Path $Game $IntroBackupName
    $inMovies = @($IntroVideos | Where-Object { Test-Path (Join-Path $movies $_) }).Count
    $inBackup = @($IntroVideos | Where-Object { Test-Path (Join-Path $backup $_) }).Count
    if ($inMovies -eq $IntroVideos.Count -and $inBackup -eq 0) { return 'Playing' }
    if ($inBackup -eq $IntroVideos.Count -and $inMovies -eq 0) { return 'Skipped' }
    return 'Mixed'
}

function Get-Dlss5State([string]$Game) {
    $feeder = Test-Path (Join-Path $Game 'dlss5-feed.addon32')
    if (Test-ReShade (Join-Path $Game 'dxgi.dll')) { if ($feeder) { return 'On' } return 'ReShade only' }
    if (Test-ReShade (Join-Path $Game 'dxgi.dll.off')) { return 'Off' }
    if ($feeder) { return 'Incomplete' }
    return 'Not installed'
}

function Show-Status([string]$Game) {
    Write-Host ''
    Write-Host "  Game folder: $Game"
    $asi = Join-Path $Game 'aliasIsolation.asi'
    $alias = 'Not installed'
    if (Test-Path $asi) {
        $alias = 'Installed'
        if (-not (Test-AsiLoader (Join-Path $Game 'd3d11.dll'))) { $alias = 'Installed, but d3d11.dll is not the ASI loader' }
    }
    Write-Host "  Alias Isolation : $alias"
    Write-Host "  Intro videos    : $(Get-IntroState $Game)"
    Write-Host "  DLSS 5 pack     : $(Get-Dlss5State $Game)"
    Write-Host ''
}

# --- Alias Isolation ---------------------------------------------------------------------------------------------

function Install-Alias([string]$Game) {
    Assert-GameClosed
    if (-not (Test-Path (Join-Path $SourceDir 'aliasIsolation.asi'))) {
        throw "The release files were not found in '$SourceDir'. Run the script from the extracted release folder."
    }

    $build = Get-CpuBuild
    $asiSource = Join-Path $SourceDir 'aliasIsolation.asi'
    if ($build -ne 'V3') {
        $asiSource = Join-Path $SourceDir "legacy\$build\aliasIsolation.asi"
        Write-Step "This CPU has no AVX2, using the $build build."
    }

    # The ASI loader goes in as d3d11.dll. Another d3d11.dll (often ReShade) is moved to dxgi.dll, which ReShade
    # loads from just the same; anything else is left alone rather than overwritten.
    $d3d11 = Join-Path $Game 'd3d11.dll'
    $dxgi = Join-Path $Game 'dxgi.dll'
    if ((Test-Path $d3d11) -and -not (Test-AsiLoader $d3d11)) {
        if ((Test-ReShade $d3d11) -and -not (Test-Path $dxgi)) {
            Move-Item $d3d11 $dxgi
            Write-Ok 'Moved ReShade from d3d11.dll to dxgi.dll to make room for the ASI loader.'
        } else {
            throw "d3d11.dll in the game folder belongs to '$(Get-ProductName $d3d11)'. Move it out of the way and run this again."
        }
    }

    Copy-Item $asiSource (Join-Path $Game 'aliasIsolation.asi') -Force
    if (-not (Test-AsiLoader $d3d11)) { Copy-Item (Join-Path $SourceDir 'd3d11.dll') $d3d11 -Force }

    # Shaders and textures are replaced; settings.txt is kept, and the mod creates it on first launch.
    $modsTarget = Join-Path $Game 'mods\aliasIsolation\data'
    New-Item -ItemType Directory -Force $modsTarget | Out-Null
    Copy-Item (Join-Path $SourceDir 'mods\aliasIsolation\data\*') $modsTarget -Recurse -Force

    Add-ToManifest $Game @('aliasIsolation.asi', 'd3d11.dll', 'mods\aliasIsolation')
    Write-Ok "Alias Isolation $ModVersion installed. In game: Delete opens the menu."
    Write-Step 'Video settings: Anti-Aliasing SMAA T1x, Motion Blur on, Chromatic Aberration off.'
}

function Uninstall-Alias([string]$Game) {
    Assert-GameClosed
    foreach ($entry in @('aliasIsolation.asi', 'aliasIsolation.asi.v1.2.0-original', 'mods\aliasIsolation')) {
        $path = Join-Path $Game $entry
        if (Test-Path $path) { Remove-Item $path -Recurse -Force; Write-Ok "Removed $entry" }
    }
    $mods = Join-Path $Game 'mods'
    if ((Test-Path $mods) -and -not (Get-ChildItem $mods -Force)) { Remove-Item $mods }

    # The loader stays if another ASI mod still needs it.
    $d3d11 = Join-Path $Game 'd3d11.dll'
    $otherAsi = @(Get-ChildItem $Game -Filter '*.asi' -File -ErrorAction SilentlyContinue)
    if ((Test-AsiLoader $d3d11) -and $otherAsi.Count -eq 0) { Remove-Item $d3d11 -Force; Write-Ok 'Removed d3d11.dll (ASI loader)' }
    elseif ($otherAsi.Count -gt 0) { Write-Warn 'Kept d3d11.dll: other .asi mods still use it.' }

    Remove-Item (Join-Path $Game $ManifestName) -ErrorAction SilentlyContinue
    Write-Ok 'Alias Isolation removed. The intro videos and the DLSS 5 pack were not touched.'
}

# --- Intro videos ------------------------------------------------------------------------------------------------

function Set-IntroVideos([string]$Game, [bool]$Skip) {
    Assert-GameClosed
    $movies = Join-Path $Game 'DATA\UI\MOVIES'
    $backup = Join-Path $Game $IntroBackupName
    $from = $movies; $to = $backup
    if (-not $Skip) { $from = $backup; $to = $movies }

    New-Item -ItemType Directory -Force $to | Out-Null
    foreach ($video in $IntroVideos) {
        $source = Join-Path $from $video
        if (Test-Path $source) { Move-Item $source (Join-Path $to $video) -Force }
    }
    if (-not $Skip -and (Test-Path $backup) -and -not (Get-ChildItem $backup -Force)) { Remove-Item $backup }

    $state = Get-IntroState $Game
    if ($state -eq 'Mixed') {
        Write-Warn 'Some intro videos are missing. Steam > Alien: Isolation > Properties > Installed Files > Verify restores them.'
    } elseif ($Skip) {
        Write-Ok 'Intro videos will be skipped.'
    } else {
        Write-Ok 'Intro videos restored.'
    }
}

# --- DLSS 5 pack -------------------------------------------------------------------------------------------------

function Install-Dlss5([string]$Game) {
    Assert-GameClosed
    $headers = @{ 'User-Agent' = 'AliasIsolationNEXT-installer' }
    $release = Invoke-RestMethod -Headers $headers "https://api.github.com/repos/$FeederRepo/releases/latest"
    $tag = $release.tag_name
    $url = "https://raw.githubusercontent.com/$FeederRepo/$tag/tools/Install-DLSS5Feeder.ps1"

    $cache = Join-Path $env:TEMP 'AliasIsolationNEXT'
    New-Item -ItemType Directory -Force $cache | Out-Null
    $script = Join-Path $cache "Install-DLSS5Feeder-$tag.ps1"
    Invoke-WebRequest -UseBasicParsing -Headers $headers $url -OutFile $script
    $hash = (Get-FileHash $script -Algorithm SHA256).Hash

    Write-Host ''
    Write-Step "DLSS5-Feeder $tag installer: $url"
    Write-Step "SHA-256: $hash"
    Write-Step 'It installs ReShade 6.8+, the feeder, LumeniteFX and a neural add-on (it asks which; OptiScaler'
    Write-Step 'DLSS-NR needs no extra download). It merges and backs up existing files, then runs a check.'
    Write-Step 'You can read the script at the path above before going on.'
    if (-not (Confirm-Step 'Run it now?')) { Write-Step 'Skipped.'; return }

    # Keep the ASI loader as d3d11.dll whatever the feeder installer picks for ReShade.
    $d3d11 = Join-Path $Game 'd3d11.dll'
    $loaderBackup = $null
    if (Test-AsiLoader $d3d11) {
        $loaderBackup = Join-Path $cache 'd3d11-asi-loader.dll'
        Copy-Item $d3d11 $loaderBackup -Force
    }

    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $script (Join-Path $Game 'AI.exe')
    if ($LASTEXITCODE -ne 0) { Write-Warn "The DLSS5-Feeder installer exited with code $LASTEXITCODE." }

    if ($loaderBackup -and -not (Test-AsiLoader $d3d11)) {
        $dxgi = Join-Path $Game 'dxgi.dll'
        if ((Test-ReShade $d3d11) -and -not (Test-Path $dxgi)) {
            Move-Item $d3d11 $dxgi
            Copy-Item $loaderBackup $d3d11
            Write-Ok 'ReShade was installed as d3d11.dll; moved it to dxgi.dll and put the ASI loader back.'
        } else {
            Write-Warn 'd3d11.dll was replaced and Alias Isolation will not load. Run -Action Install again.'
        }
    }
    Write-Step 'In game, the Alias Isolation menu (Delete) now has switches for the ReShade effects.'
}

function Set-Dlss5Enabled([string]$Game, [bool]$Enable) {
    Assert-GameClosed
    $dxgi = Join-Path $Game 'dxgi.dll'
    $off = Join-Path $Game 'dxgi.dll.off'
    if ($Enable) {
        if (Test-ReShade $off) { Move-Item $off $dxgi -Force; Write-Ok 'DLSS 5 pack turned on.' }
        else { Write-Warn 'The DLSS 5 pack is not turned off.' }
    } else {
        if (Test-ReShade $dxgi) { Move-Item $dxgi $off -Force; Write-Ok 'DLSS 5 pack turned off (ReShade will not load).' }
        else { Write-Warn 'ReShade was not found as dxgi.dll.' }
    }
}

# --- Entry point -------------------------------------------------------------------------------------------------

function Invoke-Action([string]$Name, [string]$Game) {
    switch ($Name) {
        'Install' { Install-Alias $Game }
        'Uninstall' { if (Confirm-Step 'Remove Alias Isolation from the game folder?') { Uninstall-Alias $Game } }
        'SkipIntros' { Set-IntroVideos $Game $true }
        'RestoreIntros' { Set-IntroVideos $Game $false }
        'InstallDlss5' { Install-Dlss5 $Game }
        'DisableDlss5' { Set-Dlss5Enabled $Game $false }
        'EnableDlss5' { Set-Dlss5Enabled $Game $true }
        'Status' { Show-Status $Game }
    }
}

Write-Host ''
Write-Host "  Alias Isolation NEXT $ModVersion installer"
$game = Find-GameDir

if ($Action -ne 'Menu') {
    Invoke-Action $Action $game
    exit 0
}

while ($true) {
    Show-Status $game
    Write-Host '  1) Install or update Alias Isolation'
    Write-Host '  2) Remove Alias Isolation'
    Write-Host '  3) Skip intro videos'
    Write-Host '  4) Restore intro videos'
    Write-Host '  5) Install DLSS 5 + LumeniteFX (runs the DLSS5-Feeder installer)'
    Write-Host '  6) Turn the DLSS 5 pack off'
    Write-Host '  7) Turn the DLSS 5 pack on'
    Write-Host '  Q) Quit'
    $choice = Read-Host '  Choice'
    $map = @{ '1' = 'Install'; '2' = 'Uninstall'; '3' = 'SkipIntros'; '4' = 'RestoreIntros'; '5' = 'InstallDlss5'
        '6' = 'DisableDlss5'; '7' = 'EnableDlss5' }
    if ($choice -match '^[qQ]') { break }
    if (-not $map.ContainsKey($choice)) { continue }
    try { Invoke-Action $map[$choice] $game } catch { Write-Fail $_.Exception.Message }
}
