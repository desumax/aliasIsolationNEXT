<#
.SYNOPSIS
    Installs and manages Alias Isolation NEXT for Alien: Isolation, plus two optional extras:
    skipping the intro videos, and the DLSS 5 + LumeniteFX pack from DLSS5-Feeder.

.DESCRIPTION
    Run it from the extracted release folder (next to the "files" folder), with no arguments for a menu:

        powershell -ExecutionPolicy Bypass -File .\Install-AliasIsolationNEXT.ps1

    Or pass -Action for a single step. The game is found through Steam, Epic or GOG; -GameDir overrides that.

    Every file this script puts in the game folder is listed in _aliasisolation-files.txt there, which is what
    -Action Uninstall removes. The DLSS 5 pack is not bundled: the script downloads DLSS5-Feeder's own installer
    from its official GitHub release and runs it, so ReShade, LumeniteFX and the NVIDIA runtimes come from their
    original sources under their own licences.

.PARAMETER Action
    InstallAll (Alias Isolation, skip the intro videos, then the DLSS 5 pack), Install, Uninstall, HomePage, DetectHdr,
    SkipIntros, RestoreIntros, InstallDlss5, DisableDlss5, EnableDlss5 or Status.

.PARAMETER GameDir
    The folder with AI.exe. Without it the game is looked up in Steam, the Epic Games Launcher and GOG, and if it
    isn't found there you can paste the folder or pick it in a window.

.PARAMETER Yes
    Don't ask for confirmation.
