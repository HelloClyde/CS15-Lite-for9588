[CmdletBinding()]
param(
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdkRoot = Join-Path $projectRoot 'sdk'
$buildRoot = Join-Path $projectRoot 'build\engine'
$objectRoot = Join-Path $buildRoot 'obj'
$staticLimit = 1536u * 1024u

foreach ($required in @(
    (Join-Path $projectRoot 'src\runtime\entry.S'),
    (Join-Path $projectRoot 'linker\bda.ld'),
    (Join-Path $projectRoot 'tools\pack_bda.py'),
    (Join-Path $sdkRoot 'sdk\include\bda_types.h')
)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "BBK 9588 SDK submodule is incomplete: $required"
    }
}

function Find-CrossTool([string]$Name) {
    $toolchain = Join-Path $sdkRoot '.toolchain'
    $candidates = @(
        (Join-Path $toolchain "bin\mipsel-none-elf-$Name.exe"),
        (Join-Path $toolchain "g++-mipsel-none-elf-15.2.0\bin\mipsel-none-elf-$Name.exe")
    )
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    throw "Cross tool not found: mipsel-none-elf-$Name.exe"
}

function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $Executable $($Arguments -join ' ')"
    }
}

if ($Clean -and (Test-Path -LiteralPath $buildRoot)) {
    $resolved = [IO.Path]::GetFullPath($buildRoot)
    if (-not $resolved.StartsWith(
        $projectRoot.TrimEnd('\') + '\',
        [StringComparison]::OrdinalIgnoreCase
    )) {
        throw "Refusing to clean outside project: $resolved"
    }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $objectRoot | Out-Null

$gcc = Find-CrossTool 'gcc'
$objcopy = Find-CrossTool 'objcopy'
$objdump = Find-CrossTool 'objdump'
$sizeTool = Find-CrossTool 'size'
$python = (Get-Command python -ErrorAction Stop).Source

$sources = @(
    (Join-Path $projectRoot 'src\runtime\entry.S'),
    (Join-Path $projectRoot 'src\runtime\startup.c'),
    (Join-Path $projectRoot 'src\core\memory.c'),
    (Join-Path $projectRoot 'src\core\log.c'),
    (Join-Path $projectRoot 'src\core\runtime.c'),
    (Join-Path $projectRoot 'src\assets\pak.c'),
    (Join-Path $projectRoot 'src\audio\sound.c'),
    (Join-Path $projectRoot 'src\model\model.c'),
    (Join-Path $projectRoot 'src\platform\display.c'),
    (Join-Path $projectRoot 'src\platform\bbk9588.c'),
    (Join-Path $projectRoot 'src\render\framebuffer.c'),
    (Join-Path $projectRoot 'src\render\model.c'),
    (Join-Path $projectRoot 'src\render\world.c'),
    (Join-Path $projectRoot 'src\world\map.c'),
    (Join-Path $projectRoot 'src\world\movement.c'),
    (Join-Path $projectRoot 'src\app\main.c')
)
$includes = @(
    (Join-Path $projectRoot 'src'),
    (Join-Path $sdkRoot 'sdk\include')
)
$common = @(
    '-EL', '-march=mips32', '-msoft-float', '-mno-abicalls', '-G0',
    '-fno-pic', '-Os', '-ffreestanding', '-fno-builtin',
    '-ffunction-sections', '-fdata-sections', '-fno-strict-aliasing',
    '-std=gnu11', '-Wall', '-Wextra', '-Werror',
    '-DNDEBUG'
)
foreach ($include in $includes) {
    $common += @('-I', $include)
}

$objects = @()
foreach ($source in $sources) {
    $relative = [IO.Path]::GetRelativePath($projectRoot, $source)
    $safeName = ($relative -replace '[:\\/\.]', '_') + '.o'
    $object = Join-Path $objectRoot $safeName
    $language = if ([IO.Path]::GetExtension($source) -ceq '.S') {
        @('-x', 'assembler-with-cpp')
    } else {
        @()
    }
    Write-Host "CC $relative"
    Invoke-Checked $gcc ($common + $language + @('-c', $source, '-o', $object))
    $objects += $object
}

$elf = Join-Path $buildRoot 'cs15lite9588.elf'
$raw = Join-Path $buildRoot 'cs15lite9588.bin'
$map = Join-Path $buildRoot 'cs15lite9588.map'
$dump = Join-Path $buildRoot 'cs15lite9588.dump.txt'
$bda = Join-Path $buildRoot 'CS Lite.bda'
$icon = Join-Path $projectRoot 'assets\cs-lite-icon.png'
$linker = Join-Path $projectRoot 'linker\bda.ld'

$linkArguments = @(
    '-EL', '-march=mips32', '-msoft-float', '-mno-abicalls', '-G0',
    '-fno-pic', '-nostdlib',
    '-Wl,--build-id=none', '-Wl,--gc-sections',
    "-Wl,-T,$linker", "-Wl,-Map,$map",
    '-o', $elf
)
Invoke-Checked $gcc ($linkArguments + $objects + @('-lgcc'))
Invoke-Checked $objcopy @('-O', 'binary', $elf, $raw)
& $objdump -d -h $elf | Out-File -LiteralPath $dump -Encoding ascii
if ($LASTEXITCODE -ne 0) {
    throw "objdump failed with exit code $LASTEXITCODE"
}

$sizeOutput = (& $sizeTool $elf | Out-String).Trim()
if ($LASTEXITCODE -ne 0) {
    throw "size failed with exit code $LASTEXITCODE"
}
Write-Host $sizeOutput
$sizeLine = ($sizeOutput -split "`r?`n")[-1].Trim()
$fields = $sizeLine -split '\s+'
if ($fields.Count -lt 4) {
    throw "Cannot parse size output: $sizeLine"
}
$staticBytes = [uint64]$fields[3]
if ($staticBytes -gt $staticLimit) {
    throw "CS Lite static budget exceeded: $staticBytes > $staticLimit bytes"
}

Invoke-Checked $python @(
    (Join-Path $projectRoot 'tools\pack_bda.py'),
    $raw,
    '--sdk', $sdkRoot,
    '--title', 'CS Lite',
    '--category', '4',
    '--icon', $icon,
    '--output', $bda
)

$hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $bda).Hash.ToLowerInvariant()
$item = Get-Item -LiteralPath $bda
Write-Host "CS Lite static budget: $staticBytes / $staticLimit bytes"
Write-Host "ELF: $elf"
Write-Host "BDA: $bda bytes=$($item.Length) sha256=$hash"
