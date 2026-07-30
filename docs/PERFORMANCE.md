# Performance baseline

CS15 Lite writes a metrics block to
`A:\应用\数据\CS15LITE\runtime.log` every five seconds. The block records
game FPS, render and framebuffer-submit time, visible/drawn geometry, Bot
activity, audio underruns and arena peaks. It is intentionally usable on an
unmodified 9588 without a profiler or network connection.

Current builds also record five-second average and maximum costs for
`logic`, `audio`, `world`, `entities`, `view`, `hud` and `present`. Average
fields end in `_avg_ms_x10`; divide by 10 to obtain milliseconds. Maximum
fields end in `_max_ms`. `entity_pvs_culled` reports the number of living
Bot models rejected by the current BSP potentially-visible set in the most
recent frame.

M20 performance pass 3 further splits logic into `logic_fire`,
`logic_player`, `logic_bot` and `logic_objective`. `logic_steps_avg_x10`,
`logic_steps_max` and `logic_skipped_steps` expose fixed-step catch-up. At
most two 40 ms simulation steps run before a frame; older debt is discarded
so a temporary stall cannot trigger four full Bot/collision updates and feed
another stall.

Performance pass 4 keeps the four shared T/CT locomotion chunks and the
current first-person weapon animation resident. Changing an animation frame
therefore copies positions from memory instead of issuing up to four FAT reads
from `CS15.C15PAK`. `persistent_animation_bytes`, `animation_resident_bytes`
and `animation_peak_bytes` report this arena.

Performance pass 5 promotes every first-person model, texture, muzzle sprite
and animation into one 4 MiB heap arena during startup. Weapon selection then
switches shallow runtime views without touching FAT. The current M20 pack uses
about 3.7 MiB; allocation failure retains the pass-4 on-demand path.
`view_cache_resident_bytes` confirms which path was selected.

Performance pass 6 specializes the hot world/model scanline loop for opaque
textures, with a second direct-mask path for power-of-two dimensions. The
common path caches texture metadata, accepts already-in-range UVs without
reciprocal wrapping, advances frame/depth pointers directly and accumulates
pixel counters in a register. Transparent textures retain the exact generic
wrapping and colour-key path.

Performance pass 7 writes solid RGB565 spans as aligned 32-bit colour pairs,
uses exact 32-bit Q14 world transforms for the signed-16-bit converted map
domain and compiles only the renderer/display translation units for speed.
The rest of the executable retains the size-oriented build. It also reports
`world_clear_avg_ms_x10` and `world_clear_max_ms`, separating framebuffer and
depth clearing from the remaining world stage.

## M18 simulator baseline and M20 asset acceptance

The M18 classic-world-texture pack and BDA were run through the hardware
emulator, not a host build of the renderer. M19 retains those map sections and
adds the classic player-texture profile plus bone-safe vertex seams. Iceworld
and Dust2 VERT, SURF, PLAN, MARK, CLIP, MODL, TNAM and SPWN sections were
compared byte-for-byte with v0.1.3. The M19 pack then passed the deep asset
validator and host renderer tests. M20 additionally makes BSP PVS and all
selected-map textures resident, and grows the cached visible-surface list to
the full 10,240-surface bitset capacity. This fixes dense leaves such as
cs_italy (3,980 visible surfaces), which exceeded the old 1,536-entry list.
True-device model acceptance still uses the procedure below.

| Check | Result |
|---|---:|
| Resource pack | 12,552,216 B |
| M18 Iceworld emulator HUD | 25.6 FPS |
| Iceworld map arena allocation | 40,000 B |
| Iceworld texture arena allocation | 155,648 B |
| Iceworld compiled texture set | 15,360 B |
| Iceworld texture profile | v0.1.3-compatible, max 64 / 256 colours |
| Terrorist mesh | 579 compiled bone/UV vertices / 740 source triangles |
| Counter-Terrorist mesh | 594 compiled UV vertices / 790 source triangles |
| Player skins | max 64 pixels / 256 colours; shared per team |

The renderer is capped at 25 FPS. Small fluctuations above 25 are caused by
the 500 ms smoothed counter. Large-map acceptance is therefore `P10 >= 24.0`
with no resource-load failure and no audio short writes.

## True-device run

1. Delete an older `CS15.C15PAK`, then install the matching BDA and pack.
2. Start a map, leave the buy menu, and play for at least 60 seconds.
3. Exit with the physical exit key so the final metrics are flushed.
4. Copy `A:\应用\数据\CS15LITE\runtime.log` to the PC.
5. Run:

```powershell
.\tools\analyze-runtime-log.ps1 .\runtime.log `
  -Output .\build\performance\device.json
```

For a representative result, benchmark Dust2, Nuke and Office separately.
Keep audio enabled and use the same AI difficulty for every run. A device
result is not interchangeable with emulator FPS: the emulator is useful for
deterministic regressions, while the device log is the acceptance source for
LCD submission and storage fragmentation.

## Hotspot policy

- BSP PVS data and all world textures are resident for the selected map.
  This removes mid-round FAT seeks while keeping the largest combined
  map/texture arena plan below 2.4 MiB.
- Visible-surface indices are cached per leaf, avoiding a complete BSP scan on
  every frame.
- World textures match the v0.1.3 maximum 64-pixel authored mip and retain all
  256 colours by default. The 32-pixel/64-colour profile and source-mip0
  experiment are explicit build options, never the default.
- Player models preserve every source strip/fan triangle. Their shared team
  skins use a 64-pixel authored mip by default; the former 16-pixel profile is
  an explicit low-memory option. Coincident vertices assigned to different
  bones remain separate so locomotion cannot tear a welded joint seam.
- The visible-surface cache covers the complete 10,240-surface bitset. The
  current worst case is cs_italy at 3,980 surfaces in one PVS.
- Models outside 1,800 world units are skipped; held weapons outside 900 units
  are omitted.
- Living Bot models outside the camera leaf's BSP PVS are rejected before
  vertex transforms. PVS is conservative, so this does not reduce model detail
  or hide a Bot that the map compiler considers potentially visible.
- Model scan conversion uses an exact Q16 hardware-division fast path instead
  of repeated software 64-bit division. The fallback is retained for unusual
  oversized intermediate values.
- First-person model depth is cleared lazily in touched 8x8 tiles. Untouched
  world-depth tiles are left alone until the normal next-frame world clear.
- The RGB565 frame is submitted directly in 8x8 rotated tiles.
- Historical PCM is made resident at map load when the arena allocation
  succeeds; the 512-byte streaming path remains as a low-memory fallback.
- T/CT locomotion is resident in the static animation arena. All first-person
  weapon models, textures, muzzle sprites and animation frames are held in the
  heap view cache; an allocation failure falls back to one current animation.
- Touch input drains up to 64 raw notifications per pump, samples MOVE before
  UP can replace the firmware's cached coordinate, and polls the absolute pen
  position on every pump while held.