#>
[CmdletBinding()]
param(
    [ValidateSet('Menu', 'InstallAll', 'Install', 'Uninstall', 'HomePage', 'DetectHdr', 'SkipIntros', 'RestoreIntros', 'InstallDlss5', 'DisableDlss5',
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

function Get-EpicInstalls {
    # The Epic Games Launcher keeps one JSON .item manifest per installed game.
    $manifests = Join-Path $env:ProgramData 'Epic\EpicGamesLauncher\Data\Manifests'
    if (-not (Test-Path $manifests)) { return @() }
    $found = @()
    foreach ($item in Get-ChildItem $manifests -Filter '*.item' -File -ErrorAction SilentlyContinue) {
        try {
            $manifest = Get-Content $item.FullName -Raw | ConvertFrom-Json
            if ($manifest.DisplayName -like '*Alien*Isolation*' -and $manifest.InstallLocation) {
                $found += $manifest.InstallLocation
            }
        } catch { }
    }
    return $found
}

function Get-GogInstalls {
    $found = @()
    foreach ($key in 'HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games') {
        foreach ($game in Get-ChildItem $key -ErrorAction SilentlyContinue) {
            try {
                $item = Get-ItemProperty -Path $game.PSPath -ErrorAction Stop
                if ($item.PSObject.Properties['gameName'] -and $item.gameName -like '*Alien*Isolation*' -and
                    $item.PSObject.Properties['path']) { $found += $item.path }
            } catch { }
        }
    }
    return $found
}

function Get-GameCandidates {
    foreach ($library in Get-SteamLibraries) { Join-Path $library 'steamapps\common\Alien Isolation' }
    Get-EpicInstalls
    Get-GogInstalls
}

# Accepts the game folder or the path to AI.exe itself, with or without quotes.
function Resolve-GameDir([string]$Path) {
    if (-not $Path) { return $null }
    $Path = $Path.Trim().Trim('"')
    if ($Path -like '*.exe') { $Path = Split-Path $Path -Parent }
    if ($Path -and (Test-Path (Join-Path $Path 'AI.exe'))) { return (Resolve-Path $Path).Path }
    return $null
}

function Select-GameDirWindow {
    try {
        Add-Type -AssemblyName System.Windows.Forms
        $dialog = New-Object System.Windows.Forms.FolderBrowserDialog
        $dialog.Description = 'Select the Alien: Isolation folder (the one with AI.exe)'
        $dialog.ShowNewFolderButton = $false
        if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
    } catch { Write-Warn "The folder window could not be opened: $($_.Exception.Message)" }
    return $null
}

function Find-GameDir {
    if ($GameDir) {
        $resolved = Resolve-GameDir $GameDir
        if (-not $resolved) { throw "AI.exe was not found in '$GameDir'." }
        return $resolved
    }
    foreach ($candidate in Get-GameCandidates) {
        $resolved = Resolve-GameDir $candidate
        if ($resolved) { return $resolved }
    }
    if ($Yes) { throw 'Alien: Isolation was not found (Steam, Epic, GOG). Pass -GameDir "<folder with AI.exe>".' }

    Write-Warn 'Alien: Isolation was not found in Steam, Epic Games or GOG.'
    while ($true) {
        $entered = Read-Host '  Paste the folder that contains AI.exe, or press Enter to pick it in a window (Q quits)'
        if ($entered -match '^\s*[qQ]\s*$') { exit 1 }
        if (-not $entered.Trim()) { $entered = Select-GameDirWindow }
        $resolved = Resolve-GameDir $entered
        if ($resolved) { return $resolved }
        Write-Fail "AI.exe was not found in '$entered'."
    }
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
    Write-Host "  ReShade Home tab: $(Get-HomePageState $Game)"
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
    Install-HomePage $Game
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

    $homePage = Join-Path (Get-ShaderDir $Game) 'AliasIsolation.fx'
    if (Test-Path $homePage) { Remove-Item $homePage -Force; Write-Ok 'Removed AliasIsolation.fx (ReShade Home tab page)' }

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
    Set-HdrConfig $Game
    if (Test-Path (Join-Path $Game 'aliasIsolation.asi')) { Install-HomePage $Game }
    else { Hide-HelperTechniques (Get-ShaderDir $Game) }
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

# --- ReShade Home tab --------------------------------------------------------------------------------------------

# ReShade's .ini and .fx files are read and written byte for byte (Latin-1), so nothing else in them changes.
$Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Read-TextFile([string]$Path) { return [System.IO.File]::ReadAllText($Path, $Latin1) }
function Write-TextFile([string]$Path, [string]$Text) { [System.IO.File]::WriteAllText($Path, $Text, $Latin1) }

function Test-ReShadeInstalled([string]$Game) {
    foreach ($name in 'dxgi.dll', 'dxgi.dll.off', 'd3d11.dll') { if (Test-ReShade (Join-Path $Game $name)) { return $true } }
    return $false
}

# Returns the value of Key in [Section] (or before any section when Section is ''), or $null.
function Get-IniValue([string]$Text, [string]$Section, [string]$Key) {
    $current = ''
    foreach ($line in $Text -split "`r?`n") {
        if ($line -match '^\s*\[(.+)\]\s*$') { $current = $Matches[1]; continue }
        if ($current -eq $Section -and $line -match "^\s*$([regex]::Escape($Key))\s*=(.*)$") { return $Matches[1].Trim() }
    }
    return $null
}

function Set-IniValue([string]$Text, [string]$Section, [string]$Key, [string]$Value) {
    $nl = "`n"; if ($Text -match "`r`n") { $nl = "`r`n" }
    $lines = [System.Collections.Generic.List[string]]@($Text -split "`r?`n")
    $current = ''; $sectionEnd = -1; $sectionFound = ($Section -eq '')
    if ($Section -eq '') { $sectionEnd = 0 }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*\[(.+)\]\s*$') {
            if ($current -eq $Section -and $sectionFound -and $Section -eq '') { break }
            $current = $Matches[1]
            if ($current -eq $Section) { $sectionFound = $true; $sectionEnd = $i + 1 }
            continue
        }
        if ($current -ne $Section) { continue }
        if ($lines[$i] -match "^\s*$([regex]::Escape($Key))\s*=") { $lines[$i] = "$Key=$Value"; return ($lines -join $nl) }
        if ($lines[$i].Trim()) { $sectionEnd = $i + 1 }
    }
    if (-not $sectionFound) {
        if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
        $lines.Add("[$Section]"); $lines.Add("$Key=$Value"); $lines.Add('')
    } else {
        $lines.Insert($sectionEnd, "$Key=$Value")
    }
    return ($lines -join $nl)
}

function Get-ShaderDir([string]$Game) {
    $dir = 'reshade-shaders\Shaders'
    $ini = Join-Path $Game 'ReShade.ini'
    if (Test-Path $ini) {
        $paths = Get-IniValue (Read-TextFile $ini) 'GENERAL' 'EffectSearchPaths'
        if ($paths) { $dir = (($paths -split ',')[0].Trim()) -replace '\\\*\*$', '' -replace '^\.\\', '' }
    }
    if (-not [System.IO.Path]::IsPathRooted($dir)) { $dir = Join-Path $Game $dir }
    return $dir
}

# ReShade only leaves a technique out of the Home tab when it has the "hidden" annotation, so it is added to the
# feeder's debug view and to DisplayDepth. The feeder's installer replaces DLSS5_Feed.fx, so this is redone after it.
function Hide-HelperTechniques([string]$ShaderDir) {
    foreach ($target in @(@('DLSS5_Feed.fx', 'DLSS5_Feed_Debug'), @('DisplayDepth.fx', 'DisplayDepth'))) {
        $path = Join-Path $ShaderDir $target[0]
        if (-not (Test-Path $path)) { continue }
        $text = Read-TextFile $path
        $name = [regex]::Escape($target[1])
        if ($text -match "technique\s+$name\s*<\s*hidden\s*=") { continue }
        $patched = [regex]::Replace($text, "(technique\s+$name\s*<)", '$1 hidden = true;')
        if ($patched -ne $text) { Write-TextFile $path $patched; Write-Ok "Hid $($target[1]) from ReShade's Home tab." }
    }
}

function Install-HomePage([string]$Game) {
    if (-not (Test-ReShadeInstalled $Game)) { return }
    Assert-GameClosed
    $source = Join-Path $SourceDir 'reshade-shaders\Shaders\AliasIsolation.fx'
    if (-not (Test-Path $source)) { Write-Warn "AliasIsolation.fx was not found in the release files."; return }

    $shaderDir = Get-ShaderDir $Game
    New-Item -ItemType Directory -Force $shaderDir | Out-Null
    Copy-Item $source (Join-Path $shaderDir 'AliasIsolation.fx') -Force
    $relative = (Join-Path $shaderDir 'AliasIsolation.fx').Substring($Game.TrimEnd('\').Length + 1)
    Add-ToManifest $Game @($relative)
    Hide-HelperTechniques $shaderDir

    $ini = Join-Path $Game 'ReShade.ini'
    $preset = Join-Path $Game 'ReShadePreset.ini'
    $iniText = ''
    if (Test-Path $ini) {
        $iniText = Read-TextFile $ini
        $presetPath = Get-IniValue $iniText 'GENERAL' 'PresetPath'
        if ($presetPath) {
            $preset = $presetPath -replace '^\.\\', ''
            if (-not [System.IO.Path]::IsPathRooted($preset)) { $preset = Join-Path $Game $preset }
        }

        # Collapse every entry in the Add-ons tab. ReShade inverts this list for add-ons loaded from files
        # ("Name@file"): those are collapsed when they are not listed, the built-in ones when they are.
        $collapsed = @()
        $current = Get-IniValue $iniText 'ADDON' 'OverlayCollapsed'
        if ($current) { $collapsed = @($current -split ',' | ForEach-Object { $_.Trim() } | Where-Object { $_ }) }
        $collapsed = @($collapsed | Where-Object { $_ -notlike '*@*' }) + @('Generic Depth', 'Effect Runtime Sync') |
            Select-Object -Unique
        Write-TextFile $ini (Set-IniValue $iniText 'ADDON' 'OverlayCollapsed' ($collapsed -join ','))
    }

    # Each entry is set up the first time only, so a later reinstall keeps it the way the player left it. Alias
    # Isolation goes first in the Home tab and the intro-video switch last; the mod keeps that switch matched to the
    # files on disk.
    $presetText = ''
    if (Test-Path $preset) { $presetText = Read-TextFile $preset }
    $sorting = @(); $techniques = @()
    $value = Get-IniValue $presetText '' 'TechniqueSorting'
    if ($value) { $sorting = @($value -split ',' | Where-Object { $_ }) }
    $value = Get-IniValue $presetText '' 'Techniques'
    if ($value) { $techniques = @($value -split ',' | Where-Object { $_ }) }
    $changed = $false
    $entry = 'AliasIsolation@AliasIsolation.fx'
    if ($sorting -notcontains $entry) {
        $techniques = @($entry) + $techniques; $sorting = @($entry) + $sorting; $changed = $true
    }
    $entry = 'AliasIsolation_SkipIntroVideos@AliasIsolation.fx'
    if ($sorting -notcontains $entry) {
        if ((Get-IntroState $Game) -eq 'Skipped') { $techniques += $entry }
        $sorting += $entry; $changed = $true
    }
    if ($changed) {
        $presetText = Set-IniValue $presetText '' 'Techniques' ((@($techniques) | Select-Object -Unique) -join ',')
        $presetText = Set-IniValue $presetText '' 'TechniqueSorting' ($sorting -join ',')
        Write-TextFile $preset $presetText
    }
    Write-Ok "Alias Isolation is in ReShade's Home tab (open ReShade with the Home key)."
}

# --- HDR detection for the DLSS 5 pack -----------------------------------------------------------------------------

Add-Type -Namespace AliasIsolationNext -Name Display -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
static extern int GetDisplayConfigBufferSizes(uint flags, out uint paths, out uint modes);
[System.Runtime.InteropServices.DllImport("user32.dll")]
static extern int QueryDisplayConfig(uint flags, ref uint paths, System.IntPtr pathInfo, ref uint modes, System.IntPtr modeInfo, System.IntPtr topology);
[System.Runtime.InteropServices.DllImport("user32.dll")]
static extern int DisplayConfigGetDeviceInfo(System.IntPtr packet);

// One entry per active monitor: bit 0 = HDR capable, bit 1 = HDR on in Windows.
public static int[] GetAdvancedColor() {
    const uint QDC_ONLY_ACTIVE_PATHS = 2;
    const int PathSize = 72, ModeSize = 64;
    uint pathCount, modeCount;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, out pathCount, out modeCount) != 0) return new int[0];
    System.IntPtr paths = System.Runtime.InteropServices.Marshal.AllocHGlobal((int)pathCount * PathSize);
    System.IntPtr modes = System.Runtime.InteropServices.Marshal.AllocHGlobal((int)modeCount * ModeSize);
    System.IntPtr packet = System.Runtime.InteropServices.Marshal.AllocHGlobal(32);
    var result = new System.Collections.Generic.List<int>();
    try {
        if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref pathCount, paths, ref modeCount, modes, System.IntPtr.Zero) != 0) return new int[0];
        for (int i = 0; i < pathCount; i++) {
            System.IntPtr path = System.IntPtr.Add(paths, i * PathSize);
            // DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO (9): header {type, size, adapter LUID, target id}, then value.
            System.Runtime.InteropServices.Marshal.WriteInt32(packet, 0, 9);
            System.Runtime.InteropServices.Marshal.WriteInt32(packet, 4, 32);
            System.Runtime.InteropServices.Marshal.WriteInt32(packet, 8, System.Runtime.InteropServices.Marshal.ReadInt32(path, 20));
            System.Runtime.InteropServices.Marshal.WriteInt32(packet, 12, System.Runtime.InteropServices.Marshal.ReadInt32(path, 24));
            System.Runtime.InteropServices.Marshal.WriteInt32(packet, 16, System.Runtime.InteropServices.Marshal.ReadInt32(path, 28));
            if (DisplayConfigGetDeviceInfo(packet) == 0)
                result.Add(System.Runtime.InteropServices.Marshal.ReadInt32(packet, 20) & 3);
        }
    } finally {
        System.Runtime.InteropServices.Marshal.FreeHGlobal(paths);
        System.Runtime.InteropServices.Marshal.FreeHGlobal(modes);
        System.Runtime.InteropServices.Marshal.FreeHGlobal(packet);
    }
    return result.ToArray();
}
'@

