# CS15 Lite binary formats

All multibyte fields are little-endian. Every file and chunk has an explicit
version and is validated before the target runtime uses offsets from it.

## `C15PAK` version 2

The package joins the converted map and its textures into one file so the
9588 FAT layer can read large contiguous ranges instead of opening hundreds
of small files.

The 64-byte header is:

```text
char     magic[8]        "C15PAK1\0"
uint32   version         2
uint32   endian          0x12345678
uint32   file_size
uint32   entry_count
uint32   directory_ofs
uint32   data_ofs
uint8    reserved[32]
```

Each 64-byte directory entry is:

```text
char     type[4]         BSP0, TEX0, MDL0, SND0, ...
uint32   flags
uint32   asset_id        FNV-1a of normalized name
uint32   data_ofs        512-byte aligned
uint32   packed_size
uint32   unpacked_size
uint32   crc32
char     name[32]        lower-case ASCII
uint32   reserved
```

Version 2 stores chunks uncompressed and requires directory entries to be
strictly sorted by `asset_id`. The 9588 keeps only the 20-byte package handle
and binary-searches the on-disk directory, instead of retaining the complete
records in RAM. The target rejects unsorted/duplicate IDs, overlapping chunks,
out-of-file offsets, bad CRCs, unsupported compression, or a file-size
mismatch.

## `C15BSP` version 3

The map chunk contains a 64-byte header followed by a 32-byte section table.
Sections are independently aligned to 16 bytes and carry CRC-32 values.

Header:

```text
char     magic[8]        "C15BSP1\0"
uint32   version         3
uint32   section_count
uint32   file_size
uint32   source_crc32
uint32   flags
int16    bounds[6]       mins xyz, maxs xyz
uint8    reserved[24]
```

Section record:

```text
char     type[4]
uint32   data_ofs
uint32   data_size
uint32   element_count
uint16   element_stride
uint16   flags
uint32   crc32
uint8    reserved[8]
```

Version 3 sections:

| Type | Element | Purpose |
|---|---|---|
| `VERT` | 10 bytes | Per-surface `x,y,z,u,v` signed 16-bit values; UV is Q12.4 |
| `SURF` | 16 bytes | Vertex range, texture, plane, flags, average light |
| `PLAN` | 16 bytes | Q1.14 normal, Q28.4 distance, type and sign bits |
| `NODE` | 8 bytes | BSP plane and two signed child indices |
| `LEAF` | 12 bytes | Contents, PVS offset and marksurface range |
| `MARK` | 2 bytes | Surface index referenced by a leaf |
| `VISI` | byte stream | Original GoldSrc compressed PVS; section flag 1 means streamed |
| `CLIP` | 8 bytes | Collision plane and two children |
| `MODL` | 48 bytes | Bounds, origin, hull heads and surface range |
| `TNAM` | 16 bytes | Texture name in GoldSrc miptex order |
| `SPWN` | 10 bytes | Player origin, yaw, and T/CT team identifier |
| `BSIT` | 6 bytes | Bomb-site center XYZ extracted from BSP entities |
| `HSTG` | 6 bytes | Hostage spawn XYZ |
| `RSQZ` | 14 bytes | Hostage rescue AABB, team byte and reserved byte |
| `BYZN` | 14 bytes | Buy-zone AABB, team byte and reserved byte |
| `LADR` | 14 bytes | Ladder AABB, zero team and reserved byte |
| `DENT` | 24 bytes | Door/button/breakable/platform kind, bounds, model and target hashes |

World positions are rounded to 1 GoldSrc map unit. This supports the historical
CS maps while avoiding target floating-point transforms. Texture coordinates
are converted to the selected authored mip and stored with four fractional
bits. Each surface can be drawn as a triangle fan.

`SURF` layout:

```text
uint32 first_vertex
uint16 vertex_count
uint16 texture_id
uint16 plane_id
uint16 flags
uint8  average_light
uint8  light_style
uint16 reserved
```

Surface flag `0x8000` records GoldSrc `SURF_PLANEBACK`; the remaining bits
come from BSP texinfo flags.

`SPWN` uses three signed 16-bit map coordinates, signed 16-bit yaw degrees,
an 8-bit team (`1` T, `2` CT), and one reserved byte.

`BSIT` and `HSTG` use three signed 16-bit map coordinates. A brush entity
without an explicit origin uses the bounds of its referenced BSP submodel.
Zone records store six signed 16-bit bounds followed by team and reserved
bytes. `DENT` stores kind/flags/model, the same six bounds, and 32-bit FNV-1a
hashes for `target` and `targetname`. Version 3 retains the version-2 compact
node/leaf layout and streams only the compressed PVS row needed after a
camera-leaf change.

## `C15TEX` version 1

Each texture remains indexed in target memory:

```text
char     magic[4]        "CTX1"
uint16   width
uint16   height
uint16   flags           bit 0: last palette index is transparent
uint16   palette_count   64 or 256
uint32   pixel_bytes     width * height
uint16   source_width
uint16   source_height
uint8    selected_mip
uint8    reserved[3]
uint16   palette[]       preconverted RGB565
uint8    pixels[]
```

