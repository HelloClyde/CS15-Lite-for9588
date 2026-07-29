# Performance baseline

CS15 Lite writes a metrics block to
`A:\应用\数据\CS15LITE\runtime.log` every five seconds. The block records
game FPS, render and framebuffer-submit time, visible/drawn geometry, Bot
activity, audio underruns and arena peaks. It is intentionally usable on an
unmodified 9588 without a profiler or network connection.

## M17 simulator acceptance

The M17 original-texture pack and BDA were run through the hardware emulator,
not a host build of the renderer. World-texture compression was left at its
default `off` setting. Iceworld loaded into the buy menu and gameplay with the
source mip level 0 and all 256 palette entries.

| Check | Result |
|---|---:|
| Resource pack | 19,772,440 B |
| Iceworld gameplay HUD | 25.0 FPS |
| Iceworld map arena allocation | 36,000 B |
| Iceworld texture arena allocation | 155,648 B |
| Iceworld compiled texture set | 150,528 B |
| Iceworld texture profile | original mip0 / 256 colours |

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
- World textures retain source mip level 0 and all 256 colours by default.
  The 32-pixel/64-colour profile is an explicit build option, never the
  default.
- Large maps keep a bounded front-facing PVS texture cache and page a material
  on demand when the conservative visible set is larger than that cache.
  Geometry and collision remain resident to avoid per-polygon FAT seeks.
- Models outside 1,800 world units are skipped; held weapons outside 900 units
  are omitted.
- The RGB565 frame is submitted directly in 8x8 rotated tiles.
- Audio reads 512-byte resource blocks and uses the shared 2 KiB scratch area.
