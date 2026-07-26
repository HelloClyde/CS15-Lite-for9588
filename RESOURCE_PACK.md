# Building `CS15.C15PAK`

`CS15.C15PAK` contains converted Counter-Strike/Half-Life data. The source
repository never tracks it. Private tag builds generate a matching pack for
repository members who are authorized to use the source resources. Do not
redistribute the pack or expose it through a public release. You can also build
it locally from resources you are authorized to use.

The source archive consumed by final M12 tag builds must include its SHA-256
sidecar and use this layout:

```text
cstrike\...
valve\halflife.wad
```

## Required sources

- a Counter-Strike 1.5 `cstrike` directory containing `de_dust2.bsp`,
  `cs_assault.bsp`, `cs_italy.bsp`, the 23 selected `v_*.mdl` files and their
  weapon sounds;
- the matching Half-Life `valve` directory containing `halflife.wad`.

The converter reads the inputs but never modifies them.

## Build

From the repository root:

```powershell
$cstrike = 'D:\Games\Counter-Strike-1.5\cstrike'
$valve = 'D:\Games\Half-Life\valve'
.\tools\build-resource-pack.ps1 `
  -Cstrike $cstrike `
  -Valve $valve
```

If `valve` is next to `cstrike`, the PowerShell wrapper discovers it
automatically. `cs_assault` uses many Half-Life base textures, so a final M12
pack must include `halflife.wad`. `-AllowMissing` remains available only for
converter development; it emits visible 16x16 placeholders and must not be
used for a release acceptance build.

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
