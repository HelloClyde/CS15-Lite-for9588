# Building `CS15.C15PAK`

`CS15.C15PAK` contains converted Counter-Strike/Half-Life data. The source
repository never tracks it. Private tag builds generate a matching pack for
repository members who are authorized to use the source resources. Do not
redistribute the pack or expose it through a public release. You can also build
it locally from resources you are authorized to use.

The private `v0.1.1` release also stores
`CS15-original-cstrike-assets.zip`. It contains the `cstrike` directory used
by CI, including `maps/fy_iceworld.bsp`, plus a SHA-256 sidecar. Tag builds
download and verify this archive before preprocessing any assets.

## Required sources

- a Counter-Strike 1.5 `cstrike` directory;
- an authorized `fy_iceworld.bsp` under `cstrike\maps`;
- optionally, the matching Half-Life `valve` directory for every WAD texture
  referenced by `de_dust`.

The converter reads the inputs but never modifies them.

## Build

From the repository root:

```powershell
$cstrike = 'D:\Games\Counter-Strike-1.5\cstrike'
.\tools\build-resource-pack.ps1 `
  -Cstrike $cstrike `
  -AllowMissing
```

The archived historical `cstrike` tree lacks three Half-Life WAD textures, so
CI intentionally uses `-AllowMissing`. This produces the same three marked
16x16 placeholders as the current M11 pack and keeps the output deterministic.
If you have the matching `valve` WAD directory and want to replace those
placeholders, invoke `assetc.py` directly with `--valve` and omit
`--allow-missing`; the resulting pack hash will differ.

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