# Something in the game folder that turns the game's SDR swapchain into a real HDR one.
function Get-HdrUpgrader([string]$Game) {
    foreach ($file in Get-ChildItem $Game -File -ErrorAction SilentlyContinue) {
        if ($file.Name -like 'SpecialK*.dll' -or $file.Name -like '*renodx*') { return $file.Name }
        if ($file.Extension -eq '.dll' -and (Get-ProductName $file.FullName) -like 'Special K*') { return $file.Name }
    }
    return $null
}

# Alien: Isolation only ever presents an 8-bit SDR swapchain. RTX HDR and Windows Auto HDR turn that into HDR after
# the game, but need Windows HDR on, and that is when the feeder's and OptiScaler's "auto" can take the SDR frame for
# HDR, so the neural pass looks like it does nothing. The values are therefore written out instead of left on auto:
# SDR unless something in the game folder makes the swapchain itself HDR.
function Set-HdrConfig([string]$Game) {
    $cfg = Join-Path $Game 'dlss5-feed.cfg'
    $opti = Join-Path $Game 'host64\OptiScaler.ini'
    if (-not (Test-Path $cfg) -and -not (Test-Path $opti)) { return }
    Assert-GameClosed

    $monitors = @([AliasIsolationNext.Display]::GetAdvancedColor())
    $hdrOn = @($monitors | Where-Object { $_ -band 2 }).Count -gt 0
    $hdrCapable = @($monitors | Where-Object { $_ -band 1 }).Count -gt 0
    $upgrader = Get-HdrUpgrader $Game
    $gameHdr = $hdrOn -and $upgrader

    if ($gameHdr) {
        Write-Step "Windows HDR is on and $upgrader makes the game's output HDR: the DLSS 5 pack is set to HDR."
    } elseif ($hdrOn) {
        Write-Step 'Windows HDR is on. The game itself is SDR (RTX HDR or Auto HDR convert it afterwards), so the'
        Write-Step 'DLSS 5 pack is set to SDR input. This is what keeps it working with RTX HDR.'
    } elseif ($hdrCapable) {
        Write-Step 'This monitor can do HDR but Windows HDR is off: the DLSS 5 pack is set to SDR.'
    } else {
        Write-Step 'No HDR monitor found: the DLSS 5 pack is set to SDR.'
    }

    if (Test-Path $cfg) {
        $text = Read-TextFile $cfg
        if ($gameHdr) {
            # hdr=1 sets NGX's HDR flag; the PQ bridge stays on auto since it checks the real colour space itself.
            $text = Set-IniValue $text '' 'hdr' '1'
            $text = Set-IniValue $text '' 'hdr_bridge' '-1'
        } else {
            $text = Set-IniValue $text '' 'hdr' '0'
            $text = Set-IniValue $text '' 'hdr_bridge' '0'
        }
        Write-TextFile $cfg $text
    }
    if (Test-Path $opti) {
        $backup = "$opti.aliasisolation-backup"
        if (-not (Test-Path $backup)) { Copy-Item $opti $backup }
        $text = Read-TextFile $opti
        if ($gameHdr) {
            $text = Set-IniValue $text 'InitFlags' 'HDR' 'true'
            $text = Set-IniValue $text 'HDR' 'ForceHDR' 'auto'
            $text = Set-IniValue $text 'HDR' 'UseHDR10' 'auto'
        } else {
            $text = Set-IniValue $text 'InitFlags' 'HDR' 'false'
            $text = Set-IniValue $text 'HDR' 'ForceHDR' 'false'
            $text = Set-IniValue $text 'HDR' 'UseHDR10' 'false'
        }
        Write-TextFile $opti $text
    }
    Write-Ok 'HDR settings written to dlss5-feed.cfg and host64\OptiScaler.ini.'
}

