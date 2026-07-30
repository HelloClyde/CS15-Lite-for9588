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

function Get-MetricValues(
    [object[]]$Samples, [string]$Name, [double]$Scale = 1.0
) {
    return [double[]]@(
        $Samples | ForEach-Object {
            $property = $_.PSObject.Properties[$Name]
            if ($property) {
                [double]$property.Value / $Scale
            }
        }
    )
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
            -not $_.PSObject.Properties['screen'] -or $_.screen -eq 6
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
    $logicPhase = Get-MetricValues $play 'logic_avg_ms_x10' 10.0
    $logicFirePhase =
        Get-MetricValues $play 'logic_fire_avg_ms_x10' 10.0
    $logicPlayerPhase =
        Get-MetricValues $play 'logic_player_avg_ms_x10' 10.0
    $logicBotPhase =
        Get-MetricValues $play 'logic_bot_avg_ms_x10' 10.0
    $logicObjectivePhase =
        Get-MetricValues $play 'logic_objective_avg_ms_x10' 10.0
    $logicSteps = Get-MetricValues $play 'logic_steps_avg_x10' 10.0
    $audioPhase = Get-MetricValues $play 'audio_avg_ms_x10' 10.0
    $worldPhase = Get-MetricValues $play 'world_avg_ms_x10' 10.0
    $worldClearPhase =
        Get-MetricValues $play 'world_clear_avg_ms_x10' 10.0
    $entityPhase = Get-MetricValues $play 'entities_avg_ms_x10' 10.0
    $viewPhase = Get-MetricValues $play 'view_avg_ms_x10' 10.0
    $hudPhase = Get-MetricValues $play 'hud_avg_ms_x10' 10.0
    $presentPhase = Get-MetricValues $play 'present_avg_ms_x10' 10.0
    $logicMaximum = Get-MetricValues $play 'logic_max_ms'
    $logicFireMaximum = Get-MetricValues $play 'logic_fire_max_ms'
    $logicPlayerMaximum =
        Get-MetricValues $play 'logic_player_max_ms'
    $logicBotMaximum = Get-MetricValues $play 'logic_bot_max_ms'
    $logicObjectiveMaximum =
        Get-MetricValues $play 'logic_objective_max_ms'
    $audioMaximum = Get-MetricValues $play 'audio_max_ms'
    $worldMaximum = Get-MetricValues $play 'world_max_ms'
    $worldClearMaximum =
        Get-MetricValues $play 'world_clear_max_ms'
    $entityMaximum = Get-MetricValues $play 'entities_max_ms'
    $viewMaximum = Get-MetricValues $play 'view_max_ms'
    $hudMaximum = Get-MetricValues $play 'hud_max_ms'
    $presentMaximum = Get-MetricValues $play 'present_max_ms'
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
        logic_phase_ms_average =
            [Math]::Round((Get-Average $logicPhase), 2)
        logic_phase_ms_maximum = if ($logicMaximum.Count) {
            ($logicMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        logic_fire_ms_average =
            [Math]::Round((Get-Average $logicFirePhase), 2)
        logic_fire_ms_maximum = if ($logicFireMaximum.Count) {
            ($logicFireMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        logic_player_ms_average =
            [Math]::Round((Get-Average $logicPlayerPhase), 2)
        logic_player_ms_maximum = if ($logicPlayerMaximum.Count) {
            ($logicPlayerMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        logic_bot_ms_average =
            [Math]::Round((Get-Average $logicBotPhase), 2)
        logic_bot_ms_maximum = if ($logicBotMaximum.Count) {
            ($logicBotMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        logic_objective_ms_average =
            [Math]::Round((Get-Average $logicObjectivePhase), 2)
        logic_objective_ms_maximum = if (
            $logicObjectiveMaximum.Count
        ) {
            ($logicObjectiveMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        logic_steps_average =
            [Math]::Round((Get-Average $logicSteps), 2)
        logic_skipped_steps = if (
            $global.ContainsKey('logic_skipped_steps')
        ) { $global['logic_skipped_steps'] } else { 0 }
        audio_phase_ms_average =
            [Math]::Round((Get-Average $audioPhase), 2)
        audio_phase_ms_maximum = if ($audioMaximum.Count) {
            ($audioMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        world_phase_ms_average =
            [Math]::Round((Get-Average $worldPhase), 2)
        world_phase_ms_maximum = if ($worldMaximum.Count) {
            ($worldMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        world_clear_ms_average =
            [Math]::Round((Get-Average $worldClearPhase), 2)
        world_clear_ms_maximum = if ($worldClearMaximum.Count) {
            ($worldClearMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        entity_phase_ms_average =
            [Math]::Round((Get-Average $entityPhase), 2)
        entity_phase_ms_maximum = if ($entityMaximum.Count) {
            ($entityMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        view_phase_ms_average =
            [Math]::Round((Get-Average $viewPhase), 2)
        view_phase_ms_maximum = if ($viewMaximum.Count) {
            ($viewMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        hud_phase_ms_average =
            [Math]::Round((Get-Average $hudPhase), 2)
        hud_phase_ms_maximum = if ($hudMaximum.Count) {
            ($hudMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        present_phase_ms_average =
            [Math]::Round((Get-Average $presentPhase), 2)
        present_phase_ms_maximum = if ($presentMaximum.Count) {
            ($presentMaximum | Measure-Object -Maximum).Maximum
        } else { 0 }
        map_peak_bytes = if ($global.ContainsKey('map_peak_bytes')) {
            $global['map_peak_bytes']
        } else { 0 }
        texture_peak_bytes = if (
            $global.ContainsKey('texture_peak_bytes')
        ) { $global['texture_peak_bytes'] } else { 0 }
        model_peak_bytes = if ($global.ContainsKey('model_peak_bytes')) {
            $global['model_peak_bytes']
        } else { 0 }
        animation_peak_bytes = if (
            $global.ContainsKey('animation_peak_bytes')
        ) { $global['animation_peak_bytes'] } else { 0 }
        view_cache_resident_bytes = if (
            $global.ContainsKey('view_cache_resident_bytes')
        ) { $global['view_cache_resident_bytes'] } else { 0 }
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
