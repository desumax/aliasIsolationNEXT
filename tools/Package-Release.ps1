<#
.SYNOPSIS
    Assembles the release archive from three CMake build folders (ALIASISOLATION_CPU = V3, AVX1 and SSE3).

.EXAMPLE
    .\tools\Package-Release.ps1 -V3Build ..\build\out -Avx1Build ..\build\out-avx1 -Sse3Build ..\build\out-sse3 `
        -AsiLoader ..\third_party\d3d11.dll -OutDir ..\release
#>
param(
    [Parameter(Mandatory)] [string]$V3Build,
    [Parameter(Mandatory)] [string]$Avx1Build,
    [Parameter(Mandatory)] [string]$Sse3Build,
    # Ultimate ASI Loader, 32-bit, named d3d11.dll (https://github.com/ThirteenAG/Ultimate-ASI-Loader, MIT).
    [Parameter(Mandatory)] [string]$AsiLoader,
    [Parameter(Mandatory)] [string]$OutDir,
    [string]$Version = '1.3.0-fixes'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$name = "aliasIsolationNEXT-DLSS5-$Version"
$stage = Join-Path $OutDir $name
$files = Join-Path $stage 'files'

if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
$shaders = Join-Path $files 'mods\aliasIsolation\data\shaders'
$textures = Join-Path $files 'mods\aliasIsolation\data\textures'
New-Item -ItemType Directory -Force $shaders, $textures | Out-Null

Copy-Item (Join-Path $V3Build 'src\dll\aliasIsolation.asi') $files
foreach ($variant in @(@('AVX1', $Avx1Build), @('SSE3', $Sse3Build))) {
    $target = Join-Path $files "legacy\$($variant[0])"
    New-Item -ItemType Directory -Force $target | Out-Null
    Copy-Item (Join-Path $variant[1] 'src\dll\aliasIsolation.asi') $target
}
Copy-Item $AsiLoader (Join-Path $files 'd3d11.dll')

Copy-Item (Join-Path $V3Build '*.cso') $shaders
# The TAA compute shader is only in the repository as compiled bytecode (under a .hlsl name).
Copy-Item (Join-Path $repo 'data\shaders\taa_cs.hlsl') (Join-Path $shaders 'taa_cs.cso')
Copy-Item (Join-Path $repo 'data\textures\aliasIsolationLogo.png') $textures
$reshadeShaders = Join-Path $files 'reshade-shaders\Shaders'
New-Item -ItemType Directory -Force $reshadeShaders | Out-Null
Copy-Item (Join-Path $repo 'data\reshade\AliasIsolation.fx') $reshadeShaders

Copy-Item (Join-Path $PSScriptRoot 'Install-aliasIsolationNEXT-DLSS5.ps1') $stage
Copy-Item (Join-Path $PSScriptRoot 'Install.bat') $stage
foreach ($doc in 'README.md', 'CHANGELOG.md', 'LICENSE') { Copy-Item (Join-Path $repo $doc) $stage }

$zip = Join-Path $OutDir "$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path $stage -DestinationPath $zip
Get-Item $zip | Select-Object FullName, Length