World textures keep the authored GoldSrc mip level 0 and all 256 RGB565
palette entries by default. `--compress-world-textures` is an explicit
low-memory option that selects an authored mip no larger than 32 pixels and
quantizes it to 64 colours. Model texture limits remain selected per asset
(16 for player skins, 32 for view models, at most 64 for held models) and
retain 256 colors. Runtime memory is therefore
`width*height + palette_count*2`, with no RGBA expansion or runtime
resampling. Large maps prefetch front-facing `TEX0` entries referenced by the
current PVS into a bounded cache and page an actually drawn material on
demand when the conservative set is larger than the cache.

## `C15MDL` version 1

The model chunk is an offline-compiled base-pose subset of GoldSrc
StudioMDL. The host compiler samples frame zero of the first inline `idle*`
sequence, evaluates its bone transforms, selects body parts, expands
strips/fans, and resamples textures. The 9588 therefore loads a flat mesh and
never evaluates floating-point bones. First-person motion is stored separately
in the matching `C15ANM` chunk.

Header:

```text
char     magic[8]        "C15MDL1\0"
uint32   version         1
uint32   file_size
uint32   source_crc32
uint32   vertex_count
uint32   triangle_count
uint32   texture_count
uint32   vertex_ofs
uint32   triangle_ofs
uint32   texture_ref_ofs
int16    bounds[6]       mins xyz, maxs xyz in Q12.4
uint8    reserved[8]
```

Vertex:

```text
int16    x, y, z         idle-pose GoldSrc position in Q12.4
uint8    u, v            normalized texture coordinates
```

Triangle:

```text
uint16   vertex[3]
uint8    texture_index
uint8    flags
```

The final table contains one `uint32` C15PAK asset ID for each model texture.
Those textures use the same indexed `C15TEX` representation and 64-pixel
maximum dimension as world textures. A model's complete resident cost is its
`C15MDL` chunk plus only these referenced texture chunks.

## `C15ANM` version 1

First-person `idle`, `fire`, `reload` and `draw` sequences are evaluated from
the source StudioMDL bones by the host compiler. World-player chunks contain
`idle` and `walk`: the walk pose uses the original GoldSrc gait below
`Bip01 Spine`, retains `ref_aim_carbine` on the spine and upper body, and
bone-merges the held weapon against the resulting parent matrices. Roughly
6-10 Hz samples preserve authored motion while keeping the package compact.
The target reads one frame in blocks through its 2 KiB scratch area and copies
only XYZ into the resident interleaved model vertices; UVs and topology remain
in `C15MDL`.

Header:

```text
char     magic[8]        "C15ANM1\0"
uint32   version         1
uint32   file_size
uint32   source_crc32    must match the paired C15MDL
uint32   vertex_count    must match the paired C15MDL
uint32   sequence_count  4 first-person, 2 world-player
uint32   frame_stride    vertex_count * 6
```

Each 24-byte sequence record is:

```text
char     action[4]       IDLE/FIRE/RLOD/DRAW, or IDLE/WALK
uint16   frame_count
uint16   frame_ms
uint32   source_sequence
uint32   source_frames
uint32   source_fps_q8
uint32   frame_offset
```

Every frame contains `vertex_count` absolute positions:

```text
int16    x, y, z         animated GoldSrc position in Q12.4
```

The package validator rejects duplicate actions, mismatched model CRC or
vertex count, invalid frame dimensions, out-of-file ranges and overlapping
sequence payloads.

## `MSP0` muzzle sprite

The compiler reads the historical additive GoldSrc `muzzleflash1.spr` and
`muzzleflash2.spr` files, box-filters them to 23x23, and quantizes the
result to a shared 16-color RGB565 palette. Palette index zero is transparent;
the remaining colors are saturated-additive over the rendered scene.

```text
char     magic[4]        "MSP1"
uint16   file_size
uint8    width
uint8    height
uint8    frame_count
uint8    anchor_count
uint8    display_size
uint8    reserved
uint16   palette[16]
int16    anchors[][3]    StudioMDL muzzle positions in Q12.4
uint8    pixels[]        two 4-bit indices per byte, low nibble first
```

There is one weapon-specific `MSP0` entry for every firearm in the 23-item
M17 weapon table (Knife has no muzzle flash). Although weapons share the
historical sprite art, their baked StudioMDL attachment tracks differ. At
runtime the current attachment is projected with the same view-model
placement, recoil and bob as the weapon.

## `SND0` streamed sound bank

The 43 historical mono WAV cues are converted offline to 11025 Hz signed
16-bit PCM and concatenated into one `sound/game` entry. No decoded sound or
ring buffer is resident on the target; 512-byte source blocks are read through
the shared 2 KiB scratch buffer, duplicated to 22050 Hz, mixed, and written as
1024-byte firmware PCM blocks.

```text
char     magic[8]        "C15SND1\0"
uint32   sample_rate     11025
uint16   cue_count       43
uint16   reserved        0
```

Each cue record contains:

```text
uint32   offset
uint32   pcm_bytes       even; signed 16-bit mono samples
```

Cues 0-22 follow the runtime weapon enum (Knife through M249), cue 23 is the
generic reload sound, and cues 24-28 are C4 plant, beep, explosion, disarm and
disarmed. Cues 29-42 provide footsteps, flesh/head hits, death, ricochet,
grenade bounce and three detonation types, hostage response, team-win radio,
armor and pain. Historical 22050 Hz cues are decimated to 11025 Hz offline;
the target duplicates samples while mixing. Player and Bot playback use two
logical streamed voices mixed with saturation into each firmware block.
