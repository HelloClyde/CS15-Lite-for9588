[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string[]]$Log,

    [string]$Output
)

$ErrorActionPreference = 'Stop'

function Get-Percentile([double[]]$Values, [double]$Percentile) {
    if ($Values.Count -eq 0) { return 0.0 }
    $sorted = @($Values | Sort-Object)
    $position = ($sorted.Count - 1) * $Percentile
    $lower = [Math]::Floor($position)
    $upper = [Math]::Ceiling($position)
    if ($lower -eq $upper) { return [double]$sorted[$lower] }
    $weight = $position - $lower
    return [double]$sorted[$lower] * (1.0 - $weight) +
        [double]$sorted[$upper] * $weight
}

function Get-Average([double[]]$Values) {
    if ($Values.Count -eq 0) { return 0.0 }
    return ($Values | Measure-Object -Average).Average
}

$reports = @()
foreach ($inputPath in $Log) {
    $path = (Resolve-Path -LiteralPath $inputPath).Path
    $samples = @()
    $current = @{}
    $global = @{}
    foreach ($line in Get-Content -LiteralPath $path) {
        if ($line -notmatch '^([^=]+)=(-?[0-9]+)$') { continue }
        $name = $Matches[1]
        $value = [long]$Matches[2]
        $global[$name] = $value
        if ($name -eq 'frame' -and $current.Count -ne 0) {
            if ($current.ContainsKey('fps_x10')) {
                $samples += [pscustomobject]$current
            }
            $current = @{}
        }
        $current[$name] = $value
    }
    if ($current.ContainsKey('fps_x10')) {
        $samples += [pscustomobject]$current
    }

    $play = @(
        $samples | Where-Object {
            -not $_.PSObject.Properties['screen'] -or $_.screen -eq 5
        }
    )
    $fps = [double[]]@($play | ForEach-Object { $_.fps_x10 / 10.0 })
    $render = [double[]]@(
        $play | Where-Object {
            $_.PSObject.Properties['render_ms_x10']
        } | ForEach-Object { $_.render_ms_x10 / 10.0 }
    )
    $present = [double[]]@(
        $play | Where-Object {
            $_.PSObject.Properties['present_ms_x10']
        } | ForEach-Object { $_.present_ms_x10 / 10.0 }
    )
    $reports += [pscustomobject]@{
        log = $path
        map_id = if ($global.ContainsKey('map_id')) {
            $global['map_id']
        } else { -1 }
        samples = $fps.Count
        fps_average = [Math]::Round((Get-Average $fps), 2)
        fps_median = [Math]::Round((Get-Percentile $fps 0.5), 2)
        fps_p10 = [Math]::Round((Get-Percentile $fps 0.1), 2)
        fps_minimum = if ($fps.Count) {
            [Math]::Round(($fps | Measure-Object -Minimum).Minimum, 2)
        } else { 0.0 }
        fps_maximum = if ($fps.Count) {
            [Math]::Round(($fps | Measure-Object -Maximum).Maximum, 2)
        } else { 0.0 }
        render_ms_average = [Math]::Round((Get-Average $render), 2)
        present_ms_average = [Math]::Round((Get-Average $present), 2)
        map_peak_bytes = if ($global.ContainsKey('map_peak_bytes')) {
            $global['map_peak_bytes']
        } else { 0 }
        texture_peak_bytes = if (
            $global.ContainsKey('texture_peak_bytes')
        ) { $global['texture_peak_bytes'] } else { 0 }
        model_peak_bytes = if ($global.ContainsKey('model_peak_bytes')) {
            $global['model_peak_bytes']
        } else { 0 }
        audio_short_writes = if (
            $global.ContainsKey('audio_short_writes')
        ) { $global['audio_short_writes'] } else { 0 }
    }
}

$json = $reports | ConvertTo-Json -Depth 4
if ($Output) {
    $target = [IO.Path]::GetFullPath($Output)
    New-Item -ItemType Directory -Force -Path (
        Split-Path -Parent $target
    ) | Out-Null
    Set-Content -LiteralPath $target -Value $json -Encoding utf8
    Write-Host "Performance report: $target"
}
$reports | Format-Table -AutoSize