function Get-HomePageState([string]$Game) {
    if (-not (Test-ReShadeInstalled $Game)) { return 'No ReShade' }
    if (Test-Path (Join-Path (Get-ShaderDir $Game) 'AliasIsolation.fx')) { return 'Installed' }
    return 'Not installed'
}

# The DLSS 5 step asks first, and puts the Home tab page in once ReShade is there.
function Install-Everything([string]$Game) {
    Install-Alias $Game
    Set-IntroVideos $Game $true
    Install-Dlss5 $Game
}

# --- Entry point -------------------------------------------------------------------------------------------------

function Invoke-Action([string]$Name, [string]$Game) {
    switch ($Name) {
        'InstallAll' { Install-Everything $Game }
        'Install' { Install-Alias $Game }
        'HomePage' { Install-HomePage $Game; Hide-HelperTechniques (Get-ShaderDir $Game) }
        'DetectHdr' { Set-HdrConfig $Game }
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
    Write-Host '  Enter) Install everything: Alias Isolation, skip intro videos, DLSS 5 + LumeniteFX (asks first)'
    Write-Host '  1) Install or update Alias Isolation only'
    Write-Host '  2) Remove Alias Isolation'
    Write-Host '  3) Skip intro videos'
    Write-Host '  4) Restore intro videos'
    Write-Host '  5) Install or update DLSS 5 + LumeniteFX (runs the DLSS5-Feeder installer)'
    Write-Host '  6) Turn the DLSS 5 pack off'
    Write-Host '  7) Turn the DLSS 5 pack on'
    Write-Host '  8) Redo the ReShade Home tab setup (after updating ReShade or the feeder by hand)'
    Write-Host '  9) Re-detect HDR for the DLSS 5 pack (after turning Windows HDR on or off)'
    Write-Host '  Q) Quit'
    $choice = (Read-Host '  Choice').Trim()
    $map = @{ '' = 'InstallAll'; '1' = 'Install'; '2' = 'Uninstall'; '3' = 'SkipIntros'; '4' = 'RestoreIntros'
        '5' = 'InstallDlss5'; '6' = 'DisableDlss5'; '7' = 'EnableDlss5'; '8' = 'HomePage'; '9' = 'DetectHdr' }
    if ($choice -match '^[qQ]') { break }
    if (-not $map.ContainsKey($choice)) { continue }
    try { Invoke-Action $map[$choice] $game } catch { Write-Fail $_.Exception.Message }
}
