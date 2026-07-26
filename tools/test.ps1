[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = Join-Path $projectRoot 'build\host-tests'
New-Item -ItemType Directory -Force -Path $buildRoot | Out-Null

$compiler = Get-Command gcc -ErrorAction SilentlyContinue
if ($null -eq $compiler) {
    $compiler = Get-Command clang -ErrorAction SilentlyContinue
}
if ($null -eq $compiler) {
    throw 'Host tests require gcc or clang'
}
$hostCompiler = $compiler.Source
$tests = @(
    @{
        Name = 'memory_test'
        Sources = @(
            (Join-Path $projectRoot 'tests\memory_test.c'),
            (Join-Path $projectRoot 'src\core\memory.c')
        )
    },
    @{
        Name = 'framebuffer_test'
        Sources = @(
            (Join-Path $projectRoot 'tests\framebuffer_test.c'),
            (Join-Path $projectRoot 'src\render\framebuffer.c')
        )
    },
    @{
        Name = 'display_test'
        Sources = @(
            (Join-Path $projectRoot 'tests\display_test.c'),
            (Join-Path $projectRoot 'src\platform\display.c')
        )
    },
    @{
        Name = 'movement_test'
        Sources = @(
            (Join-Path $projectRoot 'tests\movement_test.c'),
            (Join-Path $projectRoot 'src\world\movement.c')
        )
    },
    @{
        Name = 'map_contents_test'
        Sources = @(
            (Join-Path $projectRoot 'tests\map_contents_test.c'),
            (Join-Path $projectRoot 'src\world\map.c')
        )
    }
)

foreach ($test in $tests) {
    $output = Join-Path $buildRoot "$($test.Name).exe"
    $arguments = @(
        '-std=c11', '-Wall', '-Wextra', '-Werror',
        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
        '-I', (Join-Path $projectRoot 'tests\include'),
        '-I', (Join-Path $projectRoot 'src'),
        '-o', $output
    ) + $test.Sources
    & $hostCompiler @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Host compile failed: $($test.Name)"
    }
    & $output
    if ($LASTEXITCODE -ne 0) {
        throw "Host test failed: $($test.Name)"
    }
}

$python = (Get-Command python -ErrorAction Stop).Source
& $python -m unittest discover -s (Join-Path $projectRoot 'tests') -p 'test_*.py' -v
if ($LASTEXITCODE -ne 0) {
    throw 'Python asset-format tests failed'
}

Write-Host "PASS: $($tests.Count) C host tests and Python asset-format tests"
