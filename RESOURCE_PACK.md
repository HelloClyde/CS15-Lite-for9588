# Building `CS15.C15PAK`

`CS15.C15PAK` contains converted Counter-Strike/Half-Life data. The source
repository never tracks it. A matching pack may be attached manually to a
private release for repository members who are authorized to use the source
resources. Do not redistribute the pack or expose it through a public release.
You can also build it locally from resources you are authorized to use.

## Required sources

- a Counter-Strike 1.5 `cstrike` directory;
- the matching Half-Life `valve` directory for WAD textures referenced by
  `de_dust`;
- an authorized `fy_iceworld.bsp` if you want the Iceworld menu entry.

The converter reads the inputs but never modifies them.

## Build

From the repository root:

```powershell
$cstrike = 'D:\Games\Counter-Strike-1.5\cstrike'
$valve = 'D:\Games\Counter-Strike-1.5\valve'
$iceworld = 'D:\Games\Counter-Strike-1.5\cstrike\maps\fy_iceworld.bsp'

python .\tools\assetc.py build `
  --cstrike $cstrike `
  --valve $valve `
  --map de_dust `
  --map de_dust2 `
  --map "fy_iceworld=$iceworld" `
  --weapon v_knife --weapon v_glock18 --weapon v_ak47 `
  --weapon v_m4a1 --weapon v_usp `
  --model player/terror/terror=player_terror `
  --model player/urban/urban=player_urban `
  --model p_ak47=p_ak47 --model p_m4a1=p_m4a1 `
  --splash `
  --audio `
  --output .\build\assets\CS15.C15PAK `
  --manifest .\build\assets\CS15.manifest.json
```

If your legally owned installation is missing a referenced WAD texture,
`--allow-missing` can produce a development pack with marked placeholders.
Do not use that switch for a fidelity release when the matching WAD is
available.

If `fy_iceworld.bsp` is unavailable, omit its `--map` argument. The other two
maps will work, but selecting Iceworld will be rejected by resource preflight.

## Validate

```powershell
python .\tools\assetc.py inspect .\build\assets\CS15.C15PAK
```

The generated manifest records source CRCs, resident memory estimates, pack
entry count and SHA-256.

## Install

Create this exact directory on the device and copy the generated pack:

```text
A:\应用\数据\CS15LITE\CS15.C15PAK
```

The BDA and pack must use the same runtime resource revision. An outdated
pack is stopped before map loading.
