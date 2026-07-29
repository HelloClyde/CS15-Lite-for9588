[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Cstrike,

    [string]$Valve,

    [string]$Output,

    [string]$Manifest,

    [switch]$CompressWorldTextures,

    [switch]$FullWorldTextures,

    [switch]$CompressPlayerTextures,

    [switch]$AllowMissing
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

function Get-ProjectPath([string]$Path) {
    if ([IO.Path]::IsPathRooted($Path)) {
        return [IO.Path]::GetFullPath($Path)
    }
    return [IO.Path]::GetFullPath((Join-Path $projectRoot $Path))
}

$cstrikeRoot = Get-ProjectPath $Cstrike
$valveRoot = $null
if (-not [string]::IsNullOrWhiteSpace($Valve)) {
    $valveRoot = Get-ProjectPath $Valve
} else {
    $candidateValve = Join-Path (Split-Path -Parent $cstrikeRoot) 'valve'
    if (Test-Path -LiteralPath $candidateValve -PathType Container) {
        $valveRoot = [IO.Path]::GetFullPath($candidateValve)
    }
}
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = 'build\assets\CS15.C15PAK'
}
if ([string]::IsNullOrWhiteSpace($Manifest)) {
    $Manifest = 'build\assets\CS15.manifest.json'
}
$outputPath = Get-ProjectPath $Output
$manifestPath = Get-ProjectPath $Manifest
$inspectPath = [IO.Path]::ChangeExtension($manifestPath, 'inspect.json')
foreach ($required in @(
    (Join-Path $cstrikeRoot 'maps\de_dust2.bsp'),
    (Join-Path $cstrikeRoot 'maps\fy_iceworld.bsp'),
    (Join-Path $cstrikeRoot 'maps\cs_assault.bsp'),
    (Join-Path $cstrikeRoot 'maps\cs_italy.bsp'),
    (Join-Path $cstrikeRoot 'maps\de_inferno.bsp'),
    (Join-Path $cstrikeRoot 'maps\de_nuke.bsp'),
    (Join-Path $cstrikeRoot 'maps\cs_office.bsp'),
    (Join-Path $cstrikeRoot 'models\v_ak47.mdl'),
    (Join-Path $cstrikeRoot 'models\v_awp.mdl'),
    (Join-Path $cstrikeRoot 'models\v_m249.mdl'),
    (Join-Path $cstrikeRoot 'sound\weapons\ak47-1.wav')
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Original asset directory is incomplete: $required"
    }
}

New-Item -ItemType Directory -Force -Path @(
    (Split-Path -Parent $outputPath),
    (Split-Path -Parent $manifestPath)
) | Out-Null

$python = (Get-Command python -ErrorAction Stop).Source
$arguments = @(
    (Join-Path $projectRoot 'tools\assetc.py'),
    'build',
    '--cstrike', $cstrikeRoot,
    '--map', 'de_dust2',
    '--map', 'fy_iceworld',
    '--map', 'cs_assault',
    '--map', 'cs_italy',
    '--map', 'de_inferno',
    '--map', 'de_nuke',
    '--map', 'cs_office',
    '--weapon', 'v_knife',
    '--weapon', 'v_glock18',
    '--weapon', 'v_usp',
    '--weapon', 'v_p228',
    '--weapon', 'v_deagle',
    '--weapon', 'v_elite',
    '--weapon', 'v_fiveseven',
    '--weapon', 'v_m3',
    '--weapon', 'v_xm1014',
    '--weapon', 'v_mac10',
    '--weapon', 'v_tmp',
    '--weapon', 'v_mp5',
    '--weapon', 'v_ump45',
    '--weapon', 'v_p90',
    '--weapon', 'v_ak47',
    '--weapon', 'v_sg552',
    '--weapon', 'v_m4a1',
    '--weapon', 'v_aug',
    '--weapon', 'v_scout',
    '--weapon', 'v_awp',
    '--weapon', 'v_g3sg1',
    '--weapon', 'v_sg550',
    '--weapon', 'v_m249',
    '--model', 'player/terror/terror=player_terror',
    '--model', 'player/urban/urban=player_urban',
    '--model', 'p_ak47=p_ak47',
    '--model', 'p_m4a1=p_m4a1',
    '--splash',
    '--audio',
    '--output', $outputPath,
    '--manifest', $manifestPath
)
if ($valveRoot) {
    $arguments += @('--valve', $valveRoot)
    Write-Host "Half-Life WAD root: $valveRoot"
}
if ($AllowMissing) {
    $arguments += '--allow-missing'
}
if ($CompressWorldTextures) {
    $arguments += '--compress-world-textures'
}
if ($FullWorldTextures) {
    $arguments += '--full-world-textures'
}
if ($CompressPlayerTextures) {
    $arguments += '--compress-player-textures'
}

& $python @arguments
if ($LASTEXITCODE -ne 0) {
    throw 'Original asset preprocessing failed'
}
& $python (Join-Path $projectRoot 'tools\assetc.py') `
    inspect $outputPath | Out-File -LiteralPath $inspectPath -Encoding utf8
if ($LASTEXITCODE -ne 0) {
    throw 'Generated resource pack validation failed'
}

$pack = Get-Item -LiteralPath $outputPath
$hash = (
    Get-FileHash -Algorithm SHA256 -LiteralPath $pack
).Hash.ToLowerInvariant()
Write-Host "Resource pack: $outputPath"
Write-Host "Resource pack bytes: $($pack.Length)"
Write-Host "Resource pack SHA-256: $hash"
Write-Host "Resource manifest: $manifestPath"
Write-Host "Resource inspection: $inspectPath"
