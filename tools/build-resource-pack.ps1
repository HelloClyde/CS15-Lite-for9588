[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Cstrike,

    [string]$Output,

    [string]$Manifest,

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
if ([string]::IsNullOrWhiteSpace($Output)) {
    $Output = 'build\assets\CS15.C15PAK'
}
if ([string]::IsNullOrWhiteSpace($Manifest)) {
    $Manifest = 'build\assets\CS15.manifest.json'
}
$outputPath = Get-ProjectPath $Output
$manifestPath = Get-ProjectPath $Manifest
$inspectPath = [IO.Path]::ChangeExtension($manifestPath, 'inspect.json')
$iceworld = Join-Path $cstrikeRoot 'maps\fy_iceworld.bsp'

foreach ($required in @(
    (Join-Path $cstrikeRoot 'maps\de_dust.bsp'),
    (Join-Path $cstrikeRoot 'maps\de_dust2.bsp'),
    $iceworld,
    (Join-Path $cstrikeRoot 'models\v_ak47.mdl'),
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
    '--map', 'de_dust',
    '--map', 'de_dust2',
    '--map', "fy_iceworld=$iceworld",
    '--weapon', 'v_knife',
    '--weapon', 'v_glock18',
    '--weapon', 'v_ak47',
    '--weapon', 'v_m4a1',
    '--weapon', 'v_usp',
    '--model', 'player/terror/terror=player_terror',
    '--model', 'player/urban/urban=player_urban',
    '--model', 'p_ak47=p_ak47',
    '--model', 'p_m4a1=p_m4a1',
    '--splash',
    '--audio',
    '--output', $outputPath,
    '--manifest', $manifestPath
)
if ($AllowMissing) {
    $arguments += '--allow-missing'
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
