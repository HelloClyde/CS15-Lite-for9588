[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^v[0-9A-Za-z._-]+$')]
    [string]$Tag,

    [switch]$RequireResourcePack
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = Join-Path $projectRoot 'build'
$releaseRoot = Join-Path $buildRoot 'release'
$stagingRoot = Join-Path $releaseRoot 'staging'
$bdaSource = Join-Path $buildRoot 'engine\CS Lite.bda'
$packSource = Join-Path $buildRoot 'assets\CS15.C15PAK'

if (-not (Test-Path -LiteralPath $bdaSource -PathType Leaf)) {
    throw "Build the BDA first: $bdaSource"
}
if ($RequireResourcePack -and -not (
        Test-Path -LiteralPath $packSource -PathType Leaf
    )) {
    throw "Build the resource pack first: $packSource"
}
if (Test-Path -LiteralPath $releaseRoot) {
    $resolved = [IO.Path]::GetFullPath($releaseRoot)
    $expectedPrefix = [IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith(
        $expectedPrefix, [StringComparison]::OrdinalIgnoreCase
    )) {
        throw "Refusing to clean outside build: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

$runtimeStage = Join-Path $stagingRoot 'runtime'
$programDirectory = Join-Path $runtimeStage '应用\程序'
$dataDirectory = Join-Path $runtimeStage '应用\数据\CS15LITE'
$resourceStage = Join-Path $stagingRoot 'resource-tools'
New-Item -ItemType Directory -Force -Path @(
    $releaseRoot,
    $programDirectory,
    $dataDirectory,
    (Join-Path $resourceStage 'tools'),
    (Join-Path $resourceStage 'docs')
) | Out-Null

$releaseBda = Join-Path $releaseRoot 'CS15Lite.bda'
Copy-Item -LiteralPath $bdaSource -Destination $releaseBda
Copy-Item -LiteralPath $bdaSource -Destination (
    Join-Path $programDirectory 'CS15Lite.bda'
)
Copy-Item -LiteralPath (Join-Path $projectRoot 'RESOURCE_PACK.md') `
    -Destination (Join-Path $dataDirectory 'RESOURCE-PACK.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') `
    -Destination (Join-Path $runtimeStage 'LICENSE')
Copy-Item -LiteralPath (Join-Path $projectRoot 'NOTICE.md') `
    -Destination (Join-Path $runtimeStage 'NOTICE.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'tools\assetc.py') `
    -Destination (Join-Path $resourceStage 'tools\assetc.py')
Copy-Item `
    -LiteralPath (Join-Path $projectRoot 'tools\build-resource-pack.ps1') `
    -Destination (Join-Path $resourceStage 'tools\build-resource-pack.ps1')
Copy-Item -LiteralPath (Join-Path $projectRoot 'docs\formats.md') `
    -Destination (Join-Path $resourceStage 'docs\formats.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'RESOURCE_PACK.md') `
    -Destination (Join-Path $resourceStage 'RESOURCE_PACK.md')
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') `
    -Destination (Join-Path $resourceStage 'LICENSE')
Copy-Item -LiteralPath (Join-Path $projectRoot 'NOTICE.md') `
    -Destination (Join-Path $resourceStage 'NOTICE.md')

$packAvailable = Test-Path -LiteralPath $packSource -PathType Leaf
if ($packAvailable) {
    $releasePack = Join-Path $releaseRoot 'CS15.C15PAK'
    Copy-Item -LiteralPath $packSource -Destination $releasePack
    Copy-Item -LiteralPath $packSource `
        -Destination (Join-Path $dataDirectory 'CS15.C15PAK')
    $packHash = (
        Get-FileHash -Algorithm SHA256 -LiteralPath $releasePack
    ).Hash.ToLowerInvariant()
    Set-Content `
        -LiteralPath (Join-Path $releaseRoot 'CS15.C15PAK.sha256') `
        -Encoding ascii -Value "$packHash  CS15.C15PAK"
}

$runtimeZip = Join-Path $releaseRoot "CS15-Lite-for-9588-$Tag.zip"
$resourceZip = Join-Path $releaseRoot "CS15-Lite-resource-tools-$Tag.zip"
Compress-Archive -Path (Join-Path $runtimeStage '*') `
    -DestinationPath $runtimeZip -CompressionLevel Optimal
Compress-Archive -Path (Join-Path $resourceStage '*') `
    -DestinationPath $resourceZip -CompressionLevel Optimal

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($runtimeZip)
try {
    $entryNames = @($archive.Entries | ForEach-Object FullName)
    $requiredEntries = @('应用/程序/CS15Lite.bda')
    if ($packAvailable) {
        $requiredEntries += '应用/数据/CS15LITE/CS15.C15PAK'
    }
    foreach ($requiredEntry in $requiredEntries) {
        if ($entryNames -notcontains $requiredEntry) {
            throw "Install ZIP is missing: $requiredEntry"
        }
    }
} finally {
    $archive.Dispose()
}

$hashPath = Join-Path $releaseRoot 'CS15Lite.bda.sha256'
$hash = (
    Get-FileHash -Algorithm SHA256 -LiteralPath $releaseBda
).Hash.ToLowerInvariant()
Set-Content -LiteralPath $hashPath -Encoding ascii `
    -Value "$hash  CS15Lite.bda"

Write-Host "Release directory: $releaseRoot"
Get-ChildItem -LiteralPath $releaseRoot -File |
    Select-Object Name, Length
