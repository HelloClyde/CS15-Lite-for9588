# Contributing

Run the host tests and a clean BDA build before opening a change:

```powershell
git submodule update --init
.\sdk\scripts\setup_toolchain.ps1
.\tools\test.ps1
.\tools\build.ps1 -Clean
```

Do not commit commercial game data, generated `CS15.C15PAK` files, BBK
firmware, NAND images, or local emulator state. Keep true-device evidence
such as `runtime.log` free of personal paths and unrelated data.

The static image must remain within 1.5 MiB. Changes affecting resident map,
texture or model memory should update the high-water documentation in
`README.md`.
