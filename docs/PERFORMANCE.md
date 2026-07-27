# Performance baseline

CS15 Lite writes a metrics block to
`A:\应用\数据\CS15LITE\runtime.log` every five seconds. The block records
game FPS, render and framebuffer-submit time, visible/drawn geometry, Bot
activity, audio underruns and arena peaks. It is intentionally usable on an
unmodified 9588 without a profiler or network connection.

## M15 simulator acceptance

The final seven-map M15 pack and BDA were run through the hardware emulator,
not a host build of the renderer.

| Check | Result |
|---|---:|
| Nuke gameplay samples | 10 |
| Average / median FPS | 24.84 / 24.80 |
| P10 FPS | 24.70 |
| Office map arena | 1,075,328 B of 1,088,000 B |
| Nuke map arena | 834,952 B of 848,000 B |
| Inferno map arena | 783,664 B of 800,000 B |
| Iceworld map arena | 32,177 B of 36,000 B |
| Audio short writes | 0 |

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
- World textures are 64-colour, at most 32x32, and are lit during rasterization
  without full-precision lightmaps.
- Models outside 1,800 world units are skipped; held weapons outside 900 units
  are omitted.
- The RGB565 frame is submitted directly in 8x8 rotated tiles.
- Audio reads 512-byte resource blocks and uses the shared 2 KiB scratch area.
