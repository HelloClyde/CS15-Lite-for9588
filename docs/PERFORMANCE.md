# Performance baseline

CS15 Lite writes a metrics block to
`A:\应用\数据\CS15LITE\runtime.log` every five seconds. The block records
game FPS, render and framebuffer-submit time, visible/drawn geometry, Bot
activity, audio underruns and arena peaks. It is intentionally usable on an
unmodified 9588 without a profiler or network connection.

## M18 simulator baseline and M19 asset acceptance

The M18 classic-world-texture pack and BDA were run through the hardware
emulator, not a host build of the renderer. M19 retains those map sections and
adds the classic player-texture profile plus bone-safe vertex seams. Iceworld
and Dust2 VERT, SURF, PLAN, MARK, CLIP, MODL, TNAM and SPWN sections were
compared byte-for-byte with v0.1.3. The M19 pack then passed the deep asset
validator and host renderer tests; true-device model acceptance still uses the
procedure below.

| Check | Result |
|---|---:|
| Resource pack | 12,552,216 B |
| M18 Iceworld emulator HUD | 25.6 FPS |
| Iceworld map arena allocation | 36,000 B |
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

- BSP PVS data is streamed; resident sections are allocated only for the
  selected map.
- Visible-surface indices are cached per leaf, avoiding a complete BSP scan on
  every frame.
- World textures match the v0.1.3 maximum 64-pixel authored mip and retain all
  256 colours by default. The 32-pixel/64-colour profile and source-mip0
  experiment are explicit build options, never the default.
- Player models preserve every source strip/fan triangle. Their shared team
  skins use a 64-pixel authored mip by default; the former 16-pixel profile is
  an explicit low-memory option. Coincident vertices assigned to different
  bones remain separate so locomotion cannot tear a welded joint seam.
- Large maps keep a bounded front-facing PVS texture cache and page a material
  on demand when the conservative visible set is larger than that cache.
  Geometry and collision remain resident to avoid per-polygon FAT seeks.
- Models outside 1,800 world units are skipped; held weapons outside 900 units
  are omitted.
- The RGB565 frame is submitted directly in 8x8 rotated tiles.
- Audio reads 512-byte resource blocks and uses the shared 2 KiB scratch area.
